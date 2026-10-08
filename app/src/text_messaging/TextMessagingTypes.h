//=========================================================================
// Name:            TextMessagingTypes.h
// Purpose:         Shared vocabulary for the FreeDV text messaging feature.
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

#ifndef TEXT_MESSAGING__TEXT_MESSAGING_TYPES_H
#define TEXT_MESSAGING__TEXT_MESSAGING_TYPES_H

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace TextMessaging
{

// Over the air frame types. The protocol is inspired by FreeDATA but is not
// wire compatible with it, so the values are our own; they are persisted in
// nothing and may be renumbered until the first release.
enum class FrameType : uint8_t
{
    Ping = 0x10,       // "are you there" addressed to a single station
    PingAck = 0x11,    // reply to Ping, carries the SNR the ping was heard at
    Message = 0x20,    // addressed message fragment, acknowledgement requested
    MessageAck = 0x21, // acknowledgement of a complete addressed message
    Broadcast = 0x22,  // unaddressed message fragment, no acknowledgement
    MessagePartialAck = 0x23, // which fragments of a message arrived; payload is
                              // a bit per fragment, and the sender resends the rest
    Locator = 0x30,    // the sender's Maidenhead grid square; see LOCATOR_BITS
};

// Modem frame payload sizes: what codec2 hands us per modem frame, less the
// two byte CRC the raw data API appends. DATAC13 carries signalling, DATAC4
// carries message text; both are the most robust option in their size class,
// which matters more than throughput for chat. TextMessagingModem checks these
// against the modem at open time and refuses to run if they have drifted.
constexpr int SIGNALLING_FRAME_BYTES = 14; // DATAC13: 128 bits - CRC16
constexpr int TEXT_FRAME_BYTES = 54;       // DATAC4: 448 bits - CRC16

// On air header, packed to the bit (most significant bit of each byte
// first; see FrameCodec for the layout). Every frame starts with a 4-bit
// type, a 20-bit hash of the destination callsign (zero for broadcasts) and
// the origin callsign: a form bit, then 28 bits for a standard callsign
// (one or two prefix characters, a digit and up to three letters, packed as
// FT8 packs them) or 48 bits for anything else (nine characters, base 40,
// which covers portable suffixes such as VK3ABC/P). The origin's own hash
// is not sent: it is the hash of the callsign already in the frame.
//
// A signalling frame then has a "more follows" bit, a 10-bit message ID and
// one byte more: a pong's SNR, a partial acknowledgement's fragments, or a
// ping's or acknowledgement's feature byte (FEATURE_*, bits Glissando 0.5
// and older leave zero and never read). 72 bits with a standard callsign,
// one Glissando segment. A text frame has how many bursts follow in this
// keying (4 bits), the message ID, the fragment's index and the fragment
// count less one (3 bits each), 73 bits with a standard callsign, and then
// the text, Huffman coded (HamText) to the end of the frame. Zero padding
// reads as no text, so no length field is needed.
//
// A locator frame carries the sender's grid square where a signalling frame
// has its message ID: the "more follows" bit, then the square's first four
// characters in LOCATOR_BITS, packed as FT8 packs them. 69 bits with a
// standard callsign, one Glissando segment. It rides as the last burst of a
// directed message to a station that has said it understands it, until that
// station acknowledges it (FEATURE_HEARD_YOUR_LOCATOR). The empty second
// voice of a duet keying carries a short form, type 0xF, with no
// destination or "more follows" bit, which fits one segment from any
// callsign (FrameCodec::encodeShortLocator). Older builds drop both as not
// one of theirs.
//
// A station with Auto acknowledge off says so in the type of its pings,
// messages and broadcasts, so that others do not retry to it. Pongs and
// acknowledgements never need to: a station sends them only with it on.
//
// The type values avoid every first nibble the byte-aligned header of
// Glissando 0.3 and older could start with, so each build drops the other's
// frames as not its own rather than misreading them.
constexpr int TYPE_BITS = 4;
constexpr int DESTINATION_HASH_BITS = 20;
constexpr int STANDARD_CALLSIGN_BITS = 28;
constexpr int EXTENDED_CALLSIGN_BITS = 48;
constexpr int AIR_ID_BITS = 10;
constexpr int BURSTS_FOLLOWING_BITS = 4;
constexpr int FRAGMENT_FIELD_BITS = 3;
constexpr int SIGNALLING_PAYLOAD_BITS = 8;
constexpr int FEATURE_BITS = 8;
constexpr int LOCATOR_BITS = 15;
constexpr int MAX_AIR_ID = (1 << AIR_ID_BITS) - 1;

// What a ping or an acknowledgement says about its sender, in its feature
// byte. The other bits are zero, and a receiver ignores them.
constexpr uint8_t FEATURE_UNDERSTANDS_LOCATOR = 0x80; // it takes in locator frames
constexpr uint8_t FEATURE_HEARD_YOUR_LOCATOR = 0x40;  // an acknowledgement only: it has
                                                      // the addressee's locator, this contact

// Grid squares, field letters A to R and square digits 0 to 9: 18 x 18 x 10
// x 10 values, which fit LOCATOR_BITS.
constexpr int GRID_SQUARE_VALUES = 18 * 18 * 10 * 10;

// A station's locator goes to each station we message once a contact, and
// keeps riding our messages to it until it says it has it. A contact ends
// when we have neither heard from the station nor sent to it for this long.
constexpr uint64_t LOCATOR_CONTACT_IDLE_MILLISECONDS = 30 * 60 * 1000;

// A message is sent as one keying of the transmitter, so its length is bounded
// by how long we are willing to hold the channel.
constexpr int MAX_FRAGMENTS_PER_MESSAGE = 1 << FRAGMENT_FIELD_BITS;

static_assert(MAX_FRAGMENTS_PER_MESSAGE <= SIGNALLING_PAYLOAD_BITS,
              "a partial acknowledgement has one bit per fragment in a signalling payload");

// What the operator may type. Ordinary text takes about five bits a
// character, so this is four or five fragments; text of rare characters
// can need more than eight, and the protocol refuses it then.
constexpr int MAX_MESSAGE_TEXT_BYTES = 312;

// Callsigns that are not standard are packed nine characters into 48 bits.
// Anything longer is truncated for the air, never for the display.
constexpr int MAX_PACKED_CALLSIGN_CHARS = 9;

// A half duplex station hears nothing while it is keyed, and its receiver
// needs a moment to settle after it unkeys. So replying the instant a burst
// decodes talks over a station that is still turning around, and starting the
// next queued burst straight after our own talks over the reply we just asked
// for. The loopback bench caught both: two stations keyed at the same instant
// and each missed what the other sent.
//
// Every transmission therefore waits out a turnaround, measured from the last
// thing we heard and from the end of our own last burst.
// Measured from the last frame we decoded, which is not the same as the far
// end going quiet: its postamble, end of over and audio drain all follow the
// last frame we could read. This has to outlast that tail.
constexpr int TURNAROUND_AFTER_RX_MILLISECONDS = 1500;

// For a burst nobody is expected to answer. A station that heard the burst
// comes clear of it about two seconds after it ends, plus a moment's random
// pause, and may have been waiting the whole time; at a second and a half
// this wait had us keying again right on top of it, twice on the bench. The
// station that just transmitted yields: it waits long enough for a listener
// to go first with a gap carrier sense can see, and short enough, with the
// jitter added, to stay inside the reply window below. A listener is clear
// by about 2.7 s after the burst and the demodulator needs up to 1.5 s of
// preamble to notice a burst, so 3.5 s left a gap of a second and lost one
// more burst on the bench; the extra half second is the margin.
constexpr int TURNAROUND_AFTER_TX_MILLISECONDS = 4000;

// For a burst that asked for an acknowledgement. The far end waits out its own
// turnaround and then sends a burst of its own, so a wait sized for our
// changeover alone is far too short: on the bench a station finished a message
// at 14:29:23, the far end began acknowledging it at 14:29:24, and we keyed
// over the top of it at 14:29:25 and lost the reply. The window ends early
// anyway when the acknowledgement arrives, because the entry waiting on it is
// removed from the outbox. It has to outlast the plain turnaround above with
// its jitter, or a message that asked for an answer would wait no longer than
// a broadcast.
//
// The same window is served after we send a reply. The station we answered
// is already turning around and usually has more to say; on the bench it
// keyed two seconds after our acknowledgement ended, exactly when our own
// plain turnaround let us key, four times in one run. Waiting the window
// hands it the channel with a gap carrier sense can see. Traffic of our own
// does not have to wait for it: one queued transmission goes out behind the
// reply, in the same keying. The window holds back only keyings of our own,
// not a reply to whatever we hear meanwhile, and it runs from when listeners
// stop holding the channel for us, not from our unkeying. On the bench a
// fade took the message behind a reply, the listener held the channel for
// the two fragments the reply's "more follows" promised, and it keyed as the
// window counted from our unkeying ran out.
constexpr int REPLY_WINDOW_MILLISECONDS = 6000;

// Two stations that back off by exactly the same amount collide again on the
// retry, so the wait after our own burst carries jitter. It is drawn from a
// sequence seeded by the station's own callsign, which keeps it reproducible
// per station while decorrelating any two of them.
constexpr int TURNAROUND_JITTER_MILLISECONDS = 1000;
constexpr int MAX_TURNAROUND_MILLISECONDS =
    REPLY_WINDOW_MILLISECONDS + TURNAROUND_JITTER_MILLISECONDS;

// Carrier sense. While the demodulator is locked onto a burst -- from the
// moment it correlates a preamble until the packet is in -- someone else has
// the channel, and keying would talk over them. Every fragment of a message is
// a burst of its own, so sync genuinely drops for the postamble, the gap and
// the next preamble; the hold bridges that, or the channel would read as free
// between fragments of the same message.
//
// It has to bridge more than that. A receiver that joins a burst part way
// through, which is what happens every time we unkey while somebody else is
// still sending, never gets a clean preamble: on the bench it flickered in and
// out of sync three times inside one five second burst, with clear gaps of up
// to a second and a half. One second of hold left the channel reading free in
// those gaps. Two covers the widest gap seen with margin, at the cost of about
// a second more before keying after every burst received.
constexpr int CHANNEL_BUSY_HOLD_MILLISECONDS = 2000;

// The hold cannot bridge every gap: between fragments of one message the
// demodulator drops sync at the end of a payload and regains it well into the
// next preamble, four seconds apart on the bench. The protocol knows better
// than the demodulator here. Each text frame says how many more bursts its
// sender has to send in this keying, so the channel is reserved for that many
// bursts of this length and released by the last. This is the longest a
// DATAC4 fragment takes on the air, preamble to inter-burst gap; the modem
// checks it against codec2 when it opens.
constexpr int TEXT_FRAGMENT_AIR_MILLISECONDS = 5500;

// A signalling frame says only that more follows, and what follows one is
// message text. Two fragments' worth covers the first of them being lost to a
// fade; the first that is heard then sets the reservation exactly.
constexpr int SIGNALLING_FOLLOWED_RESERVATION_MILLISECONDS = 2 * TEXT_FRAGMENT_AIR_MILLISECONDS;

// A demodulator that keeps false triggering on band noise must not be able to
// silence the station for good. The longest real traffic is eight DATAC4
// fragments at about five and a half seconds each, so anything continuously
// busy for longer than this is not a transmission and is ignored.
constexpr int MAX_CHANNEL_BUSY_MILLISECONDS = 60000;

// Retry policy for addressed messages. Timeout is measured from the end of our
// transmission to the arrival of the acknowledgement.
constexpr int MAX_MESSAGE_RETRIES = 3;
constexpr int ACK_TIMEOUT_MILLISECONDS = 15000;
constexpr int PING_TIMEOUT_MILLISECONDS = 15000;

// A retry does not key the moment its timer expires. Two stations whose timers
// expire in the same second cannot see each other by carrier sense, which
// needs about a second of preamble, and the bench caught exactly that: one
// station's second retry and the other's first keyed together at 21:24:21.
// Each retry therefore waits a random backoff first, drawn from a range that
// grows with the attempt, so two stations that collided once are unlikely to
// collide the same way again.
constexpr int RETRY_BACKOFF_MILLISECONDS = 3000;
constexpr int MAX_RETRY_BACKOFF_MILLISECONDS = RETRY_BACKOFF_MILLISECONDS * MAX_MESSAGE_RETRIES;

// A message only partly received is kept while fragments keep arriving. A
// retry resends the same fragments under the same ID, so what one attempt
// lost to a fade another can fill in. The clock runs from the last fragment
// heard, not the first: an eight fragment message spends 44 seconds on the
// air per attempt, and timed from the first fragment its partial copy was
// thrown away before the second retry arrived. This comfortably outlasts the
// gap between attempts: the acknowledgement timeout, the retry backoff and
// the wait for a busy channel.
constexpr int REASSEMBLY_TIMEOUT_MILLISECONDS = 120 * 1000;

// Every protocol timer in one place, so a transport much slower than the
// codec2 data modes can scale them. The defaults are the constants above and
// are what the codec2 transport runs with; see AirTiming::forFrameSeconds()
// for a mode whose single burst takes tens of seconds.
struct AirTiming
{
    int turnaroundAfterRxMs = TURNAROUND_AFTER_RX_MILLISECONDS;
    int turnaroundAfterTxMs = TURNAROUND_AFTER_TX_MILLISECONDS;
    int replyWindowMs = REPLY_WINDOW_MILLISECONDS;
    int turnaroundJitterMs = TURNAROUND_JITTER_MILLISECONDS;
    int textFragmentAirMs = TEXT_FRAGMENT_AIR_MILLISECONDS;
    int frameAirMs = TEXT_FRAGMENT_AIR_MILLISECONDS; // one of the modem's frames, for "Woah!"
    int signallingFollowedReservationMs = SIGNALLING_FOLLOWED_RESERVATION_MILLISECONDS;
    int maxChannelBusyMs = MAX_CHANNEL_BUSY_MILLISECONDS;
    int ackTimeoutMs = ACK_TIMEOUT_MILLISECONDS;
    int pingTimeoutMs = PING_TIMEOUT_MILLISECONDS;
    int retryBackoffMs = RETRY_BACKOFF_MILLISECONDS;
    int reassemblyTimeoutMs = REASSEMBLY_TIMEOUT_MILLISECONDS;

    int maxTurnaroundMs() const { return replyWindowMs + turnaroundJitterMs; }

    // For a modem that sends a chat burst as a run of fixed length frames,
    // each carrying bytesPerFrame bytes and taking frameSeconds, and that
    // only reports a frame once all of it has arrived plus up to
    // decodeLatencySeconds of searching. A signalling burst is at most
    // SIGNALLING_FRAME_BYTES and a text burst at most TEXT_FRAME_BYTES.
    // Carrier sense on such a modem only sees a burst once its first frame
    // decodes, so every wait for the far end is sized to the whole of the
    // burst it is waiting for, not to a turnaround.
    //
    // The far end answers at its own tempo, not ours, and after a jitter of
    // its own. replyFrameSeconds and replyDecodeLatencySeconds describe the
    // slowest tempo we would hear an answer in. Given them, a wait for an
    // answer covers one at our tempo with the far end's jitter, and never
    // gives up before the first frame of one at the slowest tempo could have
    // been decoded; once it has, the channel is busy and the protocol holds
    // its timers for the rest. Without them the timings are as they were,
    // which is what Data2G uses.
    //
    // chordSeconds is the chord that opens each of our keyings (Glissando's;
    // 0 for none), closingChordSeconds the one that closes it (negative for
    // the same as the opening one) and replyChordSeconds the one that opens
    // an answer at the slowest tempo.
    //
    // answerSensedSeconds, when given, is how soon after the far end keys
    // carrier sense hears it, as Glissando's opening chord lets it. The
    // reply window, which keeps our own keyings off an answer that may be
    // coming, then ends once the start of one would have been heard rather
    // than once its first frame could have decoded: at Presto 16 s instead
    // of 25, at Adagio 54 s instead of 2 minutes. An answer too weak for its
    // chord to be heard (below about -14 dB) can be keyed over; the
    // acknowledgement and ping timeouts still wait for its first frame.
    static AirTiming forFrameSeconds(double frameSeconds, int bytesPerFrame,
                                     double decodeLatencySeconds,
                                     double replyFrameSeconds = 0.0,
                                     double replyDecodeLatencySeconds = 0.0,
                                     double chordSeconds = 0.0,
                                     double replyChordSeconds = 0.0,
                                     double closingChordSeconds = -1.0,
                                     double answerSensedSeconds = 0.0);
};

// What the station is currently waiting to hear back, which is what the chat
// window's status line reports while an acknowledgement cycle is running.
enum class AckWait
{
    Nothing,
    Message,
    Ping,
};

// How long a message of ours still waiting for its first turn on the air is
// expected to wait, for the countdown on its chip in the chat window.
struct QueuedWait
{
    int64_t messageId = 0;
    int64_t waitMs = 0;       // from now until it should key; zero if due
    bool channelBusy = false; // somebody else has the channel: nothing counts down
    int gear = 0;             // the tempo the operator chose for it; 0 for the one set now
    bool reliableLink = false; // it goes through a link that paces itself, so no tempo or wait applies
};

enum class MessageDirection
{
    Sent,
    Received,
};

// Lifecycle of a message in the chat window. Sent messages walk Queued ->
// Transmitting -> AwaitingAck -> Acknowledged, falling into Retrying on each
// timeout and Failed once the retries are used up. Broadcasts stop at Sent
// because nothing acknowledges them. Received messages are created Received.
// A message waiting to go out when transmitting is inhibited ends NotSent;
// one the operator stops, on the air or waiting behind it, ends Aborted.
enum class MessageStatus
{
    Queued,
    Transmitting,
    AwaitingAck,
    Retrying,
    Acknowledged,
    Failed,
    Sent,
    Received,
    NotSent,     // discarded unsent: the station may not transmit where it is
    Aborted,     // stopped by the operator, part way through or before it went out
};

// Chat lines are what the operator typed; system lines are the small PING and
// PONG notices the protocol writes into the same window.
enum class MessageKind
{
    Chat,
    System,
};

// One line in the chat window, and one row in the message store.
struct TextMessage
{
    MessageKind kind = MessageKind::Chat;
    int64_t id = 0;                 // message store row ID, 0 until stored
    uint16_t airId = 0;             // ID carried on air, used to match ACKs
    std::string originCallsign;     // who sent it (us, for Sent messages)
    std::string destCallsign;       // empty for broadcasts
    bool broadcast = false;
    std::string text;
    std::time_t timestamp = 0;      // when queued (Sent) or decoded (Received)
    MessageDirection direction = MessageDirection::Sent;
    MessageStatus status = MessageStatus::Queued;
    int retryCount = 0;
    float snr = 0.0f;               // SNR the message was received at

    // How much of a sent message the far end has confirmed, for the chat
    // window while it is being delivered. Not kept in the message store: a
    // message outlives the attempt to deliver it by less than a restart.
    int fragmentCount = 0;
    int fragmentsConfirmed = 0;
};

// The two data modes a burst can be sent in. One keying of the transmitter may
// carry bursts in both, for example an acknowledgement followed by a message.
enum class BurstMode
{
    Signalling, // DATAC13: pings, pongs and acknowledgements
    Text,       // DATAC4: message fragments
};

// One burst of a keying: an encoded frame and the mode it goes on the air in.
struct OutgoingBurst
{
    BurstMode mode = BurstMode::Text;
    std::vector<uint8_t> frame;
    int gear = 0; // the Glissando tempo the operator chose for it; 0 for the one set now
    std::string destination; // the station it is addressed to; empty for a broadcast

    // A frame a Glissando duet keying may send in its spare second voice,
    // which otherwise carries a filler segment: our locator, when it fits one
    // segment. Not a burst: nothing counts it, and every other transport and
    // mode ignores it. Set on the last burst of a keying.
    std::vector<uint8_t> duetFiller;
};

// What became of a keying handed to a link that acknowledges by itself (see
// ITextMessagingTransport::transmitReliably).
struct KeyingReport
{
    enum class Result
    {
        Delivered,   // the far end's modem acknowledged all of it
        Failed,      // the link gave up on it part way, or lost the far end
        NotTaken,    // the link never opened, so none of it was sent
    };

    uint64_t keyingId = 0;
    Result result = Result::Failed;
};

// Whether a station's build takes in locator frames, as far as we know: its
// pings and acknowledgements say so in their feature byte.
enum class LocatorSupport
{
    Unknown,
    No,
    Yes,
};

// What we know of a station's locator, kept across sessions so a station
// heard last week is placed as soon as it is heard again.
struct StationLocator
{
    std::string callsign;
    std::string gridSquare;  // four characters, or empty if never heard
    LocatorSupport support = LocatorSupport::Unknown;
    std::time_t updated = 0;
};

// A station we have decoded something from, shown in the heard stations list.
// One the operator typed in by hand is pinned: it may never have been heard
// (lastHeard stays 0), it does not age out, and only the operator removes it.
struct HeardStation
{
    std::string callsign;
    float snr = 0.0f;
    std::time_t lastHeard = 0;
    bool pinned = false;
};

} // namespace TextMessaging

#endif // TEXT_MESSAGING__TEXT_MESSAGING_TYPES_H
