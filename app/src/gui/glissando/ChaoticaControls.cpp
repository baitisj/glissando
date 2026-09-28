//=========================================================================
// Name:            ChaoticaControls.cpp
// Purpose:         Hand painted controls for the Glissando console.
//=========================================================================

#include "ChaoticaControls.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/eventfilter.h>
#include <wx/popupwin.h>
#include <wx/sizer.h>
#include <wx/tglbtn.h>

#include "ChaoticaTheme.h"

namespace Chaotica
{

namespace
{

constexpr double PI = 3.14159265358979323846;

// Space the plate leaves for its bevel and engraved title.
constexpr int PLATE_TITLE_HEIGHT = 26;
constexpr int PLATE_BORDER = 12;
constexpr double PLATE_NAME_PADDING = 36;  // nameplate beyond its lettering
constexpr double PLATE_NAME_MARGIN = 42;   // each side: edge, and some speed line

// A dial or meter sweeps 270 degrees and a meter 100, both centred on
// straight up.
constexpr double DIAL_SWEEP = 270.0 * PI / 180.0;
constexpr double METER_SWEEP = 100.0 * PI / 180.0;

} // namespace

//--------------------------------------------------------------- Panel

Panel::Panel(wxWindow* parent, const wxString& title)
    : wxPanel(parent, wxID_ANY)
    , title_(title)
    , titleWidth_(0)
{
    if (!title_.empty())
    {
        std::unique_ptr<wxGraphicsContext> gc(
            wxGraphicsRenderer::GetDefaultRenderer()->CreateMeasuringContext());
        if (gc)
        {
            gc->SetFont(font(FontRole::Plate), Colour::Chrome);
            titleWidth_ = (int)std::ceil(spacedTextWidth(gc.get(), title_, 3.0) + PLATE_NAME_PADDING +
                                         2 * PLATE_NAME_MARGIN);
        }
    }

    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(Colour::Plate);
    SetForegroundColour(Colour::Bone);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->AddSpacer(title_.empty() ? PLATE_BORDER : PLATE_TITLE_HEIGHT + 6);
    content_ = new wxBoxSizer(wxVERTICAL);
    outer->Add(content_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PLATE_BORDER);
    SetSizer(outer);

    Bind(wxEVT_PAINT, &Panel::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { Refresh(); event.Skip(); });
}

wxSize Panel::DoGetBestSize() const
{
    wxSize best = wxPanel::DoGetBestSize();
    best.x = std::max(best.x, titleWidth_);
    return best;
}

void Panel::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(Colour::Void));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
    if (!gc) return;

    wxSize size = GetClientSize();
    drawPlate(gc.get(), 0, 0, size.x, size.y, true);

    if (!title_.empty())
    {
        // An engraved nameplate: a darker inset strip, chrome lettering.
        gc->SetFont(font(FontRole::Plate), Colour::Chrome);
        double w = spacedTextWidth(gc.get(), title_, 3.0) + PLATE_NAME_PADDING;
        double x = (size.x - w) / 2.0;
        gc->SetPen(wxPen(Colour::PlateEdge, 1));
        gc->SetBrush(wxBrush(Colour::PlateShadow));
        gc->DrawRoundedRectangle(x, 7, w, PLATE_TITLE_HEIGHT - 6, 3);

        // Deco speed lines either side of the plate.
        gc->SetPen(wxPen(Colour::PlateEdge, 1));
        for (int i = 0; i < 3; i++)
        {
            double y = 12 + i * 4;
            gc->StrokeLine(18, y, x - 8, y);
            gc->StrokeLine(x + w + 8, y, size.x - 20, y);
        }

        drawSpacedTextCentred(gc.get(), title_, size.x / 2.0, 10, 3.0);
    }
}

//--------------------------------------------------------------- Control

Control::Control(wxWindow* parent, wxWindowID id, const wxSize& size)
    : wxControl(parent, id, wxDefaultPosition, size, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(size);
    Bind(wxEVT_PAINT, &Control::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { Refresh(); event.Skip(); });
}

void Control::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(Colour::Plate));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    paint(gc.get(), GetClientSize());
}

//--------------------------------------------------------------- Button

Button::Button(wxWindow* parent, wxWindowID id, const wxString& label, bool toggle,
               const wxSize& size)
    : Control(parent, id, size)
    , label_(label)
    , toggle_(toggle)
    , checked_(false)
    , hinted_(false)
    , pressed_(false)
{
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_LEFT_DOWN, &Button::OnMouseDown, this);
    Bind(wxEVT_LEFT_DCLICK, &Button::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &Button::OnMouseUp, this);
    Bind(wxEVT_LEAVE_WINDOW, &Button::OnMouseLeave, this);
}

