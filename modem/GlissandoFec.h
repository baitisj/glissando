//=========================================================================
// Name:            GlissandoFec.h
// Purpose:         Forward error correction for Glissando frames, ported from
//                  prototype/fec.py.
//
// Frame: 77 bit payload (the size of an FT8/FT4 message) + CRC-14 = 91 bits,
// then a K=7, rate 1/2 convolutional code (the NASA/Voyager 0o133/0o171
// pair) with 6 tail bits = 194 coded bits, padded to 195 (65 notes of 3
// bits) and bit interleaved across the whole frame.
//
// The prototype's design notes call for FT8's LDPC(174,91) in the end; the
// convolutional code is what the prototype measured, so it is what is here.
//=========================================================================

#ifndef GLISSANDO__GLISSANDO_FEC_H
#define GLISSANDO__GLISSANDO_FEC_H

#include <array>
#include <cstdint>

#include "GlissandoModem.h"

namespace Glissando
{

constexpr int CRC_BITS = 14;
constexpr int INFO_BITS = PAYLOAD_BITS + CRC_BITS;              // 91
constexpr int CONSTRAINT_LENGTH = 7;
constexpr int TAIL_BITS = CONSTRAINT_LENGTH - 1;                // 6
constexpr int CODED_BITS = 2 * (INFO_BITS + TAIL_BITS);         // 194
constexpr int FRAME_BITS = 195;                                 // 65 symbols x 3 bits

using CodedFrame = std::array<uint8_t, FRAME_BITS>;
using FrameLlrs = std::array<float, FRAME_BITS>;

// CRC-14 with FT8's polynomial, over one bit per element, first bit first.
// Returns the 14 CRC bits packed MSB first (bit 13 is the first CRC bit).
uint16_t crc14(const uint8_t* bits, int numBits);

// Payload -> CRC -> convolutional code -> pad -> interleave (fec.encode_frame).
CodedFrame encodeFrame(const Payload& payload);

// Soft decoding (fec.decode_frame). llrs are in the order encodeFrame()
// emits bits, positive meaning 0 is the more likely bit. Returns true when
// the CRC matches; the payload is filled in either way.
bool decodeFrame(const FrameLlrs& llrs, Payload& payloadOut);

} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_FEC_H
