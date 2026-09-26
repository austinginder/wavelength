#include "sfz.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace fs = std::filesystem;

namespace wl {

namespace {
std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool readText(const fs::path &p, std::string &out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// comments out, #include expanded (relative to the top file's folder), #define applied
bool preprocess(const fs::path &file, const fs::path &root, std::map<std::string, std::string> &defines, std::string &out,
                int depth, std::string &err) {
    if (depth > 16) { err = "#include nested too deeply at " + file.string(); return false; }
    std::string src;
    if (!readText(file, src)) { err = "cannot read " + file.string(); return false; }
    std::string text;
    text.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') { while (i < src.size() && src[i] != '\n') ++i; text += '\n'; continue; }
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            const size_t e = src.find("*/", i + 2);
            i = e == std::string::npos ? src.size() : e + 1;
            text += ' ';
            continue;
        }
        text += src[i];
    }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        // longest names first, so $VEL does not eat $VELOCITY
        std::vector<std::pair<std::string, std::string>> defs(defines.begin(), defines.end());
        std::sort(defs.begin(), defs.end(), [](auto &x, auto &y) { return x.first.size() > y.first.size(); });
        auto expand = [&](std::string &l) {
            for (auto &[k, v] : defs)
                for (size_t p = l.find(k); p != std::string::npos; p = l.find(k, p + v.size())) l.replace(p, k.size(), v);
        };
        const size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos && line[first] == '#') {
            const std::string rest = line.substr(first);
            if (rest.rfind("#define", 0) == 0) {
                const size_t a = rest.find('$');
                if (a != std::string::npos) {
                    size_t b = a;
                    while (b < rest.size() && !std::isspace((unsigned char)rest[b])) ++b;
                    std::string v = rest.substr(b);
                    v.erase(0, v.find_first_not_of(" \t"));
                    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
                    defines[rest.substr(a, b - a)] = v;
                }
                continue;
            }
            if (rest.rfind("#include", 0) == 0) {
                std::string inc0 = rest;
                expand(inc0);   // #include "$DIR/$DYN.txt"
                const size_t a = inc0.find('"'), b = inc0.find('"', a + 1);
                if (a == std::string::npos || b == std::string::npos) continue;
                std::string inc = inc0.substr(a + 1, b - a - 1);
                std::replace(inc.begin(), inc.end(), '\\', '/');
                if (!preprocess(root / inc, root, defines, out, depth + 1, err)) return false;
                out += '\n';
                continue;
            }
        }
        expand(line);
        out += line;
        out += '\n';
    }
    return true;
}

std::string canonical(std::string k) {
    k = lower(k);
    static const std::map<std::string, std::string> alias = {
        {"loopstart", "loop_start"}, {"loopend", "loop_end"}, {"loopmode", "loop_mode"},
        {"offby", "off_by"}, {"polyphony_group", "group"}, {"pitch", "tune"}};
    auto it = alias.find(k);
    return it == alias.end() ? k : it->second;
}

bool isOpcodeStart(const std::string &t, size_t i) {   // an identifier followed by '='
    if (i >= t.size() || !(std::isalpha((unsigned char)t[i]) || t[i] == '_')) return false;
    size_t j = i;
    while (j < t.size() && (std::isalnum((unsigned char)t[j]) || t[j] == '_')) ++j;
    return j < t.size() && t[j] == '=';
}
} // namespace

int sfzNoteNumber(const std::string &v0) {
    const std::string v = lower(v0);
    if (v.empty()) return -1;
    if (std::isdigit((unsigned char)v[0]) || v[0] == '-') return std::atoi(v.c_str());
    static const int base[] = {9, 11, 0, 2, 4, 5, 7};   // a b c d e f g
    if (v[0] < 'a' || v[0] > 'g') return -1;
    int n = base[v[0] - 'a'];
    size_t i = 1;
    if (i < v.size() && (v[i] == '#' || v[i] == 's')) { ++n; ++i; }
    else if (i < v.size() && v[i] == 'b' && i + 1 < v.size() && (std::isdigit((unsigned char)v[i + 1]) || v[i + 1] == '-')) { --n; ++i; }
    if (i >= v.size()) return -1;
    return n + 12 * (std::atoi(v.c_str() + i) + 1);
}

