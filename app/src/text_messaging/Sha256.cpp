//=========================================================================
// Name:            Sha256.cpp
// Purpose:         SHA-256 as FIPS 180-4 sets it out.
//
// Written for Glissando from FIPS 180-4; no Data2G code is used (see
// docs/DATA2G.md).
//=========================================================================

#include "Sha256.h"

#include <algorithm>
#include <cstring>

namespace TextMessaging
{

namespace
{

// The first 32 bits of the fractional parts of the cube roots of the first
// 64 primes.
constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

} // namespace

Sha256::Sha256()
{
    reset();
}

void Sha256::reset()
{
    // The first 32 bits of the fractional parts of the square roots of the
    // first 8 primes.
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(state_, initial, sizeof(state_));
    buffered_ = 0;
    length_ = 0;
}

void Sha256::block(const uint8_t* bytes)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
    {
        w[i] = ((uint32_t)bytes[4 * i] << 24) | ((uint32_t)bytes[4 * i + 1] << 16) |
               ((uint32_t)bytes[4 * i + 2] << 8) | (uint32_t)bytes[4 * i + 3];
    }
    for (int i = 16; i < 64; i++)
    {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; i++)
    {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + choose + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const uint8_t* bytes, size_t length)
{
    length_ += length;
    while (length > 0)
    {
        size_t take = std::min<size_t>(64 - buffered_, length);
        std::memcpy(buffer_ + buffered_, bytes, take);
        buffered_ += take;
        bytes += take;
        length -= take;
        if (buffered_ == 64)
        {
            block(buffer_);
            buffered_ = 0;
        }
    }
}

Sha256::Digest Sha256::finish()
{
    // A one bit, zeros to 56 bytes into a block, then the length in bits.
    uint64_t bits = length_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (buffered_ != 56) update(&zero, 1);
    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; i++) lengthBytes[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(lengthBytes, 8);

    Digest digest;
    for (int i = 0; i < 8; i++)
    {
        digest[(size_t)(4 * i)] = (uint8_t)(state_[i] >> 24);
        digest[(size_t)(4 * i + 1)] = (uint8_t)(state_[i] >> 16);
        digest[(size_t)(4 * i + 2)] = (uint8_t)(state_[i] >> 8);
        digest[(size_t)(4 * i + 3)] = (uint8_t)state_[i];
    }
    reset();
    return digest;
}

Sha256::Digest Sha256::of(const uint8_t* bytes, size_t length)
{
    Sha256 hash;
    hash.update(bytes, length);
    return hash.finish();
}

std::string Sha256::hex(const Digest& digest)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (uint8_t b : digest)
    {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

} // namespace TextMessaging
