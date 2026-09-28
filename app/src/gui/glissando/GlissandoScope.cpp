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

// Decoded frames kept on the trace at most, however slowly it scrolls.
constexpr size_t HEARD_LIMIT = 200;

// The time lens: the newest rows are drawn this many pixels tall, and the
// trace holds this many times the history it would without the lens.
constexpr double LENS_MAGNIFICATION = 3.0;
constexpr double LENS_SPAN_FACTOR = 8.0;

// Captions are wrapped to this fraction of the trace's width.
constexpr double CAPTION_WIDTH = 0.62;

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
    , historyRows_(0)
    , traceHeight_(0)
    , lens_(true)
    , lensK_(1.0)
    , lensTau_(1.0)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(420, 240));
    SetToolTip(_("Double click to put the scale's lowest note there. "
                 "Mouse wheel nudges the tuning by 1 Hz, shift for 0.1 Hz."));

    Bind(wxEVT_PAINT, &GlissandoScope::OnPaint, this);
    Bind(wxEVT_SIZE, &GlissandoScope::OnSize, this);
    Bind(wxEVT_TIMER, &GlissandoScope::OnTimer, this);
    Bind(wxEVT_LEFT_DCLICK, &GlissandoScope::OnDoubleClick, this);
    Bind(wxEVT_MOUSEWHEEL, &GlissandoScope::OnWheel, this);
    Bind(wxEVT_MOTION, &GlissandoScope::OnMotion, this);
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hoverX_ = -1; Refresh(); });

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
    // Old rows were drawn to the old scale and would now be in the wrong
    // place.
    clear();
}

void GlissandoScope::setActivity(bool receiving, bool transmitting)
{
    if (receiving == receiving_ && transmitting == transmitting_) return;
    receiving_ = receiving;
    transmitting_ = transmitting;
    Refresh();
}

