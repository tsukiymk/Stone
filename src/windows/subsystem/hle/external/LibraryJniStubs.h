#pragma once

#include <string_view>
#include <jni.h>

namespace stone::windows {

void* MatrixJniHookFallback(std::string_view name, std::string_view signature);
unsigned MmkvJniPathArguments(std::string_view name, std::string_view signature);

struct CsoJniPaths {
    unsigned arguments;
    unsigned arrayArguments;
};

CsoJniPaths CsoJniPathArguments(std::string_view name, std::string_view signature);
bool IsWebViewDirectoryMethod(JNIEnv* env, jclass type, std::string_view name, std::string_view signature);
bool IsWebViewDrawFunctionTable(JNIEnv* env, jclass type, std::string_view name, std::string_view signature);
jlong JNICALL WebViewDrawFunctionTable(JNIEnv* env, jclass type);
bool RegisterWebViewCompositor(JNIEnv* env, JavaVM* vm);
void CloseWebViewCompositor();

}
