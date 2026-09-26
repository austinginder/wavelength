// dr_flac, dr_mp3 and stb_vorbis compiled into Wavelength (their warnings are silenced for this file).
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include <dr_flac.h>
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include <dr_mp3.h>
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>
#undef L
#undef C
#undef R

#include "audio_file.hpp"

namespace wl::codecs {

namespace {
void split(const float *x, size_t frames, unsigned channels, DecodedAudio &out) {
    out.l.resize(frames);
    if (channels > 1) out.r.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        out.l[i] = x[i * channels];
        if (channels > 1) out.r[i] = x[i * channels + 1];
    }
}
} // namespace

bool flac(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err) {
    unsigned channels = 0, rate = 0;
    drflac_uint64 frames = 0;
    float *x = drflac_open_memory_and_read_pcm_frames_f32(data, size, &channels, &rate, &frames, nullptr);
    if (!x) { err = "cannot decode this FLAC file"; return false; }
    out.rate = rate;
    split(x, (size_t)frames, channels, out);
    drflac_free(x, nullptr);
    return true;
}

bool mp3(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err) {
    drmp3_config cfg{};
    drmp3_uint64 frames = 0;
    float *x = drmp3_open_memory_and_read_pcm_frames_f32(data, size, &cfg, &frames, nullptr);   // skips the encoder delay (LAME tag)
    if (!x) { err = "cannot decode this MP3 file"; return false; }
    out.rate = cfg.sampleRate;
    split(x, (size_t)frames, cfg.channels, out);
    drmp3_free(x, nullptr);
    return true;
}

bool vorbis(const uint8_t *data, size_t size, DecodedAudio &out, std::string &err) {
    int e = 0;
    stb_vorbis *v = stb_vorbis_open_memory(data, (int)size, &e, nullptr);
    if (!v) { err = "cannot decode this Ogg Vorbis file (error " + std::to_string(e) + ")"; return false; }
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    out.rate = info.sample_rate;
    const int ch = std::min(info.channels, 2);
    std::vector<float> a(4096), b(4096);
    float *bufs[2] = {a.data(), b.data()};
    for (;;) {
        const int n = stb_vorbis_get_samples_float(v, ch, bufs, 4096);
        if (n <= 0) break;
        out.l.insert(out.l.end(), a.begin(), a.begin() + n);
        if (ch > 1) out.r.insert(out.r.end(), b.begin(), b.begin() + n);
    }
    stb_vorbis_close(v);
    if (out.l.empty()) { err = "Ogg Vorbis file has no audio"; return false; }
    return true;
}

} // namespace wl::codecs
