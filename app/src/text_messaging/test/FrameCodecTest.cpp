//=========================================================================
// Name:            FrameCodecTest.cpp
// Purpose:         Pins the text messaging on air frame format.
//
// Authors:         FreeDV text messaging contributors
// License:
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//=========================================================================

#include <cstdio>
#include <string>
#include <vector>

#include "../FrameCodec.h"
#include "../HamText.h"
#include "../HamTextTable.h"

using namespace TextMessaging;

namespace
{

int failures = 0;

void check(bool condition, const char* what, int line)
{
    if (!condition)
    {
        failures++;
        fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

Frame makeMessageFrame()
{
    Frame frame;
    frame.type = FrameType::Message;
    frame.destinationCrc = FrameCodec::callsignHash("VK3ABC");
    frame.originCallsign = "W1AW";
    frame.airId = 0x2EF;
    frame.fragmentIndex = 1;
    frame.fragmentCount = 3;
    const std::string text = "Hello from the chat window";
    frame.payload.assign(text.begin(), text.end());
    return frame;
}

Frame makePingFrame()
{
    Frame frame;
    frame.type = FrameType::Ping;
    frame.destinationCrc = FrameCodec::callsignHash("VK3ABC");
    frame.originCallsign = "W1AW";
    frame.airId = 0x234;
    return frame;
}

std::string text(const Frame& frame)
{
    return std::string(frame.payload.begin(), frame.payload.end());
}

// The last byte that is not padding, plus one: how much of a frame goes on
// the air once Glissando trims the padding.
int sentBytes(const std::vector<uint8_t>& frame)
{
    int n = (int)frame.size();
    while (n > 0 && frame[(size_t)n - 1] == 0) n--;
    return n;
}

// The wire format is a compatibility surface between stations running
// different builds, so the CRC, hash and packing vectors below are pinned
// rather than recomputed by the test.
void testCallsignEncoding()
{
    CHECK(FrameCodec::callsignCrc24("W1AW") == 0xFC3980u);
    CHECK(FrameCodec::callsignCrc24("VK3ABC") == 0xFD1092u);
    CHECK(FrameCodec::callsignCrc24("CQ") == 0x4A8CF3u);
    CHECK(FrameCodec::callsignHash("W1AW") == 0xFC398u);

    // Normalization has to agree on both ends or the hashes will not match.
    CHECK(FrameCodec::callsignHash("w1aw") == 0xFC398u);
    CHECK(FrameCodec::callsignHash(" W1AW ") == 0xFC398u);
    CHECK(FrameCodec::normalizeCallsign("vk3abc/p") == "VK3ABC/P");
    CHECK(FrameCodec::normalizeCallsign("!@#$") == "");

    // Standard callsigns pack as FT8 packs them; anything else goes long.
    for (const char* call : {"W1AW", "K1ABC", "AG7EW", "VK3ABC", "2E0ABC", "W1A", "KH6XY"})
        CHECK(FrameCodec::isStandardCallsign(call));
    for (const char* call : {"VK3ABC/P", "CQ", "K1", "ABCDEFGHIJKL", "W1ABCD", "K12"})
        CHECK(!FrameCodec::isStandardCallsign(call));

    // A ping from W1AW to VK3ABC, message 0x234: type 3, VK3ABC's hash,
    // W1AW packed as " W1AW " (0x606DE9), more-follows clear, the ID.
    const uint8_t expected[] = {0x3F, 0xD1, 0x09, 0x03, 0x03, 0x6F, 0x4A, 0x34};
    std::vector<uint8_t> ping = FrameCodec::encode(makePingFrame(), SIGNALLING_FRAME_BYTES);
    CHECK(ping.size() == (size_t)SIGNALLING_FRAME_BYTES);
    for (size_t i = 0; i < sizeof(expected); i++) CHECK(ping[i] == expected[i]);
    CHECK(sentBytes(ping) == (int)sizeof(expected));

    // Round trips, long callsigns truncated rather than corrupted.
    for (const char* call : {"W1AW", "2E0ABC", "VK3ABC/P", "K1ABC/MM"})
    {
        Frame frame = makePingFrame();
        frame.originCallsign = call;
        Frame decoded;
        std::vector<uint8_t> encoded = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
        CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
        CHECK(decoded.originCallsign == call);
    }
    Frame longCall = makePingFrame();
    longCall.originCallsign = "ABCDEFGHIJKL";
    Frame decoded;
    std::vector<uint8_t> encoded = FrameCodec::encode(longCall, SIGNALLING_FRAME_BYTES);
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.originCallsign == "ABCDEFGHI");
}

void testRoundTrip()
{
    Frame original = makeMessageFrame();
    std::vector<uint8_t> encoded = FrameCodec::encode(original, TEXT_FRAME_BYTES);
    CHECK((int)encoded.size() == TEXT_FRAME_BYTES);

    Frame decoded;
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.type == original.type);
    CHECK(decoded.destinationCrc == original.destinationCrc);
    CHECK(decoded.originCallsign == "W1AW");
    CHECK(decoded.airId == 0x2EF);
    CHECK(decoded.fragmentIndex == 1);
    CHECK(decoded.fragmentCount == 3);
    CHECK(decoded.payload == original.payload);

    // Huffman coded, 26 characters take 15 bytes behind the header instead of 26.
    CHECK(sentBytes(encoded) < 15 + 26);

    // Any byte at all, UTF-8 included, comes back as it went.
    Frame bytes = makeMessageFrame();
    bytes.payload.clear();
    for (int c = 1; c < 256; c += 23) bytes.payload.push_back((uint8_t)c);
    std::string utf8 = "73 de caf\xC3\xA9";
    bytes.payload.insert(bytes.payload.end(), utf8.begin(), utf8.end());
    encoded = FrameCodec::encode(bytes, TEXT_FRAME_BYTES);
    CHECK(!encoded.empty());
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.payload == bytes.payload);

