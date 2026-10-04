#pragma once
// GarageBand's Alchemy patches. A channel strip's Alchemy record keeps its settings as preset text: the plug-in data
// starts at payload +140 (u32 313 and a short header; in some patches a ~38 KB binary block follows), and the text runs
// from "<alchemypreset>" in CRLF lines up to the first byte that isn't text: "<section>" headers and "Key = value"
// lines. A value line "Key = v smooth n" is followed by n modulation slots (Type, Id, ModMap and Depth lines), each
// Depth itself a value line that may carry slots of its own; "Key = cols rows-1" followed by rows of numbers is a
// matrix (modulation maps, MSEG points, slice markers).
//
// Every parameter is read at rest: Perform knobs and XY pads at their saved positions (through a modulation map when
// a slot names one), wheels, aftertouch and bend at 0; envelopes, LFOs, velocity and key follow stay as the moving
// modulations. Values are 0..1 and a modulation adds (2 * depth - 1) x its source. What plays:
//   - virtual-analog and noise sources: a builtin:synth patch (oscillators per source weighted by the morph / xfade
//     pad, unison, hard sync, the filter that covers most of the level with its envelope, key and velocity follow,
//     the amplitude AHDSR, a decaying pitch envelope, LFOs to pitch, cutoff, amp, pulse width and pan, mono, glide,
//     level) and Alchemy's own effects racks as Wavelength effects;
//   - additive sources (drawn partials or installed .aaz analysis data): their partials at rest on builtin:synth, as the
//     waves they amount to or an additive oscillator;
//   - installed samples (Alchemy's sampler element, and granular sources whose grains stand still): SFZ regions for
//     builtin:sampler, its VA layers as generators;
//   - spectral sources, moving grains, undecoded additive effect units or .aaz forms, additive sources layered with
//     samples, and content that isn't installed, are refused with the reason.
// Its arpeggiator becomes a track "arp" (rate, order, octaves, note length). Scales that await reference renders
// (guesses): cutoff 1.0 = 20 kHz over 128/12 octaves, AHDSR times 20 s x v^4, LFO rates 220 Hz x v^6, synced rates
// 2^(10.5 (0.5 - v)) beats, glide 1 s, unison 1-16 voices and its detune, sync 48 semitones, the filter type order and
// the effects' scales. Sources: GarageBand's 505 Alchemy channel strips (statistics over their values), Camel Audio's
// Alchemy manual and Apple's Logic Pro Instruments guide.
#include "retro_synth.hpp"
#include "sfz.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

struct AlchemyPatch {
    enum Kind { Refused, Synth, Sampler } kind = Refused;
    std::string why;             // Refused: the reason ("its sounds come from Alchemy's spectral synthesis, ...")
    std::string what;            // its class in words: "virtual analog", "samples", "additive", "granular", ...
    GarageBandSynth synth;       // Synth: the builtin:synth patch; both: fx (Alchemy's own effects) and notes
    SfzFile sfz;                 // Sampler: the instrument as SFZ regions (installed samples, VA layers as generators)
    nlohmann::json sampler = nlohmann::json::object();   // Sampler: the sampler settings around them ("mono", "glide")
    double gain = 0;             // Sampler: dB its master volume gives
    size_t samples = 0, total = 0;   // Sampler: its sample zones that are installed, of all
    nlohmann::json arp;          // any kind: its arpeggiator as a track "arp" (null = off)
    std::vector<std::string> arpNotes;   // what the "arp" leaves out
};

// What an Alchemy channel plays here, from its preset text (PatchChannel::alchemy); `name` names the patch in notes.
AlchemyPatch alchemyPatch(const std::string &text, const std::string &name);
// A quick test before decoding: false when no source switches its sampler / granular element on ("SGrOn = 1"), so the
// patch can't play samples (408 of GarageBand's 505)
bool alchemyMayPlaySamples(const std::string &text);

} // namespace wl
