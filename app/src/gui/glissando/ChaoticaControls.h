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

protected:
    // Never narrower than the nameplate, so a narrow window cannot cut the
    // title off.
    virtual wxSize DoGetBestSize() const override;

private:
    void OnPaint(wxPaintEvent& event);

    wxString title_;
    wxSizer* content_;
    int titleWidth_;
};

// Common painting plumbing: double buffered, plate coloured background.
class Control : public wxControl
{
public:
    Control(wxWindow* parent, wxWindowID id, const wxSize& size);

    virtual bool AcceptsFocus() const override { return false; }

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) = 0;

private:
    void OnPaint(wxPaintEvent& event);
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
    bool pressed_;
};

// A rotary knob over an arc of engraved ticks. Drag up or down, or turn the
// mouse wheel, to change it; shift for fine steps. Sends wxEVT_SLIDER.
class Dial : public Control
{
public:
    using Formatter = std::function<wxString(double)>;

    Dial(wxWindow* parent, wxWindowID id, const wxString& caption, double minimum, double maximum,
         double step, double value, const wxSize& size = wxSize(150, 170));

    double GetValue() const { return value_; }
    void SetValue(double value);
    void SetFormatter(Formatter formatter) { formatter_ = formatter; Refresh(); }

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override;

private:
    void change(double value);
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
    Formatter formatter_;
    bool dragging_;
    int dragY_;
    double dragValue_;
};

// A round lamp with a caption beside it.
class Lamp : public Control
{
public:
    Lamp(wxWindow* parent, const wxString& caption, const wxSize& size = wxSize(120, 22));

    void SetLit(bool lit);
    void SetCaption(const wxString& caption);

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override;

private:
    wxString caption_;
    bool lit_;
};

// A moving needle meter under glass.
class Meter : public Control
{
public:
    Meter(wxWindow* parent, const wxString& caption, double minimum, double maximum,
          const wxString& units, const wxSize& size = wxSize(190, 110));

    // NaN parks the needle and blanks the figure.
    void SetValue(double value);

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override;

private:
    wxString caption_;
    wxString units_;
    double minimum_;
    double maximum_;
    double value_;
};

// A caption over a number in a recessed window.
class Readout : public Control
{
public:
    Readout(wxWindow* parent, const wxString& caption, const wxSize& size = wxSize(120, 46));

    void SetText(const wxString& text);

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override;

private:
    wxString caption_;
    wxString text_;
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
// dialog; clicking anywhere else closes it without a choice.
void ShowChoices(wxWindow* anchor, const std::vector<Choice>& choices,
                 std::function<void(int)> chosen);

} // namespace Chaotica

#endif // GUI_GLISSANDO__CHAOTICA_CONTROLS_H
