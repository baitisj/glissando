//=========================================================================
// Name:            GlissandoScope.cpp
// Purpose:         The Glissando console's waterfall, the "visi-scope".
//=========================================================================

#include "GlissandoScope.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

#include <wx/dcbuffer.h>
#include <wx/geometry.h>
#include <wx/graphics.h>

#include "ChaoticaTheme.h"

wxDEFINE_EVENT(EVT_GLISSANDO_SCOPE_TUNE, wxCommandEvent);

using namespace Chaotica;

namespace
{

// Room around the trace for the bezel, the note names along the top, the
// frequency scale along the bottom and the time scale down the left.
constexpr int BEZEL = 10;
constexpr int TOP_MARGIN = 22;
constexpr int BOTTOM_MARGIN = 18;
constexpr int LEFT_MARGIN = 38;

// Brightness is taken relative to the row's own noise floor (its median),
// which keeps the picture steady as band noise and AGC come and go. This
// many dB above the floor is full white.
constexpr float DISPLAY_RANGE_DB = 24.0f;
constexpr float PEAK_RANGE_DB = 30.0f;

// Carrier sense: each note it counts is painted red this far either side,
// about the chord listener's bin and its neighbours, and a row drawn while
// the transmit queue was held has red edges this wide.
constexpr double SENSE_HALF_WIDTH_HZ = 3.0;
constexpr int HELD_EDGE_PIXELS = 3;

// Decoded frames kept on the trace at most, however slowly it scrolls.
constexpr size_t HEARD_LIMIT = 200;

// The time lens: the newest rows are drawn this many pixels tall, and the
// trace holds this many times the history it would without the lens.
constexpr double LENS_MAGNIFICATION = 3.0;
constexpr double LENS_SPAN_FACTOR = 8.0;

// Captions are wrapped to this fraction of the trace's width.
constexpr double CAPTION_WIDTH = 0.62;

// Blank rows of history between one row of ships and the next.
constexpr int SHIP_GAP_ROWS = 2;

// The smoke: fires behind the bezel, more of them burning as the smoke
// thickens, each letting off a puff every PUFF_EVERY seconds that lives
// PUFF_SECONDS. A plume is many faint puffs over one another, so each is
// no more than PUFF_ALPHA.
constexpr double PUFF_SECONDS = 4.0;
constexpr double PUFF_EVERY = 0.09;
constexpr double PUFF_ALPHA = 60.0;
constexpr int SMOKE_FRAME_MS = 100;

// Frames queued to be sent but still not played this long after we last
// transmitted were never going to be (the burst was dropped).
constexpr double SENT_STALE_SECONDS = 20.0;

// Pixel art, one string per line: X lit, r the rocket's exhaust, dimmer.
const char* const ROCKET[] = {
    "....X....",
    "...XXX...",
    "...XXX...",
    "..XXXXX..",
    "..X...X..",
    "..XX.XX..",
    "..XXXXX..",
    "..XXXXX..",
    ".XXXXXXX.",
    "XX.XXX.XX",
    "X..XXX..X",
    "...rrr...",
    "....r....",
};

const char* const CRAB[] = {
    "..X.....X..",
    "...X...X...",
    "..XXXXXXX..",
    ".XX.XXX.XX.",
    "XXXXXXXXXXX",
    "X.XXXXXXX.X",
    "X.X.....X.X",
    "...XX.XX...",
};

const char* const SQUID[] = {
    "...XX...",
    "..XXXX..",
    ".XXXXXX.",
    "XX.XX.XX",
    "XXXXXXXX",
    "..X..X..",
    ".X.XX.X.",
    "X.X..X.X",
};

struct Sprite
{
    const char* const* lines;
    int height;
    int width() const { return (int)std::strlen(lines[0]); }
};

template <size_t N> Sprite sprite(const char* const (&lines)[N]) { return Sprite{lines, (int)N}; }

wxFont captionTextFont()
{
    return wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE).Bold());
}

} // namespace

GlissandoScope::GlissandoScope(wxWindow* parent, wxWindowID id)
    : wxControl(parent, id, wxDefaultPosition, wxSize(560, 320), wxBORDER_NONE)
    , timer_(this)
    , scanRate_(4.0)
    , lowHz_(200.0)
    , highHz_(1100.0)
    , searchHalfWidthHz_(25.0)
    , receiving_(false)
    , transmitting_(false)
    , hoverX_(-1)
    , historyWidth_(0)
    , historyNyquistHz_(0.0)
    , historyRows_(0)
    , traceWidth_(0)
    , traceHeight_(0)
    , lens_(true)
    , lensK_(1.0)
    , lensTau_(1.0)
    , sendClock_(0.0)
    , sendEnd_(0.0)
    , sendStart_(0.0)
    , lastTick_(0.0)
    , lastTransmitting_(0.0)
    , lastQueued_(0.0)
    , gatheringHeroes_(true)
    , rowsSinceStamp_(1 << 20)
    , rowsGathering_(0)
    , shipSerial_(0)
    , smokeTimer_(this)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(420, 240));
    SetToolTip(_("Double click to put the scale's lowest note there. "
                 "Mouse wheel nudges the tuning 0.1 Hz, 10 Hz with shift, 1 Hz with control."));

    Bind(wxEVT_PAINT, &GlissandoScope::OnPaint, this);
    Bind(wxEVT_SIZE, &GlissandoScope::OnSize, this);
    Bind(wxEVT_TIMER, &GlissandoScope::OnTimer, this);
    Bind(wxEVT_LEFT_DCLICK, &GlissandoScope::OnDoubleClick, this);
    Bind(wxEVT_MOUSEWHEEL, &GlissandoScope::OnWheel, this);
    Bind(wxEVT_MOTION, &GlissandoScope::OnMotion, this);
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        hoverX_ = -1;
        mouse_ = wxPoint(-1, -1);
        Refresh();
    });

    setScanRate(scanRate_);
}

void GlissandoScope::setScanRate(double rowsPerSecond)
{
    scanRate_ = std::min(30.0, std::max(0.2, rowsPerSecond));
    timer_.Start((int)std::lround(1000.0 / scanRate_));
    updateLens();
    sizeHistory();
    Refresh();
}

void GlissandoScope::setStaff(const std::vector<double>& notesHz, const std::vector<wxString>& names,
                              double searchHalfWidthHz)
{
    notes_ = notesHz;
    names_ = names;
    searchHalfWidthHz_ = searchHalfWidthHz;
    Refresh();
}

void GlissandoScope::setSpan(double lowHz, double highHz)
{
    if (highHz <= lowHz + 10.0) return;
    if (lowHz == lowHz_ && highHz == highHz_) return;
    lowHz_ = lowHz;
    highHz_ = highHz;
    // History is kept by frequency, not by pixel, so it is simply drawn
    // again on the new scale: tuning and the duet's wider view pan and
    // zoom it rather than wiping it.
    Refresh();
}

void GlissandoScope::setActivity(bool receiving, bool transmitting)
{
    if (receiving == receiving_ && transmitting == transmitting_) return;
    receiving_ = receiving;
    transmitting_ = transmitting;
    Refresh();
}

