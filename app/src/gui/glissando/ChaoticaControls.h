//=========================================================================
// Name:            ChaoticaControls.h
// Purpose:         Hand painted controls for the Glissando console.
//
// Each control paints its own background in the plate colour, so it sits
// on a ChaoticaPanel without a seam. They send the ordinary wx events
// (wxEVT_BUTTON, wxEVT_TOGGLEBUTTON, wxEVT_SLIDER) so the console binds them
// like any other control.
//=========================================================================

#ifndef GUI_GLISSANDO__CHAOTICA_CONTROLS_H
#define GUI_GLISSANDO__CHAOTICA_CONTROLS_H

#include <cmath>
#include <functional>
#include <vector>

#include <wx/control.h>
#include <wx/panel.h>

class wxGraphicsContext;

namespace Chaotica
{

// A riveted gunmetal plate with an engraved title. Lay children out in
// GetContentSizer(); the plate leaves room for its title and bevel.
class Panel : public wxPanel
{
public:
    Panel(wxWindow* parent, const wxString& title);

    wxSizer* GetContentSizer() const { return content_; }

    // Centres the title over part of the contents (a sizer inside this
    // plate) rather than over the whole plate.
    void CentreTitleOver(wxSizer* span) { titleSpan_ = span; Refresh(); }

protected:
    // Never narrower than the nameplate, so a narrow window cannot cut the
    // title off.
    virtual wxSize DoGetBestSize() const override;

private:
    void OnPaint(wxPaintEvent& event);

    wxString title_;
    wxSizer* content_;
    wxSizer* titleSpan_ = nullptr;
    int titleWidth_;
};

// Common painting plumbing: double buffered, plate coloured background.
class Control : public wxControl
{
public:
    Control(wxWindow* parent, wxWindowID id, const wxSize& size);

    virtual bool AcceptsFocus() const override { return false; }

    // What it paints behind itself: the plate colour unless it sits on
    // something else.
    void SetBackdrop(const wxColour& colour) { backdrop_ = colour; Refresh(); }

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) = 0;

private:
    void OnPaint(wxPaintEvent& event);

    wxColour backdrop_;
};

// A bakelite push button with a chrome rim. As a toggle it stays lit while
// checked and sends wxEVT_TOGGLEBUTTON; otherwise it sends wxEVT_BUTTON.
class Button : public Control
{
public:
    Button(wxWindow* parent, wxWindowID id, const wxString& label, bool toggle = false,
           const wxSize& size = wxSize(96, 34));

    void SetLabel(const wxString& label) override;
    wxString GetLabel() const override { return label_; }
    bool IsChecked() const { return checked_; }
    void SetChecked(bool checked);

    // Faintly lit without being checked: "this is what automatic chose".
    void SetHinted(bool hinted);

    // Lit red instead of white: the button is what the operator has to press
    // before anything else can happen. Blinking is the caller's, by turning
    // this on and off.
    void SetAlarm(bool alarm);

    // A smaller second line under the label, in its own colour; empty for
    // none.
    void SetNote(const wxString& note, const wxColour& colour);
    wxString GetNote() const { return note_; }

    virtual bool Enable(bool enable = true) override;

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override;

private:
    void OnMouseDown(wxMouseEvent& event);
    void OnMouseUp(wxMouseEvent& event);
    void OnMouseLeave(wxMouseEvent& event);

    wxString label_;
    wxString note_;
    wxColour noteColour_;
    bool toggle_;
    bool checked_;
    bool hinted_;
    bool alarm_ = false;
    bool pressed_;
};

// A rotary knob over an arc of engraved ticks. Drag up or down, or turn the
// mouse wheel, to change it; shift for fine steps. Dragging past the default
// value catches on it, and a double click returns to it. Sends wxEVT_SLIDER.
class Dial : public Control
{
public:
    using Formatter = std::function<wxString(double)>;

    Dial(wxWindow* parent, wxWindowID id, const wxString& caption, double minimum, double maximum,
         double step, double value, const wxSize& size = wxSize(150, 170));

    double GetValue() const { return value_; }
    void SetValue(double value);
    void SetFormatter(Formatter formatter) { formatter_ = formatter; Refresh(); }
    // The value a double click returns to, and dragging catches on; the
    // value the dial was made with until this is called.
    void SetDefault(double value) { defaultValue_ = value; }
    // How far one click of the mouse wheel turns the dial, alone and with
    // shift or control held. Until this is called the wheel moves a
    // hundredth of the range, or one step with shift.
    void SetWheelSteps(double plain, double shift, double control);
    // The value with the pointer straight up; until this is called, halfway
    // between minimum and maximum. Either side of it the dial turns evenly,
    // so a knob can spend more of its sweep on one side.
    void SetCentre(double value) { centre_ = value; Refresh(); }

