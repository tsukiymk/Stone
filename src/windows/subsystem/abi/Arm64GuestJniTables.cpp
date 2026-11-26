#include "abi/Arm64GuestJniTables.h"

namespace stone::windows::arm64_abi {
const std::array<uint8_t, static_cast<size_t>(JniEntry::VmBase) + sizeof(JNIInvokeInterface_) / sizeof(void*)>
    jniTokens{};
const GuestJniTables guestJni;
// Each translated thread can replace its guest JNI table independently of HotSpot.
thread_local uintptr_t guestJniEnv = reinterpret_cast<uintptr_t>(guestJni.envTable.data());

}
