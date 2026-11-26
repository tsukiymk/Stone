#include "abi/AndroidLibraryImports.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "stub-libOpenSLES.h"
#include "stub-libc.h"

#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace stone::windows {
namespace {
enum Result : uint32_t {
    Success = 0,
    Preconditions = 1,
    InvalidParameter = 2,
    NoMemory = 3,
    ResourceError = 4,
    BufferInsufficient = 7,
    Unsupported = 12,
};

constexpr uint32_t Stopped = 1, Paused = 2, Playing = 3;
constexpr uint32_t BufferQueue = 6, AndroidSimpleBufferQueue = 0x800007bd, OutputMix = 4;

struct InterfaceId {
    uint32_t low;
    uint16_t mid, version, sequence;
    uint8_t node[6] = {0, 2, 0xa5, 0xd5, 0xc5, 0x1b};
    bool operator==(const InterfaceId&) const = default;
};

static_assert(sizeof(InterfaceId) == 16);
enum class Interface : unsigned {
    Object,
    Engine,
    Play,
    Queue,
    Configuration
};
constexpr std::array ids{
    InterfaceId{0x198e4940, 0xc5d7, 0x11df, 0xa2a6}, // Android simple buffer queue
    InterfaceId{0x8d97c260, 0xddd4, 0x11db, 0x958f}, // Engine
    InterfaceId{0xef0bd9c0, 0xddd7, 0x11db, 0xbf49}, // Play
    InterfaceId{0xc5657aa0, 0xdddb, 0x11db, 0x82f7}, // Record
    InterfaceId{0x09e8ede0, 0xddde, 0x11db, 0xb4f6}, // Volume
    InterfaceId{0x2bc99cc0, 0xddd4, 0x11db, 0x8d99}, // Buffer queue
    InterfaceId{0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c}, // Android configuration
    InterfaceId{0x79216360, 0xddd7, 0x11db, 0xac16}, // Object
    InterfaceId{0x7b491460, 0x8d4d, 0x11e0, 0xbd61}, // Android acoustic echo cancellation
    InterfaceId{0x0a8abfe0, 0x654c, 0x11e0, 0xba26}, // Android automatic gain control
    InterfaceId{0x58b4b260, 0x8e06, 0x11e0, 0xaa8e}, // Android noise suppression
};
constexpr std::array<std::string_view, ids.size()> iidNames{
    "SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
    "SL_IID_ENGINE",
    "SL_IID_PLAY",
    "SL_IID_RECORD",
    "SL_IID_VOLUME",
    "SL_IID_BUFFERQUEUE",
    "SL_IID_ANDROIDCONFIGURATION",
    "SL_IID_OBJECT",
    "SL_IID_ANDROIDACOUSTICECHOCANCELLATION",
    "SL_IID_ANDROIDAUTOMATICGAINCONTROL",
    "SL_IID_ANDROIDNOISESUPPRESSION",
};

struct IidExports {
    std::array<const InterfaceId*, ids.size()> pointers;
    std::array<InterfaceId, ids.size()> values = ids;

    IidExports()
    {
        for (size_t i = 0; i < values.size(); ++i)
            pointers[i] = &values[i];
    }
};

uint32_t UnsupportedEngine(void** engine)
{
    if (!engine)
        return InvalidParameter;
    *engine = nullptr;
    return Unsupported;
}

struct AudioBuffer {
    WAVEHDR header{};
    std::vector<char> samples;
};
enum class ObjectKind {
    Engine,
    Mix,
    Player
};

struct Object {
    ObjectKind kind;
    uintptr_t cells = 0;
    std::mutex mutex;
    bool realized = false, closed = false;
    uint32_t playState = Stopped, completedBuffers = 0, maxBuffers = 0;
    bool configure = false, playedAudio = false;
    WAVEFORMATEX format{};
    HWAVEOUT device = nullptr;
    HANDLE wake = nullptr;
    uintptr_t callback = 0, callbackData = 0;
    std::list<AudioBuffer> queue;
    std::thread worker;

    explicit Object(ObjectKind value) : kind(value)
    {
    }

