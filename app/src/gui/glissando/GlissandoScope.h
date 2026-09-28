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
//
// Frames the receiver decodes are written back onto the trace where they
// were heard, and scroll down with it: the tune the receiver made of each
// frame, retraced in light over the phosphor with its three signature
// motifs picked out, and beside it a caption of what the frame said, read
// out as the chat frame arrives nine bytes at a time (the header fields as
// each one becomes whole, then the characters of the message).
//=========================================================================

#ifndef GUI_GLISSANDO__GLISSANDO_SCOPE_H
#define GUI_GLISSANDO__GLISSANDO_SCOPE_H

#include <array>
#include <deque>
#include <functional>
#include <vector>

#include <wx/control.h>
#include <wx/image.h>
#include <wx/timer.h>

class wxGraphicsContext;

wxDECLARE_EVENT(EVT_GLISSANDO_SCOPE_TUNE, wxCommandEvent);

// One decoded frame, to be written on the trace.
struct GlissandoScopeFrame
{
    enum class Role
    {
        Kind,       // MESSAGE, PING, ACK...
        Station,    // who from, who to
        Field,      // numbers and the like
        Text,       // the message itself
        Unknown,    // lost with an earlier frame
    };

    struct Token
    {
        Role role = Role::Field;
        wxString text;
    };

    double startSeconds = 0.0;      // steady clock, when the frame began on the air
    double symbolSeconds = 0.16;
    double glide = 0.4;             // fraction of a symbol spent gliding
    int voice = 0;                  // a duet's high voice captions on the right
    std::array<double, 8> notesHz{};
    std::vector<int> melody;        // note index of every symbol
    std::vector<bool> motif;        // true for the signature motif's symbols
    wxString title;                 // tempo and segment, e.g. "ALLEGRO  TEXT 2"
    std::vector<Token> tokens;
    bool completed = false;         // this frame finished a chat frame
};

// A frame we are sending. While it goes out its tune is printed into the
// waterfall itself as rows of pixel art ships, one in each note's column:
// Captain Proton's rocket ships for the pentatonic scale, Chaotica's
// invaders for the tritone scales.
struct GlissandoScopeSent
{
    double symbolSeconds = 0.16;
    int voice = 0;                  // a duet's second voice sings alongside the first
    bool heroes = true;             // pentatonic
    std::array<double, 8> notesHz{};
    std::vector<int> melody;        // note index of every symbol
    double leadSeconds = 0.0;       // the opening chord, sung before this frame
    double tailSeconds = 0.0;       // the closing chord, sung after it
};

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

    // The time lens; see above.
    void setLens(bool on);
    bool lens() const { return lens_; }

    // Writes a decoded frame on the trace.
    void addHeard(const GlissandoScopeFrame& frame);

    // Queues a frame about to be sent, in the order they go out. Its notes
    // are drawn as they are played, which is while setActivity() says we
    // are transmitting; clearSent() forgets what has not been played yet.
    void addSent(const GlissandoScopeSent& frame);
    void clearSent();

    // Seconds on the steady clock, the clock GlissandoScopeFrame uses.
    static double steadySeconds();

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

    // Rows of history back from the newest at which the given steady clock
    // time was drawn; fractional, negative for times newer than the newest
    // row, and at least historyRows_ for times older than the history.
    double timeToRow(double seconds) const;

    // Pixels down the trace at which a time was drawn, through the lens or
    // not; negative for times newer than the newest row.
    double timeToY(double seconds) const;

    // The lens's scale: y = lensK_ * asinh(age / lensTau_), which is
    // lensMagnification rows per scan row at the top and logarithmic further
    // down. Recomputed with the trace height and the scan rate.
    void updateLens();
    double ageToY(double ageSeconds) const;
    double yToAge(double y) const;
    double lensSpanSeconds() const;

    // Keeps enough history for what the trace shows, keeping what is there.
    void sizeHistory();
    void renderTrace(wxImage& image);
    void paintHeard(wxGraphicsContext* gc, const wxRect& trace);

    // Plays the queued sent frames forward to the transmit clock, gathering
    // the notes sung for the next row of ships.
    void advanceSent(double now);

    // Paints the next line of the row of ships being printed, if any, into
    // the newest row of history.
    void printShips(unsigned char* row);

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

    // History, newest row first, one byte of brightness per spectrum bin
    // from 0 Hz to historyNyquistHz_, whatever part of it is on show, so
    // retuning or widening the view keeps it. It holds more rows than the
    // trace is tall when the lens squeezes them in.
    std::vector<unsigned char> history_;
    int historyWidth_;              // bins per row
    double historyNyquistHz_;
    int historyRows_;
    int traceWidth_;
    int traceHeight_;

    bool lens_;
    double lensK_;
    double lensTau_;

    // When each row of history was drawn, steady clock seconds, newest
    // first; zero for rows from before the history began.
    std::vector<double> rowSeconds_;

    // Frames written on the trace, oldest first, until they scroll off.
    std::deque<GlissandoScopeFrame> heard_;
    std::vector<float> spectrum_;

    // Frames being sent, oldest first, each placed on the transmit clock:
    // seconds spent transmitting, which stands still while the radio is let
    // go between keyings.
    struct Sending
    {
        GlissandoScopeSent frame;
        double offset = 0.0;
        size_t next = 0;            // the first symbol not yet played
    };
    std::deque<Sending> sending_;
    double sendClock_;
    double sendEnd_;                // where the next frame starts on the transmit clock
    double sendStart_;              // where the last first voice started
    double lastTick_;
    double lastTransmitting_;

    // Ships are printed a line per row of history, bottom line first, so
    // they scroll into view the right way up. The notes sung while one row
    // of ships prints make up the next.
    std::vector<double> gatheringHz_;
    bool gatheringHeroes_;
    std::vector<double> printingHz_;
    bool printingHeroes_;
    int printingLine_;              // lines printed of the current row; -1 none
    int shipSerial_;
};

#endif // GUI_GLISSANDO__GLISSANDO_SCOPE_H
