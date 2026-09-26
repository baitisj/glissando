//=========================================================================
// Name:            GlissandoConsole.h
// Purpose:         The Glissando console: a floating window holding the
//                  visi-scope, the tuning dial and the modulation controls
//                  of the Glissando melodic chirp mode, dressed as Chaotica's
//                  control room. Chat itself stays in the text chat window,
//                  which floats on its own.
//
// The console owns no radio or audio state. It asks its host (MainFrame) for
// everything and tells it about every change, so the same window can sit on
// top of the FreeDV main window or stand in for it.
//=========================================================================

#ifndef GUI_GLISSANDO__GLISSANDO_CONSOLE_H
#define GUI_GLISSANDO__GLISSANDO_CONSOLE_H

#include <vector>

#include <wx/frame.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include "GlissandoModem.h"

class GlissandoScope;

namespace Chaotica
{
class Button;
class Dial;
class Lamp;
class Meter;
class Readout;
}

struct GlissandoConsoleSettings
{
    int gear = 3;                   // the tempo chosen by hand, 1..5
    bool autoGear = true;           // shift tempo from the measured path
    Glissando::Scale scale = Glissando::Scale::Pentatonic;
    double tuningOffsetHz = 0.0;
    bool listenAllGears = true;     // decode every tempo, not just ours
    double scanRate = 4.0;          // visi-scope rows per second
    bool wideScope = false;         // show the duet's high voice too
};

// What the console shows on its meters, gathered by the host.
struct GlissandoTelemetry
{
    bool haveReport = false;        // a frame has been heard at all
    double snrDb = 0.0;
    double dopplerHz = 0.0;
    int heardGear = 0;              // tempo of the last frame heard
    double secondsSinceHeard = 0.0;
    int transmitGear = 3;           // what we would send with now
    int advisedGear = 0;            // what the last report recommends, 0 none
    bool receiving = false;         // a melody was heard just now
    bool transmitting = false;
    bool audioRunning = false;
    bool rigFrequencyKnown = false;
    double rigFrequencyHz = 0.0;
};

class IGlissandoHost
{
public:
    virtual ~IGlissandoHost() = default;

    virtual GlissandoTelemetry glissandoTelemetry() = 0;
    virtual void glissandoSettingsChanged(const GlissandoConsoleSettings& settings) = 0;

    // Averaged receive spectrum for the visi-scope; see GlissandoScope.
    virtual bool glissandoSpectrum(std::vector<float>& magnitudesDb, double& nyquistHz) = 0;

    virtual void glissandoSetAudioRunning(bool running) = 0;
    virtual void glissandoSetRigFrequency(double hz) = 0;
    virtual void glissandoShowChat() = 0;
    virtual void glissandoShowMainWindow(bool show) = 0;
    virtual bool glissandoMainWindowShown() = 0;

    // The console is going away (it has been closed); the host forgets it.
    virtual void glissandoConsoleClosed(const wxRect& lastPosition) = 0;
};

class GlissandoConsole : public wxFrame
{
public:
    GlissandoConsole(wxWindow* parent, IGlissandoHost* host, const GlissandoConsoleSettings& settings,
                     const wxRect& position);
    virtual ~GlissandoConsole();

    GlissandoConsoleSettings settings() const { return settings_; }

    // Human names for the tempos and scales, shared with the rest of the UI.
    static wxString gearLabel(int gear);
    static wxString scaleLabel(Glissando::Scale scale);
    static wxString noteName(double hz);

private:
    void buildControls();
    void applySettings(bool notifyHost);
    void updateStaff();
    void refreshTelemetry();
    void selectGear(int gear);
    void selectScale(Glissando::Scale scale);
    void setTuning(double hz);

    void OnTimer(wxTimerEvent& event);
    void OnClose(wxCloseEvent& event);

    IGlissandoHost* host_;
    GlissandoConsoleSettings settings_;
    wxTimer timer_;

    wxPanel* marquee_;
    GlissandoScope* scope_;
    Chaotica::Dial* tuningDial_;
    Chaotica::Dial* scanRateDial_;
    Chaotica::Readout* rigReadout_;
    Chaotica::Button* rigButton_;
    Chaotica::Meter* snrMeter_;
    Chaotica::Readout* dopplerReadout_;
    Chaotica::Readout* heardReadout_;
    Chaotica::Readout* tempoReadout_;
    Chaotica::Readout* frameReadout_;
    std::vector<Chaotica::Button*> gearButtons_;
    std::vector<Chaotica::Button*> scaleButtons_;
    Chaotica::Button* autoButton_;
    Chaotica::Button* listenAllButton_;
    Chaotica::Button* wideButton_;
    Chaotica::Button* engageButton_;
    Chaotica::Button* chatButton_;
    Chaotica::Button* mainWindowButton_;
    Chaotica::Lamp* engagedLamp_;
    Chaotica::Lamp* receivingLamp_;
    Chaotica::Lamp* transmittingLamp_;
};

#endif // GUI_GLISSANDO__GLISSANDO_CONSOLE_H