    // Pings and acknowledgements carry no payload; a pong's SNR and a partial
    // acknowledgement's mask are one byte.
    encoded = FrameCodec::encode(makePingFrame(), SIGNALLING_FRAME_BYTES);
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.type == FrameType::Ping);
    CHECK(decoded.airId == 0x234);
    CHECK(decoded.payload.empty());
    CHECK(decoded.fragmentCount == 1);

    Frame pong = makePingFrame();
    pong.type = FrameType::PingAck;
    pong.payload.assign(1, 0x2A);
    encoded = FrameCodec::encode(pong, SIGNALLING_FRAME_BYTES);
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.type == FrameType::PingAck);
    CHECK(decoded.originCallsign == "W1AW");
    CHECK(decoded.payload.size() == 1 && decoded.payload[0] == 0x2A);

    // Padding after a short text must not read as text.
    Frame shortFrame = makeMessageFrame();
    shortFrame.payload.assign(1, 'Y');
    encoded = FrameCodec::encode(shortFrame, TEXT_FRAME_BYTES);
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(text(decoded) == "Y");

    shortFrame.payload.clear();
    encoded = FrameCodec::encode(shortFrame, TEXT_FRAME_BYTES);
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.payload.empty());
}

// From a standard callsign, every signalling frame is one Glissando
// segment (nine bytes) and a text frame's header ten; a callsign that is not
// standard costs 20 bits more.
void testSizes()
{
    const FrameType signalling[] = {FrameType::Ping, FrameType::PingAck, FrameType::MessageAck,
                                    FrameType::MessagePartialAck, FrameType::Locator};
    for (FrameType type : signalling)
    {
        for (const char* call : {"W1AW", "VK3ABC/P"})
        {
            Frame frame = makePingFrame();
            frame.type = type;
            frame.originCallsign = call;
            frame.airId = MAX_AIR_ID;
            frame.burstsFollowing = 1;
            if (FrameCodec::signallingPayloadBits(type) > 0) frame.payload.assign(1, 0xFF);
            frame.features = 0xFF;
            frame.locator = "RR99";
            frame.destinationCrc = (1u << DESTINATION_HASH_BITS) - 1;
            std::vector<uint8_t> encoded = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
            CHECK(!encoded.empty());
            bool standard = FrameCodec::isStandardCallsign(call);
            CHECK(sentBytes(encoded) <= (standard ? 9 : 12));
        }
    }
    CHECK(FrameCodec::headerBits(FrameType::Message, true) == 73);
    CHECK(FrameCodec::headerBits(FrameType::Message, false) == 93);
    CHECK(FrameCodec::headerBits(FrameType::Ping, true) == 64);
    CHECK(FrameCodec::headerBits(FrameType::Locator, true) == 54);

    // The protocol cuts text where a frame fills; what it says fits, fits.
    std::string body;
    for (int i = 0; i < 20; i++) body += "Name here is Jeff, QTH Portland OR. ";
    size_t fits = FrameCodec::textThatFits("W1AW", body, 0);
    CHECK(fits > 60 && fits < 90);
    Frame full = makeMessageFrame();
    full.payload.assign(body.begin(), body.begin() + (long)fits);
    std::vector<uint8_t> encoded = FrameCodec::encode(full, TEXT_FRAME_BYTES);
    CHECK(!encoded.empty());
    full.payload.push_back((uint8_t)body[fits]);
    CHECK(FrameCodec::encode(full, TEXT_FRAME_BYTES).empty());
    CHECK(FrameCodec::textThatFits("VK3ABC/P", body, 0) < fits);
}

