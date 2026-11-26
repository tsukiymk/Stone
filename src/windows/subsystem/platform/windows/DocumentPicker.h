#pragma once

#include <jni.h>
#include <windows.h>

inline constexpr UINT WindowsDocumentPickerMessage = WM_APP + 6;

bool RegisterWindowsDocumentPicker(JNIEnv* env);
void ShowWindowsDocumentPicker(JNIEnv* env, HWND owner);
void CloseWindowsDocumentPicker();
