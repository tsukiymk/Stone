#include "loader/AndroidLibraryState.h"

namespace stone::windows::android_libraries {
LONG CALLBACK HandleAndroidException(EXCEPTION_POINTERS*);

bool InstallAndroidExceptionHandler()
{
    static const auto handler = AddVectoredExceptionHandler(1, HandleAndroidException);
    return handler != nullptr;
}

LONG ReportIsolatedGuestFault(const EXCEPTION_RECORD& fault, const CONTEXT& context)
{
    if (Process().runtime.read) {
        std::fprintf(stderr, "[art] unhandled exception 0x%08lx at 0x%llx, address 0x%llx\n", fault.ExceptionCode,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fault.ExceptionAddress)),
                     static_cast<unsigned long long>(fault.NumberParameters >= 2 ? fault.ExceptionInformation[1] : 0));
        const auto describe = [](uintptr_t address) {
            for (const auto& slot : Process().executableImages) {
                const auto* library = slot.load(std::memory_order_acquire);
                if (!library)
                    break;
                if (library->image.ContainsCode(address, 1)) {
                    std::fprintf(stderr, "[art] ELF %s+0x%llx\n", library->name.c_str(),
                                 static_cast<unsigned long long>(address - library->image.LoadBias()));
                    return;
                }
            }
            for (auto* image = Process().nativeSystemImages.load(std::memory_order_acquire); image; image = image->next)
                if (image->ready.load(std::memory_order_acquire) && image->image->ContainsCode(address, 1)) {
                    std::fprintf(stderr, "[art] system ELF %s+0x%llx\n", image->name.c_str(),
                                 static_cast<unsigned long long>(address - image->image->LoadBias()));
                    return;
                }
        };
        describe(reinterpret_cast<uintptr_t>(fault.ExceptionAddress));
        if (GetEnvironmentVariableW(L"STONE_ART_DIAGNOSTICS", nullptr, 0)) {
            std::fprintf(
                stderr,
                "[art] registers rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx rsp=%llx r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
                context.Rax, context.Rbx, context.Rcx, context.Rdx, context.Rsi, context.Rdi, context.Rbp, context.Rsp,
                context.R8, context.R9, context.R10, context.R11, context.R12, context.R13, context.R14, context.R15);
            uintptr_t stack[32]{};
            SIZE_T read = 0;
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp), stack, sizeof(stack),
                                  &read))
                for (size_t i = 0; i < read / sizeof(uintptr_t); ++i)
                    describe(stack[i]);
        }
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(fault.ExceptionAddress, &memory, sizeof(memory)) && memory.Type == MEM_IMAGE) {
            std::fprintf(stderr, "[art] Windows module offset 0x%llx\n",
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fault.ExceptionAddress) -
                                                         reinterpret_cast<uintptr_t>(memory.AllocationBase)));
        }
    }
    if (AndroidApplicationUid() < 90000)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto address = reinterpret_cast<uintptr_t>(fault.ExceptionAddress);
    for (const auto& slot : Process().executableImages) {
        const auto* library = slot.load(std::memory_order_acquire);
        if (!library)
            break;
        if (!library->image.ContainsCode(address, 1))
            continue;
        static std::atomic<unsigned> reported = 0;
        if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
            char message[192];
            const auto length = _snprintf_s(
                message, _TRUNCATE, "[android-native] isolated ELF fault 0x%08lx at offset 0x%llx, address 0x%llx\r\n",
                fault.ExceptionCode, static_cast<unsigned long long>(address - library->image.LoadBias()),
                static_cast<unsigned long long>(fault.NumberParameters >= 2 ? fault.ExceptionInformation[1] : 0));
            DWORD written;
            if (length > 0)
                WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, static_cast<DWORD>(length), &written, nullptr);
        }
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK HandleAndroidException(EXCEPTION_POINTERS* exception)
{
    const auto& fault = *exception->ExceptionRecord;
    auto& context = *exception->ContextRecord;
    const auto unhandled = [&]() {
        switch (fault.ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_FLT_INEXACT_RESULT:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_BREAKPOINT:
            break;
        default:
            return static_cast<LONG>(EXCEPTION_CONTINUE_SEARCH);
        }
        if (!Process().translate) {
            if (AndroidExecutableMapping(context.Rip, 1) && DeliverAndroidHardwareSignal(fault, context))
                return static_cast<LONG>(EXCEPTION_CONTINUE_EXECUTION);
            if (NativeSystemImageForCode(context.Rip, 1) && DeliverAndroidHardwareSignal(fault, context))
                return static_cast<LONG>(EXCEPTION_CONTINUE_EXECUTION);
            for (const auto& slot : Process().executableImages) {
                const auto* library = slot.load(std::memory_order_acquire);
                if (!library)
                    break;
                if (library->image.ContainsCode(context.Rip, 1) && DeliverAndroidHardwareSignal(fault, context))
                    return static_cast<LONG>(EXCEPTION_CONTINUE_EXECUTION);
            }
        }
        return ReportIsolatedGuestFault(fault, context);
    };
    if (fault.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && fault.NumberParameters >= 2 &&
        fault.ExceptionInformation[0] == 0 &&
        fault.ExceptionInformation[1] == AndroidArtThreadSlot * sizeof(uintptr_t)) {
        for (const auto& slot : Process().executableImages) {
            const auto* library = slot.load(std::memory_order_acquire);
            if (!library)
                break;
            if (library->artGs && library->artGs->RecoverTlsLoad(context))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (RecoverArtJitTlsLoad(context))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (fault.ExceptionCode == EXCEPTION_BREAKPOINT) {
        const auto address = reinterpret_cast<uintptr_t>(fault.ExceptionAddress);
        for (const auto& slot : Process().executableImages) {
            const auto* library = slot.load(std::memory_order_acquire);
            if (!library)
                break;
            if (library->artGs && library->artGs->Dispatch(address, context))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (DispatchArtJitBreakpoint(address, context))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (fault.ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        for (const auto& slot : Process().executableImages) {
            const auto* library = slot.load(std::memory_order_acquire);
            if (!library)
                break;
            if (library->ubsanSyscalls && DispatchUbsanSyscall(library->image, context))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
        return unhandled();
    }
    // Some Windows transitions clear the legacy FS base. Recover absolute
    // Bionic thread-pointer and stack-guard loads only in mapped Android code.
    if (fault.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || fault.NumberParameters < 2 ||
        fault.ExceptionInformation[0] != 0 ||
        (fault.ExceptionInformation[1] != 0 &&
         fault.ExceptionInformation[1] != AndroidStackGuardSlot * sizeof(uintptr_t) &&
         fault.ExceptionInformation[1] != AndroidArtThreadSlot * sizeof(uintptr_t))) {
        return unhandled();
    }
    const auto contains = [&](const auto& slot) {
        const auto* image = slot.load(std::memory_order_acquire);
        return image && image->ContainsCode(context.Rip, 9);
    };
    const auto containsLibrary = [&](const auto& slot) {
        const auto* library = slot.load(std::memory_order_acquire);
        return library && library->image.ContainsCode(context.Rip, 9);
    };
    if (!NativeSystemImageForCode(context.Rip, 9) && !contains(Process().executableWebViewUnwinder) &&
        !AndroidExecutableMapping(context.Rip, 9) && !std::ranges::any_of(Process().executableImages, containsLibrary))
        return unhandled();
    const auto* instruction = reinterpret_cast<const uint8_t*>(context.Rip);
    uint32_t offset;
    std::memcpy(&offset, instruction + 5, sizeof(offset));
    if (instruction[0] != 0x64 || (instruction[1] & ~4) != 0x48 || instruction[2] != 0x8b ||
        (instruction[3] & 0xc7) != 4 || instruction[4] != 0x25 || offset != fault.ExceptionInformation[1]) {
        return unhandled();
    }
    DWORD64* registers[]{&context.Rax, &context.Rcx, &context.Rdx, &context.Rbx, nullptr,      &context.Rbp,
                         &context.Rsi, &context.Rdi, &context.R8,  &context.R9,  &context.R10, &context.R11,
                         &context.R12, &context.R13, &context.R14, &context.R15};
    auto* destination = registers[((instruction[3] >> 3) & 7) + ((instruction[1] & 4) ? 8 : 0)];
    if (!destination)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto* tls = ElfImage::CurrentGuestThreadPointer();
    if (!tls && offset != AndroidStackGuardSlot * sizeof(uintptr_t))
        return EXCEPTION_CONTINUE_SEARCH;
    if (tls && RecoverAndroidFsLoad(context, offset))
        return EXCEPTION_CONTINUE_EXECUTION;
    *destination =
        offset == AndroidStackGuardSlot * sizeof(uintptr_t) ? AndroidStackGuard() : tls[offset / sizeof(uintptr_t)];
    context.Rip += 9;
    return EXCEPTION_CONTINUE_EXECUTION;
}
}
