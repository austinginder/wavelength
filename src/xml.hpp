#pragma once
// A small XML reader for project and score files (DAWproject, MusicXML): elements, attributes and
// text content, entities decoded; comments, processing instructions and DOCTYPE are skipped.
#include <memory>
#include <string>
#include <vector>

namespace wl::xml {

struct Node {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<Node>> children;
    std::string text;   // the element's own text (CDATA included), trimmed; "" for whitespace only
    const std::string *attr(const std::string &name) const;
    std::string get(const std::string &name, const std::string &def = "") const;
    double num(const std::string &name, double def = 0) const;
    const Node *child(const std::string &tag) const;                     // first child with this tag
    std::vector<const Node *> all(const std::string &tag) const;         // direct children with this tag
    void walk(const std::string &tag, std::vector<const Node *> &out) const;   // every descendant with this tag
    std::string childText(const std::string &tag, const std::string &def = "") const;   // text of the first child with this tag
    double childNum(const std::string &tag, double def = 0) const;
};

// Parses a document; returns the root element or null with `err` set.
std::unique_ptr<Node> parse(const std::string &text, std::string &err);

} // namespace wl::xml