double SfzRegion::num(const std::string &k, double def) const {
    auto it = op.find(k);
    if (it == op.end() || it->second.empty()) return def;
    char *end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end == it->second.c_str() ? def : v;
}

int SfzRegion::key(const std::string &k, int def) const {
    auto it = op.find(k);
    if (it == op.end()) return def;
    const int n = sfzNoteNumber(it->second);
    return n < 0 && lower(it->second) != "-1" ? def : n;
}

bool parseSfz(const std::string &path, SfzFile &out, std::string &err) {
    const fs::path file(path), root = file.parent_path();
    std::map<std::string, std::string> defines;
    std::string t;
    if (!preprocess(file, root, defines, t, 0, err)) return false;
    out = SfzFile{};
    std::map<std::string, std::string> control, global, master, group, region;
    std::string section;   // current header
    bool inRegion = false;
    auto flush = [&] {
        if (!inRegion) return;
        SfzRegion r;
        for (auto *m : {&global, &master, &group, &region}) for (auto &[k, v] : *m) r.op[k] = v;
        out.regions.push_back(std::move(r));
        region.clear();
        inRegion = false;
    };
    size_t i = 0;
    while (i < t.size()) {
        if (std::isspace((unsigned char)t[i])) { ++i; continue; }
        if (t[i] == '<') {
            const size_t e = t.find('>', i);
            if (e == std::string::npos) break;
            flush();
            section = lower(t.substr(i + 1, e - i - 1));
            if (section == "global") { global.clear(); master.clear(); group.clear(); }
            else if (section == "master") { master.clear(); group.clear(); }
            else if (section == "group") group.clear();
            else if (section == "region") inRegion = true;
            i = e + 1;
            continue;
        }
        if (!isOpcodeStart(t, i)) { ++i; continue; }
        const size_t eq = t.find('=', i);
        const std::string key = canonical(t.substr(i, eq - i));
        // the value runs to the end of the line or the next opcode/header (sample paths may hold spaces)
        size_t j = eq + 1, end = eq + 1;
        while (j < t.size() && t[j] != '\n' && t[j] != '\r' && t[j] != '<') {
            if (std::isspace((unsigned char)t[j]) && isOpcodeStart(t, j + 1)) break;
            ++j;
            end = j;
        }
        std::string val = t.substr(eq + 1, end - eq - 1);
        while (!val.empty() && std::isspace((unsigned char)val.back())) val.pop_back();
        val.erase(0, val.find_first_not_of(" \t"));
        i = j;
        std::map<std::string, std::string> *target = nullptr;
        if (section == "control") target = &control;
        else if (section == "global") target = &global;
        else if (section == "master") target = &master;
        else if (section == "group") target = &group;
        else if (section == "region") target = &region;
        if (!target) continue;   // <curve>, <effect>, <midi>, <sample>: not used
        if (key == "key") { (*target)["lokey"] = (*target)["hikey"] = (*target)["pitch_keycenter"] = val; continue; }
        (*target)[key] = val;
    }
    flush();
    std::string dp = control.count("default_path") ? control["default_path"] : "";
    std::replace(dp.begin(), dp.end(), '\\', '/');
    out.dir = (root / dp).lexically_normal().string();
    out.noteOffset = std::atoi(control["note_offset"].c_str()) + 12 * std::atoi(control["octave_offset"].c_str());
    for (auto &[k, v] : control) {   // set_ccN=0..127, set_hdccN=0..1
        if (k.rfind("set_cc", 0) == 0) out.cc[std::atoi(k.c_str() + 6)] = std::atof(v.c_str());
        else if (k.rfind("set_hdcc", 0) == 0) out.cc[std::atoi(k.c_str() + 8)] = std::atof(v.c_str()) * 127;
    }
    if (out.regions.empty()) { err = file.filename().string() + " has no <region>"; return false; }
    return true;
}

} // namespace wl
