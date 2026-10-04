//=========================================================================
// Name:            HamText.cpp
// Purpose:         Huffman codes chat text with a fixed ham chat table.
//=========================================================================

#include "HamText.h"

#include <algorithm>
#include <array>

namespace TextMessaging
{

void putBits(uint8_t* bytes, int bit, uint64_t value, int count)
{
    for (int i = 0; i < count; i++)
    {
        if ((value >> (count - 1 - i)) & 1)
        {
            int at = bit + i;
            bytes[at / 8] |= (uint8_t)(0x80 >> (at % 8));
        }
    }
}

uint64_t getBits(const uint8_t* bytes, int bit, int count)
{
    uint64_t value = 0;
    for (int i = 0; i < count; i++)
    {
        int at = bit + i;
        value = (value << 1) | (uint64_t)((bytes[at / 8] >> (7 - at % 8)) & 1);
    }
    return value;
}

namespace HamText
{

namespace
{

// Symbols: printable ASCII 0x20-0x7E, then ESC (a raw byte follows) and
// EMPTY (the rest of the block is padding).
constexpr int PRINTABLE = 95;
constexpr int ESC = PRINTABLE;
constexpr int EMPTY = PRINTABLE + 1;
constexpr int SYMBOLS = PRINTABLE + 2;
constexpr int MAX_BITS = 14;

// Code lengths, from character counts in typical ham chat (70%) blended
// with the prose of Glissando's own documents (30%), so ordinary English
// is not penalised for being missing from a small sample. Built by
// prototype/ham_table/gen_table.py; changing it is a protocol change, since
// both ends must hold the same table. Space costs three bits;
// e, t, a, o, i, n four; capitals and digits seven or eight.
constexpr std::array<uint8_t, SYMBOLS> LENGTHS = {
     3,  9, 12, 14, 13, 11, 13,  9, 10, 10, 13, 12,  6,  8,  7, 10, //  !"#$%&'()*+,-./
     7,  7,  8,  8,  8,  8, 10,  7,  8,  8,  9, 12, 13, 13, 13,  8, // 0123456789:;<=>?
    13,  7,  8,  7,  9,  8,  9,  7,  8,  8,  9,  9,  8,  8,  8,  8, // @ABCDEFGHIJKLMNO
     8,  8,  7,  7,  7,  9, 10,  7,  9,  8, 10, 13, 13, 13, 13, 13, // PQRSTUVWXYZ[\]^_
    13,  4,  7,  6,  5,  4,  6,  6,  5,  4, 12,  7,  5,  6,  4,  4, // `abcdefghijklmno
     6,  9,  5,  5,  4,  6,  8,  7, 10,  6, 10, 13, 13, 13, 13, 12, // pqrstuvwxyz{|}~ ESC
    14,                                                             // EMPTY
};

// Canonical codes, complemented: the shortest code is all ones and the
// longest, EMPTY's, all zeros. So zero padding never completes a code until
// MAX_BITS of it, and then it reads as EMPTY.
struct Table
{
    std::array<uint16_t, SYMBOLS> code{};
    // For decoding: per length, the first canonical (uncomplemented) code,
    // how many codes have that length, and where they start in `sorted`.
    std::array<int, MAX_BITS + 2> first{};
    std::array<int, MAX_BITS + 2> count{};
    std::array<int, MAX_BITS + 2> offset{};
    std::array<int, SYMBOLS> sorted{};

    Table()
    {
        for (int s = 0; s < SYMBOLS; s++) count[LENGTHS[(size_t)s]]++;
        int at = 0;
        for (int len = 1; len <= MAX_BITS; len++)
        {
            offset[(size_t)len] = at;
            for (int s = 0; s < SYMBOLS; s++)
            {
                if (LENGTHS[(size_t)s] == len) sorted[(size_t)at++] = s;
            }
        }
        int next = 0;
        for (int len = 1; len <= MAX_BITS; len++)
        {
            next = (next + (len > 1 ? count[(size_t)len - 1] : 0)) << (len > 1 ? 1 : 0);
            first[(size_t)len] = next;
            for (int i = 0; i < count[(size_t)len]; i++)
            {
                int s = sorted[(size_t)(offset[(size_t)len] + i)];
                code[(size_t)s] = (uint16_t)(~(next + i) & ((1 << len) - 1));
            }
        }
    }
};

const Table& table()
{
    static const Table t;
    return t;
}

int symbolOf(unsigned char c)
{
    return c >= 0x20 && c < 0x7F ? c - 0x20 : ESC;
}

int blockEnd(int bit, int endBit)
{
    return std::min((bit / TEXT_BLOCK_BITS + 1) * TEXT_BLOCK_BITS, endBit);
}

} // namespace

int characterBits(unsigned char c)
{
    int s = symbolOf(c);
    return LENGTHS[(size_t)s] + (s == ESC ? 8 : 0);
}

size_t encode(const std::string& text, size_t from, uint8_t* frame, int startBit, int endBit)
{
    const Table& t = table();
    int bit = startBit;
    size_t i = from;
    for (; i < text.size(); i++)
    {
        unsigned char c = (unsigned char)text[i];
        int bits = characterBits(c);
        // A code that would cross into the next block starts there instead.
        if (bit + bits > blockEnd(bit, endBit)) bit = (bit / TEXT_BLOCK_BITS + 1) * TEXT_BLOCK_BITS;
        if (bit + bits > endBit) break;
        if (frame != nullptr)
        {
            int s = symbolOf(c);
            putBits(frame, bit, t.code[(size_t)s], LENGTHS[(size_t)s]);
            if (s == ESC) putBits(frame, bit + LENGTHS[(size_t)s], c, 8);
        }
        bit += bits;
    }
    return i - from;
}

std::string decode(const uint8_t* frame, int startBit, int endBit)
{
    const Table& t = table();
    std::string text;
    int bit = startBit;
    while (bit < endBit)
    {
        int end = blockEnd(bit, endBit);
        int value = 0;
        int symbol = -1;
        int at = bit;
        for (int len = 1; len <= MAX_BITS && at < end; len++)
        {
            // Codes are sent complemented.
            value = (value << 1) | (int)(1 - getBits(frame, at++, 1));
            int index = value - t.first[(size_t)len];
            if (index >= 0 && index < t.count[(size_t)len])
            {
                symbol = t.sorted[(size_t)(t.offset[(size_t)len] + index)];
                break;
            }
        }

        if (symbol < 0 || symbol == EMPTY || (symbol == ESC && at + 8 > end))
        {
            bit = end; // padding to the end of the block
            continue;
        }
        if (symbol == ESC)
        {
            text.push_back((char)getBits(frame, at, 8));
            at += 8;
        }
        else
        {
            text.push_back((char)(0x20 + symbol));
        }
        bit = at;
    }
    return text;
}

} // namespace HamText

} // namespace TextMessaging
