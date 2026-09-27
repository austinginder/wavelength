#pragma once
// Audio files in: WAV, AIFF/AIFC, CAF (linear PCM), FLAC, MP3 and Ogg Vorbis, told apart by their contents
// (not the extension). Anything beyond two channels keeps the first two.
#include "wav.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wl {

struct DecodedAudio {
    double rate = 44100;
    std::vector<float> l, r;     // r empty = mono
    double loopStart = -1, loopEnd = -1;   // the file's own sustain loop (WAV smpl chunk), end exclusive; -1 = none
    size_t frames() const { return l.size(); }
};

bool decodeAudio(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err);
// Read any of the formats above into a stereo buffer (mono duplicated to both channels).
bool readAudio(const std::string &path, Audio &out, int &sampleRate, std::string &err);
// Frames [from, to) of a file: PCM in a CAF or WAV is read straight from that range (a consolidated
// sample holding a whole instrument need not be read), other formats are decoded and cut.
bool readAudioFrames(const std::string &path, double from, double to, DecodedAudio &out, std::string &err);
// ".wav", ".aif", ".aiff", ".aifc", ".caf", ".flac", ".mp3", ".ogg" (case-insensitive)
bool isAudioFileName(const std::string &path);

namespace codecs {   // audio_codecs.cpp: the single-file decoders
bool flac(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err);
bool mp3(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err);
bool vorbis(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err);
} // namespace codecs

} // namespace wl
