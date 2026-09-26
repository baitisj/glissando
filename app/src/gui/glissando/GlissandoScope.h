//=========================================================================
// Name:            GlissandoScope.h
// Purpose:         The Glissando console's waterfall, the "visi-scope".
//
// A scrolling spectrogram of the receive audio, drawn as white phosphor on
// black, over just the part of the passband the melody uses. The notes of
// the scale in use are ruled across it like a musical staff, so a
// transmission reads as a tune crossing the lines, and the band the
// receiver searches around each note is shaded.
//
// It paints from a spectrum it is handed rather than computing one, so the
// console can feed it the same averaged spectrum the main window's waterfall
// uses.
//=========================================================================

#ifndef GUI_GLISSANDO__GLISSANDO_SCOPE_H
#define GUI_GLISSANDO__GLISSANDO_SCOPE_H

#include <functional>
#include <vector>

#include <wx/control.h>
#include <wx/image.h>
#include <wx/timer.h>

wxDECLARE_EVENT(EVT_GLISSANDO_SCOPE_TUNE, wxCommandEvent);

class GlissandoScope : public wxControl
{
public:
    // Fills magnitudesDb (0 loudest, minimumDb quietest) evenly spaced from
    // 0 Hz to nyquistHz; returns false when there is nothing to show (audio
    // stopped, or transmitting).
    using SpectrumSource = std::function<bool(std::vector<float>& magnitudesDb, double& nyquistHz)>;

    GlissandoScope(wxWindow* parent, wxWindowID id = wxID_ANY);

    void setSpectrumSource(SpectrumSource source) { source_ = source; }

    // Rows drawn per second: how fast history scrolls down the screen.
    void setScanRate(double rowsPerSecond);
    double scanRate() const { return scanRate_; }

    // The notes to rule across the display, their names, and the receiver's
    // search width either side of each. Both voices of a duet may be given.
    void setStaff(const std::vector<double>& notesHz, const std::vector<wxString>& names,
                  double searchHalfWidthHz);

    // Frequency span shown, in Hz.
    void setSpan(double lowHz, double highHz);

    // Draws a bright band while a frame is being decoded or sent.
    void setActivity(bool receiving, bool transmitting);

    void clear();

    // Double clicking asks to retune so the lowest note of the scale sits
    // where the click was; EVT_GLISSANDO_SCOPE_TUNE carries the clicked
    // frequency in tenths of a Hz in GetInt(). The wheel nudges by 1 Hz, as
    // the same event with GetExtraLong() set to the nudge in tenths.

private:
    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);
    void OnTimer(wxTimerEvent& event);
    void OnDoubleClick(wxMouseEvent& event);
    void OnWheel(wxMouseEvent& event);
    void OnMotion(wxMouseEvent& event);

    wxRect traceRect() const;
    double xToHz(int x) const;
    int hzToX(double hz) const;
    void addRow();

    SpectrumSource source_;
    wxTimer timer_;
    double scanRate_;
    double lowHz_;
    double highHz_;
    std::vector<double> notes_;
    std::vector<wxString> names_;
    double searchHalfWidthHz_;
    bool receiving_;
    bool transmitting_;
    int hoverX_;

    // History, newest row at the top, one byte of brightness per pixel.
    std::vector<unsigned char> history_;
    int historyWidth_;
    int historyHeight_;
    std::vector<float> spectrum_;
};

#endif // GUI_GLISSANDO__GLISSANDO_SCOPE_H
