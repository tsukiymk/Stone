#pragma once

namespace stone::windows {
// Save dirty persistent products before reading final opt-in statistics.
// Dirty flags make repeated completion without new products a no-op for writes.
void FinishArm64ExecutionSession() noexcept;
}
