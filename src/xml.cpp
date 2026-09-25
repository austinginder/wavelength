#include "xml.hpp"

#include <cstdlib>

namespace wl::xml {

const std::string *Node::attr(const std::string &name) const {
    for (auto &a : attrs) if (a.first == name) return &a.second;
    return nullptr;
}
std::string Node::get(const std::string &name, const std::string &def) const { auto *a = attr(name); return a ? *a : def; }
double Node::num(const std::string &name, double def) const {
    auto *a = attr(name);
    if (!a || a->empty()) return def;
    char *end = nullptr;
    const double v = std::strtod(a->c_str(), &end);
    return end == a->c_str() ? def : v;
}
const Node *Node::child(const std::string &t) const {
    for (auto &c : children) if (c->tag == t) return c.get();
    return nullptr;
}
std::vector<const Node *> Node::all(const std::string &t) const {
    std::vector<const Node *> out;
    for (auto &c : children) if (c->tag == t) out.push_back(c.get());
    return out;
}
void Node::walk(const std::string &t, std::vector<const Node *> &out) const {
    for (auto &c : children) {
        if (c->tag == t) out.push_back(c.get());
        c->walk(t, out);
    }
}

namespace {

std::string decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos) { out += s[i]; continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            const long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::strtol(ent.c_str() + 2, nullptr, 16) : std::strtol(ent.c_str() + 1, nullptr, 10);
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        } else { out += s.substr(i, semi - i + 1); }
        i = semi;
    }
    return out;
}

struct Parser {
    const std::string &s;
    size_t i = 0;
    std::string err;
    bool skipMisc() {   // whitespace, text, comments, <?..?>, <!DOCTYPE ..>, CDATA
        for (;;) {
            while (i < s.size() && s[i] != '<') ++i;
            if (i >= s.size()) return false;
            if (s.compare(i, 4, "<!--") == 0) { size_t e = s.find("-->", i); if (e == std::string::npos) return false; i = e + 3; continue; }
            if (s.compare(i, 9, "<![CDATA[") == 0) { size_t e = s.find("]]>", i); if (e == std::string::npos) return false; i = e + 3; continue; }
            if (s.compare(i, 2, "<?") == 0 || s.compare(i, 2, "<!") == 0) { size_t e = s.find('>', i); if (e == std::string::npos) return false; i = e + 1; continue; }
            return true;
        }
    }
    static bool nameChar(char c) { return !(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/' || c == '>' || c == '=' || c == '"' || c == '\''); }
    std::unique_ptr<Node> element() {
        // at '<'
        ++i;
        auto node = std::make_unique<Node>();
        while (i < s.size() && nameChar(s[i])) node->tag += s[i++];
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
            if (i >= s.size()) { err = "unexpected end inside <" + node->tag + ">"; return nullptr; }
            if (s[i] == '/') { if (i + 1 < s.size() && s[i + 1] == '>') { i += 2; return node; } err = "bad tag end"; return nullptr; }
            if (s[i] == '>') { ++i; break; }
            std::string name;
            while (i < s.size() && nameChar(s[i])) name += s[i++];
            while (i < s.size() && s[i] != '=' && s[i] != '>') ++i;
            if (i >= s.size() || s[i] != '=') { node->attrs.push_back({name, ""}); continue; }
            ++i;
            while (i < s.size() && s[i] != '"' && s[i] != '\'') ++i;
            if (i >= s.size()) { err = "unterminated attribute in <" + node->tag + ">"; return nullptr; }
            const char q = s[i++];
            const size_t e = s.find(q, i);
            if (e == std::string::npos) { err = "unterminated attribute in <" + node->tag + ">"; return nullptr; }
            node->attrs.push_back({name, decode(s.substr(i, e - i))});
            i = e + 1;
        }
        // children until </tag>
        for (;;) {
            if (!skipMisc()) { err = "unexpected end: <" + node->tag + "> is not closed"; return nullptr; }
            if (s.compare(i, 2, "</") == 0) {
                const size_t e = s.find('>', i);
                if (e == std::string::npos) { err = "bad closing tag"; return nullptr; }
                i = e + 1;
                return node;
            }
            auto c = element();
            if (!c) return nullptr;
            node->children.push_back(std::move(c));
        }
    }
};

} // namespace

std::unique_ptr<Node> parse(const std::string &text, std::string &err) {
    Parser p{text, 0, ""};
    if (!p.skipMisc()) { err = "no XML element found"; return nullptr; }
    auto root = p.element();
    if (!root) err = p.err;
    return root;
}

} // namespace wl::xml
