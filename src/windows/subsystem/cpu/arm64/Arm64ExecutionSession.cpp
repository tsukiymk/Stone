#include "cpu/arm64/Arm64ExecutionSession.h"
#include "cpu/arm64/cache/Arm64BlockCache.h"
#include "cpu/arm64/Arm64ExecutionProfile.h"

namespace stone::windows {
void FinishArm64ExecutionSession() noexcept
{
    FlushArm64BlockDiskCache();
    DumpArm64ExecutionProfile();
}
}
