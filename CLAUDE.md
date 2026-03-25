# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

C++ MCP server implementing a semantic memory system ("c-mem"). Provides persistent memory with vector-based semantic search via Ollama embeddings and SQLite with sqlite-vec. Communicates over stdin/stdout using JSON-RPC (MCP protocol version `2024-11-05`).

## Build

```bash
cmake -B build
cmake --build build
```

Output binary: `build/c-mem`

Requires system packages: `nlohmann_json`, `libcurl`, `sqlite3`. C++20, CMake 3.20+.

## Runtime Dependencies

- **Ollama** running locally (default `http://localhost:11434`, override with `CMEM_OLLAMA_URL`)
  - Uses `nomic-embed-text` model (768-dim embeddings)
- **sqlite-vec** extension (override path with `CMEM_VEC_EXT`, default: `vec0`)
- `CMEM_DB` — path for project-scoped database (optional; global db at `~/.local/share/c-mem/global.db`)

## Architecture

Three layers, all in the root directory:

1. **MCP Protocol** (`mcp_server.h/cc`, namespace `mcp`) — Generic JSON-RPC server reading from stdin, writing to stdout. Handles `initialize`, `tools/list`, `tools/call`. Reusable for any MCP tool server.

2. **Memory Logic** (`c_mem.h/cc`, namespace `cmem`) — Tool implementations: store, search, forget, list, update, check_sensitive. Handles embedding via Ollama HTTP API, sensitive data detection/redaction via regex, and dual-scope (global/project) database routing.

3. **Entry Point** (`main.cc`) — Wires CMem tools into the MCP Server and runs.

### Key Design Decisions

- Dual database: `globalDb_` (always available) and `projectDb_` (only when `CMEM_DB` is set). Scope parameter on each tool selects which DB to use.
- Sensitive data is auto-detected before storing; storage is blocked unless `force=true`.
- Embeddings are serialized as raw `float[768]` binary blobs for sqlite-vec.
- Search with tag filtering over-fetches by 3x then filters in application code.

## No Tests

There is currently no test framework or test suite.
