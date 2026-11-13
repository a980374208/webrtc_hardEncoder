#pragma once
#include <pthread.h>
namespace cohavora_vaapi {
struct RetirementState {
  pthread_t owner = pthread_self();
  bool failed = false;
  bool OnOwner() const { return pthread_equal(owner, pthread_self()); }
};
inline thread_local RetirementState* active_retirement = nullptr;
class RetirementScope {
 public:
  explicit RetirementScope(RetirementState& state) : previous_(active_retirement) {
    active_retirement = &state;
  }
  ~RetirementScope() { active_retirement = previous_; }
 private:
  RetirementState* previous_;
};
// All native teardown and FD closure use this guard. Once any operation fails,
// subsequent dependent destruction is skipped and reports failure.
template<class Call>
int GuardedNative(Call call) {
  if (!active_retirement || !active_retirement->OnOwner() || active_retirement->failed)
    return -1;
  const int status = call();
  if (status != 0) active_retirement->failed = true;
  return status;
}
inline bool RetainHostStorage() {
  // Missing owner scope is also conservative; no asynchronous FFmpeg frees are
  // authorized by this runtime's strictly serialized, single-thread contract.
  return !active_retirement || !active_retirement->OnOwner() ||
      active_retirement->failed;
}
}  // namespace cohavora_vaapi
