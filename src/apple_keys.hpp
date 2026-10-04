#pragma once
// GarageBand's Vintage Electric Piano ("E-Piano" in channel strips), Vintage Clav ("Clav") and Sculpture settings,
// played on builtin:synth (garageBandSynthPatch() in retro_synth.hpp finds the patch and its instrument). Each takes
// the instrument's saved floats, params[n] = parameter #n (the CSParameterOrder id; float slot n + 1, slot 0 reserved;
// none of the three stores a prefix before its floats), values above 1e25 unset (0). The blocks: E-Piano v2 with 41
// values (plug-in id 0xd5), Clav v4 with 66 values and an 8-byte trailer (0xdf), Sculpture v6 with 579 or 580 values
// and its recorded envelopes, scale and surround chunks after them (0xde; big-endian in its factory presets). Each
// returns a builtin:synth patch, the instrument's own effects and notes on what is approximated or left out; the
// strip's other plug-ins stay with patchChainEffects(), and `transpose` stays 0 (Sculpture's Transpose is in its
// oscillators' octave). Velocity-split values resolve at velocity 0.8; velocity colour goes through the filter's
// velocity (builtin:synth has no velocity -> FM index, oscillator level or decay).
//
// Vintage Electric Piano: the model's family (tine, reed or Electra, by model index) as a voice: tines a sine with a
// decaying ratio-1 FM bark plus the tine bell (a ratio-14 FM "ding" after the filter, its level from Bell Volume),
// voiced against Wavelength's own KY Electric Piano; reeds a sine with a decaying square for the odd-harmonic bark under
// a velocity-opened filter; Electra a sine with a decaying saw. Decay and Release as the amp envelope, Tune, Voices
// (1 = mono), and its effects: drive (Tone as a high-shelf cut, then saturation), EQ and Bass Boost as shelves,
// phaser, tremolo (180 degrees of stereo phase = auto-pan) and chorus, rates in Hz or tempo-synced. Damper noise,
// Warmth, stretch tuning, key-position stereo and hammer delay aren't played; Volume isn't applied (see the levels).
// Vintage Clav: the model's family (classic, funk, mellow, harpsichord, wood, sitar, dulcimer, harp) as oscillators,
// the pickups' positions as the pulse width (the comb a pickup at that string fraction imposes), Brilliance, Shape,
// String Damping and Damper as the filter and its envelope, String Decay and Release as the amp envelope, Stiffness as
// an inharmonic FM partial, Tension Mod as a pitch drop-in, the Brilliant/Treble/Medium/Soft switches as EQ (the bass
// cuts' capacitors adding up: one high-pass at 1 / sum(1 / f)), and the effects section: wah (an auto-wah, or fixed at
// the pedal), compressor, distortion and the modulation effect (phaser, flanger as a short chorus, chorus). Release
// click, Pitch Fall, Warmth, stretch tuning and the stereo spreads aren't played.
// Sculpture: its class from the exciting object (plucked: Impulse, Pick; struck: Strike, GravStrike; bowed: Bow,
// Bow Wide; blown; noise), the morphable values blended at the morph pad (or the morph envelope's time-weighted average
// path), the string's Media and Inner Loss as its decay and brightness envelope (Damp objects shorten it), a stiff
// material as inharmonic FM, Resolution as a low-pass, Sculpture's filter, amp envelope, vibrato, LFO, envelope and
// velocity routes to cutoff and object 1's strength, pickup movement as a chorus, Tension Mod, keyboard mode, glide,
// Transpose, Tune, Warmth as unison, the waveshaper as saturation, Body EQ's Basic EQ and the delay. Refused with the
// reason: object 2 External (side-chain audio played through the string), no exciting object, a morph envelope that
// moves material, exciter or cutoff by more than 0.35 during a note, and a sustained (bowed, blown, noise) voice whose
// unplayed movement (LFO and jitter amounts, envelope amounts at half weight, Bouncing and Mass objects) adds up to 1.5.
//
// Levels: per family or class, so the engine's level probe (C3 held, then a C minor chord, velocity 0.8) measures about
// -25 LUFS; fitted over GarageBand's settings, which showed keeping the instrument's own Volume or Level knob widens the
// spread (in GarageBand it offsets each model's own loudness), so E-Piano keeps none of it and Clav and Sculpture 10 %.
// Guesses that await reference renders: the decay and release scales, the tine bell, the drive, EQ frequencies and
// chorus scales; the Clav families per model index, pickup mode order, tone-switch cuts and effect scales; Sculpture's
// string decay and brightness, the morph blend between points, filter type order, cutoff (A1: 20 Hz .. 20 kHz
// exponentially), modulation depths (an amount of 1 on cutoff = 5 octaves), vibrato in semitones, Body EQ in dB and
// the waveshaper drive. Sources: GarageBand's channel strips (6 E-Piano, 10 Clav and 15 Sculpture factory strips) and
// presets (statistics over their values), the CSParameterOrder lists GarageBand installs for the three instruments, and
// Apple's public Logic Pro guides.
#include "retro_synth.hpp"

#include <string>
#include <vector>

namespace wl {

// The builtin:synth versions of each instrument's settings (params[n] = parameter #n).
GarageBandSynth vintageEPPatch(const std::vector<float> &params);
GarageBandSynth vintageClavPatch(const std::vector<float> &params);
// Sculpture's, or none: `why` says why the patch can't play on builtin:synth (empty when it plays).
GarageBandSynth sculpturePatch(const std::vector<float> &params, std::string &why);

} // namespace wl