    uintptr_t Handle(Interface type) const
    {
        return cells + static_cast<unsigned>(type) * sizeof(uintptr_t);
    }

    MMRESULT Clear()
    {
        if (const auto error = waveOutReset(device); error != MMSYSERR_NOERROR)
            return error;
        for (auto& buffer : queue)
            waveOutUnprepareHeader(device, &buffer.header, sizeof(WAVEHDR));
        queue.clear();
        completedBuffers = 0;
        return MMSYSERR_NOERROR;
    }

    ~Object()
    {
        if (device) {
            Clear();
            waveOutClose(device);
        }
        if (wake)
            CloseHandle(wake);
        if (cells)
            FreeArm64GuestMemory(cells);
    }
};

struct Objects {
    std::mutex mutex;
    std::map<uintptr_t, std::shared_ptr<Object>> entries;
};

Objects& LiveObjects()
{
    // WAV completion threads can be alive when the Android process exits.
    static auto* objects = new Objects;
    return *objects;
}

using Vtable = std::array<uintptr_t, 16>;
using Vtables = std::array<Vtable, 5>;

const Vtables& Tables()
{
    static const auto& tables = []() -> const Vtables& {
        const auto unsupported = Arm64OpenSlSymbol("OpenSLES.Unsupported");
        Vtables result;
        for (auto& table : result)
            table.fill(unsupported);
        const auto set = [&](Interface type, size_t slot, std::string_view name) {
            result[static_cast<unsigned>(type)][slot] = Arm64OpenSlSymbol(name);
        };
        set(Interface::Object, 0, "OpenSLES.Realize");
        set(Interface::Object, 2, "OpenSLES.GetObjectState");
        set(Interface::Object, 3, "OpenSLES.GetInterface");
        set(Interface::Object, 6, "OpenSLES.Destroy");
        set(Interface::Engine, 2, "OpenSLES.CreateAudioPlayer");
        set(Interface::Engine, 7, "OpenSLES.CreateOutputMix");
        set(Interface::Play, 0, "OpenSLES.SetPlayState");
        set(Interface::Play, 1, "OpenSLES.GetPlayState");
        set(Interface::Play, 2, "OpenSLES.GetDuration");
        set(Interface::Play, 3, "OpenSLES.GetPosition");
        set(Interface::Queue, 0, "OpenSLES.Enqueue");
        set(Interface::Queue, 1, "OpenSLES.Clear");
        set(Interface::Queue, 2, "OpenSLES.GetQueueState");
        set(Interface::Queue, 3, "OpenSLES.RegisterQueueCallback");
        set(Interface::Configuration, 0, "OpenSLES.SetConfiguration");
        set(Interface::Configuration, 1, "OpenSLES.GetConfiguration");
        const auto storage = AllocateArm64GuestMemory(sizeof(result), false);
        if (!storage)
            throw std::bad_alloc{};
        std::memcpy(reinterpret_cast<void*>(storage), &result, sizeof(result));
        return *reinterpret_cast<const Vtables*>(storage);
    }();
    return tables;
}

std::shared_ptr<Object> Find(uintptr_t handle, Interface type)
{
    auto& objects = LiveObjects();
    const auto address = handle - static_cast<unsigned>(type) * sizeof(uintptr_t);
    const std::lock_guard lock(objects.mutex);
    const auto object = objects.entries.find(address);
    return object != objects.entries.end() ? object->second : nullptr;
}

uintptr_t Publish(std::shared_ptr<Object> object)
{
    const auto& tables = Tables();
    std::array<uintptr_t, 5> cells;
    object->cells = AllocateArm64GuestMemory(sizeof(cells), false);
    if (!object->cells)
        throw std::bad_alloc{};
    for (size_t i = 0; i < cells.size(); ++i)
        cells[i] = reinterpret_cast<uintptr_t>(tables[i].data());
    std::memcpy(reinterpret_cast<void*>(object->cells), cells.data(), sizeof(cells));
    const auto address = object->cells;
    auto& objects = LiveObjects();
    const std::lock_guard lock(objects.mutex);
    objects.entries.emplace(address, std::move(object));
    return address;
}

void ReapAudio(const std::shared_ptr<Object>& object)
{
    while (WaitForSingleObject(object->wake, INFINITE) == WAIT_OBJECT_0) {
        for (;;) {
            uintptr_t callback, data;
            {
                const std::lock_guard lock(object->mutex);
                if (object->closed)
                    return;
                if (object->queue.empty() || !(object->queue.front().header.dwFlags & WHDR_DONE))
                    break;
                auto& header = object->queue.front().header;
                if (waveOutUnprepareHeader(object->device, &header, sizeof(header)) != MMSYSERR_NOERROR)
                    break;
                object->queue.pop_front();
                ++object->completedBuffers;
                if (!object->playedAudio) {
                    object->playedAudio = true;
                    std::fputs("[android-audio] first OpenSL ES buffer played\n", stderr);
                }
                callback = object->callback;
                data = object->callbackData;
            }
            if (!callback)
                continue;
            try {
                CallAndroidCallback(callback, reinterpret_cast<void*>(object->Handle(Interface::Queue)),
                                    reinterpret_cast<void*>(data));
            }
            catch (const std::exception& error) {
                std::fprintf(stderr, "[android-audio] OpenSL ES buffer callback: %s\n", error.what());
            }
        }
    }
}

Result Realize(const std::shared_ptr<Object>& object)
{
    if (object->kind == ObjectKind::Player) {
        const auto wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake)
            return ResourceError;
        HWAVEOUT device{};
        if (waveOutOpen(&device, WAVE_MAPPER, &object->format, reinterpret_cast<DWORD_PTR>(wake), 0, CALLBACK_EVENT) !=
            MMSYSERR_NOERROR) {
            CloseHandle(wake);
            return ResourceError;
        }
        if (waveOutPause(device) != MMSYSERR_NOERROR) {
            waveOutClose(device);
            CloseHandle(wake);
            return ResourceError;
        }
        object->wake = wake;
        object->device = device;
        try {
            object->worker = std::thread(ReapAudio, object);
        }
        catch (...) {
            waveOutClose(device);
            CloseHandle(wake);
            object->device = nullptr;
            object->wake = nullptr;
            throw;
        }
    }
    object->realized = true;
    return Success;
}

