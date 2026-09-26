//=========================================================================
// Name:            Data2GLink.cpp
// Purpose:         KISS, AX.25 UI and command-line formats for chat over
//                  an external data2g-host.
//=========================================================================

#include "Data2GLink.h"

#include <algorithm>
#include <cctype>

namespace TextMessaging
{
namespace Data2G
{

namespace
{

constexpr uint8_t FEND = 0xC0;
constexpr uint8_t FESC = 0xDB;
constexpr uint8_t TFEND = 0xDC;
constexpr uint8_t TFESC = 0xDD;

constexpr uint8_t AX25_UI = 0x03;
constexpr uint8_t AX25_PF = 0x10;
constexpr uint8_t AX25_PID_NONE = 0xF0;
constexpr int AX25_ADDRESS_BYTES = 7;
constexpr int AX25_MAX_DIGIPEATERS = 8;

// Longest Data2G broadcast burst, and a decode's worth of searching and
// demodulating after it has all arrived.
constexpr double BURST_SECONDS = 12.0;
constexpr double DECODE_SECONDS = 4.0;

void appendAddress(std::vector<uint8_t>& out, const Ax25Address& address, bool commandBit, bool last)
{
    std::string call = address.call;
    call.resize(6, ' ');
    for (char c : call) out.push_back((uint8_t)((uint8_t)c << 1));
    uint8_t ssid = (uint8_t)(0x60 | ((address.ssid & 0x0F) << 1));
    if (commandBit) ssid |= 0x80;
    if (last) ssid |= 0x01;
    out.push_back(ssid);
}

Ax25Address readAddress(const uint8_t* in)
{
    Ax25Address address;
    for (int i = 0; i < 6; i++)
    {
        char c = (char)(in[i] >> 1);
        if (c != ' ') address.call.push_back(c);
    }
    address.ssid = (in[6] >> 1) & 0x0F;
    return address;
}

std::string upper(std::string text)
{
    for (char& c : text) c = (char)std::toupper((unsigned char)c);
    return text;
}

} // namespace

std::vector<uint8_t> kissEncode(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> out;
    out.reserve(data.size() + 8);
    out.push_back(FEND);
    out.push_back(0x00); // data frame, port 0
    for (uint8_t b : data)
    {
        if (b == FEND)
        {
            out.push_back(FESC);
            out.push_back(TFEND);
        }
        else if (b == FESC)
        {
            out.push_back(FESC);
            out.push_back(TFESC);
        }
        else
        {
            out.push_back(b);
        }
    }
    out.push_back(FEND);
    return out;
}

void KissDecoder::feed(const uint8_t* bytes, int length, std::vector<std::vector<uint8_t>>& framesOut)
{
    for (int i = 0; i < length; i++)
    {
        uint8_t b = bytes[i];
        if (b == FEND)
        {
            // The command byte leads; the low nibble is the command, the
            // high one the port. Command 0 is data.
            if (inFrame_ && current_.size() > 1 && (current_[0] & 0x0F) == 0)
            {
                framesOut.emplace_back(current_.begin() + 1, current_.end());
            }
            current_.clear();
            escaped_ = false;
            inFrame_ = true;
            continue;
        }
        if (!inFrame_) continue;

        if (escaped_)
        {
            escaped_ = false;
            if (b == TFEND) current_.push_back(FEND);
            else if (b == TFESC) current_.push_back(FESC);
            else current_.push_back(b); // a protocol error; keep the byte as sent
            continue;
        }
        if (b == FESC)
        {
            escaped_ = true;
            continue;
        }
        current_.push_back(b);
    }
}

void KissDecoder::reset()
{
    current_.clear();
    escaped_ = false;
    inFrame_ = false;
}

Ax25Address ax25Address(const std::string& callsign)
{
    Ax25Address address;
    std::string text = upper(callsign);
    size_t dash = text.find('-');
    std::string base = text.substr(0, dash);
    for (char c : base)
    {
        if (address.call.size() == 6) break;
        if (std::isalnum((unsigned char)c)) address.call.push_back(c);
        else if (c == '/') break; // "VK2ABC/P": the suffix does not fit AX.25
    }
    if (dash != std::string::npos)
    {
        std::string ssid = text.substr(dash + 1);
        if (!ssid.empty() && ssid.size() <= 2 &&
            std::all_of(ssid.begin(), ssid.end(), [](char c) { return std::isdigit((unsigned char)c); }))
        {
            int value = std::stoi(ssid);
            if (value <= 15) address.ssid = value;
        }
    }
    if (address.call.empty()) address.call = "NOCALL";
    return address;
}

std::vector<uint8_t> ax25UiFrame(const std::string& source, const std::string& destination,
                                 const std::vector<uint8_t>& info)
{
    std::vector<uint8_t> frame;
    frame.reserve(2 * AX25_ADDRESS_BYTES + 2 + info.size());
    // A command frame: C bit set in the destination, clear in the source.
    appendAddress(frame, ax25Address(destination), true, false);
    appendAddress(frame, ax25Address(source), false, true);
    frame.push_back(AX25_UI);
    frame.push_back(AX25_PID_NONE);
    frame.insert(frame.end(), info.begin(), info.end());
    return frame;
}

bool parseAx25Ui(const std::vector<uint8_t>& frame, Ax25Ui& out)
{
    // The address field runs until an address with its low bit set.
    size_t end = 0;
    int addresses = 0;
    for (;;)
    {
        if (end + AX25_ADDRESS_BYTES > frame.size()) return false;
        addresses++;
        bool last = (frame[end + AX25_ADDRESS_BYTES - 1] & 0x01) != 0;
        end += AX25_ADDRESS_BYTES;
        if (last) break;
        if (addresses >= 2 + AX25_MAX_DIGIPEATERS) return false;
    }
    if (addresses < 2) return false;
    if (end + 2 > frame.size()) return false;
    if ((frame[end] & ~AX25_PF) != AX25_UI) return false;
    if (frame[end + 1] != AX25_PID_NONE) return false;

    out.destination = readAddress(&frame[0]);
    out.source = readAddress(&frame[AX25_ADDRESS_BYTES]);
    out.info.assign(frame.begin() + (long)end + 2, frame.end());
    return true;
}

std::vector<uint8_t> payloadForChatFrame(const std::string& myCallsign,
                                         const std::vector<uint8_t>& chatFrame)
{
    std::vector<uint8_t> info(INFO_TAG, INFO_TAG + INFO_TAG_BYTES);
    info.insert(info.end(), chatFrame.begin(), chatFrame.end());
    return ax25UiFrame(myCallsign, AX25_DESTINATION, info);
}

bool chatFrameFromPayload(const std::vector<uint8_t>& payload, std::vector<uint8_t>& chatFrameOut)
{
    Ax25Ui ui;
    if (!parseAx25Ui(payload, ui)) return false;
    if (ui.destination.call != AX25_DESTINATION) return false;
    if ((int)ui.info.size() <= INFO_TAG_BYTES) return false;
    if (!std::equal(INFO_TAG, INFO_TAG + INFO_TAG_BYTES, ui.info.begin())) return false;

    chatFrameOut.assign(ui.info.begin() + INFO_TAG_BYTES, ui.info.end());
    return true;
}

CommandEvent parseCommandLine(const std::string& line)
{
    CommandEvent event;
    std::string text = line;
    while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) start++;
    text = text.substr(start);