// How many of the fires burn: a couple at the first wisp, more catching
// quickly at first and then more slowly, all of them at the thickest.
static int burningFires(double smoke, int fires)
{
    if (smoke <= 0.0) return 0;
    return std::min(fires, 2 + (int)std::floor(std::sqrt(smoke) * (fires - 2) + 0.5));
}

const std::vector<GlissandoScope::Fire>& GlissandoScope::fires()
{
    // Where they are never changes: the first to catch, then the rest in
    // the order they catch as the smoke thickens.
    static const std::vector<Fire> FIRES = {
        {Edge::Bottom, 0.62, 0.16, 0.0},
        {Edge::Top, 0.30, 0.14, 1.7},
        {Edge::Right, 0.70, 0.14, 3.1},
        {Edge::Left, 0.40, 0.14, 5.2},
        {Edge::Bottom, 0.20, 0.12, 4.4},
        {Edge::Top, 0.78, 0.12, 2.5},
        {Edge::Right, 0.25, 0.10, 0.9},
        {Edge::Left, 0.82, 0.10, 3.8},
        {Edge::Bottom, 0.88, 0.08, 1.2},
        {Edge::Top, 0.55, 0.08, 4.9},
        {Edge::Top, 0.06, 0.05, 2.2},
        {Edge::Bottom, 0.40, 0.07, 5.8},
        {Edge::Left, 0.10, 0.07, 0.4},
    };
    return FIRES;
}

double GlissandoScope::seepDensity(const Fire& fire, double along, double now)
{
    // Thick here, thin there, the pattern drifting and changing slowly,
    // and thinning out towards the ends of the stretch.
    double t = now - std::floor(now / 10000.0) * 10000.0;
    double d = 0.55 + 0.30 * std::sin(2.2 * along + 0.31 * t + fire.phase) +
               0.20 * std::sin(5.1 * along - 0.47 * t + 2.0 * fire.phase) +
               0.12 * std::sin(9.7 * along + 0.83 * t + 3.0 * fire.phase);
    d *= 1.0 - along * along * along * along;
    return std::clamp(d, 0.0, 1.0);
}

void GlissandoScope::setSmoke(double level)
{
    smoke_ = std::clamp(level, 0.0, 1.0);
    if (smoke_ > 0.0 && !smokeTimer_.IsRunning()) smokeTimer_.Start(SMOKE_FRAME_MS);
}

void GlissandoScope::tickSmoke()
{
    double now = steadySeconds();
    while (!puffs_.empty() && now - puffs_.front().born > PUFF_SECONDS) puffs_.pop_front();

    const std::vector<Fire>& all = fires();
    nextPuff_.resize(all.size(), 0.0);
    int burning = burningFires(smoke_, (int)all.size());
    for (int i = 0; i < burning; i++)
    {
        if (now < nextPuff_[i]) continue;
        unsigned hash = puffSerial_++ * 2654435761u;
        Puff puff;
        puff.fire = (size_t)i;
        puff.along = (hash >> 12 & 0xffff) / 32768.0 - 1.0;
        puff.born = now;
        puff.density = seepDensity(all[i], puff.along, now);
        puff.strength = smoke_ * puff.density;
        puff.seed = (hash >> 8 & 0xff) / 255.0 * 6.283185307179586;
        if (puff.strength > 0.02) puffs_.push_back(puff);
        nextPuff_[i] = now + PUFF_EVERY * (0.7 + 0.6 * ((hash >> 4 & 0xff) / 255.0));
    }

    // The mouse moving through the smoke shoves the puffs near it along
    // with it, the more the nearer; they drift on a little and settle, as
    // smoke does when a hand passes through it.
    double dt = std::clamp(now - lastSmokeTick_, 0.0, 0.3);
    lastSmokeTick_ = now;
    double moveX = 0.0, moveY = 0.0;
    if (dt > 0.0 && mouse_.x >= 0)
    {
        moveX = mouseMovedX_ / dt;
        moveY = mouseMovedY_ / dt;
    }
    mouseMovedX_ = mouseMovedY_ = 0.0;
    double settle = std::exp(-dt / 0.7);
    wxRect trace = traceRect();
    for (Puff& puff : puffs_)
    {
        if (moveX != 0.0 || moveY != 0.0)
        {
            double x, y, rx, ry;
            placePuff(puff, trace, now, x, y, rx, ry);
            double reach = std::max(rx, ry) + 30.0;
            double d = std::hypot(x - mouse_.x, y - mouse_.y);
            if (d < reach)
            {
                double w = (1.0 - d / reach) * (1.0 - d / reach);
                puff.driftX += 0.5 * w * moveX;
                puff.driftY += 0.5 * w * moveY;
                double speed = std::hypot(puff.driftX, puff.driftY);
                if (speed > 300.0)
                {
                    puff.driftX *= 300.0 / speed;
                    puff.driftY *= 300.0 / speed;
                }
            }
        }
        puff.pushX += puff.driftX * dt;
        puff.pushY += puff.driftY * dt;
        puff.driftX *= settle;
        puff.driftY *= settle;
    }

    if (smoke_ <= 0.0 && puffs_.empty()) smokeTimer_.Stop();
    Refresh(false);
}

void GlissandoScope::seamPoint(const wxRect& trace, const Fire& fire, double along, double& x, double& y,
                               double& outX, double& outY, double& length)
{
    // Where along the seam, and which way is out from there.
    double at = std::clamp(fire.centre + along * fire.halfLength, 0.0, 1.0);
    x = trace.x, y = trace.y, outX = outY = 0.0, length = trace.width;
    switch (fire.edge)
    {
        case Edge::Top: x = trace.x + at * trace.width, y = trace.y, outY = -1.0, length = trace.width; break;
        case Edge::Bottom:
            x = trace.x + at * trace.width, y = trace.y + trace.height, outY = 1.0, length = trace.width;
            break;
        case Edge::Right:
            x = trace.x + trace.width, y = trace.y + at * trace.height, outX = 1.0, length = trace.height;
            break;
        case Edge::Left: x = trace.x, y = trace.y + at * trace.height, outX = -1.0, length = trace.height; break;
    }
}

void GlissandoScope::placePuff(const Puff& puff, const wxRect& trace, double now, double& x, double& y,
                               double& rx, double& ry) const
{
    const Fire& fire = fires()[puff.fire];
    double age = now - puff.born;
    double x0, y0, outX, outY, length;
    seamPoint(trace, fire, puff.along, x0, y0, outX, outY, length);

    // Out of the seam, then up quickly, curling over as it goes: each puff
    // turns about a centre that rises with it, the turn widening.
    double out = 12.0 * (1.0 - std::exp(-age / 0.5));
    double rise = 11.0 * age + 3.5 * age * age;
    double curl = 2.0 + 5.0 * age;
    double turn = puff.seed + (puff.seed > 3.14159 ? 1.9 : -1.9) * age;
    x = x0 + outX * out + curl * std::sin(turn) + puff.pushX;
    y = y0 + outY * out - rise + 0.6 * curl * (std::cos(turn) - 1.0) + puff.pushY;

    // A flat sheet along the seam, thickening as it spreads: wide off the
    // top and bottom, tall off the sides.
    double longR = 0.12 * fire.halfLength * length + 10.0 + 9.0 * age;
    double shortR = 3.0 + 4.5 * age;
    bool across = fire.edge == Edge::Top || fire.edge == Edge::Bottom;
    rx = across ? longR : shortR;
    ry = across ? shortR : longR;
}

