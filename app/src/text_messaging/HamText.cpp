//=========================================================================
// Name:            HamText.cpp
// Purpose:         Huffman codes chat text with a fixed ham chat table.
//=========================================================================

#include "HamText.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "HamTextTable.h"

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

// Symbols: printable ASCII 0x20-0x7E, then the phrases, then ESC (a raw
// byte follows) and EMPTY (the rest of the block is padding). The table of
// phrases and code lengths is generated: see HamTextTable.h.
constexpr int PRINTABLE = 95;
constexpr int FIRST_PHRASE = PRINTABLE;
constexpr int ESC = SYMBOL_COUNT - 2;
constexpr int EMPTY = SYMBOL_COUNT - 1;
constexpr int LONGEST_CODE = 16; // bound on SYMBOL_BITS, for the decoder's arrays

// Canonical codes, complemented: the shortest code is all ones and the
// longest, EMPTY's, all zeros. So zero padding never completes a code until
// maxBits of it, and then it reads as EMPTY.
struct Table
{
    std::array<uint16_t, SYMBOL_COUNT> code{};
    int maxBits = 0;
    // For decoding: per length, the first canonical (uncomplemented) code,
    // how many codes have that length, and where they start in `sorted`.
    std::array<int, LONGEST_CODE + 2> first{};
    std::array<int, LONGEST_CODE + 2> count{};
    std::array<int, LONGEST_CODE + 2> offset{};
    std::array<int, SYMBOL_COUNT> sorted{};
    // Phrase indices, longest phrase first, and each phrase's length.
    std::array<int, PHRASE_COUNT> byLength{};
    std::array<size_t, PHRASE_COUNT> phraseLength{};

    Table()
    {
        for (int s = 0; s < SYMBOL_COUNT; s++)
        {
            count[SYMBOL_BITS[s]]++;
            maxBits = std::max(maxBits, (int)SYMBOL_BITS[s]);
        }
        int at = 0;
        for (int len = 1; len <= maxBits; len++)
        {
            offset[(size_t)len] = at;
            for (int s = 0; s < SYMBOL_COUNT; s++)
            {
                if (SYMBOL_BITS[s] == len) sorted[(size_t)at++] = s;
            }
        }
        int next = 0;
        for (int len = 1; len <= maxBits; len++)
        {
            next = (next + (len > 1 ? count[(size_t)len - 1] : 0)) << (len > 1 ? 1 : 0);
            first[(size_t)len] = next;
            for (int i = 0; i < count[(size_t)len]; i++)
            {
                int s = sorted[(size_t)(offset[(size_t)len] + i)];
                code[(size_t)s] = (uint16_t)(~(next + i) & ((1 << len) - 1));
            }
        }

        for (int k = 0; k < PHRASE_COUNT; k++)
        {
            byLength[(size_t)k] = k;
            phraseLength[(size_t)k] = std::strlen(PHRASES[k]);
        }
        std::stable_sort(byLength.begin(), byLength.end(),
                         [this](int a, int b) { return phraseLength[(size_t)a] > phraseLength[(size_t)b]; });
    }

    // The phrase that starts at text[i], longest first, or -1.
    int phraseAt(const std::string& text, size_t i) const
    {
        for (int k : byLength)
        {
            if (text.compare(i, phraseLength[(size_t)k], PHRASES[k]) == 0) return k;
        }
        return -1;
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
    return SYMBOL_BITS[s] + (s == ESC ? 8 : 0);
}

std::vector<PhraseSpan> phraseSpans(const std::string& text)
{
    const Table& t = table();
    std::vector<PhraseSpan> spans;
    for (size_t i = 0; i < text.size();)
    {
        int k = t.phraseAt(text, i);
        if (k < 0)
        {
            i++;
            continue;
        }
        spans.push_back({i, t.phraseLength[(size_t)k]});
        i += t.phraseLength[(size_t)k];
    }
    return spans;
}

size_t encode(const std::string& text, size_t from, uint8_t* frame, int startBit, int endBit)
{
    const Table& t = table();
    int bit = startBit;
    size_t i = from;
    while (i < text.size())
    {
        // The longest phrase that starts here, else the character.
        unsigned char c = (unsigned char)text[i];
        int phrase = t.phraseAt(text, i);
        int s = phrase >= 0 ? FIRST_PHRASE + phrase : symbolOf(c);
        int bits = SYMBOL_BITS[s] + (s == ESC ? 8 : 0);
        // A code that would cross into the next block starts there instead.
        // A phrase that does not fit is left whole for the next frame.
        if (bit + bits > blockEnd(bit, endBit)) bit = (bit / TEXT_BLOCK_BITS + 1) * TEXT_BLOCK_BITS;
        if (bit + bits > endBit) break;
        if (frame != nullptr)
        {
            putBits(frame, bit, t.code[(size_t)s], SYMBOL_BITS[s]);
            if (s == ESC) putBits(frame, bit + SYMBOL_BITS[s], c, 8);
        }
        bit += bits;
        i += phrase >= 0 ? t.phraseLength[(size_t)phrase] : 1;
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
        for (int len = 1; len <= t.maxBits && at < end; len++)
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
        else if (symbol >= FIRST_PHRASE)
        {
            text += PHRASES[symbol - FIRST_PHRASE];
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
