#pragma once
// Measurements an agent needs in place of listening: pitch, brightness, spectral balance,
// stereo image, onsets and the amplitude envelope of a stem, a mix or a window of either.
#include "wav.hpp"

#include <nlohmann/json.hpp>

namespace wl {

struct Analysis {
    double seconds = 0, peakDb = -120, truePeakDb = -120, lufs = -120, lra = 0, rmsDb = -120;
    bool silent = true;
    // pitch: the median fundamental of voiced frames (YIN); confidence = voiced share of active frames
    double pitchHz = 0, pitchConfidence = 0;
    int pitchKey = -1;            // nearest MIDI key
    double pitchCents = 0;        // deviation from that key
    // spectrum (power-averaged over active frames)
    double centroidHz = 0, rolloffHz = 0;   // rolloff: 85% of the energy lies below
    double bandsDb[6] = {-120, -120, -120, -120, -120, -120};   // sub <60, bass 60-250, low-mid 250-2k, high-mid 2k-6k, presence 6k-12k, air >12k (share of total energy)
    // stereo: side/mid RMS ratio (0 = mono) and left/right correlation
    double width = 0, correlation = 1;
    // envelope of the whole file / window
    double attackMs = 0;          // 10% to 90% of the peak level
    double decayMs = 0;           // peak until 20 dB below it (0 if it never falls that far)
    double sustainDb = -120;      // median level of the active part, relative to the peak
    double activeSeconds = 0;     // time above -60 dBFS
    double firstSoundSeconds = -1, lastSoundSeconds = -1;
    std::vector<double> onsets;   // seconds
};

// Analyze [start, end) seconds of `a` (end <= 0: to the end).
Analysis analyzeAudio(const Audio &a, int sampleRate, double start = 0, double end = 0);
nlohmann::json analysisToJson(const Analysis &x, bool withOnsets = true);
std::string keyName(int key);

} // namespace wl