void GlissandoScope::paintSmoke(wxGraphicsContext* gc, const wxRect& trace)
{
    // Each puff is pushed out of the seam it came from, flat along it, then
    // rises, faster as it goes, swelling and paling as it thins.
    double now = steadySeconds();
    wxSize size = GetClientSize();
    const std::vector<Fire>& all = fires();
    gc->SetPen(*wxTRANSPARENT_PEN);

    for (const Puff& puff : puffs_)
    {
        double age = now - puff.born;
        double f = std::clamp(age / PUFF_SECONDS, 0.0, 1.0);

        // Where the seam seeps thickest the smoke comes out dense right at
        // the base, and only spreads thin higher up; elsewhere it gathers
        // more gently.
        double hot = std::clamp((puff.density - 0.6) / 0.4, 0.0, 1.0);
        double envelope = std::min(1.0, age / (0.6 - 0.45 * hot)) * std::pow(1.0 - f, 1.5) *
                          (1.0 + 1.6 * hot * std::exp(-age / 0.7));
        double alpha = PUFF_ALPHA * puff.strength * envelope;
        if (alpha < 0.5) continue;

        double x, y, rx, ry;
        placePuff(puff, trace, now, x, y, rx, ry);

        // Dark and close by the fire, paler as it spreads.
        double pale = std::min(1.0, age / 3.0);
        unsigned char r = (unsigned char)std::lround(120 + 95 * pale);
        unsigned char g = (unsigned char)std::lround(112 + 99 * pale);
        unsigned char b = (unsigned char)std::lround(104 + 102 * pale);

        // Thinning out before an edge of the control would cut it off.
        double room = std::min(std::min(x / rx, (size.x - x) / rx), std::min(y / ry, (size.y - y) / ry));
        double edge = std::clamp(room, 0.0, 1.0);
        unsigned char a = (unsigned char)std::lround(alpha * edge);
        if (a == 0) continue;

        // A round soft blob, squashed into the sheet's shape.
        gc->PushState();
        gc->Translate(x, y);
        gc->Scale(rx, ry);
        gc->SetBrush(gc->CreateRadialGradientBrush(0, 0, 0, 0, 1.0, wxColour(r, g, b, a), wxColour(r, g, b, 0)));
        gc->DrawEllipse(-1.0, -1.0, 2.0, 2.0);
        gc->PopState();
    }

    // And along each burning stretch, a faint warm glow in the seam, as
    // thick and thin as the smoke coming out of it.
    int burning = burningFires(smoke_, (int)all.size());
    for (int i = 0; i < burning; i++)
    {
        const Fire& fire = all[i];
        for (int k = -4; k <= 4; k++)
        {
            double along = k / 4.5;
            double x, y, outX, outY, length;
            seamPoint(trace, fire, along, x, y, outX, outY, length);
            x += 4.0 * outX, y += 4.0 * outY;
            unsigned char a = (unsigned char)std::lround(30.0 * smoke_ * seepDensity(fire, along, now));
            if (a == 0) continue;
            double radius = 0.5 * fire.halfLength * length / 4.5 + 6.0;
            gc->SetBrush(gc->CreateRadialGradientBrush(x, y, x, y, radius, wxColour(255, 140, 60, a),
                                                       wxColour(255, 90, 30, 0)));
            gc->DrawEllipse(x - radius, y - radius, 2 * radius, 2 * radius);
        }
    }
}

void GlissandoScope::clear()
{
    // Decoded frames stay: they are placed by frequency and time, not by
    // pixel.
    std::fill(history_.begin(), history_.end(), 0);
    std::fill(ships_.begin(), ships_.end(), 0);
    std::fill(sensed_.begin(), sensed_.end(), 0);
    std::fill(heldRows_.begin(), heldRows_.end(), 0);
    Refresh();
}

void GlissandoScope::setCarrierSense(bool held, const std::vector<double>& notesHz)
{
    held_ = held;
    heldHz_ = notesHz;
}

double GlissandoScope::steadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void GlissandoScope::addHeard(const GlissandoScopeFrame& frame)
{
    heard_.push_back(frame);
    while (heard_.size() > HEARD_LIMIT) heard_.pop_front();
    Refresh(false);
}

void GlissandoScope::addSent(const GlissandoScopeSent& frame)
{
    if (frame.melody.empty()) return;
    lastQueued_ = steadySeconds();
    // A second voice sings alongside the first; anything else follows on
    // from what is queued, or starts now.
    // An opening chord goes before the first frame, a closing one after the last.
    double offset = frame.voice > 0 ? sendStart_ : std::max(sendEnd_, sendClock_) + frame.leadSeconds;
    if (frame.voice == 0) sendStart_ = offset;
    sendEnd_ = std::max(sendEnd_, offset + frame.symbolSeconds * frame.melody.size() + frame.tailSeconds);
    sending_.push_back(Sending{frame, offset, 0});
}

void GlissandoScope::clearSent()
{
    sending_.clear();
    gatheringHz_.clear();
    sendEnd_ = sendStart_ = sendClock_;
}

void GlissandoScope::advanceSent(double now)
{
    double elapsed = lastTick_ > 0.0 ? std::min(1.0, std::max(0.0, now - lastTick_)) : 0.0;
    lastTick_ = now;
    if (transmitting_)
    {
        sendClock_ += elapsed;
        lastTransmitting_ = now;
    }
    else if (!sending_.empty() && now - std::max(lastTransmitting_, lastQueued_) > SENT_STALE_SECONDS)
    {
        // Measured from when we last transmitted or were handed a frame,
        // whichever is later: frames are made before the transmitter keys,
        // and one handed over after a long quiet spell is not stale.
        clearSent();
    }

    for (Sending& sending : sending_)
    {
        const GlissandoScopeSent& frame = sending.frame;
        while (sending.next < frame.melody.size() &&
               sending.offset + sending.next * frame.symbolSeconds <= sendClock_)
        {
            // A CW tail's silences are -1: nothing sung.
            if (frame.melody[sending.next] >= 0)
            {
                int note = std::min(7, frame.melody[sending.next]);
                double hz = frame.notesHz[note];
                gatheringHeroes_ = frame.heroes;
                if (std::find(gatheringHz_.begin(), gatheringHz_.end(), hz) == gatheringHz_.end())
                {
                    gatheringHz_.push_back(hz);
                }
            }
            sending.next++;
        }
    }
    while (!sending_.empty() && sending_.front().next >= sending_.front().frame.melody.size())
    {
        sending_.pop_front();
    }
}

