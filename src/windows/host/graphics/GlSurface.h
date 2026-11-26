#pragma once

#include <jni.h>
#include <cstdint>
#include <vector>

bool BeginWindowsGlFrame(int width, int height);
bool BeginWindowsWebViewGlFrame(int width, int height);
bool ReadWindowsGlFrame(std::vector<jint>& pixels, int width, int height);
bool CaptureWindowsGlFrame(std::vector<jint>& pixels, int width, int height);
void CloseWindowsGl();
uintptr_t WindowsAngleExport(bool egl, const char* name);
void* WindowsAngleGuestDisplay();
unsigned int InitializeWindowsAngleDisplay(void* display, int* major, int* minor);
