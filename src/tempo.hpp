#pragma once
// Beats <-> seconds through a tempo map. A point with `ramp` reaches its bpm by a linear
// ramp (in beats) from the previous point (accelerando / ritardando); otherwise tempo steps.
#include <cstddef>
#include <vector>

namespace wl {

struct TempoPoint { double beat, bpm; bool ramp = false; };

class TempoMap {
public:
    explicit TempoMap(std::vector<TempoPoint> points = {{0, 120}});
    double beatToSec(double beat) const;
    double secToBeat(double sec) const;
    double bpmAtBeat(double beat) const;
private:
    std::vector<TempoPoint> pts_;
    std::vector<double> secAt_;   // seconds at each point
    double segSec(size_t i, double beats) const;
};

} // namespace wl