void GlissandoScope::printShips()
{
    rowsSinceStamp_ = std::min(rowsSinceStamp_ + 1, 1 << 20);
    rowsGathering_ = gatheringHz_.empty() ? 0 : rowsGathering_ + 1;
    if (gatheringHz_.empty() || historyWidth_ <= 0 || historyNyquistHz_ <= 0.0) return;

    Sprite ship = gatheringHeroes_ ? sprite(ROCKET) : (shipSerial_ + 1) % 2 ? sprite(SQUID) : sprite(CRAB);
    if (transmitting_)
    {
        // Notes for a whole ship's height of rows, and a gap since the last.
        if (rowsGathering_ < ship.height || rowsSinceStamp_ < ship.height + SHIP_GAP_ROWS) return;
    }
    else if (rowsSinceStamp_ < ship.height)
    {
        // We have stopped sending with notes left over, too soon after the
        // last row for a row of its own: those notes already have a ship.
        for (double hz : stampedHz_)
        {
            gatheringHz_.erase(std::remove(gatheringHz_.begin(), gatheringHz_.end(), hz), gatheringHz_.end());
        }
        if (gatheringHz_.empty()) return;
    }

    // Laid over the newest rows, back to when its notes began, rather than
    // printed a line a row from now on: that way a ship stands where its
    // notes were sung, however slowly the trace scrolls. One pixel to a
    // spectrum bin, centred on its note.
    int width = ship.width();
    double binsPerHz = (historyWidth_ - 1) / historyNyquistHz_;
    for (int r = 0; r < ship.height && r < historyRows_; r++)
    {
        const char* line = ship.lines[r];
        unsigned char* row = ships_.data() + (size_t)r * historyWidth_;
        for (double hz : gatheringHz_)
        {
            int first = (int)std::lround(hz * binsPerHz) - width / 2;
            for (int i = 0; i < width; i++)
            {
                int b = first + i;
                if (line[i] == '.' || b < 0 || b >= historyWidth_) continue;
                row[b] = std::max<unsigned char>(row[b], line[i] == 'r' ? 150 : 255);
            }
        }
    }

    stampedHz_.swap(gatheringHz_);
    gatheringHz_.clear();
    rowsSinceStamp_ = 0;
    rowsGathering_ = 0;
    shipSerial_++;
}

void GlissandoScope::setLens(bool on)
{
    if (on == lens_) return;
    lens_ = on;
    Refresh();
}

double GlissandoScope::lensSpanSeconds() const
{
    return LENS_SPAN_FACTOR * traceHeight_ / scanRate_;
}

void GlissandoScope::updateLens()
{
    // K asinh(age / tau) is K / tau pixels a second at the top, which is to
    // be LENS_MAGNIFICATION times the scan rate, and reaches the bottom of
    // the trace at the lens's whole span. K * asinh(span / tau) grows with
    // tau, so a bisection finds it.
    double h = std::max(1, traceHeight_);
    double slope = LENS_MAGNIFICATION * scanRate_;
    double span = lensSpanSeconds();
    double lo = 1e-3, hi = std::max(1.0, span);
    for (int i = 0; i < 60; i++)
    {
        double tau = 0.5 * (lo + hi);
        if (slope * tau * std::asinh(span / tau) > h)
            hi = tau;
        else
            lo = tau;
    }
    lensTau_ = 0.5 * (lo + hi);
    lensK_ = slope * lensTau_;
}

double GlissandoScope::ageToY(double ageSeconds) const
{
    return lensK_ * std::asinh(ageSeconds / lensTau_);
}

double GlissandoScope::yToAge(double y) const
{
    return lensTau_ * std::sinh(y / lensK_);
}

void GlissandoScope::sizeHistory()
{
    if (traceHeight_ <= 0) return;
    // The lens reaches back lensSpanSeconds(); a little more, so the bottom
    // pixel of the trace always has rows under it after a scan rate change.
    // Kept that deep with the lens off too, so turning it off and on again
    // does not throw away everything older than one screen.
    int rows = std::max(traceHeight_, (int)std::ceil(lensSpanSeconds() * scanRate_ * 1.1) + 2);
    if (rows == historyRows_ && history_.size() == (size_t)historyWidth_ * rows) return;
    history_.resize((size_t)historyWidth_ * rows, 0);
    ships_.resize((size_t)historyWidth_ * rows, 0);
    sensed_.resize((size_t)historyWidth_ * rows, 0);
    heldRows_.resize((size_t)rows, 0);
    rowSeconds_.resize((size_t)rows, 0.0);
    historyRows_ = rows;
}

double GlissandoScope::timeToY(double seconds) const
{
    if (!lens_) return timeToRow(seconds);
    if (rowSeconds_.empty() || rowSeconds_[0] <= 0.0) return traceHeight_ + 1.0;
    double age = rowSeconds_[0] - seconds;
    if (age < 0.0) return age * LENS_MAGNIFICATION * scanRate_;
    // Past the history there is nothing to draw on.
    if (timeToRow(seconds) >= historyRows_) return traceHeight_ + 1.0;
    return ageToY(age);
}

double GlissandoScope::timeToRow(double seconds) const
{
    if (rowSeconds_.empty() || rowSeconds_[0] <= 0.0) return historyRows_;
    if (seconds >= rowSeconds_[0]) return -(seconds - rowSeconds_[0]) * scanRate_;

    // Rows are newest first: find the first drawn at or before `seconds`.
    auto it = std::lower_bound(rowSeconds_.begin(), rowSeconds_.end(), seconds,
                               [](double row, double t) { return row > t; });
    size_t i = (size_t)(it - rowSeconds_.begin());
    if (i >= rowSeconds_.size() || rowSeconds_[i] <= 0.0) return historyRows_;
    double newer = rowSeconds_[i - 1];
    double older = rowSeconds_[i];
    return (double)(i - 1) + (newer - seconds) / std::max(1e-6, newer - older);
}

wxRect GlissandoScope::traceRect() const
{
    wxSize size = GetClientSize();
    return wxRect(BEZEL + LEFT_MARGIN, BEZEL + TOP_MARGIN,
                  std::max(1, size.x - 2 * BEZEL - LEFT_MARGIN - 6),
                  std::max(1, size.y - 2 * BEZEL - TOP_MARGIN - BOTTOM_MARGIN));
}

double GlissandoScope::xToHz(int x) const
{
    wxRect r = traceRect();
    return lowHz_ + (highHz_ - lowHz_) * (x - r.x) / std::max(1, r.width - 1);
}

int GlissandoScope::hzToX(double hz) const
{
    wxRect r = traceRect();
    return r.x + (int)std::lround((hz - lowHz_) / (highHz_ - lowHz_) * (r.width - 1));
}

void GlissandoScope::OnSize(wxSizeEvent& event)
{
    wxRect r = traceRect();
    if (r.width != traceWidth_ || r.height != traceHeight_)
    {
        // History is kept in spectrum bins, so a resize keeps it all.
        traceWidth_ = r.width;
        traceHeight_ = r.height;
        updateLens();
        sizeHistory();
    }
    Refresh();
    event.Skip();
}

void GlissandoScope::OnTimer(wxTimerEvent& event)
{
    if (&event.GetTimer() == &smokeTimer_)
    {
        tickSmoke();
        return;
    }

    advanceSent(steadySeconds());
    addRow();
    Refresh(false);
}