void GlissandoScope::clear()
{
    // Decoded frames stay: they are placed by frequency and time, not by
    // pixel, so they are still in the right place on the new scale.
    std::fill(history_.begin(), history_.end(), 0);
    Refresh();
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

void GlissandoScope::setLens(bool on)
{
    if (on == lens_) return;
    lens_ = on;
    sizeHistory();
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
    if (historyWidth_ <= 0 || traceHeight_ <= 0) return;
    // The lens reaches back lensSpanSeconds(); a little more, so the bottom
    // pixel of the trace always has rows under it after a scan rate change.
    int rows = traceHeight_;
    if (lens_) rows = std::max(rows, (int)std::ceil(lensSpanSeconds() * scanRate_ * 1.1) + 2);
    if (rows == historyRows_) return;
    history_.resize((size_t)historyWidth_ * rows, 0);
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
    if (r.width != historyWidth_ || r.height != traceHeight_)
    {
        // A new width is a new picture; a new height keeps the rows.
        if (r.width != historyWidth_)
        {
            history_.clear();
            rowSeconds_.clear();
            historyRows_ = 0;
        }
        historyWidth_ = r.width;
        traceHeight_ = r.height;
        updateLens();
        sizeHistory();
    }
    Refresh();
    event.Skip();
}

void GlissandoScope::OnTimer(wxTimerEvent&)
{
    addRow();
    Refresh(false);
}

void GlissandoScope::addRow()
{
    if (historyWidth_ <= 0 || historyRows_ <= 0) return;

    // Scroll everything down one row.
    std::memmove(history_.data() + historyWidth_, history_.data(),
                 (size_t)historyWidth_ * (historyRows_ - 1));
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
    double nyquist = 0.0;
    if (!source_ || !source_(spectrum_, nyquist) || spectrum_.empty() || nyquist <= 0.0)
    {
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

    double binsPerHz = (spectrum_.size() - 1) / nyquist;
    for (int x = 0; x < historyWidth_; x++)
    {
        // Several pixels share a bin at this zoom; interpolate between bins
        // so notes are smooth lines rather than stairs.
        double hz = lowHz_ + (highHz_ - lowHz_) * x / std::max(1, historyWidth_ - 1);
        double bin = hz * binsPerHz;
        int b0 = std::min((int)spectrum_.size() - 1, std::max(0, (int)std::floor(bin)));
        int b1 = std::min((int)spectrum_.size() - 1, b0 + 1);
        double t = bin - b0;
        float db = (float)((1.0 - t) * spectrum_[b0] + t * spectrum_[b1]);
        float level = std::min(1.0f, std::max(0.0f, (db - floor) / DISPLAY_RANGE_DB));
        // A little gamma so weak signals show without the noise turning grey.
        row[x] = (unsigned char)std::lround(255.0 * std::pow(level, 1.6));
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
    int clicks = event.GetWheelRotation() / std::max(1, event.GetWheelDelta());
    if (clicks == 0) return;
    wxCommandEvent tune(EVT_GLISSANDO_SCOPE_TUNE, GetId());
    tune.SetEventObject(this);
    tune.SetInt(0);
    tune.SetExtraLong(clicks * (event.ShiftDown() ? 1 : 10));
    ProcessWindowEvent(tune);
}

void GlissandoScope::OnMotion(wxMouseEvent& event)
{
    hoverX_ = traceRect().Contains(event.GetPosition()) ? event.GetX() : -1;
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
    if (historyWidth_ == trace.width && traceHeight_ == trace.height && !history_.empty())
    {
        wxImage image(historyWidth_, traceHeight_, false);
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
    // which is what happens to most of them where the lens squeezes.
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
    auto put = [&](size_t pixel, unsigned v) {
        rgb[3 * pixel + 0] = (unsigned char)(8 + v * 224 / 255);
        rgb[3 * pixel + 1] = (unsigned char)(9 + v * 227 / 255);
        rgb[3 * pixel + 2] = (unsigned char)(12 + v * 218 / 255);
    };
    const int w = historyWidth_;

    if (!lens_ || rowSeconds_.empty() || rowSeconds_[0] <= 0.0)
    {
        int rows = std::min(traceHeight_, historyRows_);
        for (size_t i = 0; i < (size_t)w * rows; i++) put(i, history_[i]);
        for (size_t i = (size_t)w * rows; i < (size_t)w * traceHeight_; i++) put(i, 0);
        return;
    }

    std::vector<unsigned> line((size_t)w);
    double newest = rowSeconds_[0];
    for (int y = 0; y < traceHeight_; y++)
    {
        // The rows under this pixel: its top and bottom edge, in rows back.
        double r0 = timeToRow(newest - yToAge(y));
        double r1 = timeToRow(newest - yToAge(y + 1));
        size_t out = (size_t)y * w;
        if (r0 >= historyRows_ - 1)
        {
            for (int x = 0; x < w; x++) put(out + x, 0);
            continue;
        }
        if (r1 - r0 <= 1.0)
        {
            // Magnified: blend the two rows either side of the pixel's middle.
            double r = std::max(0.0, 0.5 * (r0 + r1));
            int a = std::min(historyRows_ - 1, (int)r);
            int b = std::min(historyRows_ - 1, a + 1);
            unsigned f = (unsigned)std::lround((r - a) * 256.0);
            const unsigned char* ra = history_.data() + (size_t)a * w;
            const unsigned char* rb = history_.data() + (size_t)b * w;
            for (int x = 0; x < w; x++) put(out + x, (ra[x] * (256 - f) + rb[x] * f) >> 8);
            continue;
        }
        // Squeezed: the brightest of the rows, so nothing faint is lost.
        int a = (int)r0;
        int b = std::min(historyRows_ - 1, std::max(a, (int)std::ceil(r1) - 1));
        std::fill(line.begin(), line.end(), 0u);
        for (int row = a; row <= b; row++)
        {
            const unsigned char* src = history_.data() + (size_t)row * w;
            for (int x = 0; x < w; x++) line[(size_t)x] = std::max<unsigned>(line[(size_t)x], src[x]);
        }
        for (int x = 0; x < w; x++) put(out + x, line[(size_t)x]);
    }
}
