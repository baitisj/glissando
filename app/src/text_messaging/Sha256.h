//=========================================================================
// Name:            Sha256.h
// Purpose:         SHA-256 (FIPS 180-4), for checking a file broadcast to
//                  the GLISS group arrived whole, and for naming it. Fed in
//                  pieces or all at once.
//
// Written for Glissando from FIPS 180-4; no Data2G code is used (see
// docs/DATA2G.md).
//=========================================================================

#ifndef TEXT_MESSAGING__SHA256_H
#define TEXT_MESSAGING__SHA256_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TextMessaging
{

class Sha256
{
public:
    using Digest = std::array<uint8_t, 32>;

    Sha256();

    void update(const uint8_t* bytes, size_t length);
    void update(const std::vector<uint8_t>& bytes) { update(bytes.data(), bytes.size()); }

    // The digest of everything fed in; the hash starts again afterwards.
    Digest finish();

    static Digest of(const uint8_t* bytes, size_t length);
    static Digest of(const std::vector<uint8_t>& bytes) { return of(bytes.data(), bytes.size()); }

    // Lower-case hex, for tests and logs.
    static std::string hex(const Digest& digest);

private:
    void reset();
    void block(const uint8_t* bytes);

    uint32_t state_[8];
    uint8_t buffer_[64];
    size_t buffered_;
    uint64_t length_;   // bytes fed in
};

} // namespace TextMessaging

#endif // TEXT_MESSAGING__SHA256_H