// Codes never cross a 72-bit block, so a segment heard after a lost one
// still reads, and zero padding reads as no text at all.
void testHamText()
{
    std::string sample = "AG7SU de AG7EW GM Tyler, UR 559 here in Portland OR";
    int bits = 0;
    for (char c : sample) bits += HamText::characterBits((unsigned char)c);
    CHECK(bits < 6 * (int)sample.size());
    CHECK(HamText::characterBits(' ') == 3);
    CHECK(HamText::characterBits(0xC3) > 8);

    const int frameBits = 8 * TEXT_FRAME_BYTES;
    std::vector<uint8_t> frame((size_t)TEXT_FRAME_BYTES, 0);
    size_t written = HamText::encode(sample, 0, frame.data(), 73, frameBits);
    CHECK(written == sample.size());
    CHECK(HamText::decode(frame.data(), 73, frameBits) == sample);
    for (int block = 2 * HamText::TEXT_BLOCK_BITS; block < frameBits; block += HamText::TEXT_BLOCK_BITS)
    {
        std::string tail = HamText::decode(frame.data(), block, frameBits);
        CHECK(tail.size() <= sample.size() && sample.compare(sample.size() - tail.size(), tail.size(), tail) == 0);
    }

    std::vector<uint8_t> zeros((size_t)TEXT_FRAME_BYTES, 0);
    CHECK(HamText::decode(zeros.data(), 0, frameBits).empty());
}

// Phrases ride as one symbol each: found longest first, exactly (case and
// leading space), the same ones the COMMS highlight shows, and cheaper than
// spelling them out.
void testHamTextPhrases()
{
    std::string text = "CQ CQ CQ de AG7EW, can you hear the beacon? The YOU and the";
    std::vector<HamText::PhraseSpan> spans = HamText::phraseSpans(text);
    std::vector<std::string> found;
    for (const auto& span : spans) found.push_back(text.substr(span.start, span.length));
    std::vector<std::string> expected = {"CQ CQ", " de", " you", " the", " and", " the"};
    CHECK(found == expected);
    CHECK(HamText::phraseSpans("the").empty());
    CHECK(HamText::phraseSpans(" They").empty());

    const int frameBits = 8 * TEXT_FRAME_BYTES;
    std::vector<uint8_t> frame((size_t)TEXT_FRAME_BYTES, 0);
    CHECK(HamText::encode(text, 0, frame.data(), 73, frameBits) == text.size());
    CHECK(HamText::decode(frame.data(), 73, frameBits) == text);

    // " the" costs less than its four characters.
    int spelled = 0;
    for (char c : std::string(" the")) spelled += HamText::characterBits((unsigned char)c);
    std::vector<uint8_t> one((size_t)TEXT_FRAME_BYTES, 0);
    CHECK(HamText::encode(" the", 0, one.data(), 0, spelled - 1) == 4);

    // A phrase is never split between frames: it waits whole for the next.
    bool cutAtPhrase = false;
    for (size_t n = 1; n < 60; n++)
    {
        std::string longText = std::string(n, 'x') + " the end";
        size_t fits = FrameCodec::textThatFits("AG7EW", longText, 0);
        CHECK(fits <= n || fits >= n + 4);
        cutAtPhrase = cutAtPhrase || fits == n;
    }
    CHECK(cutAtPhrase);
}

// A phrase that holds a shorter one (" antenna" holds " ant", "ing" holds
// "in") always rides as the whole, longer phrase, never as the short one
// plus letters. Every listed phrase, typed alone, is one symbol.
void testHamTextLongestPhrase()
{
    const int frameBits = 8 * TEXT_FRAME_BYTES;
    for (int k = 0; k < HamText::PHRASE_COUNT; k++)
    {
        std::string phrase = HamText::PHRASES[k];
        std::vector<HamText::PhraseSpan> spans = HamText::phraseSpans(phrase);
        CHECK(spans.size() == 1 && spans[0].start == 0 && spans[0].length == phrase.size());

        // The encoder agrees: the phrase fits in exactly its own code.
        int bits = HamText::SYMBOL_BITS[95 + k];
        std::vector<uint8_t> frame((size_t)TEXT_FRAME_BYTES, 0);
        CHECK(HamText::encode(phrase, 0, frame.data(), 0, bits) == phrase.size());
        CHECK(HamText::encode(phrase, 0, frame.data(), 0, bits - 1) == 0);
        CHECK(HamText::decode(frame.data(), 0, frameBits).compare(0, phrase.size(), phrase) == 0);
    }

    std::string text = "my antenna is up, ant down, antennas";
    std::vector<std::string> found;
    for (const auto& span : HamText::phraseSpans(text)) found.push_back(text.substr(span.start, span.length));
    std::vector<std::string> antennas;
    for (const auto& f : found)
        if (f.compare(0, 4, " ant") == 0) antennas.push_back(f);
    std::vector<std::string> expected = {" antenna", " ant", " antenna"};
    CHECK(antennas == expected);
}

