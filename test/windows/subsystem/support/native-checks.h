#pragma once

#include "test-suite.h"
#include "elf-fixtures.h"

namespace stone::windows::test::checks {
inline constexpr std::string_view ubsanSoname = "libclang_rt.ubsan_standalone-x86_64-android.so";
void CheckElfImports();
void CheckArm64TlsDescriptor();
void CheckArm64ExternalTls();
void CheckArm64InvalidTls();
void CheckArm64InspectionRestoresBackend();
void CheckArm64StaticTls();
void CheckArm64StaticTlsRejectedWithoutAllocation();
void CheckArm64IndirectRelocations();
void CheckArm64FunctionBridge();
void CheckArm64Properties();
void CheckArm64LogCallbacks();
void CheckArm64SignalSets();
void CheckArm64ThreadStackAttributes();
void CheckArm64StackProtection();
void CheckArm64SharedCommittedStack();
void CheckArm64PairwiseAccumulate();
void CheckArm64NativeLoader();
void CheckArm64StackProbesBody();
void CheckArm64StackProbes();
void CheckArm64UserDatabase();
void CheckArm64LibrarySearchPath();
void CheckArm64MappedCode();
void CheckArm64CodePublication();
void CheckArm64FutexRequeue();
void CheckArm64MemoryFaultRecovery();
void CheckArm64SignalWaitInfo();
void CheckArm64VectorIo();
void CheckArm64AllocationStatistics();
void CheckArm64FloatingStrings();
void CheckArm64NetworkResolver();
void CheckArm64ThreadPriorities();
void CheckArm64RandomBytes();
void CheckArm64StringStdioResolver();
void CheckArm64RoundNearest(bool away);
void CheckArm64NearestIntegerConversions();
void CheckArm64SaturatingNarrow();
void CheckArm64ScalarOrderedComparisons();
void CheckArm64HighNarrowArithmetic();
void CheckArm64VectorRoundDown();
void CheckArm64PolynomialMultiply();
void CheckArm64ElfProtection();
void CheckArm64CodeReadProtection();
void CheckArm64Syslog();
void CheckArm64VolumeStatistics();
void CheckArm64SetExtendedAttributes();
void CheckArm64AdvisoryMappingHint();
void CheckArm64DataMappingLookup();
void CheckArm64Fcntl();
void CheckArm64LogAssert();
void CheckArm64JniBuffers();
void CheckArm64CfiAndPerror();
void CheckArm64HostJni();
void CheckTypedImportPrototypes();
void CheckTypedImportIdentityAndLifetime();
void CheckElfCodeBoundaries();
void CheckNativeSignalRecovery();
void CheckElfInspection();
void CheckArtInspectionBindings();
void CheckArtGsRewrite(bool symbols = false, bool narrow = false);
void CheckArtTlsRecovery(bool narrow);
void CheckDynamicArtGs();
void CheckDynamicArtSwitch();
void CheckArtGsLoadRegisters(unsigned kind);
void CheckFsTlsRecovery(uint8_t offset, bool high);
void CheckArtGsControl(bool tail, bool narrow);
void CheckChromiumJni();
void CheckDrawTable();
void CheckUnknownUbsan();
}