void GlissandoScope::addRow()
{
    if (historyRows_ <= 0) return;

    double nyquist = 0.0;
    bool have = source_ && source_(spectrum_, nyquist) && !spectrum_.empty() && nyquist > 0.0;

    // A spectrum of a new shape (the sound card's rate changed) cannot be
    // drawn with the old one: start the history again.
    if (have && ((int)spectrum_.size() != historyWidth_ || nyquist != historyNyquistHz_))
    {
        historyWidth_ = (int)spectrum_.size();
        historyNyquistHz_ = nyquist;
        history_.assign((size_t)historyWidth_ * historyRows_, 0);
        ships_.assign((size_t)historyWidth_ * historyRows_, 0);
        sensed_.assign((size_t)historyWidth_ * historyRows_, 0);
    }

    // Scroll everything down one row.
    if (historyWidth_ > 0)
    {
        std::memmove(history_.data() + historyWidth_, history_.data(),
                     (size_t)historyWidth_ * (historyRows_ - 1));
        std::memmove(ships_.data() + historyWidth_, ships_.data(), (size_t)historyWidth_ * (historyRows_ - 1));
        std::fill(ships_.begin(), ships_.begin() + historyWidth_, 0);
        printShips();

        std::memmove(sensed_.data() + historyWidth_, sensed_.data(), (size_t)historyWidth_ * (historyRows_ - 1));
        unsigned char* sensed = sensed_.data();
        std::fill(sensed, sensed + historyWidth_, 0);
        double binsPerHz = (historyWidth_ - 1) / historyNyquistHz_;
        for (double hz : heldHz_)
        {
            int from = std::max(0, (int)std::ceil((hz - SENSE_HALF_WIDTH_HZ) * binsPerHz));
            int to = std::min(historyWidth_ - 1, (int)std::floor((hz + SENSE_HALF_WIDTH_HZ) * binsPerHz));
            for (int b = from; b <= to; b++) sensed[b] = 255;
        }
    }
    std::move_backward(heldRows_.begin(), heldRows_.end() - 1, heldRows_.end());
    heldRows_[0] = held_ ? 1 : 0;
    std::move_backward(rowSeconds_.begin(), rowSeconds_.end() - 1, rowSeconds_.end());
    rowSeconds_[0] = steadySeconds();

    // Frames that have scrolled off the bottom are done with.
    double oldest = rowSeconds_.back();
    while (!heard_.empty() && oldest > 0.0 &&
           heard_.front().startSeconds + heard_.front().symbolSeconds * heard_.front().melody.size() < oldest)
    {
        heard_.pop_front();
    }

    unsigned char* row = history_.data();
    if (!have)
    {
        // Nothing to show while we transmit; our ships are in ships_.
        std::fill(row, row + historyWidth_, 0);
        return;
    }

    std::vector<float> sorted(spectrum_);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    float peak = *std::max_element(spectrum_.begin(), spectrum_.end());
    // On a quiet channel (a loopback cable, or a receiver with the RF gain
    // down) the median is the display's floor and a strong melody's window
    // leakage would paint the whole row white; never let the floor sit more
    // than PEAK_RANGE_DB under the loudest bin.
    float floor = std::max(sorted[sorted.size() / 2], peak - PEAK_RANGE_DB);

    // Kept bin by bin across the whole spectrum; renderTrace() picks out
    // and stretches the part on show.
    for (int b = 0; b < historyWidth_; b++)
    {
        float level = std::min(1.0f, std::max(0.0f, (spectrum_[(size_t)b] - floor) / DISPLAY_RANGE_DB));
        // A little gamma so weak signals show without the noise turning grey.
        row[b] = (unsigned char)std::lround(255.0 * std::pow(level, 1.6));
    }
}

void GlissandoScope::OnDoubleClick(wxMouseEvent& event)
{
    if (!traceRect().Contains(event.GetPosition())) return;
    wxCommandEvent tune(EVT_GLISSANDO_SCOPE_TUNE, GetId());
    tune.SetEventObject(this);
    tune.SetInt((int)std::lround(xToHz(event.GetX()) * 10.0));
    tune.SetExtraLong(0);
    ProcessWindowEvent(tune);
}

void GlissandoScope::OnWheel(wxMouseEvent& event)
{
    // Smooth-scrolling wheels and touchpads send part clicks: save them up.
    int delta = std::max(1, event.GetWheelDelta());
    wheelRotation_ += event.GetWheelRotation();
    int clicks = wheelRotation_ / delta;
    wheelRotation_ -= clicks * delta;
    if (clicks == 0) return;
    wxCommandEvent tune(EVT_GLISSANDO_SCOPE_TUNE, GetId());
    tune.SetEventObject(this);
    tune.SetInt(0);
    // Tenths of a Hz: 0.1 Hz a click, 10 Hz with shift, 1 Hz with control,
    // as the Melody offset dial.
    tune.SetExtraLong(clicks * (event.ControlDown() ? 10 : event.ShiftDown() ? 100 : 1));
    ProcessWindowEvent(tune);
}

void GlissandoScope::OnMotion(wxMouseEvent& event)
{
    hoverX_ = traceRect().Contains(event.GetPosition()) ? event.GetX() : -1;
    if (mouse_.x >= 0 && smokeTimer_.IsRunning())
    {
        mouseMovedX_ += event.GetX() - mouse_.x;
        mouseMovedY_ += event.GetY() - mouse_.y;
    }
    mouse_ = event.GetPosition();
    Refresh(false);
}

