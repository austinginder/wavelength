#pragma once
// The agent docs built into the binary (AGENTS.md, docs/job-format.md, docs/effects.md), so an
// agent with only the executable, or connected over MCP, reads the guide that matches it.
#include <string>
#include <vector>

namespace wl {

struct DocInfo {
    std::string name, title;   // "agents", "Wavelength, operating guide for AI agents"
    size_t size;
    std::vector<std::string> headings;   // its "##" sections
};
std::vector<DocInfo> listDocs();

// The whole doc, or one section: the first heading containing `section` (case-insensitive) down to
// the next heading of the same or a higher level. False with `err` when there is no such doc or section.
bool docText(const std::string &name, const std::string &section, std::string &out, std::string &err);

} // namespace wl
