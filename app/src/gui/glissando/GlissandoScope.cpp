//=========================================================================
// Name:            GlissandoScope.cpp
// Purpose:         The Glissando console's waterfall, the "visi-scope".
//=========================================================================

#include "GlissandoScope.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include <wx/dcbuffer.h>
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
    , historyHeight_(0)
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
    std::fill(history_.begin(), history_.end(), 0);
    Refresh();
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
    if (r.width != historyWidth_ || r.height != historyHeight_)
    {
        historyWidth_ = r.width;
        historyHeight_ = r.height;
        history_.assign((size_t)historyWidth_ * historyHeight_, 0);
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
    if (historyWidth_ <= 0 || historyHeight_ <= 0) return;

    // Scroll everything down one row.
    std::memmove(history_.data() + historyWidth_, history_.data(),
                 (size_t)historyWidth_ * (historyHeight_ - 1));

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
    if (historyWidth_ == trace.width && historyHeight_ == trace.height && !history_.empty())
    {
        wxImage image(historyWidth_, historyHeight_, false);
        unsigned char* rgb = image.GetData();
        for (size_t i = 0; i < history_.size(); i++)
        {
            unsigned v = history_[i];
            rgb[3 * i + 0] = (unsigned char)(8 + v * 224 / 255);
            rgb[3 * i + 1] = (unsigned char)(9 + v * 227 / 255);
            rgb[3 * i + 2] = (unsigned char)(12 + v * 218 / 255);
        }
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

    // Activity: a band across the top edge, like the screen flaring.
    if (receiving_ || transmitting_)
    {
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(gc->CreateLinearGradientBrush(trace.x, trace.y, trace.x, trace.y + 30,
                                                   wxColour(255, 255, 255, transmitting_ ? 120 : 70),
                                                   wxColour(255, 255, 255, 0)));
        gc->DrawRectangle(trace.x, trace.y, trace.width, 30);
    }

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

    // Time scale down the left: seconds ago, from the scan rate.
    double seconds = trace.height / scanRate_;
    double step = seconds > 240 ? 60 : seconds > 60 ? 15 : seconds > 20 ? 5 : 1;
    for (double s = 0; s <= seconds; s += step)
    {
        int y = trace.y + (int)std::lround(s * scanRate_);
        gc->SetPen(wxPen(Colour::Dim, 1));
        gc->StrokeLine(trace.x - 9, y, trace.x - 5, y);
        wxString label = s >= 60 ? wxString::Format("%.0fm", s / 60) : wxString::Format("%.0fs", s);
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