void GlissandoScope::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(Colour::Plate));
    dc.Clear();

    wxRect trace = traceRect();

    // The trace itself, blitted as an image: phosphor white with a faint
    // blue-grey tint in the dark, like an old cathode ray tube.
    if (traceWidth_ == trace.width && traceHeight_ == trace.height && traceWidth_ > 0 && traceHeight_ > 0)
    {
        wxImage image(traceWidth_, traceHeight_, false);
        renderTrace(image);
        dc.DrawBitmap(wxBitmap(image), trace.x, trace.y);
    }

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
    if (!gc) return;

    // Bezel: a chrome frame with rounded corners around a black screen.
    gc->SetPen(wxPen(Colour::Chrome, 3));
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    gc->DrawRoundedRectangle(trace.x - 4, trace.y - 4, trace.width + 8, trace.height + 8, 8);
    gc->SetPen(wxPen(Colour::PlateShadow, 2));
    gc->DrawRoundedRectangle(trace.x - 1, trace.y - 1, trace.width + 2, trace.height + 2, 5);

    gc->Clip(trace.x, trace.y, trace.width, trace.height);

    // The staff: the receiver's search band around each note, shaded, and
    // the note itself as a dotted line.
    for (size_t i = 0; i < notes_.size(); i++)
    {
        int x0 = hzToX(notes_[i] - searchHalfWidthHz_);
        int x1 = hzToX(notes_[i] + searchHalfWidthHz_);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(255, 255, 255, 16)));
        gc->DrawRectangle(x0, trace.y, std::max(1, x1 - x0), trace.height);

        gc->SetPen(wxPen(wxColour(220, 214, 200, 110), 1, wxPENSTYLE_SHORT_DASH));
        int x = hzToX(notes_[i]);
        gc->StrokeLine(x, trace.y, x, trace.y + trace.height);
    }

    // Activity: a band across the top edge, like the screen flaring; white
    // while hearing a frame, red while on the air.
    if (receiving_ || transmitting_)
    {
        wxColour flare = transmitting_ ? Colour::Alarm : wxColour(255, 255, 255);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(gc->CreateLinearGradientBrush(
            trace.x, trace.y, trace.x, trace.y + 30,
            wxColour(flare.Red(), flare.Green(), flare.Blue(), transmitting_ ? 170 : 70),
            wxColour(flare.Red(), flare.Green(), flare.Blue(), 0)));
        gc->DrawRectangle(trace.x, trace.y, trace.width, 30);
    }

    // The lens: a sheen of glass over the magnified rows, and its rim
    // where the scale passes one row a pixel and starts to squeeze.
    if (lens_)
    {
        double rimAge = lensTau_ * std::sqrt(LENS_MAGNIFICATION * LENS_MAGNIFICATION - 1.0);
        double rim = trace.y + ageToY(rimAge);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(gc->CreateLinearGradientBrush(trace.x, trace.y, trace.x, rim,
                                                   wxColour(255, 255, 255, 24), wxColour(255, 255, 255, 0)));
        gc->DrawRectangle(trace.x, trace.y, trace.width, rim - trace.y);

        double x0 = trace.x, x1 = trace.x + trace.width, mid = trace.x + trace.width / 2.0;
        wxGraphicsPath edge = gc->CreatePath();
        edge.MoveToPoint(x0, rim - 6);
        edge.AddQuadCurveToPoint(mid, rim + 10, x1, rim - 6);
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->SetPen(wxPen(wxColour(200, 198, 192, 110), 1));
        gc->StrokePath(edge);
        wxGraphicsPath reflection = gc->CreatePath();
        reflection.MoveToPoint(x0, rim - 2);
        reflection.AddQuadCurveToPoint(mid, rim + 14, x1, rim - 2);
        gc->SetPen(wxPen(wxColour(200, 198, 192, 28), 3));
        gc->StrokePath(reflection);

        gc->SetFont(font(FontRole::Caption), wxColour(200, 198, 192, 150));
        wxString label = wxString::Format(_("TIME LENS x%.0f"), LENS_MAGNIFICATION);
        double tw = 0, th = 0;
        gc->GetTextExtent(label, &tw, &th);
        gc->DrawText(label, x1 - tw - 8, rim - th - 8);
    }

    paintHeard(gc.get(), trace);

    if (hoverX_ >= 0)
    {
        gc->SetPen(wxPen(wxColour(255, 255, 255, 140), 1));
        gc->StrokeLine(hoverX_, trace.y, hoverX_, trace.y + trace.height);
    }

    gc->ResetClip();

    // Note names along the top.
    gc->SetFont(font(FontRole::Caption), Colour::Bone);
    for (size_t i = 0; i < notes_.size() && i < names_.size(); i++)
    {
        double tw = 0, th = 0;
        gc->GetTextExtent(names_[i], &tw, &th);
        gc->DrawText(names_[i], hzToX(notes_[i]) - tw / 2, trace.y - TOP_MARGIN + 2);
    }

    // Smoke, if the transmitter has been pushed too hard, seeping out all
    // round the screen.
    if (!puffs_.empty()) paintSmoke(gc.get(), trace);

    // Frequency scale along the bottom, every 100 or 200 Hz.
    gc->SetFont(font(FontRole::Caption), Colour::Dim);
    double spacing = (highHz_ - lowHz_) > 1200 ? 200.0 : 100.0;
    for (double hz = std::ceil(lowHz_ / spacing) * spacing; hz <= highHz_; hz += spacing)
    {
        int x = hzToX(hz);
        gc->SetPen(wxPen(Colour::Dim, 1));
        gc->StrokeLine(x, trace.y + trace.height + 4, x, trace.y + trace.height + 8);
        wxString label = wxString::Format("%.0f", hz);
        double tw = 0, th = 0;
        gc->GetTextExtent(label, &tw, &th);
        gc->DrawText(label, x - tw / 2, trace.y + trace.height + 7);
    }

    // Time scale down the left: seconds ago, from the scan rate, or through
    // the lens at round ages spaced out enough to read.
    std::vector<double> ages;
    if (lens_)
    {
        static const double NICE[] = {0, 1, 2, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 240, 300, 420,
                                      600, 900, 1200, 1800, 2400, 3600, 5400, 7200};
        double lastY = -1e9;
        for (double a : NICE)
        {
            double y = ageToY(a);
            if (y > trace.height) break;
            if (y - lastY < 16) continue;
            ages.push_back(a);
            lastY = y;
        }
    }
    else
    {
        double seconds = trace.height / scanRate_;
        double step = seconds > 240 ? 60 : seconds > 60 ? 15 : seconds > 20 ? 5 : 1;
        for (double a = 0; a <= seconds; a += step) ages.push_back(a);
    }
    for (double s : ages)
    {
        int y = trace.y + (int)std::lround(lens_ ? ageToY(s) : s * scanRate_);
        gc->SetPen(wxPen(Colour::Dim, 1));
        gc->StrokeLine(trace.x - 9, y, trace.x - 5, y);
        wxString label = s >= 120 ? wxString::Format("%.0fm", s / 60) : wxString::Format("%.0fs", s);
        double tw = 0, th = 0;
        gc->GetTextExtent(label, &tw, &th);
        gc->DrawText(label, trace.x - 11 - tw, y - th / 2);
    }

    if (hoverX_ >= 0)
    {
        double hz = xToHz(hoverX_);
        wxString label = wxString::Format("%.1f Hz", hz);
        if (!notes_.empty())
        {
            size_t nearest = 0;
            for (size_t i = 1; i < notes_.size(); i++)
            {
                if (std::fabs(notes_[i] - hz) < std::fabs(notes_[nearest] - hz)) nearest = i;
            }
            if (nearest < names_.size())
            {
                label += wxString::Format("  %s %+.1f", names_[nearest], hz - notes_[nearest]);
            }
        }
        gc->SetFont(font(FontRole::Caption), Colour::Glow);
        double tw = 0, th = 0;
        gc->GetTextExtent(label, &tw, &th);
        double lx = std::min<double>(hoverX_ + 6, trace.x + trace.width - tw - 4);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(0, 0, 0, 170)));
        gc->DrawRectangle(lx - 3, trace.y + 4, tw + 6, th + 2);
        gc->DrawText(label, lx, trace.y + 5);
    }
}

