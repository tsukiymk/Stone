#pragma once

#include <string>
#include <cstdint>

#include <jni.h>
#include <windows.h>
#include <span>
#include <vector>

HWND WindowsHostWindow();
bool SetWindowsHostIcon(std::span<const jbyte> png);
bool SetWindowsHostAppearance(const std::wstring& label, const std::wstring& icon, const std::wstring& command);
void SetWindowsHostAppUserModelId(const wchar_t* id);
using WindowsTouchSink = void (*)(int action, float x, float y, int buttons) noexcept;
using WindowsBackSink = void (*)() noexcept;

struct WindowsArtInput {
    WindowsTouchSink touch;
    WindowsBackSink back;
    void (*key)(bool down, int code, int modifiers, int repeat) noexcept;
    void (*text)(wchar_t character) noexcept;
    void (*focus)(bool focused) noexcept;
    void (*scroll)(bool horizontal, float x, float y, int delta) noexcept;
};

bool OpenWindowsArtWindow(WindowsArtInput input);
bool CloseWindowsArtWindow();
bool SetWindowsArtResizable(bool enabled);
bool SetWindowsArtOrientation(bool wide);
uint64_t WindowsArtClientSize();
bool PresentWindowsArtFrame(std::vector<jint>& frame, int width, int height, bool secure = false);
int WindowsArtCaptureProtection();
void SetWindowsVulkanPresentation(bool active);
HMONITOR WindowsHostMonitor();
int AndroidDisplayDensity();
bool RegisterWindowsNatives(JNIEnv* env);
bool RegisterWindowsLog(JNIEnv* env);
void PresentWindowsNativeFrame(std::span<const jint> pixels, int width, int height);
