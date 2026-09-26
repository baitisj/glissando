//=========================================================================
// Name:            GlissandoFec.cpp
// Purpose:         Forward error correction for Glissando frames, ported from
//                  prototype/fec.py.
//=========================================================================

#include "GlissandoFec.h"

namespace Glissando
{

namespace
{

constexpr uint16_t CRC14_POLY = 0x2757;     // the polynomial FT8 uses
constexpr int NUM_STATES = 1 << (CONSTRAINT_LENGTH - 1);
constexpr int GENERATOR[2] = {0133, 0171};  // octal, as in the prototype

// numpy.random.default_rng(0x61155A).permutation(195), i.e. fec.INTERLEAVE.
// Coded bit INTERLEAVE[i] goes out as frame bit i.
constexpr int INTERLEAVE[FRAME_BITS] = {
    113, 126, 137, 192, 187, 175, 86, 88, 11, 34, 121, 128, 82, 152, 76, 143, 97, 133, 124, 8,
    56, 25, 106, 131, 193, 130, 20, 149, 156, 83, 176, 84, 171, 118, 100, 37, 114, 15, 123, 32,
    18, 54, 3, 49, 163, 138, 120, 135, 63, 134, 9, 58, 28, 99, 79, 2, 164, 67, 95, 89,
    40, 30, 94, 45, 50, 14, 31, 91, 141, 38, 96, 186, 110, 70, 170, 182, 19, 146, 47, 61,
    17, 26, 12, 60, 29, 148, 165, 35, 172, 117, 103, 159, 194, 101, 16, 183, 157, 179, 166, 64,
    144, 5, 23, 44, 27, 180, 154, 52, 57, 77, 174, 24, 4, 181, 177, 7, 111, 104, 139, 92,
    68, 80, 189, 0, 145, 87, 107, 105, 115, 178, 36, 155, 158, 191, 42, 136, 10, 65, 39, 55,
    129, 109, 140, 59, 6, 132, 147, 93, 48, 162, 173, 142, 21, 41, 53, 85, 161, 184, 150, 81,
    160, 190, 43, 167, 71, 62, 169, 125, 102, 122, 33, 78, 13, 98, 108, 127, 112, 153, 75, 51,
    66, 151, 73, 185, 119, 22, 69, 72, 168, 116, 74, 46, 1, 188, 90,
};

int parity(unsigned x)
{
    int p = 0;
    while (x != 0)
    {
        p ^= (int)(x & 1);
        x >>= 1;
    }
    return p;
}

// The two coded bits for input u from state s. The state is the last K-1
// input bits with the newest in the LSB, so the K bit register is (s << 1) | u.
void encoderOutput(int state, int input, int out[2])
{
    unsigned reg = ((unsigned)state << 1) | (unsigned)input;
    out[0] = parity(reg & (unsigned)GENERATOR[0]);
    out[1] = parity(reg & (unsigned)GENERATOR[1]);
}

int nextState(int state, int input)
{
    return ((state << 1) | input) & (NUM_STATES - 1);
}

} // namespace

uint16_t crc14(const uint8_t* bits, int numBits)
{
    // Long division by the generator, with CRC_BITS zeros appended (fec.crc14).
    uint32_t reg = 0;
    for (int i = 0; i < numBits + CRC_BITS; i++)
    {
        uint32_t bit = i < numBits ? (bits[i] & 1u) : 0u;
        reg = (reg << 1) | bit;
        if (reg & (1u << CRC_BITS)) reg ^= CRC14_POLY | (1u << CRC_BITS);
    }
    return (uint16_t)reg;
}

CodedFrame encodeFrame(const Payload& payload)
{
    uint8_t info[INFO_BITS];
    for (int i = 0; i < PAYLOAD_BITS; i++) info[i] = payload[i] & 1;
    uint16_t crc = crc14(info, PAYLOAD_BITS);
    for (int i = 0; i < CRC_BITS; i++) info[PAYLOAD_BITS + i] = (uint8_t)((crc >> (CRC_BITS - 1 - i)) & 1);

    // Convolutional code; the tail flushes the encoder back to state 0, and
    // the last of the 195 bits is a zero pad.
    uint8_t coded[FRAME_BITS] = {};
    int state = 0;
    for (int i = 0; i < INFO_BITS + TAIL_BITS; i++)
    {
        int input = i < INFO_BITS ? info[i] : 0;
        int out[2];
        encoderOutput(state, input, out);
        coded[2 * i] = (uint8_t)out[0];
        coded[2 * i + 1] = (uint8_t)out[1];
        state = nextState(state, input);
    }

    CodedFrame frame;
    for (int i = 0; i < FRAME_BITS; i++) frame[i] = coded[INTERLEAVE[i]];
    return frame;
}

bool decodeFrame(const FrameLlrs& llrs, Payload& payloadOut)
{
    double deinterleaved[FRAME_BITS];
    for (int i = 0; i < FRAME_BITS; i++) deinterleaved[INTERLEAVE[i]] = llrs[i];

    // Predecessors of each state. State ns is reached with input u = ns & 1
    // from ns >> 1 and from (ns >> 1) | 32, listed in that order, the order
    // the prototype's table has them (it matters only for exact ties).
    int prevState[NUM_STATES][2];
    int prevSign[NUM_STATES][2][2]; // +1 for a coded 0, -1 for a coded 1
    for (int ns = 0; ns < NUM_STATES; ns++)
    {
        for (int p = 0; p < 2; p++)
        {
            int s = (ns >> 1) | (p ? NUM_STATES / 2 : 0);
            int out[2];
            encoderOutput(s, ns & 1, out);
            prevState[ns][p] = s;
            prevSign[ns][p][0] = 1 - 2 * out[0];
            prevSign[ns][p][1] = 1 - 2 * out[1];
        }
    }

    // Soft Viterbi maximising the correlation between LLRs and +/-1 code bits.
    constexpr int STEPS = CODED_BITS / 2; // 97
    double metric[NUM_STATES];
    double nextMetric[NUM_STATES];
    uint8_t decisions[STEPS][NUM_STATES];
    for (int s = 0; s < NUM_STATES; s++) metric[s] = -1e18;
    metric[0] = 0.0;

    for (int t = 0; t < STEPS; t++)
    {
        double l0 = deinterleaved[2 * t];
        double l1 = deinterleaved[2 * t + 1];
        for (int ns = 0; ns < NUM_STATES; ns++)
        {
            double c0 = metric[prevState[ns][0]] + 0.5 * (prevSign[ns][0][0] * l0 + prevSign[ns][0][1] * l1);
            double c1 = metric[prevState[ns][1]] + 0.5 * (prevSign[ns][1][0] * l0 + prevSign[ns][1][1] * l1);
            bool pickSecond = c1 > c0;
            nextMetric[ns] = pickSecond ? c1 : c0;
            decisions[t][ns] = pickSecond ? 1 : 0;
        }
        for (int s = 0; s < NUM_STATES; s++) metric[s] = nextMetric[s];
    }

    // The tail forces the zero state; trace back from there.
    uint8_t bits[STEPS];
    int state = 0;
    for (int t = STEPS - 1; t >= 0; t--)
    {
        bits[t] = (uint8_t)(state & 1);
        state = prevState[state][decisions[t][state]];
    }

    for (int i = 0; i < PAYLOAD_BITS; i++) payloadOut[i] = bits[i];
    uint16_t crc = 0;
    for (int i = 0; i < CRC_BITS; i++) crc = (uint16_t)((crc << 1) | bits[PAYLOAD_BITS + i]);
    return crc14(bits, PAYLOAD_BITS) == crc;
}

} // namespace Glissando
