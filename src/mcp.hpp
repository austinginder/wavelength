#pragma once
// `wavelength mcp`: a Model Context Protocol server on stdin/stdout (JSON-RPC, one message per line),
// so Claude Desktop, Claude Code, Cursor and other MCP clients can drive Wavelength. Every tool runs
// this executable as a child process with --json: the same commands an agent runs in a shell, and a
// crashing plugin ends the child, never the server. Renders come back with their song picture.
#include <cstdio>

namespace wl {

int runMcp(std::FILE *out);

} // namespace wl
