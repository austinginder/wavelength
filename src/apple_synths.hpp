#pragma once
// GarageBand's Vintage B3, ES2, ES1 and EFM1 settings, played on builtin:synth (garageBandSynthPatch() in
// retro_synth.hpp finds the patch and its instrument). Each takes the instrument's saved floats, params[k] = the
// strip's float slot k + 1 (slot 0 is reserved): the parameter ID for Vintage B3, ES1 and EFM1, ES2's own stored row
// for ES2. Values above 1e25 count as unset (0). Each returns a builtin:synth patch, its effects and notes on what
// is approximated or left out; `transpose` stays 0 (EFM1's Transpose is in its oscillators' `semi`).
//
// Vintage B3: the upper manual's drawbars (3 dB a step, 8 = full) as up to nine sines, percussion as a fading 2nd
// or 3rd harmonic, key click as a noise burst, scanner vibrato as a pitch LFO, sustain as the release; its EQ,
// distortion, rotor cabinet (brake = bypassed) and reverb as effects. The lower manual and pedals aren't played,
// and the 234 preset-key registrations stored before the floats aren't needed (the drawbar floats are the sound).
// ES2: three oscillators weighted by the mix triangle (Digiwaves play as sines: Apple's wavetables aren't copied;
// oscillator 1's FM from oscillator 2 as a sine FM; oscillators 2 and 3's sync waves hard-synced to oscillator 1;
// Sine Level after the filter), the filter the blend favours, drive, the router's ENV2, velocity, keyboard, pad
// and LFO routes to cutoff, pitch, pulse width, amp and pan, ENV1 -> pitch as the pitch envelope, ENV3 as the
// amp, voices, glide, Analog as unison detune, the distortion and the modulation effect as a chorus.
// ES1: the oscillator (triangle, saw, square, narrowing pulses), the sub (square, its octave and pulse variants,
// noise), the filter with its ADSR and mod envelope, the amp modes, the LFO, glide, Analog and the chorus modes.
// EFM1: a sine carrier with a sine modulator at the harmonic ratio, the modulation envelope as the FM decay and
// sustain, the sub, the volume envelope, voices and glide, stereo detune as unison, the LFO's vibrato.
//
// Guesses that await reference renders: (A1) a 0..1 cutoff knob spans 20 Hz .. 20 kHz exponentially; (A2) a
// router or envelope intensity of 1 sweeps the cutoff 10 octaves; (A3) EFM1's FM Intensity 1 = modulation index
// 8. Also the B3 vibrato depths (8/15/24 cents), percussion level, click, EQ frequencies and distortion and reverb
// scales; ES2's FM index (100% = 6), distortion and chorus scales and the order of its sync waves; ES1's wave
// order, pulse widths, sub variants and LFO and mod-envelope depths. Velocity-split values resolve at velocity 0.8.
// Sources: GarageBand's channel strips (84 ES2, 11 Vintage B3, 6 ES1 and 6 EFM1 factory strips), Logic's
// CSParameterOrder lists for the four instruments, and Apple's public Logic Pro guides (the ES2 router's pitch
// intensity scale).
#include "retro_synth.hpp"

#include <vector>

namespace wl {

// The builtin:synth versions of each instrument's settings (params[k] = float slot k + 1).
GarageBandSynth vintageB3Patch(const std::vector<float> &params);
GarageBandSynth es2Patch(const std::vector<float> &params);
GarageBandSynth es1Patch(const std::vector<float> &params);
GarageBandSynth efm1Patch(const std::vector<float> &params);

} // namespace wl