void Destroy(const std::shared_ptr<Object>& object)
{
    auto& objects = LiveObjects();
    {
        const std::lock_guard lock(objects.mutex);
        if (!objects.entries.erase(object->cells))
            return;
    }
    {
        const std::lock_guard lock(object->mutex);
        object->closed = true;
        if (object->wake)
            SetEvent(object->wake);
    }
    if (object->worker.joinable()) {
        if (object->worker.get_id() == std::this_thread::get_id())
            object->worker.detach();
        else
            object->worker.join();
    }
}

using Memory = std::function<uint8_t*(uintptr_t, unsigned, bool)>;

struct Guest {
    std::span<const uint64_t, 8> args;
    const Memory& memory;

    template <typename T> T Read(uintptr_t address) const
    {
        T value;
        std::memcpy(&value, memory(address, sizeof(T), false), sizeof(T));
        return value;
    }

    template <typename T> void Write(uintptr_t address, T value) const
    {
        std::memcpy(memory(address, sizeof(T), true), &value, sizeof(T));
    }

    bool IsString(uintptr_t address, std::string_view value) const
    {
        if (!address)
            return false;
        for (size_t i = 0; i < value.size(); ++i) {
            if (Read<uint8_t>(address + i) != static_cast<uint8_t>(value[i]))
                return false;
        }
        return Read<uint8_t>(address + value.size()) == 0;
    }
};

Result RequiredInterfaces(const Guest& guest, Object& object, unsigned index)
{
    const auto count = static_cast<uint32_t>(guest.args[index]);
    const auto pointers = guest.args[index + 1], required = guest.args[index + 2];
    if (!count)
        return Success;
    if (!pointers || !required || count > 255)
        return InvalidParameter;
    for (uint32_t i = 0; i < count; ++i) {
        const auto address = guest.Read<uintptr_t>(pointers + i * sizeof(uintptr_t));
        if (!address)
            return InvalidParameter;
        const auto id = guest.Read<InterfaceId>(address);
        if (object.kind == ObjectKind::Player && id == ids[6])
            object.configure = true;
        else if (id == ids[7] || (object.kind == ObjectKind::Engine && id == ids[1]) ||
                 (object.kind == ObjectKind::Player && (id == ids[0] || id == ids[2] || id == ids[5])))
            continue;
        else if (guest.Read<uint32_t>(required + i * sizeof(uint32_t)))
            return Unsupported;
    }
    return Success;
}

