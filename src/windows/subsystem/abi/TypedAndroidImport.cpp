#include "TypedAndroidImport.h"
#include "AndroidLibraryImports.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace stone::windows {
namespace {
struct ScalarImport {
    uintptr_t address;
    AndroidScalarInvoker invoke;
    std::vector<char> parameters;
    char result;
    bool jniEnvironment;
    uintptr_t thunk = 0;

    ScalarImport(uintptr_t address, AndroidScalarInvoker invoke, const AndroidScalarPrototype& prototype)
        : address(address), invoke(invoke), parameters(prototype.parameters.begin(), prototype.parameters.end()),
          result(prototype.result), jniEnvironment(prototype.jniEnvironment)
    {
    }
};

auto& ScalarImports()
{
    // Imported code and its descriptors remain valid through native TLS cleanup.
    static auto* imports = new std::map<std::string, ScalarImport, std::less<>>;
    return *imports;
}

static_assert(offsetof(AndroidScalarArguments, floats) == 48 && offsetof(AndroidScalarArguments, stack) == 112);
extern "C" void AndroidToWindowsScalars();
}

uintptr_t ImportAndroidScalars(std::string_view name, uintptr_t address, AndroidScalarInvoker invoke,
                               const AndroidScalarPrototype& prototype)
{
    if (!address || !invoke)
        throw std::invalid_argument("missing typed Android import target");
    if (std::string_view("VBSIJFDL").find(prototype.result) == std::string_view::npos ||
        std::ranges::any_of(prototype.parameters,
                            [](char type) {
                                return std::string_view("BSIJFDL").find(type) == std::string_view::npos;
                            }) ||
        (prototype.jniEnvironment && (prototype.parameters.empty() || prototype.parameters.front() != 'L')))
        throw std::invalid_argument("invalid typed Android import prototype");
    const std::lock_guard lock(AndroidLoaderMutex());
    const auto [entry, inserted] = ScalarImports().try_emplace(std::string(name), address, invoke, prototype);
    auto& record = entry->second;
    if (!inserted && (record.address != address || record.invoke != invoke || record.result != prototype.result ||
                      record.jniEnvironment != prototype.jniEnvironment ||
                      !std::ranges::equal(record.parameters, prototype.parameters)))
        throw std::invalid_argument("conflicting typed Android import: " + std::string(name));
    if (!record.thunk) {
        const auto descriptor = reinterpret_cast<uintptr_t>(&record);
        const auto thunk = Import(name, descriptor, AndroidToWindowsScalars);
        if (!AndroidImportMatches(thunk, descriptor, AndroidToWindowsScalars))
            throw std::invalid_argument("conflicting typed Android import bridge: " + std::string(name));
        record.thunk = thunk;
    }
    return record.thunk;
}

std::optional<AndroidScalarImportInfo> FindAndroidScalarImport(uintptr_t thunk)
{
    if (!thunk)
        return {};
    const std::lock_guard lock(AndroidLoaderMutex());
    for (const auto& [name, entry] : ScalarImports())
        if (entry.thunk == thunk)
            return AndroidScalarImportInfo{
                entry.address, entry.invoke, {entry.parameters, entry.result, entry.jniEnvironment}};
    return {};
}

extern "C" uint64_t StoneInvokeAndroidScalars(const ScalarImport* target, const AndroidScalarArguments* source)
{
    return target->invoke(*source, target->address);
}
}