    size_t space = text.find(' ');
    std::string verb = upper(text.substr(0, space));
    std::string rest = space == std::string::npos ? std::string() : text.substr(space + 1);
    std::string restUpper = upper(rest);

    if ((verb == "PTT" || verb == "BUSY") && (restUpper == "ON" || restUpper == "OFF"))
    {
        event.type = verb == "PTT" ? CommandEvent::Type::Ptt : CommandEvent::Type::Busy;
        event.on = restUpper == "ON";
    }
    else if (verb == "MODE" && !rest.empty())
    {
        event.type = CommandEvent::Type::Mode;
        event.mode = rest;
    }
    return event;
}

void LineSplitter::feed(const char* bytes, int length, std::vector<std::string>& linesOut)
{
    for (int i = 0; i < length; i++)
    {
        char c = bytes[i];
        if (c == '\r' || c == '\n')
        {
            if (!current_.empty()) linesOut.push_back(current_);
            current_.clear();
        }
        else if (current_.size() < 1024)
        {
            current_.push_back(c);
        }
    }
}

AirTiming airTiming()
{
    // A whole keying fits one Data2G burst, so it counts as a single
    // "frame" as big as the largest thing chat sends at once.
    constexpr int keyingBytes = (MAX_FRAGMENTS_PER_MESSAGE + 1) * TEXT_FRAME_BYTES;
    return AirTiming::forFrameSeconds(BURST_SECONDS, keyingBytes, DECODE_SECONDS);
}

} // namespace Data2G
} // namespace TextMessaging
