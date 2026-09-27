#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// A little-endian byte writer for the Pip-Boy's snapshots (see PipDataKind in the protocol).
namespace vegas {
class Pack {
public:
    void u8(std::uint8_t v) { bytes_.push_back(v); }
    void u16(std::uint16_t v) { put(&v, 2); }
    void u32(std::uint32_t v) { put(&v, 4); }
    void i32(std::int32_t v) { put(&v, 4); }
    void f32(float v) { put(&v, 4); }
    // A string as a length and its Latin-1 bytes (what New Vegas holds); control characters other than
    // newline become spaces; null is empty.
    void str(const char* s, std::size_t max = 4000) {
        std::size_t n = 0;
        while (s && s[n] && n < max) ++n;
        u16(static_cast<std::uint16_t>(n));
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            u8(c == '\n' || c >= 32 ? c : ' ');
        }
    }
    void str(const std::string& s, std::size_t max = 4000) { str(s.c_str(), max); }
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    std::size_t size() const { return bytes_.size(); }
private:
    void put(const void* p, std::size_t n) { const auto* c = static_cast<const std::uint8_t*>(p); bytes_.insert(bytes_.end(), c, c + n); }
    std::vector<std::uint8_t> bytes_;
};
} // namespace vegas
