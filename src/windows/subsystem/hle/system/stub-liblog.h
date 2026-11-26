#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace stone::windows {
struct AndroidLogMessage {
    size_t size;
    int32_t buffer, priority;
    const char* tag;
    const char* file;
    uint32_t line;
    const char* text;
};

static_assert(sizeof(AndroidLogMessage) == 48);
void WriteAndroidLogMessage(const AndroidLogMessage* message);
void WriteAndroidLogToStderr(const AndroidLogMessage* message);
void CallAndroidLogAborter(const char* message);
bool CallAndroidLogAborterIfRegistered(const char* message);
void SetTranslatedAndroidLogger(std::function<void(const AndroidLogMessage*)> callback);
void SetTranslatedAndroidAborter(std::function<void(const char*)> callback);
int IsAndroidLogEnabled(int priority, const char* tag, int fallback);
void SetAndroidLogDefaultTag(const char* tag);
}