// Whether the sender acknowledges by itself rides in the type of its pings,
// messages and broadcasts. Acknowledgements and pongs never carry it.
void testAutoAckFlag()
{
    Frame on = makeMessageFrame();
    std::vector<uint8_t> plain = FrameCodec::encode(on, TEXT_FRAME_BYTES);
    CHECK(!plain.empty() && (plain[0] >> 4) == 0x8);
    Frame decoded;
    CHECK(FrameCodec::decode(plain.data(), (int)plain.size(), decoded));
    CHECK(decoded.senderAutoAck);

    Frame off = makeMessageFrame();
    off.senderAutoAck = false;
    std::vector<uint8_t> flagged = FrameCodec::encode(off, TEXT_FRAME_BYTES);
    CHECK(!flagged.empty() && (flagged[0] >> 4) == 0xB);
    CHECK(FrameCodec::decode(flagged.data(), (int)flagged.size(), decoded));
    CHECK(decoded.type == FrameType::Message);
    CHECK(!decoded.senderAutoAck);
    CHECK(decoded.payload == off.payload);
    for (size_t i = 1; i < plain.size(); i++) CHECK(plain[i] == flagged[i]);

    off.type = FrameType::Broadcast;
    flagged = FrameCodec::encode(off, TEXT_FRAME_BYTES);
    CHECK(!flagged.empty() && (flagged[0] >> 4) == 0xD);
    CHECK(FrameCodec::decode(flagged.data(), (int)flagged.size(), decoded));
    CHECK(decoded.type == FrameType::Broadcast && !decoded.senderAutoAck);

    Frame ping = makePingFrame();
    ping.senderAutoAck = false;
    flagged = FrameCodec::encode(ping, SIGNALLING_FRAME_BYTES);
    CHECK(!flagged.empty() && (flagged[0] >> 4) == 0x4);
    CHECK(sentBytes(flagged) <= 9);
    CHECK(FrameCodec::decode(flagged.data(), (int)flagged.size(), decoded));
    CHECK(decoded.type == FrameType::Ping && !decoded.senderAutoAck && decoded.payload.empty());

    Frame pong = makePingFrame();
    pong.type = FrameType::PingAck;
    pong.payload.assign(1, 0x2A);
    pong.senderAutoAck = false;
    flagged = FrameCodec::encode(pong, SIGNALLING_FRAME_BYTES);
    CHECK(!flagged.empty() && (flagged[0] >> 4) == 0x5);
    CHECK(FrameCodec::decode(flagged.data(), (int)flagged.size(), decoded));
    CHECK(decoded.senderAutoAck && decoded.payload[0] == 0x2A);
}

// Glissando 0.3 and older began a frame with a type byte. Neither build may
// read the other's frames as its own.
void testOldBuildsApart()
{
    // Old type bytes, with their more-follows (0x80) and no-auto-ACK (0x08) bits.
    const uint8_t oldTypes[] = {0x10, 0x11, 0x20, 0x21, 0x22, 0x23, 0x28, 0x2A, 0x90, 0x91, 0xA1, 0xA3, 0x18};
    for (uint8_t old : oldTypes)
    {
        std::vector<uint8_t> frame = FrameCodec::encode(makeMessageFrame(), TEXT_FRAME_BYTES);
        frame[0] = old;
        Frame decoded;
        CHECK(!FrameCodec::decode(frame.data(), (int)frame.size(), decoded));
    }

    // What an old build sees: the first byte, less its top bit, must not be
    // one of its types, whatever the destination hash puts in the low nibble.
    const uint8_t oldKnown[] = {0x10, 0x11, 0x20, 0x21, 0x22, 0x23, 0x28, 0x2A};
    for (int code = 0; code < 16; code++)
    {
        FrameType type;
        bool noAutoAck;
        if (!FrameCodec::typeFromCode((uint8_t)code, type, noAutoAck)) continue;
        for (int low = 0; low < 16; low++)
        {
            uint8_t first = (uint8_t)(((code << 4) | low) & 0x7F);
            for (uint8_t known : oldKnown) CHECK(first != known);
        }
    }
}

