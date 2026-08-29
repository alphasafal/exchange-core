#pragma once

#include <chrono>

#include "xc/core/types.hpp"

namespace xc {

/// Source of time for the engine.
///
/// The engine never calls a clock function directly. Every timestamp it stamps
/// comes through this interface, because replay has to drive time from the
/// journal rather than from the wall: a replayed run that read the real clock
/// would produce different timestamps from the original and could not be
/// compared against it.
///
/// Timestamps are for reporting and for measurement. They deliberately play no
/// part in ordering -- priority is defined by the engine's sequence number,
/// which cannot tie and cannot go backwards.
class Clock {
  public:
    virtual ~Clock() = default;
    virtual Nanos now() const = 0;
};

/// Monotonic wall time. The default in production.
///
/// Uses steady_clock rather than system_clock: the system clock can step
/// backwards when NTP corrects it, which would make an engine's own timestamps
/// non-monotonic and any latency measured across such a step meaningless.
class SteadyClock final : public Clock {
  public:
    Nanos now() const override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};

/// A clock the caller advances by hand. Used by replay, where time comes from
/// the journal, and by tests, where a reproducible timestamp is worth more than
/// a real one.
class ManualClock final : public Clock {
  public:
    explicit ManualClock(Nanos start = 0) : now_(start) {}

    Nanos now() const override { return now_; }
    void set(Nanos value) { now_ = value; }
    void advance(Nanos delta) { now_ += delta; }

  private:
    Nanos now_ = 0;
};

}  // namespace xc
