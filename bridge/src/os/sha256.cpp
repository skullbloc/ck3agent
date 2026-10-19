// sha256.cpp — FIPS 180-4 SHA-256 (see os/sha256.h). Standard C++17 only.

#include "os/sha256.h"

#include <cstring>

namespace bridge {
namespace os {

namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256() { reset(); }

void Sha256::reset() {
    static const std::uint32_t kInit[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                           0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::memcpy(h_, kInit, sizeof(h_));
    fill_ = 0;
    total_ = 0;
}

void Sha256::block(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
               (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + S1 + ch + kK[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, std::size_t n) {
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    total_ += n;
    if (fill_) {
        const std::size_t take = (64 - fill_ < n) ? 64 - fill_ : n;
        std::memcpy(buf_ + fill_, p, take);
        fill_ += take; p += take; n -= take;
        if (fill_ == 64) { block(buf_); fill_ = 0; }
    }
    while (n >= 64) { block(p); p += 64; n -= 64; }
    if (n) { std::memcpy(buf_, p, n); fill_ = n; }
}

void Sha256::finish(std::uint8_t out[32]) {
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t pad = 0x80;
    update(&pad, 1);
    const std::uint8_t zero = 0;
    while (fill_ != 56) update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = std::uint8_t(bits >> (56 - 8 * i));
    update(len, 8);
    for (int i = 0; i < 8; ++i) {
        out[4 * i]     = std::uint8_t(h_[i] >> 24);
        out[4 * i + 1] = std::uint8_t(h_[i] >> 16);
        out[4 * i + 2] = std::uint8_t(h_[i] >> 8);
        out[4 * i + 3] = std::uint8_t(h_[i]);
    }
}

void hex_lower(const std::uint8_t* bytes, std::size_t n, char* out) {
    static const char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i]     = kHex[bytes[i] >> 4];
        out[2 * i + 1] = kHex[bytes[i] & 0xf];
    }
    out[2 * n] = '\0';
}

void sha256_hex(const void* data, std::size_t n, char out[65]) {
    Sha256 s;
    s.update(data, n);
    std::uint8_t d[32];
    s.finish(d);
    hex_lower(d, 32, out);
}

}  // namespace os
}  // namespace bridge
