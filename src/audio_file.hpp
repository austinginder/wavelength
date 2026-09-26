#pragma once
// Audio files in: WAV, AIFF/AIFC, FLAC, MP3 and Ogg Vorbis, told apart by their contents
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
    size_t frames() const { return l.size(); }
};

bool decodeAudio(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err);
// Read any of the formats above into a stereo buffer (mono duplicated to both channels).
bool readAudio(const std::string &path, Audio &out, int &sampleRate, std::string &err);
// ".wav", ".aif", ".aiff", ".aifc", ".flac", ".mp3", ".ogg" (case-insensitive)
bool isAudioFileName(const std::string &path);

namespace codecs {   // audio_codecs.cpp: the single-file decoders
bool flac(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err);
bool mp3(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err);
bool vorbis(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err);
} // namespace codecs

} // namespace wl
