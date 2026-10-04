//=========================================================================
// Name:            SnoopFeed.cpp
// Purpose:         Everything the station hears, whoever it was meant for.
//
// Written for Glissando; see SnoopFeed.h.
//=========================================================================

#include "SnoopFeed.h"

#include <cstdio>

namespace TextMessaging
{

namespace
{

float decodeSnr(uint8_t encoded)
{
    // As the protocol packs it: half dB steps, signed.
    return (float)(int8_t)encoded / 2.0f;
}

std::string heldList(uint32_t mask, int count)
{
    std::string list;
    for (int i = 0; i < count && i < 32; i++)
    {
        if ((mask & (1u << i)) == 0) continue;
        if (!list.empty()) list += ",";
        list += std::to_string(i + 1);
    }
    return list.empty() ? "none" : list;
}

} // namespace

std::string describeSnoopFrame(const SnoopEvent& event)
{
    char buffer[96];
    switch (event.frameType)
    {
        case FrameType::Ping:
            return "PING";
        case FrameType::PingAck:
            if (!event.hasReportedSnr) return "PONG";
            snprintf(buffer, sizeof(buffer), "PONG, heard at %.1f dB", (double)event.reportedSnr);
            return buffer;
        case FrameType::MessageAck:
            snprintf(buffer, sizeof(buffer), "ACK #%u", (unsigned)event.airId);
            return buffer;
        case FrameType::MessagePartialAck:
            // The payload says which fragments arrived, not how many there
            // were, so the list stops at the last one held.
            snprintf(buffer, sizeof(buffer), "PARTIAL ACK #%u, holds %s", (unsigned)event.airId,
                     heldList(event.fragmentsHeld, 8).c_str());
            return buffer;
        case FrameType::Message:
        case FrameType::Broadcast:
            snprintf(buffer, sizeof(buffer), "#%u fragment %d/%d%s", (unsigned)event.airId,
                     event.fragmentIndex + 1, event.fragmentCount, event.repeat ? " again" : "");
            return buffer;
    }
    return "?";
}

SnoopFeed::SnoopFeed()
    : myCallsignCrc_(0)
    , sequence_(0)
    , nextListenerId_(1)
{
}

void SnoopFeed::setMyCallsign(const std::string& callsign)
{
    std::lock_guard<std::mutex> lock(mutex_);
    myCallsign_ = FrameCodec::normalizeCallsign(callsign);
    myCallsignCrc_ = myCallsign_.empty() ? 0 : FrameCodec::callsignHash(myCallsign_);
    learnLocked(myCallsign_);
}

void SnoopFeed::addKnownCallsign(const std::string& callsign)
{
    std::lock_guard<std::mutex> lock(mutex_);
    learnLocked(FrameCodec::normalizeCallsign(callsign));
}

void SnoopFeed::learnLocked(const std::string& callsign)
{
    if (callsign.empty()) return;
    callsignsByCrc_[FrameCodec::callsignHash(callsign)] = callsign;
}

std::string SnoopFeed::resolveLocked(uint32_t crc, bool& known) const
{
    auto found = callsignsByCrc_.find(crc);
    known = found != callsignsByCrc_.end();
    if (known) return found->second;

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "#%06X", (unsigned)(crc & 0xFFFFFFu));
    return buffer;
}

void SnoopFeed::pruneLocked(std::time_t now)
{
    for (auto it = inbox_.begin(); it != inbox_.end();)
    {
        if (now - it->second.lastHeard > REASSEMBLY_TIMEOUT_SECONDS) it = inbox_.erase(it);
        else ++it;
    }
}

void SnoopFeed::recordLocked(SnoopEvent& event, std::vector<SnoopEvent>& out)
{
    event.sequence = ++sequence_;
    history_.push_back(event);
    while (history_.size() > HISTORY) history_.pop_front();
    out.push_back(event);
}

