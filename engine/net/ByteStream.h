#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace iron {

// Append-only byte buffer for network serialization. Little-endian-agnostic in
// the sense that both ends run the same code; primitives are written via memcpy
// of their native representation (all our peers are same-arch x86-64 today).
class ByteWriter {
public:
    void u8(std::uint8_t v)  { put(&v, sizeof(v)); }
    void u16(std::uint16_t v){ put(&v, sizeof(v)); }
    void u32(std::uint32_t v){ put(&v, sizeof(v)); }
    void i32(std::int32_t v) { put(&v, sizeof(v)); }
    void f32(float v)        { put(&v, sizeof(v)); }
    void boolean(bool v)     { std::uint8_t b = v ? 1 : 0; put(&b, sizeof(b)); }
    void raw(std::span<const std::byte> bytes) {
        buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    }

    const std::vector<std::byte>& data() const { return buf_; }

private:
    void put(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<std::byte> buf_;
};

// Reads primitives back from a byte span with bounds checking. On underflow it
// sets a sticky failed() flag and returns zeroed values — callers check failed()
// and abort cleanly; never reads out of bounds.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes) : buf_(bytes) {}

    std::uint8_t  u8()  { std::uint8_t v{};  get(&v, sizeof(v)); return v; }
    std::uint16_t u16() { std::uint16_t v{}; get(&v, sizeof(v)); return v; }
    std::uint32_t u32() { std::uint32_t v{}; get(&v, sizeof(v)); return v; }
    std::int32_t  i32() { std::int32_t v{};  get(&v, sizeof(v)); return v; }
    float         f32() { float v{};         get(&v, sizeof(v)); return v; }
    bool          boolean() { std::uint8_t b{}; get(&b, sizeof(b)); return b != 0; }

    std::size_t remaining() const { return failed_ ? 0 : buf_.size() - pos_; }
    bool failed() const { return failed_; }

private:
    void get(void* p, std::size_t n) {
        if (failed_ || pos_ + n > buf_.size()) { failed_ = true; std::memset(p, 0, n); return; }
        std::memcpy(p, buf_.data() + pos_, n);
        pos_ += n;
    }
    std::span<const std::byte> buf_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

// Default (de)serialization for trivially-copyable types — POD command/value
// structs get serialization for free. Non-POD types (e.g. Inventory) provide
// their own serialize/deserialize overloads, which win via overload resolution
// (non-template exact match beats this template; and SFINAE removes this for
// non-trivially-copyable types anyway).
template <typename T>
std::enable_if_t<std::is_trivially_copyable_v<T>>
serialize(ByteWriter& w, const T& v) {
    w.raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&v), sizeof(T)));
}

template <typename T>
std::enable_if_t<std::is_trivially_copyable_v<T>>
deserialize(ByteReader& r, T& v) {
    // Read sizeof(T) bytes (bounds-checked) directly into v.
    std::byte tmp[sizeof(T)];
    for (std::size_t i = 0; i < sizeof(T); ++i) tmp[i] = std::byte{r.u8()};
    if (!r.failed()) std::memcpy(&v, tmp, sizeof(T));
}

}  // namespace iron