void GlissandoScope::paintHeard(wxGraphicsContext* gc, const wxRect& trace)
{
    auto yOf = [&](double seconds) { return trace.y + timeToY(seconds); };
    const double bottom = trace.y + trace.height;

    struct Placed
    {
        const GlissandoScopeFrame* frame;
        double yFirst, yLast;
    };
    std::vector<Placed> placed;

    for (const GlissandoScopeFrame& frame : heard_)
    {
        const size_t symbols = frame.melody.size();
        if (symbols == 0) continue;
        const double L = frame.symbolSeconds;
        double yFirst = yOf(frame.startSeconds);                // the frame's start, lower down
        double yLast = yOf(frame.startSeconds + L * symbols);   // its end, higher up
        if (yLast > bottom || yFirst < trace.y - 400) continue;

        // The tune as the receiver heard it: each symbol's held note lit up
        // over the phosphor, as a bar along the hold where the scope
        // scrolls fast enough to give it a few rows, otherwise as a bead.
        // The glides are left to the phosphor: drawn across the trace they
        // would zigzag over everything. Motif notes are the brighter ones.
        wxGraphicsPath data = gc->CreatePath();
        wxGraphicsPath motif = gc->CreatePath();
        for (size_t k = 0; k < symbols; k++)
        {
            int note = std::min(7, std::max(0, frame.melody[k]));
            double x = hzToX(frame.notesHz[(size_t)note]);
            double holdStart = frame.startSeconds + L * (k + frame.glide);
            double y0 = yOf(holdStart);                     // lower
            double y1 = yOf(frame.startSeconds + L * (k + 1));
            if (y1 > bottom + 2 || y0 < trace.y - 2) continue;

            bool isMotif = k < frame.motif.size() && frame.motif[k];
            // Squeezed below a few tenths of a pixel a note, only the
            // motifs are kept: enough to see a frame was there.
            if (!isMotif && y0 - y1 < 0.4) continue;
            wxGraphicsPath& path = isMotif ? motif : data;
            double r = isMotif ? 2.2 : 1.5;
            if (y0 - y1 >= 2 * r + 1)
                path.AddRoundedRectangle(x - r, y1, 2 * r, y0 - y1, r);
            else
                path.AddCircle(x, (y0 + y1) / 2, r);
        }
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(232, 236, 230, 130)));
        gc->FillPath(data);
        gc->SetBrush(wxBrush(wxColour(255, 252, 240, 235)));
        gc->FillPath(motif);

        // Down the left edge, the frame's sequence: a bright bar for each
        // motif the receiver locked on to, a thin one for the data between.
        double edge = trace.x + 3;
        gc->SetPen(wxPen(wxColour(128, 124, 114, 200), 1));
        gc->StrokeLine(edge, yFirst, edge, yLast);
        for (size_t k = 0; k < symbols;)
        {
            if (k < frame.motif.size() && frame.motif[k])
            {
                size_t end = k;
                while (end < symbols && end < frame.motif.size() && frame.motif[end]) end++;
                gc->SetPen(wxPen(Colour::Glow, 3));
                gc->StrokeLine(edge, yOf(frame.startSeconds + L * k), edge, yOf(frame.startSeconds + L * end));
                k = end;
            }
            else
            {
                k++;
            }
        }

        placed.push_back({&frame, yFirst, yLast});
    }

    // Captions, newest first; one that would cover a newer one is left out,
    // which is what happens to most of them where the lens squeezes. A duet's
    // filler comes after all the rest, so the locator in its spare voice never
    // hides the message beside it: it shows only where there is room.
    std::stable_partition(placed.begin(), placed.end(), [](const Placed& p) { return p.frame->filler; });
    std::vector<wxRect2DDouble> taken;
    for (auto it = placed.rbegin(); it != placed.rend(); ++it)
    {
        const GlissandoScopeFrame& frame = *it->frame;
        double yFirst = it->yFirst, yLast = it->yLast;

        // The caption: tempo and segment, then what this frame added to the
        // chat frame, wrapped to fit.
        struct Piece
        {
            wxString text;
            wxFont font;
            wxColour colour;
            double w = 0, h = 0;
        };
        std::vector<Piece> pieces;
        pieces.push_back({frame.title, font(FontRole::Caption), Colour::Dim});
        for (const GlissandoScopeFrame::Token& token : frame.tokens)
        {
            switch (token.role)
            {
                case GlissandoScopeFrame::Role::Kind:
                    pieces.push_back({token.text, font(FontRole::Plate), Colour::Glow});
                    break;
                case GlissandoScopeFrame::Role::Station:
                    pieces.push_back({token.text, font(FontRole::Button), Colour::Bone});
                    break;
                case GlissandoScopeFrame::Role::Field:
                case GlissandoScopeFrame::Role::Unknown:
                    pieces.push_back({token.text, font(FontRole::Caption), Colour::Dim});
                    break;
                case GlissandoScopeFrame::Role::Text:
                    pieces.push_back({wxString(wxUniChar(0x201C)) + token.text + wxUniChar(0x201D), captionTextFont(),
                                      Colour::Phosphor});
                    break;
            }
        }
        if (frame.completed) pieces.push_back({_("RECEIVED"), font(FontRole::Plate), Colour::Glow});

        const double gap = 7, pad = 5;
        double maxWidth = std::max(120.0, trace.width * CAPTION_WIDTH);
        double lineH = 0, x = 0, widest = 0;
        int lines = 1;
        for (Piece& p : pieces)
        {
            gc->SetFont(p.font, p.colour);
            gc->GetTextExtent(p.text, &p.w, &p.h);
            lineH = std::max(lineH, p.h);
            if (x > 0 && x + gap + p.w > maxWidth)
            {
                lines++;
                x = 0;
            }
            x += (x > 0 ? gap : 0) + p.w;
            widest = std::max(widest, x);
        }
        double chipW = widest + 2 * pad;
        double chipH = lines * lineH + 2 * pad - 2;
        double cx = frame.voice == 0 ? trace.x + 12 : trace.x + trace.width - chipW - 8;
        double cy = (yFirst + yLast) / 2 - chipH / 2;
        cy = std::min(bottom - chipH - 2, std::max<double>(trace.y + 2, cy));
        wxRect2DDouble chip(cx, cy - 1, chipW, chipH + 2);
        bool covered = false;
        for (const wxRect2DDouble& other : taken) covered = covered || chip.Intersects(other);
        if (covered) continue;
        taken.push_back(chip);

        gc->SetBrush(wxBrush(wxColour(0, 0, 0, 180)));
        gc->SetPen(frame.completed ? wxPen(wxColour(255, 252, 240, 210), 1) : wxPen(wxColour(200, 198, 192, 80), 1));
        gc->DrawRoundedRectangle(cx, cy, chipW, chipH, 4);

        x = 0;
        double y = cy + pad - 1;
        for (const Piece& p : pieces)
        {
            if (x > 0 && x + gap + p.w > maxWidth)
            {
                x = 0;
                y += lineH;
            }
            if (x > 0) x += gap;
            gc->SetFont(p.font, p.colour);
            gc->DrawText(p.text, cx + pad + x, y + (lineH - p.h) / 2);
            x += p.w;
        }
    }
}

