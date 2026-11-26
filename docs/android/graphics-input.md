# Graphics, windows, input, and configuration

Stone drives image ViewRoot, software Canvas, and the current Activity inside its own Win32 window. Fonts, resources, and libhwui come from the selected image. Client manages host windows and input events.

## Software drawing and bitmaps

An Activity root can cache a software frame until invalidated. Layout, animation, View invalidation, and explicit redraws trigger new drawing. Draw observers still run each frame, and invalidations during drawing are not lost. Failed frames are not cached as valid. Resizing and destruction release old bitmaps and Canvas objects; child windows are composited independently.

Software windows convert ImageDecoder default/hardware allocator requests and BitmapFactory HARDWARE preferences to software storage while keeping image decoding in the image implementation. BitmapFactory uses a separate Options copy and writes back output fields without temporarily modifying caller inputs.

New bitmap defaults follow the window's current display density, including Windows DPI changes. This keeps cached text images at their measured pixel size when drawn on the software Canvas, while bitmaps with an explicit resource density retain Android's normal density scaling.

BitmapRegionDecoder uses the image's native decoder for cropped image regions. Native x64 ART also registers the image EGL14 JNI alongside the existing EGL and GLES bindings; software window and hardware Surface limitations still apply.

Stone's NativeWindow, image queues, and HardwareBuffer cannot be mixed with image-native handles. Hardware Surface conversions, drivers, and GPU paths have separate limits. Working software pixels do not establish support for the complete Android graphics stack.

Software Surface requests for a nonzero frame rate return an unsupported status; they do not change the Windows display refresh rate. Clearing a rate succeeds because no refresh-rate vote is retained. Invalid rates and public compatibility/strategy values retain Android argument errors. Image-owned Surfaces keep the image's frame-rate implementation, while Stone handles never enter it.

The Windows-backed video MediaCodecs can publish decoded pixels into a Stone software Surface queue. SurfaceView uses the software composition path. SurfaceTexture streams upload RGBA buffers to a real ANGLE external-OES texture, preserving frame timestamps and image Handler notifications; EGL14 can render back into a software Surface. Single-buffer SurfaceTexture mode is unsupported. See [audio and media](audio.md#video-mediacodec) for decoder formats and output modes.

Guest EGL first probes the Windows D3D11 ANGLE backend, then Vulkan and OpenGL, and finally D3D11 WARP. A display handle is selected only after initialization succeeds. Available backends depend on the ANGLE build and host drivers.

## Window resizing

The launcher Activity's resizeableActivity takes precedence over the application declaration; aliases use their target's policy. Default policy follows the target SDK and image resizeMode. Resizable windows provide borders and a maximize button; non-resizable windows keep a fixed size.

After a resize drag ends, maximization, or restoration, Stone updates display information, pixel/dp dimensions, orientation, layout, and window bounds, then dispatches image configuration callbacks or recreates the Activity. Minimization does not publish zero dimensions, and a scaled placeholder may remain visible during dragging. Android split-screen and picture-in-picture are not provided.

The visible Activity's fixed portrait/landscape requests also swap the window dimensions through this same configuration path. Releasing a fixed-axis request restores the previous user axis. A maximized window keeps the monitor's geometry; sensor-driven physical device rotation is unavailable.

## Text, keyboard, and mouse

The virtual keyboard uses image `Virtual.kcm` and keylayout files. The image handles case, symbols, modifiers, and shortcut matching. Consecutive Win32 text events can reuse the current input connection, but commit characters individually and recheck the target. Focus changes or event boundaries close the connection.

Left-button presses and drags emulate a single finger with SOURCE_TOUCHSCREEN and TOOL_TYPE_FINGER, including release and capture cancellation. Right-button gestures and wheel events retain mouse semantics. Focus notifies both View and ViewTreeObserver; the image editor draws cursors and editing menus. Removing a child window dispatches detach and releases its input resources.

Committed UTF-16 text is distinct from the complete IME composition process. Windows IME pre-edit text, candidate windows, multitouch, and full keyboard layout/device capabilities remain limited.

## Language and theme

ART initialization reads Windows preferred languages and application light/dark settings and publishes them to LocaleList, the default Locale, and Resources. Host changes are observed in the background. Application language and night-mode overrides are persisted per application; clearing an override resumes following the host.

ACTION_APP_LOCALE_SETTINGS uses the current package URI, with languages from the APK localeConfig. Cancellation saves nothing. Confirmation updates resources through LocaleManager and follows normal Activity recreation. Automatic night mode uses the Windows application theme rather than calculating sunrise and sunset.

## Capture protection

When the current Activity or a visible child window sets FLAG_SECURE, Client applies Windows capture protection to the top-level HWND. Background or hidden pages do not affect the current page. Diagnostic PNG/state output masks protected content as well, without replacing normally displayed content.

Capture policy depends on Windows session and window capabilities. Software drawing, actual host capture, and hardware graphics are separate boundaries; support for one does not establish compatibility for the others.