void Button::SetLabel(const wxString& label)
{
    label_ = label;
    Refresh();
}

void Button::SetChecked(bool checked)
{
    if (checked_ == checked) return;
    checked_ = checked;
    Refresh();
}

void Button::SetNote(const wxString& note, const wxColour& colour)
{
    if (note_ == note && noteColour_ == colour) return;
    note_ = note;
    noteColour_ = colour;
    Refresh();
}

void Button::SetHinted(bool hinted)
{
    if (hinted_ == hinted) return;
    hinted_ = hinted;
    Refresh();
}

void Button::SetAlarm(bool alarm)
{
    if (alarm == alarm_) return;
    alarm_ = alarm;
    Refresh();
}

bool Button::Enable(bool enable)
{
    bool changed = Control::Enable(enable);
    Refresh();
    return changed;
}

void Button::OnMouseDown(wxMouseEvent&)
{
    if (!IsEnabled()) return;
    pressed_ = true;
    CaptureMouse();
    Refresh();
}

void Button::OnMouseUp(wxMouseEvent& event)
{
    if (HasCapture()) ReleaseMouse();
    if (!pressed_) return;
    pressed_ = false;
    Refresh();

    if (!GetClientRect().Contains(event.GetPosition()) || !IsEnabled()) return;

    if (toggle_)
    {
        checked_ = !checked_;
        wxCommandEvent toggled(wxEVT_TOGGLEBUTTON, GetId());
        toggled.SetEventObject(this);
        toggled.SetInt(checked_ ? 1 : 0);
        ProcessWindowEvent(toggled);
    }
    else
    {
        wxCommandEvent clicked(wxEVT_BUTTON, GetId());
        clicked.SetEventObject(this);
        ProcessWindowEvent(clicked);
    }
    Refresh();
}

void Button::OnMouseLeave(wxMouseEvent&)
{
    if (pressed_ && !HasCapture())
    {
        pressed_ = false;
        Refresh();
    }
}

void Button::paint(wxGraphicsContext* gc, const wxSize& size)
{
    bool alarm = alarm_ && !pressed_;
    bool lit = checked_ || pressed_ || alarm;
    double x = 3, y = 3, w = size.x - 6, h = size.y - 6;
    double r = h / 2.0;

    // Chrome rim, then the face: dark bakelite, or glowing white when lit.
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateLinearGradientBrush(x, y, x, y + h, Colour::Chrome, Colour::PlateShadow));
    gc->DrawRoundedRectangle(x, y, w, h, r);

    double inset = 2.5;
    if (alarm)
    {
        gc->SetBrush(gc->CreateLinearGradientBrush(x, y, x, y + h, Colour::Alarm, wxColour(142, 42, 32)));
    }
    else if (lit)
    {
        gc->SetBrush(gc->CreateLinearGradientBrush(x, y, x, y + h, Colour::Glow, Colour::Chrome));
    }
    else
    {
        wxColour top = hinted_ ? wxColour(70, 68, 64) : wxColour(48, 46, 45);
        gc->SetBrush(gc->CreateLinearGradientBrush(x, y, x, y + h, top, Colour::Bakelite));
    }
    gc->DrawRoundedRectangle(x + inset, y + inset + (pressed_ ? 1 : 0), w - 2 * inset,
                             h - 2 * inset, r - inset);

    wxColour ink = !IsEnabled() ? Colour::Dim : (lit ? Colour::Bakelite : Colour::Bone);
    gc->SetFont(font(FontRole::Button), ink);
    double tw = 0, th = 0;
    gc->GetTextExtent("X", &tw, &th);
    double press = pressed_ ? 1 : 0;

    if (note_.empty())
    {
        drawSpacedTextCentred(gc, label_, size.x / 2.0, (size.y - th) / 2.0 + press, 1.5);
        return;
    }

    // Label and note stacked as one block, centred.
    wxColour noteInk = IsEnabled() ? noteColour_ : Colour::Dim;
    gc->SetFont(font(FontRole::Readout), noteInk);
    double nw = 0, nh = 0;
    gc->GetTextExtent("0", &nw, &nh);
    double gap = 3;
    double top = (size.y - th - gap - nh) / 2.0 + press;

    gc->SetFont(font(FontRole::Button), ink);
    drawSpacedTextCentred(gc, label_, size.x / 2.0, top, 1.5);
    gc->SetFont(font(FontRole::Readout), noteInk);
    drawSpacedTextCentred(gc, note_, size.x / 2.0, top + th + gap, 1.0);
}

