#pragma once
// os/sha256.h — FIPS 180-4 SHA-256, portable C++17, no allocation.
//
// The image-file hash witness (ADR 0001 amendments, "A second identity
// witness, everywhere": observed_image_sha256 beside the format identity).
// The bridge has no crypto dependency and must not grow one for a file hash,
// so this is the textbook algorithm; test_os_sha256 pins it to the FIPS 180-2
// / NIST CAVS vectors on every OS the layer builds on.

#include <cstddef>
#include <cstdint>

namespace bridge {
namespace os {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t n);
    // Writes 32 bytes. The object is spent afterwards (call reset() to reuse).
    void finish(std::uint8_t out[32]);
    void reset();

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[8];
    std::uint8_t  buf_[64];
    std::size_t   fill_;
    std::uint64_t total_;
};

// 64 lowercase hex digits + NUL into out (65 bytes).
void sha256_hex(const void* data, std::size_t n, char out[65]);
void hex_lower(const std::uint8_t* bytes, std::size_t n, char* out);  // 2n digits + NUL

}  // namespace os
}  // namespace bridge