    // A push-pull knob: a click that doesn't turn it pushes it in or pops it
    // out, and sends wxEVT_TOGGLEBUTTON (GetInt() 1 for in). Double clicks
    // are two pushes rather than a return to the default.
    void SetPushable(bool pushable) { pushable_ = pushable; }
    bool IsPushed() const { return pushed_; }
    void SetPushed(bool pushed);
    // Pushed in, the ring around the knob is lit; dim says it is pushed in
    // but has nothing to go on, alarm lights it red.
    void SetRing(bool dim, bool alarm);
    bool IsDragging() const { return dragging_; }

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override;

private:
    void change(double value);
    // Where value sits along the sweep, 0 to 1, and back.
    double fractionOf(double value) const;
    double valueAt(double fraction) const;
    void OnMouseDown(wxMouseEvent& event);
    void OnMouseUp(wxMouseEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void OnMouseWheel(wxMouseEvent& event);
    void OnDoubleClick(wxMouseEvent& event);

    wxString caption_;
    double minimum_;
    double maximum_;
    double step_;
    double value_;
    double defaultValue_;
    double centre_ = NAN;
    Formatter formatter_;
    double wheelPlain_ = 0.0;
    double wheelShift_ = 0.0;
    double wheelControl_ = 0.0;
    int wheelRotation_ = 0;
    bool dragging_;
    int dragY_;
    double dragValue_;
    bool pushable_ = false;
    bool pushed_ = false;
    bool ringDim_ = false;
    bool ringAlarm_ = false;
    bool turned_ = false;           // the knob turned since the button went down
};

// A round lamp with a caption beside it.
class Lamp : public Control
{
public:
    // What colour it lights: white, or red or amber for what wants the
    // operator.
    enum class Tint
    {
        White,
        Red,
        Amber,
    };

    Lamp(wxWindow* parent, const wxString& caption, const wxSize& size = wxSize(120, 22));

    void SetLit(bool lit);
    void SetCaption(const wxString& caption);
    void SetTint(Tint tint);

    // A small bar after the caption, filled from 0 to 1; negative for none.
    void SetProgress(double fraction);

    // Clickable, it sends wxEVT_BUTTON when clicked.
    void SetClickable(bool clickable);

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override;

private:
    wxString caption_;
    bool lit_;
    Tint tint_ = Tint::White;
    double progress_ = -1.0;
    bool clickable_ = false;
};

// A moving needle meter under glass.
class Meter : public Control
{
public:
    // What the face shows: four major divisions from minimum to maximum,
    // with the ticks and labels from redFrom to redTo painted red.
    struct Scale
    {
        wxString caption;
        double minimum = 0.0;
        double maximum = 1.0;
        wxString labelFormat = "%.0f";      // the numbers on the arc
        wxString figureFormat = "%.1f";     // the reading in the corner
        double redFrom = NAN;
        double redTo = NAN;
    };

    Meter(wxWindow* parent, const Scale& scale, const wxSize& size = wxSize(190, 110));

    // NaN parks the needle and blanks the figure.
    void SetValue(double value);

    // Repaints the face with another scale; the value stays as it is.
    void SetScale(const Scale& scale);

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override;

private:
    bool red(double v) const;

    Scale scale_;
    double value_;
};

// A caption over a number in a recessed window.
class Readout : public Control
{
public:
    Readout(wxWindow* parent, const wxString& caption, const wxSize& size = wxSize(120, 46));

    void SetText(const wxString& text);

    // A caption in alarm red says the reading needs the operator's eye.
    void SetCaption(const wxString& caption, bool alarm = false);

    // Where the display window starts below the caption, so that controls
    // beside a readout can line up with the window rather than the caption.
    static int WindowTop();

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override;

private:
    wxString caption_;
    wxString text_;
    bool alarm_ = false;
};

// One entry in a drop-down of choices; see ShowChoices().
struct Choice
{
    wxString label;
    bool enabled = true;
    wxString tooltip;
};

// Drops a column of plate buttons down from under the anchor, the way a
// switchboard's patch list folds out. Choosing one closes the column and
// calls chosen with its index, from the event loop, so chosen may open a
// dialog; clicking anywhere else closes it without a choice. closed, if
// given, is called last either way: after chosen has returned, so after
// any modal dialog chosen opened has gone.
void ShowChoices(wxWindow* anchor, const std::vector<Choice>& choices,
                 std::function<void(int)> chosen, std::function<void()> closed = {});

} // namespace Chaotica

#endif // GUI_GLISSANDO__CHAOTICA_CONTROLS_H