//--------------------------------------------------------------- Dial

Dial::Dial(wxWindow* parent, wxWindowID id, const wxString& caption, double minimum,
           double maximum, double step, double value, const wxSize& size)
    : Control(parent, id, size)
    , caption_(caption)
    , minimum_(minimum)
    , maximum_(maximum)
    , step_(step)
    , value_(value)
    , defaultValue_(value)
    , dragging_(false)
    , dragY_(0)
    , dragValue_(value)
{
    formatter_ = [](double v) { return wxString::Format("%.1f", v); };
    SetCursor(wxCursor(wxCURSOR_SIZENS));
    SetToolTip(_("Drag up or down, or turn the mouse wheel. Shift for fine steps; "
                 "double click to return to the default."));
    Bind(wxEVT_LEFT_DOWN, &Dial::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &Dial::OnMouseUp, this);
    Bind(wxEVT_MOTION, &Dial::OnMouseMove, this);
    Bind(wxEVT_MOUSEWHEEL, &Dial::OnMouseWheel, this);
    Bind(wxEVT_LEFT_DCLICK, &Dial::OnDoubleClick, this);
}

void Dial::SetValue(double value)
{
    value_ = std::min(maximum_, std::max(minimum_, value));
    Refresh();
}

void Dial::change(double value)
{
    value = std::round(value / step_) * step_;
    value = std::min(maximum_, std::max(minimum_, value));
    if (value == value_) return;
    value_ = value;
    Refresh();

    wxCommandEvent changed(wxEVT_SLIDER, GetId());
    changed.SetEventObject(this);
    changed.SetInt((int)std::lround(value_ * 10.0));
    ProcessWindowEvent(changed);
}

void Dial::OnMouseDown(wxMouseEvent& event)
{
    dragging_ = true;
    dragY_ = event.GetY();
    dragValue_ = value_;
    CaptureMouse();
}

void Dial::OnMouseUp(wxMouseEvent&)
{
    dragging_ = false;
    if (HasCapture()) ReleaseMouse();
}

void Dial::OnMouseMove(wxMouseEvent& event)
{
    if (!dragging_) return;
    // A full sweep of the dial for 300 pixels of travel, a tenth of that with
    // shift held.
    double perPixel = (maximum_ - minimum_) / 300.0 * (event.ShiftDown() ? 0.1 : 1.0);
    change(dragValue_ + (dragY_ - event.GetY()) * perPixel);
}

void Dial::OnMouseWheel(wxMouseEvent& event)
{
    int clicks = event.GetWheelRotation() / std::max(1, event.GetWheelDelta());
    double coarse = std::max(step_, (maximum_ - minimum_) / 100.0);
    change(value_ + clicks * (event.ShiftDown() ? step_ : coarse));
}

void Dial::OnDoubleClick(wxMouseEvent&)
{
    change(defaultValue_);
}

