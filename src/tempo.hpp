#pragma once
// Beats <-> seconds through a stepped tempo map.
#include <vector>

namespace wl {

struct TempoPoint { double beat, bpm; };

class TempoMap {
public:
    explicit TempoMap(std::vector<TempoPoint> points = {{0, 120}});
    double beatToSec(double beat) const;
    double secToBeat(double sec) const;
    double bpmAtBeat(double beat) const;
private:
    std::vector<TempoPoint> pts_;
    std::vector<double> secAt_;   // seconds at each point
};

} // namespace wl