void SnoopFeed::onFrame(const Frame& frame, float snr, SnoopSource source, std::time_t now)
{
    std::vector<SnoopEvent> events;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Our own transmission, heard back through the radio.
        if (!myCallsign_.empty() && frame.originCallsign == myCallsign_) return;

        learnLocked(frame.originCallsign);
        pruneLocked(now);

        SnoopEvent event;
        event.kind = SnoopEvent::Kind::Frame;
        event.timestamp = now;
        event.source = source;
        event.frameType = frame.type;
        event.origin = frame.originCallsign;
        event.broadcast = frame.type == FrameType::Broadcast || frame.destinationCrc == 0;
        if (!event.broadcast)
        {
            event.destination = resolveLocked(frame.destinationCrc, event.destinationKnown);
            event.toMe = myCallsignCrc_ != 0 && frame.destinationCrc == myCallsignCrc_;
        }
        event.airId = frame.airId;
        event.fragmentIndex = frame.fragmentIndex;
        event.fragmentCount = frame.fragmentCount;
        event.burstsFollowing = frame.burstsFollowing;
        event.snr = snr;

        bool text = frame.type == FrameType::Message || frame.type == FrameType::Broadcast;
        bool wellFormed = frame.fragmentCount >= 1 && frame.fragmentCount <= 32 &&
                          frame.fragmentIndex < frame.fragmentCount;

        if (frame.type == FrameType::PingAck && !frame.payload.empty())
        {
            event.hasReportedSnr = true;
            event.reportedSnr = decodeSnr(frame.payload[0]);
        }
        else if (frame.type == FrameType::MessagePartialAck && !frame.payload.empty())
        {
            event.fragmentsHeld = frame.payload[0];
        }

        if (!text || !wellFormed)
        {
            recordLocked(event, events);
        }
        else
        {
            std::string fragment(frame.payload.begin(), frame.payload.end());
            event.text = fragment;

            Reassembly& reassembly = inbox_[Key(frame.originCallsign, frame.airId)];

            // Same ID but a different shape or different words: the sender's
            // IDs have come round again, and this is a new message.
            bool fresh = reassembly.fragments.size() != frame.fragmentCount;
            if (!fresh && reassembly.completed && reassembly.fragments[frame.fragmentIndex] != fragment)
            {
                fresh = true;
            }
            if (fresh)
            {
                reassembly = Reassembly();
                reassembly.fragments.assign(frame.fragmentCount, "");
            }

            uint32_t bit = 1u << frame.fragmentIndex;
            event.repeat = reassembly.completed || (reassembly.heldMask & bit) != 0;

            reassembly.fragments[frame.fragmentIndex] = fragment;
            reassembly.heldMask |= bit;
            reassembly.lastHeard = now;
            reassembly.snrSum += snr;
            reassembly.snrCount++;
            event.fragmentsHeld = reassembly.heldMask;

            recordLocked(event, events);

            uint32_t all = frame.fragmentCount == 32 ? 0xFFFFFFFFu : (1u << frame.fragmentCount) - 1u;
            if (!reassembly.completed && reassembly.heldMask == all)
            {
                reassembly.completed = true;

                SnoopEvent message = event;
                message.kind = SnoopEvent::Kind::Message;
                message.text.clear();
                for (const std::string& part : reassembly.fragments) message.text += part;
                message.snr = reassembly.snrSum / (float)reassembly.snrCount;
                message.repeat = false;
                recordLocked(message, events);
            }
        }
    }

    std::lock_guard<std::mutex> lock(listenersMutex_);
    for (const SnoopEvent& event : events)
    {
        for (const auto& entry : listeners_) entry.second(event);
    }
}

int SnoopFeed::addListener(Listener listener)
{
    std::lock_guard<std::mutex> lock(listenersMutex_);
    int id = nextListenerId_++;
    listeners_[id] = std::move(listener);
    return id;
}

void SnoopFeed::removeListener(int id)
{
    std::lock_guard<std::mutex> lock(listenersMutex_);
    listeners_.erase(id);
}

std::vector<SnoopEvent> SnoopFeed::recent() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<SnoopEvent>(history_.begin(), history_.end());
}

} // namespace TextMessaging