void Dial::paint(wxGraphicsContext* gc, const wxSize& size)
{
    double captionHeight = 18.0;
    double readoutHeight = 26.0;
    double cx = size.x / 2.0;
    double radius = std::min(size.x - 20.0, size.y - captionHeight - readoutHeight - 8.0) / 2.0;
    double cy = captionHeight + 4 + radius;

    gc->SetFont(font(FontRole::Caption), Colour::Dim);
    drawSpacedTextCentred(gc, caption_, cx, 2, 2.0);

    // Engraved ticks around the knob: 27 minor, every third one major.
    double start = -PI / 2.0 - DIAL_SWEEP / 2.0;
    for (int i = 0; i <= 27; i++)
    {
        double a = start + DIAL_SWEEP * i / 27.0;
        bool major = i % 9 == 0;
        double inner = radius * (major ? 0.80 : 0.86);
        gc->SetPen(wxPen(major ? Colour::Chrome : Colour::Dim, major ? 2 : 1));
        gc->StrokeLine(cx + inner * std::cos(a), cy + inner * std::sin(a),
                       cx + radius * 0.96 * std::cos(a), cy + radius * 0.96 * std::sin(a));
    }

    // The knob: a fluted chrome skirt and a dark cap.
    double knob = radius * 0.72;
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateRadialGradientBrush(cx - knob * 0.3, cy - knob * 0.3, cx, cy, knob,
                                               Colour::Chrome, Colour::PlateShadow));
    gc->DrawEllipse(cx - knob, cy - knob, knob * 2, knob * 2);

    gc->SetPen(wxPen(wxColour(0, 0, 0, 90), 1));
    for (int i = 0; i < 24; i++)
    {
        double a = 2.0 * PI * i / 24.0;
        gc->StrokeLine(cx + knob * 0.82 * std::cos(a), cy + knob * 0.82 * std::sin(a),
                       cx + knob * std::cos(a), cy + knob * std::sin(a));
    }

    double cap = knob * 0.74;
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateLinearGradientBrush(cx, cy - cap, cx, cy + cap, wxColour(60, 58, 56),
                                               Colour::Bakelite));
    gc->DrawEllipse(cx - cap, cy - cap, cap * 2, cap * 2);

    // The pointer, a lit slot in the cap.
    double fraction = (value_ - minimum_) / std::max(1e-9, maximum_ - minimum_);
    double a = start + DIAL_SWEEP * fraction;
    gc->SetPen(wxPen(Colour::Glow, 3, wxPENSTYLE_SOLID));
    gc->StrokeLine(cx + cap * 0.25 * std::cos(a), cy + cap * 0.25 * std::sin(a),
                   cx + cap * 0.92 * std::cos(a), cy + cap * 0.92 * std::sin(a));
    drawGlow(gc, cx + cap * 0.8 * std::cos(a), cy + cap * 0.8 * std::sin(a), 9, 0.5);

    // Recessed readout window.
    double rw = std::min(size.x - 10.0, 120.0);
    double ry = size.y - readoutHeight;
    gc->SetPen(wxPen(Colour::PlateEdge, 1));
    gc->SetBrush(wxBrush(Colour::PlateShadow));
    gc->DrawRoundedRectangle(cx - rw / 2, ry, rw, readoutHeight - 3, 3);
    gc->SetFont(font(FontRole::Readout), Colour::Glow);
    wxString text = formatter_(value_);
    double tw = 0, th = 0;
    gc->GetTextExtent(text, &tw, &th);
    gc->DrawText(text, cx - tw / 2, ry + (readoutHeight - 3 - th) / 2);
}

//--------------------------------------------------------------- Lamp

Lamp::Lamp(wxWindow* parent, const wxString& caption, const wxSize& size)
    : Control(parent, wxID_ANY, size)
    , caption_(caption)
    , lit_(false)
{
    // empty
}

void Lamp::SetLit(bool lit)
{
    if (lit_ == lit) return;
    lit_ = lit;
    Refresh();
}

void Lamp::SetCaption(const wxString& caption)
{
    if (caption_ == caption) return;
    caption_ = caption;
    Refresh();
}

void Lamp::paint(wxGraphicsContext* gc, const wxSize& size)
{
    double r = std::min(size.y / 2.0 - 3.0, 7.0);
    double cx = r + 5, cy = size.y / 2.0;

    if (lit_) drawGlow(gc, cx, cy, std::min(r * 3.0, size.y / 2.0), 0.9);

    gc->SetPen(wxPen(Colour::Chrome, 1.5));
    gc->SetBrush(lit_ ? gc->CreateRadialGradientBrush(cx - r * 0.3, cy - r * 0.3, cx, cy, r,
                                                      Colour::Glow, Colour::Chrome)
                      : gc->CreateRadialGradientBrush(cx - r * 0.3, cy - r * 0.3, cx, cy, r,
                                                      wxColour(70, 68, 66), Colour::Bakelite));
    gc->DrawEllipse(cx - r, cy - r, r * 2, r * 2);

    gc->SetFont(font(FontRole::Button), lit_ ? Colour::Glow : Colour::Dim);
    double tw = 0, th = 0;
    gc->GetTextExtent("X", &tw, &th);
    drawSpacedText(gc, caption_, cx + r + 8, cy - th / 2, 1.5);
}

//--------------------------------------------------------------- Meter

Meter::Meter(wxWindow* parent, const wxString& caption, double minimum, double maximum,
             const wxString& units, const wxSize& size)
    : Control(parent, wxID_ANY, size)
    , caption_(caption)
    , units_(units)
    , minimum_(minimum)
    , maximum_(maximum)
    , value_(std::nan(""))
{
    // empty
}