Result Create(const Guest& guest, ObjectKind kind)
{
    const auto index = kind == ObjectKind::Engine ? 0 : 1;
    const auto output = guest.args[index];
    if (!output)
        return InvalidParameter;
    guest.Write<uintptr_t>(output, 0);
    auto object = std::make_shared<Object>(kind);
    if (kind == ObjectKind::Engine) {
        const auto count = static_cast<uint32_t>(guest.args[1]);
        const auto options = guest.args[2];
        if (count && !options)
            return InvalidParameter;
        if (count > 32)
            return InvalidParameter;
        for (unsigned i = 0; i < count; ++i) {
            const auto option = guest.Read<std::array<uint32_t, 2>>(options + i * 8);
            if (option[0] != 1 && option[0] != 2)
                return InvalidParameter;
        }
    }
    else if (kind == ObjectKind::Player) {
        struct Pcm {
            uint32_t type, channels, milliHertz, bits, container, mask, endian;
        };

        if (!guest.args[2] || !guest.args[3])
            return InvalidParameter;
        const auto source = guest.Read<std::array<uintptr_t, 2>>(guest.args[2]);
        const auto sink = guest.Read<uintptr_t>(guest.args[3]);
        if (!source[0] || !source[1] || !sink)
            return InvalidParameter;
        const auto locator = guest.Read<std::array<uint32_t, 2>>(source[0]);
        if ((locator[0] != BufferQueue && locator[0] != AndroidSimpleBufferQueue) ||
            guest.Read<uint32_t>(sink) != OutputMix)
            return Unsupported;
        if (!locator[1] || locator[1] > 255)
            return InvalidParameter;
        const auto mix = Find(guest.Read<uintptr_t>(sink + 8), Interface::Object);
        if (!mix || mix->kind != ObjectKind::Mix)
            return InvalidParameter;
        {
            const std::lock_guard lock(mix->mutex);
            if (!mix->realized || mix->closed)
                return Preconditions;
        }
        const auto pcm = guest.Read<Pcm>(source[1]);
        if (pcm.type != 2 && pcm.type != 4)
            return Unsupported;
        if (pcm.channels < 1 || pcm.channels > 2 || pcm.endian != 2 || pcm.bits != pcm.container ||
            pcm.milliHertz < 8000000 || pcm.milliHertz > 384000000 || pcm.milliHertz % 1000)
            return Unsupported;
        const auto representation =
            pcm.type == 4 ? guest.Read<uint32_t>(source[1] + sizeof(pcm)) : (pcm.bits == 8 ? 2u : 1u);
        if (representation == 3
                ? pcm.bits != 32
                : (pcm.bits == 8 ? representation != 2 : (pcm.bits != 16 && pcm.bits != 32) || representation != 1))
            return Unsupported;
        auto& format = object->format;
        format.wFormatTag = representation == 3 ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(pcm.channels);
        format.nSamplesPerSec = pcm.milliHertz / 1000;
        format.wBitsPerSample = static_cast<WORD>(pcm.bits);
        format.nBlockAlign = static_cast<WORD>(pcm.channels * pcm.bits / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        object->maxBuffers = locator[1];
    }
    const auto interfaces = kind == ObjectKind::Engine ? 3 : kind == ObjectKind::Mix ? 2 : 4;
    if (const auto result = RequiredInterfaces(guest, *object, interfaces); result != Success)
        return result;
    guest.Write(output, Publish(std::move(object)));
    return Success;
}

Result GetInterface(const Guest& guest, const Object& object)
{
    if (!guest.args[1] || !guest.args[2])
        return InvalidParameter;
    guest.Write<uintptr_t>(guest.args[2], 0);
    const auto id = guest.Read<InterfaceId>(guest.args[1]);
    Interface type;
    if (id == ids[7])
        type = Interface::Object;
    else if (id == ids[1] && object.kind == ObjectKind::Engine)
        type = Interface::Engine;
    else if (id == ids[2] && object.kind == ObjectKind::Player)
        type = Interface::Play;
    else if ((id == ids[0] || id == ids[5]) && object.kind == ObjectKind::Player)
        type = Interface::Queue;
    else if (id == ids[6] && object.configure)
        type = Interface::Configuration;
    else
        return Unsupported;
    if (!object.realized && type != Interface::Configuration)
        return Preconditions;
    guest.Write(guest.args[2], object.Handle(type));
    return Success;
}

Result SetPlayState(Object& object, uint32_t state)
{
    if (state != Stopped && state != Paused && state != Playing)
        return InvalidParameter;
    if (object.playState == state)
        return Success;
    if (state == Stopped && object.Clear() != MMSYSERR_NOERROR)
        return ResourceError;
    const auto error = state == Playing ? waveOutRestart(object.device) : waveOutPause(object.device);
    if (error != MMSYSERR_NOERROR)
        return ResourceError;
    object.playState = state;
    return Success;
}

Result Enqueue(const Guest& guest, Object& object)
{
    const auto length = static_cast<uint32_t>(guest.args[2]);
    if (!guest.args[1] || !length || length % object.format.nBlockAlign)
        return InvalidParameter;
    if (object.queue.size() >= object.maxBuffers)
        return BufferInsufficient;
    const auto* samples = reinterpret_cast<const char*>(guest.memory(guest.args[1], length, false));
    auto& buffer = object.queue.emplace_back();
    try {
        buffer.samples.assign(samples, samples + length);
    }
    catch (...) {
        object.queue.pop_back();
        throw;
    }
    auto& header = buffer.header;
    header.lpData = buffer.samples.data();
    header.dwBufferLength = length;
    if (waveOutPrepareHeader(object.device, &header, sizeof(header)) == MMSYSERR_NOERROR) {
        if (waveOutWrite(object.device, &header, sizeof(header)) == MMSYSERR_NOERROR)
            return Success;
        waveOutUnprepareHeader(object.device, &header, sizeof(header));
    }
    object.queue.pop_back();
    return ResourceError;
}

Result Dispatch(std::string_view name, const Guest& guest)
{
    if (name == "slCreateEngine")
        return Create(guest, ObjectKind::Engine);
    if (name == "OpenSLES.Unsupported")
        return Unsupported;
    name.remove_prefix(sizeof("OpenSLES.") - 1);
    Interface type = Interface::Object;
    if (name == "CreateOutputMix" || name == "CreateAudioPlayer")
        type = Interface::Engine;
    else if (name == "SetPlayState" || name == "GetPlayState" || name == "GetDuration" || name == "GetPosition")
        type = Interface::Play;
    else if (name == "Enqueue" || name == "Clear" || name == "GetQueueState" || name == "RegisterQueueCallback")
        type = Interface::Queue;
    else if (name == "SetConfiguration" || name == "GetConfiguration")
        type = Interface::Configuration;
    const auto object = Find(guest.args[0], type);
    if (!object)
        return InvalidParameter;
    if (name == "Destroy") {
        Destroy(object);
        return Success;
    }
    const std::lock_guard lock(object->mutex);
    if (object->closed)
        return Preconditions;
    if (type == Interface::Object) {
        if (name == "Realize") {
            if (guest.args[1])
                return Unsupported;
            return object->realized ? Preconditions : Realize(object);
        }
        if (name == "GetObjectState") {
            if (!guest.args[1])
                return InvalidParameter;
            guest.Write<uint32_t>(guest.args[1], object->realized ? 2u : 1u);
            return Success;
        }
        if (name == "GetInterface")
            return GetInterface(guest, *object);
    }
    else if (type == Interface::Engine) {
        if (!object->realized || object->kind != ObjectKind::Engine)
            return Preconditions;
        return Create(guest, name == "CreateOutputMix" ? ObjectKind::Mix : ObjectKind::Player);
    }
    else if (object->kind != ObjectKind::Player) {
        return InvalidParameter;
    }
    else if (type == Interface::Configuration) {
        if (!object->configure)
            return Unsupported;
        if (!guest.IsString(guest.args[1], "androidPlaybackStreamType"))
            return Unsupported;
        if (name == "SetConfiguration") {
            if (object->realized)
                return Preconditions;
            if (!guest.args[2] || guest.args[3] != sizeof(int32_t))
                return InvalidParameter;
            const auto stream = guest.Read<int32_t>(guest.args[2]);
            // The default WinMM output handles media playback.
            if (stream != 3)
                return Unsupported;
        }
        else {
            if (!guest.args[2])
                return InvalidParameter;
            const auto capacity = guest.Read<uint32_t>(guest.args[2]);
            guest.Write<uint32_t>(guest.args[2], sizeof(int32_t));
            if (!guest.args[3])
                return Success;
            if (capacity < sizeof(int32_t))
                return BufferInsufficient;
            guest.Write<int32_t>(guest.args[3], 3);
        }
        return Success;
    }
    else if (!object->realized) {
        return Preconditions;
    }
    else if (type == Interface::Play) {
        if (name == "SetPlayState")
            return SetPlayState(*object, static_cast<uint32_t>(guest.args[1]));
        if (!guest.args[1])
            return InvalidParameter;
        uint32_t value = object->playState;
        if (name == "GetDuration")
            value = UINT32_MAX;
        if (name == "GetPosition") {
            MMTIME position{};
            position.wType = TIME_MS;
            if (waveOutGetPosition(object->device, &position, sizeof(position)) != MMSYSERR_NOERROR)
                return ResourceError;
            if (position.wType == TIME_MS)
                value = position.u.ms;
            else if (position.wType == TIME_SAMPLES)
                value = static_cast<uint32_t>(uint64_t(position.u.sample) * 1000 / object->format.nSamplesPerSec);
            else if (position.wType == TIME_BYTES)
                value = static_cast<uint32_t>(uint64_t(position.u.cb) * 1000 / object->format.nAvgBytesPerSec);
            else
                return Unsupported;
        }
        guest.Write(guest.args[1], value);
        return Success;
    }
    else if (type == Interface::Queue) {
        if (name == "Enqueue")
            return Enqueue(guest, *object);
        if (name == "Clear") {
            if (object->Clear() != MMSYSERR_NOERROR)
                return ResourceError;
            if (object->playState != Playing && waveOutPause(object->device) != MMSYSERR_NOERROR)
                return ResourceError;
        }
        else if (name == "GetQueueState") {
            if (!guest.args[1])
                return InvalidParameter;
            guest.Write(guest.args[1],
                        std::array{static_cast<uint32_t>(object->queue.size()), object->completedBuffers});
        }
        else {
            if (object->playState != Stopped)
                return Preconditions;
            object->callback = guest.args[1];
            object->callbackData = guest.args[2];
        }
        return Success;
    }
    return Unsupported;
}
} // namespace

uintptr_t Libopensles(std::string_view name)
{
    if (name == "slCreateEngine")
        return Import(name, &UnsupportedEngine);
    if (name.starts_with("OpenSLES."))
        return Import(
            name, +[] {
                return Unsupported;
            });
    const auto found = std::ranges::find(iidNames, name);
    if (found == iidNames.end())
        return 0;
    static const IidExports native;
    const auto& exports = IsArm64TranslationEnabled() ? []() -> const IidExports& {
        static const auto* data = [] {
            const auto address = AllocateArm64GuestMemory(sizeof(IidExports), false);
            if (!address)
                throw std::bad_alloc{};
            return new (reinterpret_cast<void*>(address)) IidExports;
        }();
        return *data;
    }()
        : native;
    return reinterpret_cast<uintptr_t>(&exports.pointers[found - iidNames.begin()]);
}

uint64_t CallArm64OpenSl(std::string_view name, std::span<const uint64_t, 8> arguments, const Memory& memory)
{
    try {
        return Dispatch(name, {arguments, memory});
    }
    catch (const std::bad_alloc&) {
        return NoMemory;
    }
    catch (const std::system_error&) {
        return ResourceError;
    }
}
} // namespace stone::windows
