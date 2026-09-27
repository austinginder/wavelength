#include "docs.hpp"

#include <algorithm>
#include <sstream>

namespace wl {

struct DocAsset { const char *name; const char *text; size_t size; };
extern const DocAsset kDocs[];
extern const size_t kDocCount;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// "## Mixing" -> level 2, "Mixing"; 0 when the line is not a heading (fenced code is skipped by the caller)
int heading(const std::string &line, std::string &text) {
    size_t n = 0;
    while (n < line.size() && line[n] == '#') ++n;
    if (n == 0 || n > 6 || n >= line.size() || line[n] != ' ') return 0;
    text = line.substr(n + 1);
    return (int)n;
}

std::vector<std::string> lines(const DocAsset &d) {
    std::vector<std::string> out;
    std::istringstream in(std::string(d.text, d.size));
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

} // namespace

std::vector<DocInfo> listDocs() {
    std::vector<DocInfo> out;
    for (size_t i = 0; i < kDocCount; ++i) {
        DocInfo d{kDocs[i].name, "", kDocs[i].size, {}};
        bool fence = false;
        for (const auto &l : lines(kDocs[i])) {
            if (l.rfind("```", 0) == 0) fence = !fence;
            std::string t;
            const int lv = fence ? 0 : heading(l, t);
            if (lv == 1 && d.title.empty()) d.title = t;
            else if (lv == 2) d.headings.push_back(t);
        }
        out.push_back(d);
    }
    return out;
}

bool docText(const std::string &name, const std::string &section, std::string &out, std::string &err) {
    const DocAsset *doc = nullptr;
    std::string names;
    for (size_t i = 0; i < kDocCount; ++i) {
        if (name == kDocs[i].name) doc = &kDocs[i];
        names += (names.empty() ? "" : ", ") + std::string(kDocs[i].name);
    }
    if (!doc) { err = "no doc '" + name + "' (docs: " + names + ")"; return false; }
    if (section.empty()) { out.assign(doc->text, doc->size); return true; }
    const auto ls = lines(*doc);
    const std::string want = lower(section);
    bool fence = false;
    int level = 0;
    std::string text;
    std::vector<std::string> found;
    for (const auto &l : ls) {
        if (l.rfind("```", 0) == 0) fence = !fence;
        std::string t;
        const int lv = fence ? 0 : heading(l, t);
        if (level == 0) {
            if (lv > 0 && lower(t).find(want) != std::string::npos) { level = lv; text = l + "\n"; }
            continue;
        }
        if (lv > 0 && lv <= level) break;
        text += l + "\n";
    }
    if (level == 0) {
        std::string heads;
        for (auto &d : listDocs())
            if (d.name == name) for (auto &h : d.headings) heads += (heads.empty() ? "" : "; ") + h;
        err = "no section matching '" + section + "' in " + name + " (sections: " + heads + ")";
        return false;
    }
    out = text;
    return true;
}

} // namespace wl