void Meter::SetValue(double value)
{
    if ((std::isnan(value) && std::isnan(value_)) || value == value_) return;
    value_ = value;
    Refresh();
}

void Meter::paint(wxGraphicsContext* gc, const wxSize& size)
{
    // Glass faced dial, ivory scale, black needle: the one bright thing on
    // the console besides the lamps, as a real meter face would be.
    double x = 4, y = 4, w = size.x - 8, h = size.y - 8;
    gc->SetPen(wxPen(Colour::Chrome, 2));
    gc->SetBrush(gc->CreateLinearGradientBrush(x, y, x, y + h, Colour::Bone, wxColour(180, 174, 160)));
    gc->DrawRoundedRectangle(x, y, w, h, 6);

    // The needle pivots just above the bottom edge and the scale is an arc
    // over it; labels sit inside the arc, the caption under them.
    double cx = size.x / 2.0;
    double cy = y + h - 10;
    double radius = std::min(w / 2.0 - 12.0, (h - 14.0) / 0.98);
    double start = -PI / 2.0 - METER_SWEEP / 2.0;

    gc->SetFont(font(FontRole::Caption), Colour::Bakelite);
    int majors = 4;
    for (int i = 0; i <= majors * 5; i++)
    {
        double a = start + METER_SWEEP * i / (majors * 5.0);
        bool major = i % 5 == 0;
        double inner = radius * (major ? 0.86 : 0.92);
        gc->SetPen(wxPen(Colour::Bakelite, major ? 2 : 1));
        gc->StrokeLine(cx + inner * std::cos(a), cy + inner * std::sin(a),
                       cx + radius * std::cos(a), cy + radius * std::sin(a));
        if (major)
        {
            double v = minimum_ + (maximum_ - minimum_) * i / (majors * 5.0);
            wxString label = wxString::Format("%+.0f", v);
            double tw = 0, th = 0;
            gc->GetTextExtent(label, &tw, &th);
            double lr = radius * 0.74;
            gc->DrawText(label, cx + lr * std::cos(a) - tw / 2, cy + lr * std::sin(a) - th / 2);
        }
    }

    double tw = 0, th = 0;
    gc->SetFont(font(FontRole::Plate), Colour::Bakelite);
    gc->GetTextExtent("X", &tw, &th);
    drawSpacedText(gc, caption_, x + 8, y + h - th - 5, 2.0);

    wxString figure = std::isnan(value_) ? wxString("---")
                                         : wxString::Format("%+.1f %s", value_, units_);
    gc->GetTextExtent(figure, &tw, &th);
    gc->DrawText(figure, x + w - tw - 8, y + h - th - 5);

    double v = std::isnan(value_) ? minimum_ : std::min(maximum_, std::max(minimum_, value_));
    double a = start + METER_SWEEP * (v - minimum_) / std::max(1e-9, maximum_ - minimum_);
    gc->SetPen(wxPen(Colour::Bakelite, 2));
    gc->StrokeLine(cx, cy, cx + radius * 0.98 * std::cos(a), cy + radius * 0.98 * std::sin(a));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(Colour::Bakelite));
    gc->DrawEllipse(cx - 4, cy - 4, 8, 8);

    // Glass highlight.
    gc->SetBrush(gc->CreateLinearGradientBrush(x, y, x, y + h * 0.4, wxColour(255, 255, 255, 60),
                                               wxColour(255, 255, 255, 0)));
    gc->DrawRoundedRectangle(x + 3, y + 3, w - 6, h * 0.4, 5);
}

//--------------------------------------------------------------- Readout

Readout::Readout(wxWindow* parent, const wxString& caption, const wxSize& size)
    : Control(parent, wxID_ANY, size)
    , caption_(caption)
    , text_("---")
{
    // empty
}

void Readout::SetText(const wxString& text)
{
    if (text_ == text) return;
    text_ = text;
    Refresh();
}

int Readout::WindowTop()
{
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsRenderer::GetDefaultRenderer()->CreateMeasuringContext());
    if (!gc) return 16;

    gc->SetFont(font(FontRole::Caption), Colour::Dim);
    double tw = 0, th = 0;
    gc->GetTextExtent("X", &tw, &th);
    return (int)std::ceil(th + 4);
}

