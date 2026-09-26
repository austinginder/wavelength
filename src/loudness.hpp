#pragma once
// Integrated loudness per ITU-R BS.1770-4 / EBU R128 (K-weighting, 400 ms blocks with
// 75% overlap, absolute gate at -70 LUFS, relative gate 10 LU below the ungated mean).
#include "wav.hpp"

#include <vector>

namespace wl {

// Integrated loudness of frames [from, to) in LUFS; returns -70 or lower for silence.
double integratedLufs(const Audio &a, int sampleRate, size_t from = 0, size_t to = (size_t)-1);
// Loudness range in LU per EBU Tech 3342: 3 s short-term windows every 100 ms, gated at -70 LUFS
// and 20 LU below their mean; the spread between the 10th and 95th percentile. 0 for silence.
double loudnessRange(const Audio &a, int sampleRate, size_t from = 0, size_t to = (size_t)-1);
// Ungated K-weighted loudness of consecutive windows of `windowSec`, one every `hopSec` (LUFS; -120
// for silence): a loudness-over-time curve for spotting dropouts and contours.
std::vector<double> loudnessTimeline(const Audio &a, int sampleRate, double windowSec, double hopSec, size_t from = 0, size_t to = (size_t)-1);

// K-weighted power per frame, summed over channels (what loudness integrates), for level matching.
std::vector<float> kWeightedPower(const Audio &a, int sampleRate);

} // namespace wl
