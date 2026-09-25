#pragma once
// A small XML reader for project files (DAWproject): elements and attributes, entities decoded;
// text content, comments, processing instructions and DOCTYPE are skipped.
#include <memory>
#include <string>
#include <vector>

namespace wl::xml {

struct Node {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<Node>> children;
    const std::string *attr(const std::string &name) const;
    std::string get(const std::string &name, const std::string &def = "") const;
    double num(const std::string &name, double def = 0) const;
    const Node *child(const std::string &tag) const;                     // first child with this tag
    std::vector<const Node *> all(const std::string &tag) const;         // direct children with this tag
    void walk(const std::string &tag, std::vector<const Node *> &out) const;   // every descendant with this tag
};

// Parses a document; returns the root element or null with `err` set.
std::unique_ptr<Node> parse(const std::string &text, std::string &err);

} // namespace wl::xml
