#include "stub-libandroid-input.h"
#include "stub-libc.h"
#include "runtime/art/ArtJni.h"

#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

namespace stone::windows {
namespace {
constexpr size_t AxisCount = 64;
constexpr jint MotionType = 2; // AINPUT_EVENT_TYPE_MOTION
constexpr size_t CurrentSample = SIZE_MAX;

enum Axis : size_t {
    X,
    Y,
    Pressure,
    Size,
    TouchMajor,
    TouchMinor,
    ToolMajor,
    ToolMinor,
    Orientation
};

struct Pointer {
    jint id, toolType;
    jfloat rawX, rawY;
};
}

struct AndroidMotionEvent {
    jint source, deviceId, action, metaState, buttonState;
    int64_t downTime, eventTime;
    std::vector<Pointer> pointers;
    std::vector<int64_t> historyTimes;
    // Samples are in time order, with the current sample last.
    std::vector<std::array<float, AxisCount>> axes;

    float AxisValue(int axis, size_t pointer, size_t sample = CurrentSample) const
    {
        if (axis < 0 || axis >= static_cast<int>(AxisCount) || pointer >= pointers.size())
            return 0;
        if (sample == CurrentSample)
            sample = historyTimes.size();
        if (sample > historyTimes.size())
            return 0;
        return axes[sample * pointers.size() + pointer][static_cast<size_t>(axis)];
    }
};

AndroidMotionEvent* CopyAndroidMotionEvent(JNIEnv* env, jobject motionEvent)
{
    if (!env || !motionEvent || ArtJni<&JNINativeInterface_::ExceptionCheck>(env) ||
        ArtJni<&JNINativeInterface_::PushLocalFrame>(env, 2) < 0)
        return nullptr;
    const auto popLocalFrame = [env](void*) {
        ArtJni<&JNINativeInterface_::PopLocalFrame>(env, nullptr);
    };
    const std::unique_ptr<void, decltype(popLocalFrame)> frame(motionEvent, popLocalFrame);
    const auto type = ArtJni<&JNINativeInterface_::GetObjectClass>(env, motionEvent);
    if (!type)
        return nullptr;

    enum Member {
        Source,
        Device,
        Action,
        Meta,
        Button,
        PointerCount,
        HistoryCount,
        DownTime,
        EventTime,
        HistoricalTime,
        Id,
        ToolType,
        AxisValue,
        HistoricalAxis,
        RawX,
        RawY,
        MemberCount
    };

    struct Signature {
        const char* name;
        const char* descriptor;
    };

    constexpr std::array<Signature, MemberCount> signatures{{
        {"getSource", "()I"},
        {"getDeviceId", "()I"},
        {"getAction", "()I"},
        {"getMetaState", "()I"},
        {"getButtonState", "()I"},
        {"getPointerCount", "()I"},
        {"getHistorySize", "()I"},
        {"getDownTime", "()J"},
        {"getEventTimeNanos", "()J"},
        {"getHistoricalEventTimeNanos", "(I)J"},
        {"getPointerId", "(I)I"},
        {"getToolType", "(I)I"},
        {"getAxisValue", "(II)F"},
        {"getHistoricalAxisValue", "(III)F"},
        {"getRawX", "(I)F"},
        {"getRawY", "(I)F"},
    }};
    std::array<jmethodID, MemberCount> methods{};
    for (size_t i = 0; i < methods.size(); ++i) {
        methods[i] = ArtJni<&JNINativeInterface_::GetMethodID>(env, type, signatures[i].name, signatures[i].descriptor);
        if (!methods[i])
            return nullptr;
    }

    struct PendingJavaException {};

    const auto call = [&]<auto Invoke>(Member member, auto... arguments) {
        const std::array<jvalue, sizeof...(arguments)> values{jvalue{.i = static_cast<jint>(arguments)}...};
        const auto value = ArtJni<Invoke>(env, motionEvent, methods[member], values.data());
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            throw PendingJavaException{};
        return value;
    };
    const auto integer = [&](Member member, auto... arguments) {
        return call.operator()<&JNINativeInterface_::CallIntMethodA>(member, arguments...);
    };
    const auto floating = [&](Member member, auto... arguments) {
        return call.operator()<&JNINativeInterface_::CallFloatMethodA>(member, arguments...);
    };
    const auto wide = [&](Member member, auto... arguments) {
        return call.operator()<&JNINativeInterface_::CallLongMethodA>(member, arguments...);
    };
    try {
        const auto pointerCount = integer(PointerCount);
        const auto historyCount = integer(HistoryCount);
        if (pointerCount <= 0 || historyCount < 0)
            return nullptr;
        auto result = std::make_unique<AndroidMotionEvent>();
        result->source = integer(Source);
        result->deviceId = integer(Device);
        result->action = integer(Action);
        result->metaState = integer(Meta);
        result->buttonState = integer(Button);
        const auto downMillis = wide(DownTime);
        result->downTime = static_cast<int64_t>(static_cast<uint64_t>(downMillis) * 1'000'000);
        result->eventTime = wide(EventTime);

        const auto pointers = static_cast<size_t>(pointerCount);
        const auto history = static_cast<size_t>(historyCount);
        result->pointers.resize(pointers);
        result->historyTimes.resize(history);
        result->axes.resize((history + 1) * pointers);
        for (jint i = 0; i < pointerCount; ++i) {
            result->pointers[i] = {integer(Id, i), integer(ToolType, i), floating(RawX, i), floating(RawY, i)};
        }
        for (size_t sample = 0; sample <= history; ++sample) {
            const bool current = sample == history;
            const auto index = static_cast<jint>(sample);
            if (!current)
                result->historyTimes[sample] = wide(HistoricalTime, index);
            for (jint pointer = 0; pointer < pointerCount; ++pointer) {
                auto& readings = result->axes[sample * pointers + static_cast<size_t>(pointer)];
                for (jint axis = 0; axis < static_cast<jint>(AxisCount); ++axis) {
                    readings[axis] =
                        current ? floating(AxisValue, axis, pointer) : floating(HistoricalAxis, axis, pointer, index);
                }
            }
        }
        return result.release();
    }
    catch (const PendingJavaException&) {
        return nullptr;
    }
    catch (const std::bad_alloc&) {
        return nullptr;
    }
    catch (const std::length_error&) {
        return nullptr;
    }
}

uintptr_t AndroidInputImport(std::string_view symbol, bool hostAbi)
{
#define BIND(name, ...)                                                                                                \
    if (symbol == #name) {                                                                                             \
        const auto target = reinterpret_cast<uintptr_t>(__VA_ARGS__);                                                  \
        return hostAbi ? target : BridgeAndroidImport(symbol, target);                                                 \
    }
#define FIELD(name, member)                                                                                            \
    BIND(                                                                                                              \
        name, +[](const AndroidMotionEvent* event) {                                                                   \
            return event->member;                                                                                      \
        })
#define AXIS(name, axis)                                                                                               \
    BIND(                                                                                                              \
        name, +[](const AndroidMotionEvent* event, size_t pointer) {                                                   \
            return event->AxisValue(axis, pointer);                                                                    \
        })
#define HISTORY(name, axis)                                                                                            \
    BIND(                                                                                                              \
        name, +[](const AndroidMotionEvent* event, size_t pointer, size_t sample) {                                    \
            return sample < event->historyTimes.size() ? event->AxisValue(axis, pointer, sample) : 0.0f;               \
        })
    BIND(
        AInputEvent_getType, +[](const AndroidMotionEvent*) {
            return MotionType;
        });
    BIND(
        AInputEvent_release, +[](const AndroidMotionEvent* event) {
            delete event;
        });
    FIELD(AInputEvent_getSource, source);
    FIELD(AInputEvent_getDeviceId, deviceId);
    FIELD(AMotionEvent_getAction, action);
    FIELD(AMotionEvent_getMetaState, metaState);
    FIELD(AMotionEvent_getButtonState, buttonState);
    FIELD(AMotionEvent_getDownTime, downTime);
    FIELD(AMotionEvent_getEventTime, eventTime);
    BIND(
        AMotionEvent_getPointerCount, +[](const AndroidMotionEvent* event) {
            return event->pointers.size();
        });
    BIND(
        AMotionEvent_getHistorySize, +[](const AndroidMotionEvent* event) {
            return event->historyTimes.size();
        });
    BIND(
        AMotionEvent_getHistoricalEventTime, +[](const AndroidMotionEvent* event, size_t sample) -> int64_t {
            return sample < event->historyTimes.size() ? event->historyTimes[sample] : 0;
        });
    BIND(
        AMotionEvent_getPointerId, +[](const AndroidMotionEvent* event, size_t pointer) {
            return pointer < event->pointers.size() ? event->pointers[pointer].id : -1;
        });
    BIND(
        AMotionEvent_getToolType, +[](const AndroidMotionEvent* event, size_t pointer) {
            return pointer < event->pointers.size() ? event->pointers[pointer].toolType : 0;
        });
    BIND(
        AMotionEvent_getRawX, +[](const AndroidMotionEvent* event, size_t pointer) {
            return pointer < event->pointers.size() ? event->pointers[pointer].rawX : 0.0f;
        });
    BIND(
        AMotionEvent_getRawY, +[](const AndroidMotionEvent* event, size_t pointer) {
            return pointer < event->pointers.size() ? event->pointers[pointer].rawY : 0.0f;
        });
    BIND(
        AMotionEvent_getAxisValue, +[](const AndroidMotionEvent* event, jint axis, size_t pointer) {
            return event->AxisValue(axis, pointer);
        });
    BIND(
        AMotionEvent_getHistoricalAxisValue,
        +[](const AndroidMotionEvent* event, jint axis, size_t pointer, size_t sample) {
            return sample < event->historyTimes.size() ? event->AxisValue(axis, pointer, sample) : 0.0f;
        });
    AXIS(AMotionEvent_getX, X);
    AXIS(AMotionEvent_getY, Y);
    AXIS(AMotionEvent_getPressure, Pressure);
    AXIS(AMotionEvent_getTouchMajor, TouchMajor);
    AXIS(AMotionEvent_getTouchMinor, TouchMinor);
    AXIS(AMotionEvent_getOrientation, Orientation);
    HISTORY(AMotionEvent_getHistoricalX, X);
    HISTORY(AMotionEvent_getHistoricalY, Y);
    HISTORY(AMotionEvent_getHistoricalTouchMajor, TouchMajor);
#undef HISTORY
#undef AXIS
#undef FIELD
#undef BIND
    return 0;
}
}
