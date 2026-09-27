#pragma once
// A picture of a song, for agents that can see images but can't hear: the sections and bars, the
// mix's loudness over time with each section's level, its spectrum, and a lane per track with its
// notes over its post-fader level. `render --png` writes song.png next to mix.wav; `wavelength
// picture job.json` draws the arrangement alone before anything is rendered.
#include "job.hpp"
#include "wav.hpp"

#include <string>
#include <utility>
#include <vector>

namespace wl {

struct PictureTrack {
    std::string name;
    bool muted = false, failed = false;
    double lufs = -120;          // the stem's loudness (before the fader); -120 = not rendered
    double postLufs = -120;      // its loudness after its fader, rides and pan (before any bus), shown on the lane
    std::vector<float> level;    // post-fader mean square per `levelHop` seconds; empty = not rendered
};

struct Picture {
    std::string title;
    int width = 1400;
    double from = 0, seconds = 0;   // the stretch of the render shown (render seconds; a window's pre-roll is left out)
    double levelHop = 0.05;
    std::vector<PictureTrack> tracks;   // in job order
    const Audio *mix = nullptr;         // the final mix (null: the arrangement alone)
    double mixLufs = -120, lra = 0, truePeak = -120;
    std::vector<double> sectionLufs;    // per marker, measured in the render (empty: not rendered)
    std::vector<std::pair<double, double>> sectionChecks;   // per marker: the step the arrangement check needs there (0 = none) and the measured one
    std::vector<std::pair<double, double>> dropouts;   // render seconds
};

// Draws `pic` for `job` and writes it as a PNG; `height` gets the image height.
bool writePicture(const std::string &path, const Job &job, const Picture &pic, int &height, std::string &err);

} // namespace wl
