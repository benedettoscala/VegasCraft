#include "Log.h"
#include <windows.h>
#include <mutex>
#include <set>

namespace vegas::log {
namespace {
FILE* out = nullptr;
std::mutex lock;
std::set<std::string> seen;
void write(const char* format, va_list args) {
    if (!out) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(out, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::vfprintf(out, format, args);
    std::fputc('\n', out);
    std::fflush(out);
}
}
bool open(const std::wstring& path, bool append) {
    std::scoped_lock guard(lock);
    if (out) return true;
    // Always an appending handle: the plugin's loader and its core write the same file.
    if (!append) if (FILE* fresh = _wfopen(path.c_str(), L"w")) std::fclose(fresh);
    out = _wfopen(path.c_str(), L"a");
    return out != nullptr;
}
void close() {
    std::scoped_lock guard(lock);
    if (out) std::fclose(out);
    out = nullptr;
}
FILE* file() { return out; }
void line(const char* format, ...) {
    std::scoped_lock guard(lock);
    va_list args;
    va_start(args, format);
    write(format, args);
    va_end(args);
}
void once(const char* key, const char* format, ...) {
    std::scoped_lock guard(lock);
    if (!seen.insert(key).second) return;
    va_list args;
    va_start(args, format);
    write(format, args);
    va_end(args);
}
} // namespace vegas::log