// How many bursts follow in the keying is how listeners know how long to
// leave the channel alone.
void testBurstsFollowing()
{
    Frame text = makeMessageFrame();
    text.burstsFollowing = 5;
    std::vector<uint8_t> encoded = FrameCodec::encode(text, TEXT_FRAME_BYTES);
    Frame decoded;
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.fragmentIndex == 1);
    CHECK(decoded.fragmentCount == 3);
    CHECK(decoded.burstsFollowing == 5);

    text.burstsFollowing = MAX_TEXT_BURSTS_FOLLOWING;
    encoded = FrameCodec::encode(text, TEXT_FRAME_BYTES);
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.burstsFollowing == MAX_TEXT_BURSTS_FOLLOWING);
    text.burstsFollowing = MAX_TEXT_BURSTS_FOLLOWING + 1;
    CHECK(FrameCodec::encode(text, TEXT_FRAME_BYTES).empty());

    // A signalling frame can only say whether any follow.
    Frame ping = makePingFrame();
    ping.burstsFollowing = 3;
    encoded = FrameCodec::encode(ping, SIGNALLING_FRAME_BYTES);
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.type == FrameType::Ping);
    CHECK(decoded.burstsFollowing == 1);

    ping.burstsFollowing = 0;
    encoded = FrameCodec::encode(ping, SIGNALLING_FRAME_BYTES);
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.burstsFollowing == 0);
}

// A partial acknowledgement's one payload byte is a bit per fragment received.
void testPartialAcknowledgement()
{
    Frame partial;
    partial.type = FrameType::MessagePartialAck;
    partial.destinationCrc = FrameCodec::callsignHash("W1AW");
    partial.originCallsign = "VK3ABC";
    partial.airId = 0x3EF;
    partial.payload.assign(1, 0x05); // fragments 1 and 3 of three arrived

    std::vector<uint8_t> encoded = FrameCodec::encode(partial, SIGNALLING_FRAME_BYTES);
    CHECK((int)encoded.size() == SIGNALLING_FRAME_BYTES);
    CHECK((encoded[0] >> 4) == 0x7);
    CHECK(encoded[8] == 0x05); // the mask, last of the nine bytes

    Frame decoded;
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.type == FrameType::MessagePartialAck);
    CHECK(FrameCodec::isSignallingFrameType(decoded.type));
    CHECK(decoded.airId == 0x3EF);
    CHECK(decoded.originCallsign == "VK3ABC");
    CHECK(decoded.payload.size() == 1 && decoded.payload[0] == 0x05);
}

void testEncodeRejections()
{
    Frame oversized = makeMessageFrame();
    oversized.payload.assign(100, 'X');
    CHECK(FrameCodec::encode(oversized, TEXT_FRAME_BYTES).empty());

    // A text frame's text has to fit whatever frame it is put in.
    CHECK(FrameCodec::encode(makeMessageFrame(), SIGNALLING_FRAME_BYTES).empty());

    // One byte is all a pong carries, and a ping none.
    Frame fatSignalling = makePingFrame();
    fatSignalling.type = FrameType::PingAck;
    fatSignalling.payload.assign(2, 0x11);
    CHECK(FrameCodec::encode(fatSignalling, SIGNALLING_FRAME_BYTES).empty());
    fatSignalling.type = FrameType::Ping;
    fatSignalling.payload.assign(1, 0x11);
    CHECK(FrameCodec::encode(fatSignalling, SIGNALLING_FRAME_BYTES).empty());

    // There is nowhere to record a fragment number in a signalling frame, so
    // one that claims to be fragmented must be refused rather than silently
    // sent as a single fragment.
    Frame fragmentedSignalling = makePingFrame();
    fragmentedSignalling.fragmentCount = 2;
    CHECK(FrameCodec::encode(fragmentedSignalling, SIGNALLING_FRAME_BYTES).empty());

    Frame noCallsign = makeMessageFrame();
    noCallsign.originCallsign = "";
    CHECK(FrameCodec::encode(noCallsign, TEXT_FRAME_BYTES).empty());

    Frame badFragments = makeMessageFrame();
    badFragments.fragmentIndex = 3;
    badFragments.fragmentCount = 3;
    CHECK(FrameCodec::encode(badFragments, TEXT_FRAME_BYTES).empty());

    Frame zeroCount = makeMessageFrame();
    zeroCount.fragmentCount = 0;
    CHECK(FrameCodec::encode(zeroCount, TEXT_FRAME_BYTES).empty());

    Frame tooMany = makeMessageFrame();
    tooMany.fragmentCount = MAX_FRAGMENTS_PER_MESSAGE + 1;
    CHECK(FrameCodec::encode(tooMany, TEXT_FRAME_BYTES).empty());

    Frame bigId = makeMessageFrame();
    bigId.airId = MAX_AIR_ID + 1;
    CHECK(FrameCodec::encode(bigId, TEXT_FRAME_BYTES).empty());

    Frame wideHash = makeMessageFrame();
    wideHash.destinationCrc = FrameCodec::callsignCrc24("VK3ABC");
    CHECK(FrameCodec::encode(wideHash, TEXT_FRAME_BYTES).empty());
}

