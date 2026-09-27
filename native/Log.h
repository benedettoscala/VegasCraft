#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>

namespace vegas::log {
// VegasCraft.log in the game directory. Thread-safe; every line is flushed.
// `append` keeps what is already there (the core after a hot reload); otherwise it starts empty.
bool open(const std::wstring& path, bool append = false);
void close();
void line(const char* format, ...) __attribute__((format(printf, 1, 2)));
// Logs at most once per `key` (diagnostics that would otherwise repeat every frame).
void once(const char* key, const char* format, ...) __attribute__((format(printf, 2, 3)));
FILE* file();
} // namespace vegas::log
