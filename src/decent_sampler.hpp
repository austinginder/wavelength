#pragma once
// DecentSampler instruments. A .dspreset is XML: <DecentSampler> holds <groups> of <group>s of <sample>s (and
// <oscillator>s), an <effects> chain, a <ui> whose knobs, menus and buttons carry <binding>s, <tags>, <midi> and
// <modulators>. A .dsbundle folder holds presets and their samples; a .dslibrary is a zip of a bundle.
// readDecentPreset() turns one preset into SFZ regions for builtin:sampler (as GarageBand's Alchemy patches do) and its
// effects chain into Wavelength effects played after the instrument. What plays:
//   - samples: key and velocity ranges, root, tuning (`tuning`, the `tunning` spelling, groupTuning, globalTuning),
//     volume (dB or linear; <groups>, <group> and <sample> volumes and modVolume multiply), pan (inherited, plus
//     groupPan and globalPan), start and end, loops (the file's own when the preset gives no points, as DecentSampler
//     does) with their crossfade, round robins (seqMode/seqPosition: every sample at the chosen position plays; with no
//     seqMode they all play), release triggers, CC-gated samples at rest, the amplitude envelope on DecentSampler's
//     curves (attack, decay, sustain, release, their curve shapes; 0.5 s release by default), ampVelTrack (gain
//     1 - t + t * velocity / 127), pitchKeyTrack, ampEnvEnabled=false as one-shots, tags with their volume and enabled
//     state, choke groups (tags / silencedByTags), and oscillators as SFZ generators;
//   - every control's bindings at its saved value (a knob's `value`, a menu's selected option, a button's state),
//     through the binding's factor and translation (linear over the control's range, table, fixed_value): instrument,
//     group and tag volumes, tuning, pan, envelopes, velocity tracking, groups and tags switched on and off, effect
//     parameters. Velocity bindings: to the amplitude (a velocity curve) and to a filter's frequency (the filter opens
//     with velocity, per voice);
//   - <effects>: low-, high- and band-pass filters, peak, notch and shelf EQ, gain, reverb, delay, chorus, phaser and
//     convolution, as Wavelength effects. Reverb and delay add their wet signal to the full dry one, as DecentSampler
//     does (measured against DecentSampler 1.11), so each becomes the effect at a matching mix plus a gain;
//   - levels as DecentSampler plays them: 5.3 dB under the samples' own in the centre, panned at constant power
//     (sampler.cpp), and its output stage (`output`).
// Left out, each with a note: MIDI CC and note bindings, modulators (LFOs, envelopes), glide, legato and first-note
// triggers, tag polyphony, buses and auxiliary outputs, and effects without a counterpart.
#include "sfz.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

struct DecentPreset {
    std::string name;            // the preset's file name without ".dspreset"
    SfzFile sfz;                 // its samples as SFZ regions: absolute paths, or entry names inside `zip`
    std::string zip;             // the .dslibrary its samples are read from ("" = files on disk)
    nlohmann::json effects = nlohmann::json::array();   // its effects chain, played after the instrument
    // DecentSampler's own output stage, after the effects whatever the track's "effects" says: it compresses what passes
    // -6 dBFS at 4:1 (2 ms attack, 200 ms release) and stops peaks at 0 dBFS (measured on DecentSampler 1.11: a sine
    // 12 dB over that threshold comes out 9 dB lower, noise peaking there untouched)
    nlohmann::json output = nlohmann::json::array();
    std::vector<std::string> notes;   // what is approximated or left out
    size_t samples = 0, total = 0;    // regions that play, of the <sample> elements
};

// `path`: a .dspreset, a .dsbundle folder or a .dslibrary (zip); "<bundle or library>#<preset name>" picks one of
// several presets inside. Samples missing on disk are left out with a note; a preset none of whose samples play fails.
bool readDecentPreset(const std::string &path, DecentPreset &out, std::string &err);
// The presets inside a .dslibrary: entry names, leaving out macOS's "__MACOSX" and "._" copies
std::vector<std::string> decentLibraryPresets(const std::string &zipPath, std::string &err);
// For listing: how many <sample> elements a preset's text holds, and the folders (relative, '/'-separated) its samples are in
size_t decentSampleCount(const std::string &text, std::vector<std::string> *sampleDirs = nullptr);

} // namespace wl