void testDecodeRejections()
{
    std::vector<uint8_t> encoded = FrameCodec::encode(makeMessageFrame(), TEXT_FRAME_BYTES);
    Frame decoded;

    CHECK(!FrameCodec::decode(nullptr, TEXT_FRAME_BYTES, decoded));
    CHECK(!FrameCodec::decode(encoded.data(), 9, decoded)); // shorter than the header

    for (uint8_t code : {0x0, 0x1, 0x2, 0x9, 0xA, 0xF})
    {
        std::vector<uint8_t> corrupted = encoded;
        corrupted[0] = (uint8_t)((code << 4) | (corrupted[0] & 0x0F));
        CHECK(!FrameCodec::decode(corrupted.data(), (int)corrupted.size(), decoded));
    }

    // Fragment index past the count: index is bits 67-69, count 70-72.
    std::vector<uint8_t> corrupted = encoded;
    putBits(corrupted.data(), 67, 0, 6);
    putBits(corrupted.data(), 67, 0x3F, 3);
    CHECK(!FrameCodec::decode(corrupted.data(), (int)corrupted.size(), decoded));

    // A standard callsign value no callsign packs to.
    corrupted = encoded;
    putBits(corrupted.data(), FrameCodec::ORIGIN_BIT + 1, 0, STANDARD_CALLSIGN_BITS);
    putBits(corrupted.data(), FrameCodec::ORIGIN_BIT + 1, (1u << STANDARD_CALLSIGN_BITS) - 1, STANDARD_CALLSIGN_BITS);
    CHECK(!FrameCodec::decode(corrupted.data(), (int)corrupted.size(), decoded));

    // A ping truncated by a byte is short of its header.
    std::vector<uint8_t> ping = FrameCodec::encode(makePingFrame(), SIGNALLING_FRAME_BYTES);
    CHECK(FrameCodec::decode(ping.data(), 8, decoded));
    CHECK(decoded.features == 0);
    CHECK(!FrameCodec::decode(ping.data(), 7, decoded));
}

// Every frame type W1AW can send VK3ABC/P starts with what VK3ABC/P expects
// of a frame from W1AW; a frame from anyone else, or to anyone else, does
// not.
void testExpectedFrameStart()
{
    const int n = FrameCodec::EXPECTED_START_BYTES;
    for (const char* sender : {"W1AW", "K1ABC/P"})
    {
        uint8_t fromSender[n], fromSenderMask[n], anybody[n], anybodyMask[n];
        FrameCodec::expectedFrameStart("vk3abc/p", sender, fromSender, fromSenderMask);
        FrameCodec::expectedFrameStart("VK3ABC/P", "", anybody, anybodyMask);

        auto matches = [n](const std::vector<uint8_t>& frame, const uint8_t* bytes, const uint8_t* masks) {
            for (int i = 0; i < n; i++)
                if ((frame[(size_t)i] ^ bytes[i]) & masks[i]) return false;
            return true;
        };

        const FrameType types[] = {FrameType::Ping, FrameType::PingAck, FrameType::Message,
                                   FrameType::MessageAck, FrameType::Broadcast, FrameType::MessagePartialAck};
        for (FrameType type : types)
        {
            for (uint8_t following : {0, 1, 3})
            {
                Frame frame;
                frame.type = type;
                frame.destinationCrc = FrameCodec::callsignHash("VK3ABC/P");
                frame.originCallsign = sender;
                frame.airId = 0x234;
                frame.fragmentIndex = 0;
                frame.fragmentCount = 1;
                frame.burstsFollowing = following;
                frame.senderAutoAck = following != 1;
                const bool signalling = FrameCodec::isSignallingFrameType(type);
                frame.payload = signalling ? std::vector<uint8_t>() : std::vector<uint8_t>{'7', '3'};
                if (FrameCodec::signallingPayloadBits(type) > 0) frame.payload = {1};
                const int bytes = signalling ? SIGNALLING_FRAME_BYTES : TEXT_FRAME_BYTES;
                std::vector<uint8_t> encoded = FrameCodec::encode(frame, bytes);
                CHECK(!encoded.empty());
                if (encoded.empty()) continue;
                CHECK(matches(encoded, fromSender, fromSenderMask));
                CHECK(matches(encoded, anybody, anybodyMask));

                frame.originCallsign = "W1AX";
                encoded = FrameCodec::encode(frame, bytes);
                CHECK(!matches(encoded, fromSender, fromSenderMask));
                CHECK(matches(encoded, anybody, anybodyMask));

                frame.destinationCrc = FrameCodec::callsignHash("VK3ABD");
                encoded = FrameCodec::encode(frame, bytes);
                CHECK(!matches(encoded, anybody, anybodyMask));
            }
        }

        // The destination and, with a sender, its callsign and form bit.
        int knownBits = 0;
        for (int i = 0; i < n; i++)
            for (int bit = 0; bit < 8; bit++) knownBits += (fromSenderMask[i] >> bit) & 1;
        CHECK(knownBits == DESTINATION_HASH_BITS + FrameCodec::originBits(FrameCodec::isStandardCallsign(sender)));
        CHECK(anybodyMask[0] == 0x0F);
        for (int i = 3; i < n; i++) CHECK(anybodyMask[i] == 0);
    }
}

