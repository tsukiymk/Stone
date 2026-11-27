[CmdletBinding()]
param([Parameter(Mandatory)][string]$RuntimeDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../../../..").Path
$debugRoot = Join-Path $root 'bin/x64/Debug'
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
if (!$runtime.StartsWith($debugRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -and
    $runtime -ine $debugRoot) { throw 'ANGLE runtime checks require inputs under bin/x64/Debug.' }
# Load by absolute path with the same dependency search flags as Stone. This
# uses the deployed DLLs beside the selected runtime's executable.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class AngleRuntimeProbe {
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32", SetLastError=true)]
    public static extern IntPtr GetProcAddress(IntPtr module, string name);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate IntPtr Lookup(string name);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate IntPtr Platform(uint platform, IntPtr native, IntPtr[] attributes);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate uint Initialize(IntPtr display, out int major, out int minor);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate uint Bind(uint api);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate uint Choose(IntPtr display, int[] attrs, [Out] IntPtr[] configs, int size, out int count);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate IntPtr Context(IntPtr display, IntPtr config, IntPtr share, int[] attrs);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate IntPtr Surface(IntPtr display, IntPtr config, int[] attrs);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate uint Current(IntPtr display, IntPtr draw, IntPtr read, IntPtr context);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void Color(float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void Clear(uint mask);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void Read(int x, int y, int width, int height, uint format, uint type, [Out] byte[] pixels);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate uint Destroy(IntPtr display, IntPtr value);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate uint Terminate(IntPtr display);
}
'@
$gles = [AngleRuntimeProbe]::LoadLibraryEx((Join-Path $runtime 'libGLESv2.dll'), [IntPtr]::Zero, 0x1100)
$egl = [AngleRuntimeProbe]::LoadLibraryEx((Join-Path $runtime 'libEGL.dll'), [IntPtr]::Zero, 0x1100)
if ($gles -eq [IntPtr]::Zero -or $egl -eq [IntPtr]::Zero) {
    throw "ANGLE LoadLibraryEx failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}
function Get-Proc([IntPtr]$Module, [string]$Name, [type]$Type) {
    $address = [AngleRuntimeProbe]::GetProcAddress($Module, $Name)
    if ($address -eq [IntPtr]::Zero) { throw "Missing ANGLE export: $Name" }
    return [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($address, $Type)
}
$lookup = Get-Proc $egl 'eglGetProcAddress' ([AngleRuntimeProbe+Lookup])
foreach ($name in @('eglCreateImageKHR', 'eglDestroyImageKHR', 'eglDestroySyncKHR', 'eglWaitSyncKHR',
        'glEGLImageTargetTexture2DOES')) {
    if ($lookup.Invoke($name) -eq [IntPtr]::Zero) { throw "Missing ANGLE extension: $name" }
}
foreach ($name in @('eglGetCurrentContext', 'eglGetCurrentDisplay')) {
    if ([AngleRuntimeProbe]::GetProcAddress($egl, $name) -eq [IntPtr]::Zero) { throw "Missing ANGLE export: $name" }
}
foreach ($name in @('glBindTexture', 'glDeleteTextures', 'glFinish', 'glFlush', 'glGetError')) {
    if ([AngleRuntimeProbe]::GetProcAddress($gles, $name) -eq [IntPtr]::Zero) { throw "Missing ANGLE export: $name" }
}
$platform = Get-Proc $egl 'eglGetPlatformDisplay' ([AngleRuntimeProbe+Platform])
$initialize = Get-Proc $egl 'eglInitialize' ([AngleRuntimeProbe+Initialize])
$bind = Get-Proc $egl 'eglBindAPI' ([AngleRuntimeProbe+Bind])
$choose = Get-Proc $egl 'eglChooseConfig' ([AngleRuntimeProbe+Choose])
$context = Get-Proc $egl 'eglCreateContext' ([AngleRuntimeProbe+Context])
$surface = Get-Proc $egl 'eglCreatePbufferSurface' ([AngleRuntimeProbe+Surface])
$current = Get-Proc $egl 'eglMakeCurrent' ([AngleRuntimeProbe+Current])
$destroySurface = Get-Proc $egl 'eglDestroySurface' ([AngleRuntimeProbe+Destroy])
$destroyContext = Get-Proc $egl 'eglDestroyContext' ([AngleRuntimeProbe+Destroy])
$terminate = Get-Proc $egl 'eglTerminate' ([AngleRuntimeProbe+Terminate])
$color = Get-Proc $gles 'glClearColor' ([AngleRuntimeProbe+Color])
$clear = Get-Proc $gles 'glClear' ([AngleRuntimeProbe+Clear])
$read = Get-Proc $gles 'glReadPixels' ([AngleRuntimeProbe+Read])
# D3D11 WARP makes CI independent of a physical GPU.
$display = $platform.Invoke(0x3202, [IntPtr]::Zero, [IntPtr[]]@(0x3203, 0x3208, 0x3209, 0x320b, 0x3038))
$ctx = $surf = [IntPtr]::Zero
try {
    $major = $minor = 0
    if (!$initialize.Invoke($display, [ref]$major, [ref]$minor) -or !$bind.Invoke(0x30a0)) { throw 'ANGLE display initialization failed.' }
    $configs = [IntPtr[]]@([IntPtr]::Zero)
    $count = 0
    if (!$choose.Invoke($display, @(0x3033, 1, 0x3040, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038), $configs, 1, [ref]$count) -or !$count) {
        throw 'ANGLE pbuffer configuration failed.'
    }
    $ctx = $context.Invoke($display, $configs[0], [IntPtr]::Zero, @(0x3098, 2, 0x3038))
    $surf = $surface.Invoke($display, $configs[0], @(0x3057, 1, 0x3056, 1, 0x3038))
    if ($ctx -eq [IntPtr]::Zero -or $surf -eq [IntPtr]::Zero -or !$current.Invoke($display, $surf, $surf, $ctx)) {
        throw 'ANGLE context/pbuffer creation failed.'
    }
    $color.Invoke(1, 0, 0, 1)
    $clear.Invoke(0x4000)
    $pixel = [byte[]]::new(4)
    $read.Invoke(0, 0, 1, 1, 0x1908, 0x1401, $pixel)
    if (($pixel -join ',') -ne '255,0,0,255') { throw "ANGLE readback mismatch: $pixel" }
    Write-Host "PASS: bundled ANGLE exports, EGL $major.$minor, D3D11 WARP pbuffer and red pixel readback."
} finally {
    [void]$current.Invoke($display, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($surf -ne [IntPtr]::Zero) { [void]$destroySurface.Invoke($display, $surf) }
    if ($ctx -ne [IntPtr]::Zero) { [void]$destroyContext.Invoke($display, $ctx) }
    [void]$terminate.Invoke($display)
}
