#pragma once
// SFZ instruments (the open text format for multisampled instruments): the file's headers flattened
// into one opcode map per <region> (control -> global -> master -> group -> region, later wins),
// with #define and #include applied. builtin:sampler turns the regions into zones.
#include <map>
#include <string>
#include <vector>

namespace wl {

struct SfzRegion {
    std::map<std::string, std::string> op;   // opcode -> value, aliases folded to one name
    std::string get(const std::string &k, const std::string &def = "") const {
        auto it = op.find(k);
        return it == op.end() ? def : it->second;
    }
    double num(const std::string &k, double def) const;
    int key(const std::string &k, int def) const;   // a MIDI key: number or note name ("c#4" = 61)
    bool has(const std::string &k) const { return op.count(k) > 0; }
};

struct SfzFile {
    std::string dir;                  // the folder sample paths are relative to (the .sfz's own)
    std::vector<SfzRegion> regions;
    int noteOffset = 0;               // <control> note_offset + 12 * octave_offset, added to every key
    std::map<int, double> cc;         // <control> set_ccN: controller values at the start (others are 0)
};

bool parseSfz(const std::string &path, SfzFile &out, std::string &err);
// "c4" = 60, "c#4"/"db4" = 61, or a plain number; -1 if neither
int sfzNoteNumber(const std::string &v);

} // namespace wl