// Grid squares pack as FT8 packs them, and only what an operator could
// mean by one is taken.
void testGridSquares()
{
    CHECK(FrameCodec::normalizeLocator("cn87") == "CN87");
    CHECK(FrameCodec::normalizeLocator(" CN87UX ") == "CN87ux");
    CHECK(FrameCodec::normalizeLocator("RR99xx") == "RR99xx");
    CHECK(FrameCodec::normalizeLocator("").empty());
    CHECK(FrameCodec::normalizeLocator("CN8").empty());
    CHECK(FrameCodec::normalizeLocator("CN87u").empty());
    CHECK(FrameCodec::normalizeLocator("SN87").empty());
    CHECK(FrameCodec::normalizeLocator("C887").empty());
    CHECK(FrameCodec::normalizeLocator("CN8A").empty());
    CHECK(FrameCodec::normalizeLocator("CN87uy").empty());
    CHECK(FrameCodec::normalizeLocator("CN87ux12").empty());

    CHECK(FrameCodec::packGridSquare("AA00") == 0);
    CHECK(FrameCodec::packGridSquare("RR99") == GRID_SQUARE_VALUES - 1);
    CHECK(FrameCodec::packGridSquare("CN87ux") == FrameCodec::packGridSquare("CN87"));
    CHECK(FrameCodec::packGridSquare("nowhere") == -1);
    CHECK(GRID_SQUARE_VALUES <= (1 << LOCATOR_BITS));

    bool roundTrips = true;
    for (int value = 0; value < GRID_SQUARE_VALUES; value++)
    {
        std::string grid = FrameCodec::unpackGridSquare(value);
        if (grid.size() != 4 || FrameCodec::packGridSquare(grid) != value) roundTrips = false;
    }
    CHECK(roundTrips);
    CHECK(FrameCodec::unpackGridSquare(GRID_SQUARE_VALUES).empty());
    CHECK(FrameCodec::unpackGridSquare(-1).empty());
}

// A locator frame is one segment from a standard callsign, carries the
// square and not the subsquare, and a value past the last square is
// corruption.
void testLocatorFrame()
{
    Frame frame;
    frame.type = FrameType::Locator;
    frame.destinationCrc = FrameCodec::callsignHash("VK3ABC");
    frame.originCallsign = "W1AW";
    frame.locator = "fn31pr";
    std::vector<uint8_t> encoded = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
    CHECK(!encoded.empty());
    CHECK(sentBytes(encoded) <= 9);

    Frame decoded;
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.type == FrameType::Locator);
    CHECK(decoded.locator == "FN31");
    CHECK(decoded.originCallsign == "W1AW");
    CHECK(decoded.destinationCrc == FrameCodec::callsignHash("VK3ABC"));
    CHECK(decoded.burstsFollowing == 0);
    CHECK(decoded.airId == 0);

    frame.burstsFollowing = 1;
    encoded = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.burstsFollowing == 1);

    // The square sits right after "more follows".
    int at = FrameCodec::headerBits(FrameType::Locator, true);
    CHECK(getBits(encoded.data(), at, LOCATOR_BITS) == (uint64_t)FrameCodec::packGridSquare("FN31"));
    putBits(encoded.data(), at, (1u << LOCATOR_BITS) - 1, LOCATOR_BITS);
    CHECK(!FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));

    Frame nowhere = frame;
    nowhere.locator = "";
    CHECK(FrameCodec::encode(nowhere, SIGNALLING_FRAME_BYTES).empty());

    // From a callsign that is not standard it is two segments.
    frame.originCallsign = "VK3ABC/P";
    encoded = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.locator == "FN31" && decoded.originCallsign == "VK3ABC/P");
    CHECK(sentBytes(encoded) > 9 && sentBytes(encoded) <= 12);
}

