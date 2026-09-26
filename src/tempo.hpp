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
    // A render window (render --from): seconds count from `beat`, so the window starts at 0 s while
    // beats stay song beats (transport, bars and tempo-synced plugins keep the song's position).
    void setOrigin(double beat);
    double originSec() const { return originSec_; }
private:
    std::vector<TempoPoint> pts_;
    double originSec_ = 0;
    double absBeatToSec(double beat) const;
    double absSecToBeat(double sec) const;
    std::vector<double> secAt_;   // seconds at each point
    double segSec(size_t i, double beats) const;
};

} // namespace wl
