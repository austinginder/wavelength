#pragma once
// Integrated loudness per ITU-R BS.1770-4 / EBU R128 (K-weighting, 400 ms blocks with
// 75% overlap, absolute gate at -70 LUFS, relative gate 10 LU below the ungated mean).
#include "wav.hpp"

namespace wl {

// Integrated loudness of frames [from, to) in LUFS; returns -70 or lower for silence.
double integratedLufs(const Audio &a, int sampleRate, size_t from = 0, size_t to = (size_t)-1);

} // namespace wl