// The short form, for a duet's spare voice, is one segment from any
// callsign the air alphabet packs, the longest included, and reads back as
// a locator with no destination that ends its keying.
void testShortLocatorFrame()
{
    for (const char* call : {"W1AW", "VK3ABC/P", "TEST1/P", "VE3/W1ABC"})
    {
        Frame frame;
        frame.type = FrameType::Locator;
        frame.destinationCrc = FrameCodec::callsignHash("K1ABC"); // ignored
        frame.burstsFollowing = 1;                                // ignored
        frame.originCallsign = call;
        frame.locator = "rr99xx";
        std::vector<uint8_t> encoded = FrameCodec::encodeShortLocator(frame, SIGNALLING_FRAME_BYTES);
        CHECK(encoded.size() == (size_t)SIGNALLING_FRAME_BYTES);
        CHECK(sentBytes(encoded) <= 9);
        CHECK((encoded[0] >> 4) == 0xF);

        Frame decoded;
        CHECK(FrameCodec::decode(encoded.data(), 9, decoded));
        CHECK(decoded.type == FrameType::Locator);
        CHECK(decoded.originCallsign == FrameCodec::normalizeCallsign(call));
        CHECK(decoded.locator == "RR99");
        CHECK(decoded.destinationCrc == 0 && decoded.burstsFollowing == 0);

        // Cut short, or past the last square, it is not a frame.
        CHECK(!FrameCodec::decode(encoded.data(), 1, decoded));
        int at = TYPE_BITS + FrameCodec::originBits(FrameCodec::isStandardCallsign(call));
        CHECK(getBits(encoded.data(), at, LOCATOR_BITS) == (uint64_t)FrameCodec::packGridSquare("RR99"));
        putBits(encoded.data(), at, (1u << LOCATOR_BITS) - 1, LOCATOR_BITS);
        CHECK(!FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    }

    Frame frame;
    frame.type = FrameType::Locator;
    frame.originCallsign = "W1AW";
    CHECK(FrameCodec::encodeShortLocator(frame, SIGNALLING_FRAME_BYTES).empty()); // no square
    frame.locator = "CN87";
    CHECK(FrameCodec::encodeShortLocator(frame, 5).empty());                       // no room
    frame.originCallsign = "";
    CHECK(FrameCodec::encodeShortLocator(frame, SIGNALLING_FRAME_BYTES).empty()); // no callsign

    FrameType type;
    bool noAutoAck = true;
    CHECK(FrameCodec::typeFromCode(0xF, type, noAutoAck) && type == FrameType::Locator && !noAutoAck);
}

// Pings and acknowledgements end in the feature byte, in the ninth byte
// that Glissando 0.5 sent as zero and never read; pongs and partial
// acknowledgements have their own byte there and carry none.
void testFeatureByte()
{
    for (FrameType type : {FrameType::Ping, FrameType::MessageAck})
    {
        Frame frame = makePingFrame();
        frame.type = type;
        frame.features = FEATURE_UNDERSTANDS_LOCATOR | FEATURE_HEARD_YOUR_LOCATOR;
        std::vector<uint8_t> encoded = FrameCodec::encode(frame, SIGNALLING_FRAME_BYTES);
        CHECK(!encoded.empty());
        CHECK(encoded[8] == frame.features);
        CHECK(sentBytes(encoded) == 9);

        Frame decoded;
        CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
        CHECK(decoded.features == frame.features);

        // What 0.5 sends: the byte left zero.
        encoded[8] = 0;
        CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
        CHECK(decoded.features == 0);
    }

    Frame pong = makePingFrame();
    pong.type = FrameType::PingAck;
    pong.payload.assign(1, 0x2A);
    pong.features = 0xFF;
    std::vector<uint8_t> encoded = FrameCodec::encode(pong, SIGNALLING_FRAME_BYTES);
    Frame decoded;
    CHECK(FrameCodec::decode(encoded.data(), (int)encoded.size(), decoded));
    CHECK(decoded.features == 0);
    CHECK(decoded.payload.size() == 1 && decoded.payload[0] == 0x2A);
}

} // namespace

int main()
{
    testCallsignEncoding();
    testExpectedFrameStart();
    testRoundTrip();
    testSizes();
    testHamText();
    testHamTextPhrases();
    testHamTextLongestPhrase();
    testBurstsFollowing();
    testAutoAckFlag();
    testOldBuildsApart();
    testPartialAcknowledgement();
    testEncodeRejections();
    testDecodeRejections();
    testGridSquares();
    testLocatorFrame();
    testShortLocatorFrame();
    testFeatureByte();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    printf("all text messaging frame codec checks passed\n");
    return 0;
}
