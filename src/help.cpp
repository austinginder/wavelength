#include "help.hpp"

#include "term.hpp"

#include <cctype>

#include <algorithm>
#include <map>
#include <sstream>
#include <vector>

namespace wl {

// Every command's usage and details. Usage lines start "  wavelength <command>"; a continuation of the
// usage is indented further; details are indented six spaces. The notes after the last command apply
// to every command.
const char *kUsage = R"(Wavelength, a headless music engine for AI agents (https://wavelength.run)

Usage:
  wavelength plugins [--rescan] [--json] | plugins --block <plugin> [--reason TEXT] | --unblock <plugin>
      List installed CLAP, VST3 and VST2 plugins and the built-in instruments (cached; --rescan
      reloads every bundle). A blocked plugin (one that opens a licence window on every load,
      or crashes) is never loaded: render, params, presets and audition refuse it by name.
  wavelength presets <plugin> [--search TEXT] [--rescan] [--json]
      List a plugin's presets to use as "preset": CLAP preset discovery, VST3 program lists,
      preset files in its preset folders, NKS presets, DX7 cartridges, bank entries.
      `presets arp`: GarageBand's and Logic's Arpeggiator presets and the patches that
      arpeggiate, for a track's "arp" (macOS).
  wavelength samples [--search TEXT] [--kit NAME [--roundrobin]] [--soundfont NAME] [--patch NAME] [--dspreset NAME] [--install-soundfont [--force]] [--json]
      List sample libraries for builtin:sampler (Bitwig multisamples, drum kit folders, SFZ,
      DecentSampler presets, SoundFonts, Logic and GarageBand instruments and patches, Apple Loops)
      and impulse responses for the convolve effect; --kit shows the General MIDI key each of a kit's
      files is mapped to, --soundfont a SoundFont's presets, --patch a GarageBand or Logic patch (its
      channels, whether and what it plays here, its effects and sends with their rooms for convolve),
      --dspreset a DecentSampler preset (its samples, its effects as played here, what is left out).
      --install-soundfont downloads MuseScore General (MIT), the General MIDI set.
  wavelength loops [--search TEXT] [--key KEY] [--midi] [--json] | loops --notes <name> [--key KEY] [--json]
      List the Apple Loops GarageBand and Logic install (macOS): category, key, tempo, length in
      beats, and "notes" for software-instrument loops. A clip plays one as {"file": "lib:Apple
      Loops/<name>.caf"}: it follows the song's tempo, and "key": "song" moves it into the job's key.
      --key shows the shift into a key; --midi lists only loops with notes; --notes prints a loop's
      notes in beats (moved into --key), ready for a track.
  wavelength audition <plugin> [--jobs 4] [--limit N] [--rebuild] [--json] | audition --retag
      Render every preset once (C4, 1 s) in worker processes and index how it sounds: octave
      offset, loudness, brightness, band balance, envelope, width. `presets` then shows tags
      (dark, bright, sub, pluck, slow attack, wide, self-playing, octave -1...) you can search.
  wavelength compat [<plugin>...] [--format clap|vst3|vst2|au] [--jobs 1] [--presets 3] [--timeout 120] [--report FILE.md] [--rebuild] [--json]
      Test every installed plugin (or the ones named), each in its own process: it opens, renders
      (a C2-C5 chord, or noise through an effect), a few of its presets load and change it, and its
      state saves and reloads. Crashes, hangs, silence, garbage, licence windows and presets that
      change nothing are reported per plugin. Results are cached (bundle date + engine version), so
      a rerun tests only new or updated plugins; --rebuild tests them all. --report writes Markdown.
  wavelength analyze <file.wav | render-dir> [--start S] [--end S] [--song-time] [--grid BPM [--div 4]] [--every S] [--peaks [--top N]] [--json]
      Measure what can't be heard: pitch, brightness, spectral balance, stereo width,
      onsets and envelope of a WAV (or a window of it). A render folder analyzes its mix,
      every stem and every marker section. --start/--end are seconds into the file; with
      --song-time they are song time (a render's lead-in is added from its report.json).
      --grid lists each onset's beat and its timing offset from the nearest 1/div-beat step.
      --every S prints the loudness of every S-second window (file time, or song time from the
      first beat with --song-time), labelled with the render's sections: the song's contour at a
      glance, dropouts and drops included. --peaks lists the strongest spectral peaks of the
      window (Hz, note and cents, level; 12 or --top N) and their spacing: where a comb,
      resonator or flanger sits. A render folder also analyzes bus stems.
  wavelength params <plugin> [--preset NAME] [--state FILE] [--format F] [--all] [--set "Name=v"]... [--map "Name" [--steps N]] [--json]
      Show a plugin's parameters, optionally after loading a state/preset.
      Hidden and read-only parameters are omitted unless --all is given. --set "Name=0.55"
      prints the plugin's display text for a plain value (or the value it reads for display
      text, "Name=800 Hz", or a note name); --map "Name" tabulates value -> display across the
      range (21 rows, or --steps N). Neither changes anything.
  wavelength render <job.json> [--out DIR] [--stems float|24|16|none] [--deliver mp3,flac,...] [--jobs N] [--tracks "A,B"] [--level-from report.json] [--from BAR --to BAR [--loop]] [--png | --no-png] [--keep] [--fallbacks] [--cache] [--mix JSON] [--json] [--verbose]
      Render a job to DIR/stems/*.wav and DIR/mix.wav (default DIR: ./out). Plugin tracks render
      in worker processes, N at once (default: half the cores, up to 4; --jobs 0 = one process);
      a worker whose plugin crashes is started again (job "retries", default 2); a track that
      still fails is left out of the mix and listed in "failedTracks", and the render then
      reports "ok": false and exits 1 (mix.wav and report.json are still written).
      --tracks renders only the named tracks (and, muted, any track that keys their
      sidechains), with the song's buses and master, to check a part without the whole song.
      Their stems keep the full render's numbers and names (05-lead.wav stays 05-lead.wav).
      --level-from out/report.json keeps the master at that render's gains (its loudness-target
      and normalize gain) instead of targeting again: a --tracks render then plays each part at
      the level it has in the full mix.
      --from 41 --to 45 renders only bars 41-44 (after --preroll bars, default 2, rendered and cut:
      reverbs and held notes are already going); the files hold just those bars, and the report's
      "window" says where they sit in the song. Quick previews of a section, alone or with --tracks.
      --loop with --from/--to renders those bars as a seamless loop for games and apps: only the notes
      that start inside them, rendered on past the end, with the tail (reverbs, releases) folded back
      onto the start. Every file (mix, stems as layers, bus stems) is exactly the loop's length and
      carries a smpl loop chunk (Godot's WAV import loops it; so do samplers).
      --png (or the job's "picture": true) draws DIR/song.png: sections and bars, the mix's loudness over
      time with each section's level, its spectrum, and a lane per track with its notes over its
      post-fader level. An agent that can read images sees the whole song at a glance.
      --keep (a song's job, rendered whole) keeps this render with the song: its MP3, picture and report
      go to render/ and the manifest's "render" names them, so the song can be heard without rendering.
      --fallbacks plays every track's first available fallback, as a computer without its plugins would
      (`wavelength fallbacks --suggest` proposes them).
      --cache reuses each track whose audio can't have changed since an earlier --cache render (its
      notes, sound, effects and files, the tempo and the window): a mixer change (gain, pan, mute,
      sends, rides) renders no track again, a note change renders that track. The report marks reused
      tracks "cached". A plugin update or a changed preset file named by "preset" isn't noticed. Kept
      in the cache folder (tracks/), oldest first out past $WAVELENGTH_TRACK_CACHE_GB (default 4).
      --mix '{"Bass": {"gain": -3, "pan": 0.2, "mute": true}}' (or --mix @file.json) sets tracks' faders,
      pans and mutes for this render only; the job file stays as it is. --no-png skips the picture a job
      asks for with "picture": true.
      --deliver mp3,flac (mp3:256, flac:16, wav:16, wav:24; none) replaces the job's "deliver": files
      written next to mix.wav, decoded again and measured (report mix.deliveries). MP3 needs LAME
      (libmp3lame, or $WAVELENGTH_LAME; "none" = use ffmpeg) or ffmpeg. --tracks and --from renders skip it.
  wavelength master <mix.wav> --chain <chain.json | job.json> [--loudness LUFS] [--lead-in S] [--input-lead-in S] [--out DIR] [--deliver mp3,flac] [--json]
      Put a finished mix through a master chain (effects list, master object or a song's job:
      its master, markers and tempo; a file, or JSON inline) without re-rendering; reports
      loudness before and after. A mix with a lead-in (read from the render's report.json, or
      --input-lead-in) is lined up with the markers and keeps its lead-in unless --lead-in.
      --deliver (or the job's "deliver") writes MP3/FLAC/WAV files of the result, as for render.
  wavelength state save <plugin> --out FILE [--preset NAME | --state FILE [--format F]] [--set "Name=value"]... [--json]
      Load an optional starting preset or state, apply parameter values, save a preset
      (.clap-preset for CLAP plugins, .vstpreset for VST3, .aupreset for Audio Units).
  wavelength import <project.dawproject> [--out DIR] [--bitwig FILE.bwproject | none] [--json]
      Turn a DAWproject export (Bitwig, Studio One, Cubase...) into a job: arrangement notes,
      tracks with their plugins and saved states, volume, pan, mute, sends, groups, tempo,
      markers, volume/pan automation. Lists what the file can't carry (a DAW's own devices).
      `render project.dawproject` imports into <out>/import and renders in one go.
  wavelength import <project.bwproject> [--out DIR] [--list] [--json]
      Turn a Bitwig Studio project into a job without exporting it: tempo, tracks with their
      plugins, states and Bitwig's own devices, faders, pans, mutes, sends, arranger note clips
      (play start, loops) and automation of faders, pans and instrument plugin parameters. Audio
      clips and Bitwig 6 automation clips aren't read yet: export a DAWproject for those. --list shows
      the project's tracks and devices instead. `render project.bwproject` imports and renders.
  wavelength import <score.musicxml | score.mxl> [--out DIR] [--instrument PLUGIN] [--json]
      Turn a MusicXML score (MuseScore, Sibelius, Finale, Dorico, music21) into a job: a track per
      part, repeats and endings played out, ties, chords and voices, concert pitch for transposing
      instruments, dynamics and hairpins as velocities, staccato and accents, tempo marks, key
      signatures (with a mode) as "keys", rehearsal marks as markers. Notes keep their score marks
      in "marks". Parts get General MIDI sounds like a MIDI import. `render score.mxl` does both steps.
  wavelength import <song.mid | loop.caf> [--out DIR] [--instrument PLUGIN] [--json]
      Turn a Standard MIDI File into a job: tempo map, time signature, markers, one track per
      MIDI track and channel (notes, sustain pedal, volume/pan/expression, other CCs, pitch
      bend). Channel 10 plays builtin:drums; other channels a General MIDI-family sound from
      the sample library, or PLUGIN for all of them. `render song.mid` imports and renders.
      A software-instrument Apple Loop (a .caf with notes inside) imports the same way.
  wavelength export <job.json> [--out song.mid | song.dawproject] [--no-print] [--json]
      Write the job's parts as a MIDI file (type 1): tempo map, time signature, markers, and a
      track per job track with its notes, CC, pitch bend and pressure automation. To a .dawproject
      (Bitwig, Studio One, Cubase): the tracks with their plugins and plugin states as the job sets
      them up, notes, faders, pans, sends, buses, the master, tempo map, markers, fader and pan
      curves, built-in eq/compressor/limiter as the standard devices. Built-in instruments are
      printed (rendered dry to audio on the track, notes kept; --no-print: notes only); lists what
      has no counterpart.
  wavelength kit [install [names...] [--force] | remove NAME] [--json]
      Free instruments for a machine without plugins (a cloud container, CI): Surge XT (with its
      factory patches), OB-Xf (with its patches) and Dexed from their own releases, and the General
      MIDI SoundFont. They go into Wavelength's own folder, never the system's plugin folders. Without
      a subcommand it lists them with their licence, size and status; an entry whose plugin is already
      installed elsewhere is skipped unless --force.
  wavelength docs [agents | job-format | effects | song-format] [--section TEXT] [--json]
      The docs that match this binary, built in: the operating guide for agents (read it first), the
      job format and the effects reference. --section prints one part (a heading or part of one).
  wavelength mcp
      Runs a Model Context Protocol server on stdin/stdout for MCP clients (Claude Desktop, Claude Code,
      Cursor): tools to read the guide, list instruments, presets, samples and parameters, lint, render
      (with the song picture), draw an arrangement, analyze, find bars and import MIDI/MusicXML/DAWproject/Bitwig projects;
      for songs: save, history, undo, diff, comments and replies, fallbacks, pack.
  wavelength picture <job.json> [--out FILE.png] [--width PX] [--json]
      Draws the arrangement before rendering: sections, bars and a lane per track with its notes
      (default: arrangement.png next to the job). render --png draws the full picture with levels.
  wavelength card <plugin> [<preset>...] [+ <plugin> [<preset>...]]... [--state FILE] [--out FILE.png] [--width PX] [--jobs N] [--probe DIR] [--json]
      A patch card: how a preset sounds, for an agent that reads images. Every patch plays one probe
      (a held C4, C2 C3 C4 C5, a C minor chord, eight 16ths at 120 BPM) in a child render; the card
      shows the held note zoomed (spectrum with note names and the harmonics of the sounding note,
      level, attack, note-off and tail), its pitch over time and waveform, the four octaves, the
      chord and the run, with the numbers and flags: sounds an octave down (and the transpose that
      fixes it), slow attack, long tail, percussive, silent at C2, pitch moving, noise before the
      first note, a blurred run. One patch = a full card (card.png); several = a contact sheet of
      compact cards to compare (cards.png). "+" starts the next plugin's presets:
        wavelength card Vaporizer2 "SY Basic Hypersaw" "LD Mighty Lead" + Zebralette3 "Four by Two"
      --json has the measurements per patch; --probe DIR keeps the probe's job, stems and report.
  wavelength stage <job.json> [--targets FILE] [--report FILE] [--out DIR] [--write FILE] [--apply] [--dry-run] [--jobs N] [--json]
      Gain staging: every fader from its stem loudness. One render (no stems, picture or deliveries;
      tracks cached for your next render) measures each track before its fader; its gain becomes
      target - stem LUFS. Targets come from the track's role, a word of its name (Kick -12, Clap -17.5,
      Hats -20, Bass -15.5, Sub -19, Lead -16, Layer -21, Arp -21, Pad -22, Keys -18, FX -22 ...),
      then targets.json beside the job and --targets FILE: {"Lead": -17, "Kick": {"lufs": -12, "peak": -1},
      "role:Pad": -23, "bus:Hall": -24}. The gains go to gains.json beside the job (what a generator
      script reads; --write elsewhere); --apply also sets them in the job; --report FILE measures an
      existing render instead. Tracks whose peak lands over 0 dBFS after the fader and tracks no role
      fits are listed.
  wavelength timeline <job.json> [--every BARS] [--json]
      Song time of every marker and of every BARS bars (default 8) from the tempo map (ramps
      included), in song seconds and in file time (after the lead-in), with the tempo there,
      the last sound and the render's end: plan a length or find bar 57 without rendering.
  wavelength lint <job.json> [--tracks "Soprano,Alto,Bass"] [--low "Bass"] [--split "Organ=4"]
                  [--from BAR] [--to BAR] [--section NAME] [--crossings] [--json]
      Voice-leading check between melodic tracks (one voice each: its top note, its lowest for
      --low tracks, or N voices top to bottom for --split chord tracks): parallel fifths and
      octaves in similar motion, and with --crossings a voice below the next one in order (high
      to low). Each problem shows both chords' notes and where they are; --from/--to/--section
      limit the report to a bar range or a marker's section.
  wavelength lint <job.json> --harmony [--key "D minor"] [--ignore "SFX,Ping"] [--chords] [--max-bars 2]
                  [--from BAR] [--to BAR] [--section NAME] [--json]
      Harmony check on the notes: the key of every stretch of bars (the job's "keys", --key, or
      detected), a chord chart with --chords, one- or two-bar chords outside the key that go
      straight back (heard as a key change), clashes (a minor 2nd/9th held a beat, one note outside
      the key) and in-key rubs grouped per pair of tracks.
  wavelength save [song] [-m MESSAGE] [--json]
      Saves a revision of the song (docs/song-format.md): the job and the manifest's source, notes and
      media files, in history/ inside the song folder. The first save writes the manifest,
      wavelength.json. Every full render of a song's job adds a revision too, and its report names it.
  wavelength history [song] [--named] [--json] | history [song] --to-git DIR | --bundle FILE.bundle
      The revisions, newest last (* marks the current one). --to-git writes them as commits to a new
      git repository outside the song (--bundle: one file), the same commits on every run.
  wavelength undo [song] | redo [song] | restore [song] <revision> [--json]
      Undo and redo step through changes the way an editor does; restore brings back any revision's
      files. Each adds a revision, so nothing is ever lost.
  wavelength diff [song] [A [B]] [--json]
      What changed in the music between two revisions (r12 or 12; A defaults to the last revision, B
      to the folder now): tracks, sounds, mix, effects and curves by bar, and notes per bar.
  wavelength comments [song] [--all] [--json] | --reply ID --text TEXT [--done] | --resolve ID | --reopen ID
      The song's comments (review.json) with the revision each was made on and whether the music they
      point at has changed since. Open ones by default; --all includes resolved.
  wavelength pack [song] [--out FILE.wavelength] [--no-history] [--no-render] [--no-review] [--json]
      One shareable file: the song folder as a ZIP (the job, listed files, comments and history).
      Refuses a job that uses files outside the song. Lists the plugins and libraries it needs.
  wavelength unpack <file.wavelength> [--out DIR] [--force] [--json]
      Checks every entry first (no paths outside the folder, no links, no .git); never runs anything.
      render and serve open a .wavelength file directly.
  wavelength validate <song | file.wavelength> [--json]
      A song folder or package against the format spec.
  wavelength fallbacks [song | job.json] [--suggest [--write] [--no-measure]] [--json]
      What each track plays on this computer: its own plugin or library, or which fallback. --suggest
      proposes a built-in stand-in (a synth patch, the drum kit, a General MIDI program) for every
      track without one, each level-matched to the song's last render in out/ or render/ (one child
      render per fallback position; --no-measure skips that); --write puts them in the job.
  wavelength migrate [song] [--license SPDX] [--author NAME] [--dry-run] [--no-copy] [--json]
      Brings a song folder made before the format up to it: writes wavelength.json (from site.json when
      there is one), makes paths inside the song relative, copies files the job uses out of out/ into
      media/, names library samples ("lib:Legend 909/Kick.wav") and preset files (by preset name), copies
      other outside files into media/ (--no-copy: only reports them), updates review.json, keeps the last
      render when it matches the job, and saves a revision. Lists what it could not fix.
  wavelength purge [folder...] [--dry-run] [--masters] [--json]
      Frees the disk renders take while every song stays playable: in each render folder under the
      folders (default the current one; a render folder is where a render wrote report.json: out/, a
      shootout's out folders, the review page's previews) it deletes mix.wav, the stems, FLAC and WAV
      deliveries and the preview cache. A render with no MP3 gets mix.mp3 first. Reports and pictures
      stay; so does anything the song's job or manifest names (media/, render/) and every WAV no render
      wrote (listed at the end). Renders written in the last 5 minutes may still be running and are
      skipped. Outputs of `wavelength master` are finished masters and stay unless --masters.
      Render again to get stems and mix.wav back.
  wavelength serve [SONGS_DIR] [--port 7400] [--host 127.0.0.1] [--open] [--ui DIR]
      A local web UI for reviewing songs (a folder of song folders, default the current one): the
      arrangement with its chords and harmony problems, loudness, stems, and quick previews: any
      bars, any tracks, rendered through the song's sends, buses and master in seconds. Comments
      pinned to bars, tracks and notes go to each song's review.json for the agent. Read-only on
      the music; plugins only run in render child processes. --open opens the browser.
  wavelength version [--check] [--json]
      This build's version (and the song format it reads); --check also asks GitHub for the latest release.
  wavelength upgrade [--check] [--force] [--json]
      Replaces this binary with the latest release for this computer when there is a newer one: downloaded
      from GitHub, checked against the release's SHA256SUMS.txt and run once before it takes this one's
      place (a release folder's docs and examples are refreshed too). --check only reports. A development
      build (-dev) is left alone unless --force.

<plugin> is a plugin id, a plugin name (Apricot, "BBC Symphony Orchestra"), or a path to a
.clap/.vst3/.vst bundle. Prefix with clap:, vst3:, vst2: or au: (Audio Units, macOS) when a name exists in more than one format.
State formats: auto (default), clap-preset, vstpreset, nksf, fxp, serum, juce-valuetree (.odin), h2p,
dx7 (<cartridge>.syx#<voice>), synplant, cherry, ngrr, microtonic, soundbox, decentsampler,
reaktor (.ens#<snapshot>), firefly (.ff2preset), juce-xml (.vvp), juce-string (.vital), raw.
Exit status is non-zero on any error; with --json, errors are {"ok":false,"error":...}.
)";


namespace {

struct Block { std::vector<std::string> usage, details; };
struct Entry { std::string name; std::vector<Block> blocks; };

// the short list: groups in order, one line per command (a command the table misses shows under "More")
struct Summary { const char *group, *name, *text; };
const Summary kSummaries[] = {
    {"Make music", "render", "Render a job: stems, a mix, a report and the song picture"},
    {"Make music", "stage", "Set every fader from its stem loudness (gains.json)"},
    {"Make music", "lint", "Check harmony and voice leading before rendering"},
    {"Make music", "picture", "Draw the arrangement before rendering"},
    {"Make music", "timeline", "Song time of bars and markers from the tempo map"},
    {"Make music", "analyze", "Measure pitch, brightness, width and onsets of audio"},
    {"Make music", "master", "Put a finished mix through a master chain"},
    {"Find sounds", "plugins", "Installed plugins and built-in instruments"},
    {"Find sounds", "presets", "A plugin's presets, to use by name"},
    {"Find sounds", "samples", "Sample libraries, GarageBand patches, reverb rooms"},
    {"Find sounds", "loops", "Apple Loops by category, key and tempo; their notes"},
    {"Find sounds", "audition", "Render every preset once and tag how it sounds"},
    {"Find sounds", "card", "Picture of how presets sound: one full card, or a sheet to compare"},
    {"Find sounds", "compat", "Test every plugin: loads, renders, presets, state"},
    {"Find sounds", "params", "A plugin's parameters and their display text"},
    {"Find sounds", "state", "Save a preset from a state and parameter values"},
    {"Find sounds", "kit", "Free instruments for a machine without plugins"},
    {"Songs", "save", "Save a revision of the song"},
    {"Songs", "history", "The song's revisions, or export them to git"},
    {"Songs", "undo", "Undo, redo, or restore any revision"},
    {"Songs", "diff", "What changed in the music between revisions"},
    {"Songs", "comments", "Read and answer the review comments"},
    {"Songs", "fallbacks", "What each track plays here; suggest stand-ins"},
    {"Songs", "pack", "Pack the song into one .wavelength file"},
    {"Songs", "unpack", "Unpack a .wavelength file, checking it first"},
    {"Songs", "validate", "Check a song or package against the format"},
    {"Songs", "migrate", "Bring an older song folder up to the format"},
    {"Songs", "purge", "Delete render WAVs and stems, keep MP3s playable"},
    {"Import and export", "import", "A job from MIDI, MusicXML, DAWproject or a Bitwig project"},
    {"Import and export", "export", "A job as a MIDI file or a DAWproject"},
    {"People and agents", "serve", "A local review page for listening together"},
    {"People and agents", "mcp", "An MCP server for Claude Desktop, Cursor and more"},
    {"People and agents", "docs", "The operating guide and references, built in"},
    {"Wavelength", "version", "This build's version; --check for a newer one"},
    {"Wavelength", "upgrade", "Install the latest release"},
};
// other names that open a command's help
const std::map<std::string, std::string> kAliases = {{"redo", "undo"}, {"restore", "undo"}};

size_t indentOf(const std::string &l) { return l.find_first_not_of(' ') == std::string::npos ? l.size() : l.find_first_not_of(' '); }

void parse(std::string &header, std::vector<Entry> &entries, std::vector<std::string> &notes) {
    std::istringstream in(kUsage);
    std::string line;
    bool inCommands = false, afterCommands = false;
    std::getline(in, header);
    while (std::getline(in, line)) {
        const size_t ind = indentOf(line);
        if (line.rfind("  wavelength ", 0) == 0) {
            std::istringstream words(line);
            std::string w, name;
            words >> w >> name;
            if (entries.empty() || entries.back().name != name) entries.push_back({name, {}});
            entries.back().blocks.push_back({{line.substr(2)}, {}});
            inCommands = true;
        } else if (inCommands && !afterCommands && ind >= 10 && !entries.empty() && entries.back().blocks.back().details.empty()) {
            entries.back().blocks.back().usage.push_back(line.substr(2));   // the usage goes on
        } else if (inCommands && !afterCommands && ind == 6 && !entries.empty()) {
            entries.back().blocks.back().details.push_back(line.substr(6));
        } else if (inCommands && line.empty()) {
            afterCommands = true;
        } else if (afterCommands && !line.empty()) {
            notes.push_back(line);
        }
    }
}

// usage: the program dim, the command bright, <values> in colour, [options] and | dim, --flags in colour
std::string styleUsage(const term::Style &st, const std::string &line, const std::string &name) {
    if (!st.on) return line;
    std::string out;
    size_t i = 0;
    if (line.rfind("wavelength ", 0) == 0) { out += st.dim("wavelength") + " "; i = 11; }
    else { size_t k = line.find_first_not_of(' '); out += line.substr(0, k); i = k; }
    bool first = true;
    while (i < line.size()) {
        const char c = line[i];
        if (c == '<') { const size_t e = line.find('>', i); const size_t end = e == std::string::npos ? line.size() : e + 1; out += st.cyan(line.substr(i, end - i)); i = end; continue; }
        if (c == '[' || c == ']' || c == '|') { out += st.dim(std::string(1, c)); ++i; continue; }
        if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            size_t e = i + 2;
            while (e < line.size() && (std::isalnum((unsigned char)line[e]) || line[e] == '-')) ++e;
            out += st.cyan(line.substr(i, e - i));
            i = e;
            continue;
        }
        if (std::isalpha((unsigned char)c) && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '|')) {   // a word: the command's own words are bright
            size_t e = i;
            while (e < line.size() && (std::isalnum((unsigned char)line[e]) || line[e] == '-' || line[e] == '_')) ++e;
            const std::string word = line.substr(i, e - i);
            const bool command = (first && word == name) || word == "save" || word == "install" || word == "remove" || word == "redo" || word == "restore";
            out += command ? st.accent(word) : word;
            first = false;
            i = e;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// details: --flags in colour, `code` bold (without its backticks)
std::string styleText(const term::Style &st, const std::string &line) {
    if (!st.on) return line;
    std::string out;
    for (size_t i = 0; i < line.size();) {
        const char c = line[i];
        if (c == '`') {
            const size_t e = line.find('`', i + 1);
            if (e != std::string::npos) { out += st.bold(line.substr(i + 1, e - i - 1)); i = e + 1; continue; }
        }
        if (c == '-' && i + 2 < line.size() && line[i + 1] == '-' && std::isalpha((unsigned char)line[i + 2]) && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '(' || line[i - 1] == '/')) {
            size_t e = i + 2;
            while (e < line.size() && (std::isalnum((unsigned char)line[e]) || line[e] == '-')) ++e;
            out += st.cyan(line.substr(i, e - i));
            i = e;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// on a terminal, a usage line longer than the window breaks before an option, under the command's first argument
std::vector<std::string> fitUsage(const std::string &line, bool on) {
    const size_t room = (size_t)std::max(60, term::width() - 2);
    if (!on || line.size() <= room || line.rfind("wavelength ", 0) != 0) return {line};
    const size_t second = line.find(' ', 11);
    const size_t hang = second == std::string::npos ? 11 : second + 1;
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        const char c = i < line.size() ? line[i] : ' ';
        if (c == '[') ++depth;
        if (c == ']') --depth;
        if ((c == ' ' && depth == 0) || i == line.size()) {
            const std::string word = line.substr(start, i - start);
            if (!cur.empty() && cur.size() + 1 + word.size() > room) { out.push_back(cur); cur = std::string(hang, ' ') + word; }
            else cur += (cur.empty() ? "" : " ") + word;
            start = i + 1;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void printEntry(std::FILE *out, const term::Style &st, const Entry &e) {
    for (size_t b = 0; b < e.blocks.size(); ++b) {
        if (b) std::fputs("\n", out);
        for (auto &u : e.blocks[b].usage)
            for (auto &part : fitUsage(u, st.on)) std::fprintf(out, "%s\n", styleUsage(st, part, e.name).c_str());
        if (!e.blocks[b].details.empty()) std::fputs("\n", out);
        const auto &details = e.blocks[b].details;
        for (size_t k = 0; k < details.size(); ++k) {
            if (st.on && k > 0 && details[k].rfind("--", 0) == 0) std::fputs("\n", out);   // each option's paragraph apart
            std::fprintf(out, "  %s\n", styleText(st, details[k]).c_str());
        }
    }
}

} // namespace

int printHelp(std::FILE *out, const std::string &command, const std::string &version) {
    const term::Style &st = term::out();
    std::string header;
    std::vector<Entry> entries;
    std::vector<std::string> notes;
    parse(header, entries, notes);
    auto find = [&](std::string name) -> const Entry * {
        if (kAliases.count(name)) name = kAliases.at(name);
        for (auto &e : entries) if (e.name == name) return &e;
        return nullptr;
    };
    if (command == "all") {
        if (!st.on) { std::fputs(kUsage, out); return 0; }   // agents get the same text as always
        std::fprintf(out, "%s %s\n", st.accent("Wavelength").c_str(), st.dim(version).c_str());
        for (auto &e : entries) { std::fputs("\n", out); printEntry(out, st, e); }
        std::fputs("\n", out);
        for (auto &n : notes) std::fprintf(out, "%s\n", st.dim(n).c_str());
        return 0;
    }
    if (!command.empty()) {
        const Entry *e = find(command);
        if (!e) {
            std::fprintf(out, "%s no command '%s'\n\n", st.fail().c_str(), command.c_str());
        } else {
            printEntry(out, st, *e);
            // what <plugin> and state formats mean, for the commands that take them
            bool plugin = false;
            for (auto &b : e->blocks) for (auto &u : b.usage) plugin |= u.find("<plugin>") != std::string::npos || u.find("PLUGIN") != std::string::npos;
            if (plugin) {
                std::fputs("\n", out);
                for (auto &n : notes) if (n.rfind("Exit status", 0) != 0) std::fprintf(out, "%s\n", st.dim(n).c_str());
            }
            return 0;
        }
    }
    // the short list
    std::fprintf(out, "%s %s %s\n\n", st.accent("Wavelength").c_str(), version.c_str(), st.dim("\u00b7 a headless music engine for AI agents \u00b7 https://wavelength.run").c_str());
    std::fprintf(out, "%s  wavelength %s %s\n", st.bold("Usage").c_str(), st.cyan("<command>").c_str(), st.dim("[options]").c_str());
    size_t col = 0;
    for (auto &s : kSummaries) col = std::max(col, std::string(s.name).size());
    col += 3;
    std::string group;
    std::vector<std::string> listed;
    for (auto &s : kSummaries) {
        if (!find(s.name)) continue;
        if (group != s.group) { group = s.group; std::fprintf(out, "\n%s\n", st.bold(group).c_str()); }
        std::fprintf(out, "  %s%s\n", term::pad(st.accent(s.name), col).c_str(), s.text);
        listed.push_back(s.name);
    }
    std::vector<std::string> more;
    for (auto &e : entries) if (std::find(listed.begin(), listed.end(), e.name) == listed.end()) more.push_back(e.name);
    if (!more.empty()) {
        std::fprintf(out, "\n%s\n", st.bold("More").c_str());
        std::string names;
        for (auto &m : more) names += (names.empty() ? "" : ", ") + st.accent(m);
        std::fprintf(out, "  %s\n", names.c_str());
    }
    std::fprintf(out, "\n  %s  %s\n  %s  %s\n  %s  %s\n", term::pad(st.cyan("wavelength help <command>"), 27).c_str(), st.dim("a command's options and details").c_str(),
                 term::pad(st.cyan("wavelength help all"), 27).c_str(), st.dim("every command in full").c_str(),
                 term::pad(st.cyan("wavelength docs agents"), 27).c_str(), st.dim("the operating guide: read it before a first job").c_str());
    return command.empty() ? 0 : 1;
}

} // namespace wl
