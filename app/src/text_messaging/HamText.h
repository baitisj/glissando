//=========================================================================
// Name:            HamText.h
// Purpose:         Huffman codes chat text with a fixed table drawn from
//                  ham chat, so common characters cost fewer bits.
//
// Written for Glissando (Jeff, 2026-10-04). Plain 8-bit text spent eight
// bits on every character. Measured on 94 typical chat lines, this table
// spends about five, and with the shorter header in FrameCodec a message
// takes about 38% fewer frames.
//
// Every frame decodes on its own: the table is fixed, so nothing a
// listener missed earlier is needed to read what it hears now. Codes are
// also kept inside TEXT_BLOCK_BITS blocks, counted from the start of the
// frame, which on Glissando are its 9-byte segments. A segment heard after
// one that was lost still reads, as plain bytes did.
//
// A few whole phrases, such as " the" and "CQ CQ", are symbols of their
// own (HamTextTable.h, generated from prototype/ham_table/phrases.txt).
// The encoder codes the longest phrase that starts at each character, and
// phraseSpans() finds the same ones, so the COMMS entry box can show which
// parts of a message ride as one symbol.
//
// Characters outside printable ASCII (accents, UTF-8) go as an escape code
// and the raw byte. The rarest code of all, all zero bits, means "the
// rest of this block is empty": it is what zero padding reads as, so the
// text needs no length field and no end marker.
//=========================================================================

#ifndef TEXT_MESSAGING__HAM_TEXT_H
#define TEXT_MESSAGING__HAM_TEXT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TextMessaging
{

// Writes count bits of value, most significant first, at bit position bit
// of bytes (bit 0 is the top bit of bytes[0]). The bits must be zero.
void putBits(uint8_t* bytes, int bit, uint64_t value, int count);

// Reads count (at most 64) bits at bit position bit.
uint64_t getBits(const uint8_t* bytes, int bit, int count);

namespace HamText
{

constexpr int TEXT_BLOCK_BITS = 72;

// How many bits a character costs, escape included.
int characterBits(unsigned char c);

// Where the encoder will code a phrase as one symbol: each phrase found
// scanning text from the start, longest match first, as encode() scans it.
struct PhraseSpan
{
    size_t start;
    size_t length;
};
std::vector<PhraseSpan> phraseSpans(const std::string& text);

// Codes text from character `from` into the bits [startBit, endBit) of a
// zeroed frame, as many characters as fit, keeping each code inside its
// block. Returns how many characters went in. With frame null, only counts.
size_t encode(const std::string& text, size_t from, uint8_t* frame, int startBit, int endBit);

// Reads the text in bits [startBit, endBit). startBit is where a frame's
// text begins, or the start of a block when the ones before it were lost.
std::string decode(const uint8_t* frame, int startBit, int endBit);

} // namespace HamText

} // namespace TextMessaging

#endif // TEXT_MESSAGING__HAM_TEXT_H
