#pragma once
// GarageBand's and Logic's synth patches, played on builtin:synth: `"plugin": "builtin:synth", "preset":
// "Sweet Cream Synth Lead"`. garageBandSynthPatch() finds the patch and its instrument; each instrument's
// settings have their own translation (Retro Synth here; Vintage B3, ES2, ES1 and EFM1 in apple_synths.hpp; Vintage
// Electric Piano, Vintage Clav and Sculpture in apple_keys.hpp; Alchemy's virtual-analog and additive patches in
// alchemy.hpp).
//
// Retro Synth: Retro Synth's saved settings (parameter #n = params[n], unused ones 1e30) become a
// builtin:synth patch: its engine (Analog and Sync oscillators, FM as a sine carrier with a sine modulator,
// Table as saw stand-ins), filter, envelopes, LFO and vibrato, glide and autobend, unison and its chorus or
// flanger, plus the patch's own effects (logic_patches.hpp). Parameter numbers from Logic's CSParameterOrder
// list for Retro Synth and GarageBand's 114 Retro Synth channel strips; the scales marked "guess" (cutoff in
// Hz, envelope and LFO depth in octaves, FM ratio and index, sync pitch, voice detune) await reference renders.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

struct GarageBandSynth {
    std::string name, instrument;  // the patch, and the instrument it plays ("Retro Synth", "Vintage B3", "ES2", ...)
    std::string engine;            // the instrument's mode where it has one (Retro Synth: "Analog", "Sync", "Table", "FM";
                                   // Alchemy: "virtual analog" or "additive"; the Vintage keyboards their model's family:
                                   // "tine", "classic"; Sculpture its exciter's class: "plucked", "bowed")
    nlohmann::json synth;         // a builtin:synth patch object (merged over "Init")
    int transpose = 0;            // semitones (Retro Synth's Transpose)
    nlohmann::json fx = nlohmann::json::array();   // its chorus or flanger, then the patch's effects
    std::vector<std::string> notes;                // what is approximated or left out
    nlohmann::json arp;                            // the instrument's own arpeggiator as a track "arp" (Alchemy's; null = none)
};

// The builtin:synth version of Retro Synth settings (params[n] = parameter #n).
GarageBandSynth retroSynthPatch(const std::vector<float> &params);
// A GarageBand or Logic patch on an instrument re-created here, by name (any case); false when there is none. `why`
// says why a patch on such an instrument doesn't play on builtin:synth (an Alchemy patch built on spectral synthesis or
// moving grains, content that isn't installed, or samples, which the sampler plays; a Sculpture patch played by
// side-chain audio, or made of movement a static voice can't play).
bool garageBandSynthPatch(const std::string &name, GarageBandSynth &out, std::string *why = nullptr);
// "Retro Synth (Analog mode)", "Alchemy (virtual analog)", "Sculpture (plucked)": the instrument a re-created patch
// plays and its mode
std::string garageBandSynthKind(const GarageBandSynth &g);
// Every installed patch on such an instrument: name and a description ("Retro Synth (Analog mode)") (cached per process).
const std::vector<std::pair<std::string, std::string>> &garageBandSynthPatches();

} // namespace wl
