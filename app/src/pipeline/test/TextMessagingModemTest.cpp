// Pins delivery through the decoded-segment callback after the Chorus/scope merge.
#include <cmath>
#include <cstdio>

#include "../TextMessagingModem.h"

struct TextMessagingModemTestAccess
{
    static void receive(TextMessagingModem& modem, const Glissando::StreamDecode& decode)
    {
        modem.onGlissandoDecode(decode);
    }
};

namespace
{
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); ++failures; \
} } while (false)

void testCompletedMessage(bool chorus)
{
    TextMessagingModem modem;
    TextMessagingModem::GlissandoConfig config;
    config.chorus = chorus;
    config.callsign = "W1AW";
    config.tuningOffsetHz = 12.5;
    modem.setGlissando(config);

    TextMessaging::Frame message;
    message.type = TextMessaging::FrameType::Message;
    message.originCallsign = "VK3ABC";
    message.airId = 42;
    const std::string text = "Deliver this completed message";
    message.payload.assign(text.begin(), text.end());
    auto bytes = TextMessaging::FrameCodec::encode(message, TextMessaging::TEXT_FRAME_BYTES);
    CHECK(!bytes.empty());
    auto payloads = Glissando::segmentBursts({{true, bytes}}, 1);
    CHECK(payloads.size() > 1);

    int received = 0;
    modem.setFrameCallback([&](const TextMessaging::Frame& frame, float) {
        ++received;
        CHECK(frame.type == message.type);
        CHECK(frame.originCallsign == message.originCallsign);
        CHECK(frame.airId == message.airId);
        CHECK(frame.payload == message.payload);
    });
    for (size_t i = 0; i < payloads.size(); ++i)
    {
        Glissando::StreamDecode decode{};
        decode.gear = 3;
        decode.decode.payload = payloads[i];
        decode.decode.scale = Glissando::Scale::Pentatonic;
        decode.decode.scaleDegree = 3;
        decode.decode.frequencyOffsetHz = 2.0;
        decode.decode.startSample = (long long)i * Glissando::gearInfo(3).frameSamples();
        TextMessagingModemTestAccess::receive(modem, decode);
        // A second search seeing the same segment must not deliver it twice.
        TextMessagingModemTestAccess::receive(modem, decode);
    }
    CHECK(received == 1);
    auto heard = modem.takeGlissandoHeard();
    CHECK(heard.size() == payloads.size());
    auto notes = Glissando::scaleDegreeNotes(Glissando::Scale::Pentatonic, 0, 3);
    for (const auto& frame : heard)
        for (size_t i = 0; i < notes.size(); ++i)
            CHECK(std::fabs(frame.notesHz[i] - notes[i] - 14.5) < 1e-9);
    CHECK(!heard.empty() && heard.back().segment.completed);
}
} // namespace

int main()
{
    testCompletedMessage(false);
    testCompletedMessage(true);
    return failures == 0 ? 0 : 1;
}