void GlissandoScope::renderTrace(wxImage& image)
{
    // Phosphor white with a faint blue-grey tint in the dark, like an old
    // cathode ray tube.
    unsigned char* rgb = image.GetData();
    const int w = traceWidth_;
    const int bins = historyWidth_;
    if (bins <= 0 || history_.empty() || ships_.size() != history_.size())
    {
        for (size_t i = 0; i < (size_t)w * traceHeight_; i++)
        {
            rgb[3 * i + 0] = 8;
            rgb[3 * i + 1] = 9;
            rgb[3 * i + 2] = 12;
        }
        return;
    }

    // Where each pixel column falls among the bins. Several pixels share a
    // bin at this zoom; interpolating between bins keeps notes smooth lines
    // rather than stairs. Columns beyond the spectrum stay dark.
    std::vector<int> b0s((size_t)w);
    std::vector<unsigned> fracs((size_t)w);
    double binsPerHz = (bins - 1) / historyNyquistHz_;
    for (int x = 0; x < w; x++)
    {
        double hz = lowHz_ + (highHz_ - lowHz_) * x / std::max(1, w - 1);
        double bin = hz * binsPerHz;
        if (bin < 0.0 || bin > bins - 1)
        {
            b0s[(size_t)x] = -1;
            continue;
        }
        int b0 = std::min(bins - 2, (int)bin);
        b0s[(size_t)x] = std::max(0, b0);
        fracs[(size_t)x] = (unsigned)std::lround((bin - b0s[(size_t)x]) * 256.0);
    }

    std::vector<unsigned> line((size_t)bins + 1, 0u);
    std::vector<unsigned> shipLine((size_t)bins + 1, 0u);
    std::vector<unsigned> senseLine((size_t)bins + 1, 0u);
    bool held = false;
    const bool sensing = sensed_.size() == history_.size() && heldRows_.size() == (size_t)historyRows_;
    const unsigned alarm[3] = {Chaotica::Colour::Alarm.Red(), Chaotica::Colour::Alarm.Green(),
                               Chaotica::Colour::Alarm.Blue()};
    auto putLine = [&](int y) {
        unsigned char* out = rgb + 3 * (size_t)y * w;
        for (int x = 0; x < w; x++, out += 3)
        {
            if (held && (x < HELD_EDGE_PIXELS || x >= w - HELD_EDGE_PIXELS))
            {
                for (int c = 0; c < 3; c++) out[c] = (unsigned char)alarm[c];
                continue;
            }
            int b0 = b0s[(size_t)x];
            unsigned v = 0;
            unsigned red = 0;
            if (b0 >= 0)
            {
                unsigned f = std::min(256u, fracs[(size_t)x]);
                v = (line[(size_t)b0] * (256 - f) + line[(size_t)b0 + 1] * f) >> 8;
                unsigned s = (shipLine[(size_t)b0] * (256 - f) + shipLine[(size_t)b0 + 1] * f) >> 8;
                red = (senseLine[(size_t)b0] * (256 - f) + senseLine[(size_t)b0 + 1] * f) >> 8;
                // Our ships sit behind the waterfall: they show through
                // the dark and a faint noise floor, and anything heard
                // half as bright as the ship hides it.
                unsigned cover = std::min(255u, 2 * v);
                v = std::max(v, s * (255 - cover) / 255);
            }
            unsigned rgbOut[3] = {8 + v * 224 / 255, 9 + v * 227 / 255, 12 + v * 218 / 255};
            if (red > 0)
            {
                // Where carrier sense listens: a dim red on the noise, and
                // what it hears there as bright as the alarm lamps.
                unsigned shade[3] = {70 + v * (alarm[0] - 70) / 255, 10 + v * (alarm[1] - 10) / 255,
                                     12 + v * (alarm[2] - 12) / 255};
                for (int c = 0; c < 3; c++) rgbOut[c] = (rgbOut[c] * (255 - red) + shade[c] * red) / 255;
            }
            for (int c = 0; c < 3; c++) out[c] = (unsigned char)rgbOut[c];
        }
    };
    auto rowAt = [&](int row) { return history_.data() + (size_t)row * bins; };
    auto shipsAt = [&](int row) { return ships_.data() + (size_t)row * bins; };
    auto sensedAt = [&](int row) { return sensed_.data() + (size_t)row * bins; };
    auto heldAt = [&](int row) { return sensing && heldRows_[(size_t)row] != 0; };

    bool throughLens = lens_ && !rowSeconds_.empty() && rowSeconds_[0] > 0.0;
    double newest = throughLens ? rowSeconds_[0] : 0.0;
    for (int y = 0; y < traceHeight_; y++)
    {
        std::fill(line.begin(), line.end(), 0u);
        std::fill(shipLine.begin(), shipLine.end(), 0u);
        std::fill(senseLine.begin(), senseLine.end(), 0u);
        held = false;
        if (!throughLens)
        {
            if (y < historyRows_)
            {
                const unsigned char* src = rowAt(y);
                const unsigned char* ships = shipsAt(y);
                for (int b = 0; b < bins; b++)
                {
                    line[(size_t)b] = src[b];
                    shipLine[(size_t)b] = ships[b];
                }
                if (sensing)
                {
                    const unsigned char* sensed = sensedAt(y);
                    for (int b = 0; b < bins; b++) senseLine[(size_t)b] = sensed[b];
                    held = heldAt(y);
                }
            }
            putLine(y);
            continue;
        }

        // The rows under this pixel: its top and bottom edge, in rows back.
        double r0 = timeToRow(newest - yToAge(y));
        double r1 = timeToRow(newest - yToAge(y + 1));
        if (r0 >= historyRows_ - 1)
        {
            putLine(y);
            continue;
        }
        if (r1 - r0 <= 1.0)
        {
            // Magnified: blend the two rows either side of the pixel's middle.
            double r = std::max(0.0, 0.5 * (r0 + r1));
            int a = std::min(historyRows_ - 1, (int)r);
            int b = std::min(historyRows_ - 1, a + 1);
            unsigned f = (unsigned)std::lround((r - a) * 256.0);
            const unsigned char* ra = rowAt(a);
            const unsigned char* rb = rowAt(b);
            const unsigned char* sa = shipsAt(a);
            const unsigned char* sb = shipsAt(b);
            for (int k = 0; k < bins; k++)
            {
                line[(size_t)k] = (ra[k] * (256 - f) + rb[k] * f) >> 8;
                shipLine[(size_t)k] = (sa[k] * (256 - f) + sb[k] * f) >> 8;
            }
            if (sensing)
            {
                const unsigned char* na = sensedAt(a);
                const unsigned char* nb = sensedAt(b);
                for (int k = 0; k < bins; k++) senseLine[(size_t)k] = (na[k] * (256 - f) + nb[k] * f) >> 8;
                held = heldAt(f < 128 ? a : b);
            }
            putLine(y);
            continue;
        }
        // Squeezed: the brightest of the rows, so nothing faint is lost.
        int a = (int)r0;
        int b = std::min(historyRows_ - 1, std::max(a, (int)std::ceil(r1) - 1));
        for (int row = a; row <= b; row++)
        {
            const unsigned char* src = rowAt(row);
            const unsigned char* ships = shipsAt(row);
            for (int k = 0; k < bins; k++)
            {
                line[(size_t)k] = std::max<unsigned>(line[(size_t)k], src[k]);
                shipLine[(size_t)k] = std::max<unsigned>(shipLine[(size_t)k], ships[k]);
            }
            if (sensing)
            {
                const unsigned char* sensed = sensedAt(row);
                for (int k = 0; k < bins; k++)
                    senseLine[(size_t)k] = std::max<unsigned>(senseLine[(size_t)k], sensed[k]);
                held = held || heldAt(row);
            }
        }
        putLine(y);
    }
}
