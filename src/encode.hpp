#pragma once
// Delivery files next to mix.wav: MP3 (LAME, loaded at run time, or ffmpeg), FLAC (built in) and
// 16/24-bit WAV. Each written file is decoded again and measured, so the report gives the true peak
// and loudness a listener gets (lossy encoding raises true peak).
#include "wav.hpp"

#include <string>
#include <vector>

namespace wl {

struct DeliverySpec {
    std::string format;     // "mp3", "flac" or "wav"
    int bitrate = 320;      // MP3 kbps (CBR)
    int bits = 24;          // FLAC / WAV sample size (16 is dithered)
    std::string file;       // output path: relative to the render folder or absolute ("" = mix.<ext>)
};

// "mp3", "mp3:256", "flac", "flac:16", "wav:16", or {"format", "bitrate", "bits", "file"} as JSON text
bool parseDeliverySpec(const std::string &text, DeliverySpec &out, std::string &err);

struct Delivery {
    DeliverySpec spec;
    std::string file, encoder;
    double truePeakDb = -120, lufs = -120, peakDb = -120;   // measured from the decoded file
    double overshootDb = 0;                                // decoded true peak - mix.wav true peak
};

// Where a delivery goes: its `file` (relative to outDir, or absolute), else mix.mp3, mix.flac (24-bit),
// mix-16bit.flac, mix-16bit.wav / mix-24bit.wav
std::string deliveryPath(const DeliverySpec &spec, const std::string &outDir);

// Writes every delivery of `mix` (with `leadFrames` of silence first and `skipFrames` left out,
// like mix.wav), decodes each and measures it. `wavPath` is the written mix.wav (the ffmpeg fallback
// encodes from it). Adds a warning when an MP3's true peak goes above -1 dBTP.
bool writeDeliveries(const std::vector<DeliverySpec> &specs, const Audio &mix, int sampleRate, size_t leadFrames, size_t skipFrames,
                     const std::string &outDir, const std::string &wavPath, double mixTruePeakDb, std::vector<Delivery> &out,
                     std::vector<std::string> &warnings, std::string &err);

// A FLAC file (16 or 24 bit, fixed predictors, stereo decorrelation, partitioned Rice residuals).
bool writeFlac(const std::string &path, const Audio &a, int sampleRate, int bits, size_t leadFrames, size_t skipFrames, std::string &err);

} // namespace wl