void Readout::paint(wxGraphicsContext* gc, const wxSize& size)
{
    gc->SetFont(font(FontRole::Caption), Colour::Dim);
    drawSpacedText(gc, caption_, 4, 1, 1.5);

    double tw = 0, th = 0;
    double y = WindowTop();
    gc->SetPen(wxPen(Colour::PlateEdge, 1));
    gc->SetBrush(wxBrush(Colour::PlateShadow));
    gc->DrawRoundedRectangle(2, y, size.x - 4, size.y - y - 2, 3);

    gc->SetFont(font(FontRole::Readout), Colour::Glow);
    gc->GetTextExtent(text_, &tw, &th);
    gc->DrawText(text_, 8, y + (size.y - y - 2 - th) / 2);
}

//--------------------------------------------------------------- Choices

namespace
{

// A plain popup rather than wxPopupTransientWindow: the transient kind holds
// a pointer grab on GTK that swallows the buttons' own mouse capture, so a
// click on a choice never arrives. Closing on a click elsewhere is done with
// an event filter instead.
class ChoicePopup : public wxPopupWindow, public wxEventFilter
{
public:
    ChoicePopup(wxWindow* anchor, const std::vector<Choice>& choices, std::function<void(int)> chosen)
        : wxPopupWindow(anchor, wxBORDER_NONE)
        , anchor_(anchor)
        , chosen_(std::move(chosen))
    {
        SetBackgroundColour(Colour::PlateEdge);
        auto* panel = new wxPanel(this);
        panel->SetBackgroundColour(Colour::Plate);
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        int width = std::max(anchor->GetSize().GetWidth(), 150);
        for (size_t i = 0; i < choices.size(); i++)
        {
            auto* button = new Button(panel, wxID_ANY, choices[i].label, false, wxSize(width, 32));
            button->Enable(choices[i].enabled);
            if (!choices[i].tooltip.empty()) button->SetToolTip(choices[i].tooltip);
            int index = (int)i;
            button->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) { close(index); });
            sizer->Add(button, 0, wxALL, 3);
        }
        panel->SetSizer(sizer);

        auto* frame = new wxBoxSizer(wxVERTICAL);
        frame->Add(panel, 1, wxEXPAND | wxALL, 1);
        SetSizerAndFit(frame);

        wxEvtHandler::AddFilter(this);
    }

    virtual ~ChoicePopup()
    {
        wxEvtHandler::RemoveFilter(this);
    }

    virtual int FilterEvent(wxEvent& event) override
    {
        if (closing_) return Event_Skip;

        wxEventType type = event.GetEventType();
        if (type == wxEVT_LEFT_DOWN || type == wxEVT_RIGHT_DOWN || type == wxEVT_MIDDLE_DOWN)
        {
            // A press anywhere but on the column closes it; a press on the
            // anchor closes it and is eaten, so the anchor does not at once
            // open it again.
            wxWindow* window = wxDynamicCast(event.GetEventObject(), wxWindow);
            if (!isInside(window))
            {
                bool onAnchor = window == anchor_;
                close(-1);
                return onAnchor ? Event_Processed : Event_Skip;
            }
        }
        else if (type == wxEVT_CHAR_HOOK && static_cast<wxKeyEvent&>(event).GetKeyCode() == WXK_ESCAPE)
        {
            close(-1);
            return Event_Processed;
        }
        else if (type == wxEVT_ACTIVATE && !static_cast<wxActivateEvent&>(event).GetActive() &&
                 event.GetEventObject() == wxGetTopLevelParent(anchor_))
        {
            close(-1);
        }
        return Event_Skip;
    }

private:
    bool isInside(wxWindow* window) const
    {
        for (; window != nullptr; window = window->GetParent())
        {
            if (window == this) return true;
        }
        return false;
    }

    void close(int index)
    {
        if (closing_) return;
        closing_ = true;
        Hide();

        // Called back once the column has gone, so a dialog it opens comes
        // up over the console rather than under a closing popup.
        if (index >= 0) anchor_->CallAfter([chosen = chosen_, index]() { chosen(index); });
        CallAfter([this]() { Destroy(); });
    }

    wxWindow* anchor_;
    std::function<void(int)> chosen_;
    bool closing_ = false;
};

} // namespace

void ShowChoices(wxWindow* anchor, const std::vector<Choice>& choices, std::function<void(int)> chosen)
{
    auto* popup = new ChoicePopup(anchor, choices, std::move(chosen));
    wxPoint below = anchor->ClientToScreen(wxPoint(0, anchor->GetSize().GetHeight() + 2));
    popup->Position(below, wxSize(0, 0));
    popup->Show();
}

} // namespace Chaotica
