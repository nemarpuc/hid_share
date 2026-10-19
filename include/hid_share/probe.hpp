// SPDX-License-Identifier: MIT
#pragma once

namespace hids {

// The presses of one axis of the calibration wizard (docs/MATH.md, "Gain").
// The cursor starts in the top left corner; each press sends some counts
// toward the far edge. The press that finds the edge moves nothing, so it is
// taken out of the count and the travel is exact to one fine step.
class ProbeAxis {
  public:
    // A press of `counts` (at least 1) that was sent.
    void press(int counts);
    void clear() noexcept;

    // The counts of the presses.
    [[nodiscard]] int total() const noexcept;
    // The latest press; 0 when there is none.
    [[nodiscard]] int last() const noexcept;
    // The latest press moved nothing: the travel is the total without it.
    // 0 unless there are at least two presses and the latest is `fine`.
    [[nodiscard]] int finish(int fine) const noexcept;

  private:
    int total_{};
    int last_{};
    int count_{};
};

} // namespace hids
