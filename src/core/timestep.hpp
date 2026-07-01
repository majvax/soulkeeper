#pragma once
#include <algorithm>

namespace core {

// Fixed-timestep accumulator, shared by the client engine loop and the server
// loop. It is timing-source agnostic: you feed it the real elapsed seconds each
// frame (from SDL on the client, std::chrono on the server) and it tells you how
// many fixed steps to run plus the leftover fraction for render interpolation.
//
//   ts.add_time(frame_seconds);
//   while (ts.consume()) update(ts.dt());
//   render(ts.alpha());            // client only
//   sleep_for(ts.time_until_next); // server only, to stay low-CPU
class FixedTimestep
{
public:
    explicit FixedTimestep(double hz, double max_frame = 0.25) : fixed_dt_{ 1.0 / hz }, max_frame_{ max_frame } {}

    [[nodiscard]] float dt() const noexcept { return static_cast<float>(fixed_dt_); }

    // Add elapsed real time. Clamped to avoid the "spiral of death" after a stall.
    void add_time(double frame_seconds) noexcept { accumulator_ += std::min(frame_seconds, max_frame_); }

    // Returns true and removes one fixed step while enough time is banked.
    [[nodiscard]] bool consume() noexcept
    {
        if (accumulator_ < fixed_dt_) { return false; }
        accumulator_ -= fixed_dt_;
        return true;
    }

    // Fraction into the next pending step, in [0, 1) — for render interpolation.
    [[nodiscard]] float alpha() const noexcept { return static_cast<float>(accumulator_ / fixed_dt_); }

    // Seconds until the next step is due — for a server to sleep instead of spin.
    [[nodiscard]] double time_until_next() const noexcept { return std::max(0.0, fixed_dt_ - accumulator_); }

private:
    double fixed_dt_;
    double max_frame_;
    double accumulator_ = 0.0;
};

} // namespace core
