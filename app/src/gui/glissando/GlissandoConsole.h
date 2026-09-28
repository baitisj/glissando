//=========================================================================
// Name:            GlissandoConsole.h
// Purpose:         The Glissando console: a floating window holding the
//                  visi-scope, the tuning dial and the modulation controls
//                  of the Glissando melodic chirp mode, dressed as Chaotica's
//                  control room. Chat itself is in the chat window, which
//                  floats on its own.
//
// The console is the application's main window. It owns no radio or audio
// state: it asks its host (MainFrame, which stays hidden) for everything and
// tells it about every change.
//=========================================================================

#ifndef GUI_GLISSANDO__GLISSANDO_CONSOLE_H
#define GUI_GLISSANDO__GLISSANDO_CONSOLE_H

#include <vector>

#include <wx/frame.h>
#include <wx/checkbox.h>
#include <wx/panel.h>
#include <wx/spinctrl.h>
#include <wx/timer.h>

#include "GlissandoModem.h"
#include "GlissandoScope.h"

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
    int scaleDegree = 0;
    bool chorus = false;
    double tuningOffsetHz = 0.0;
    bool listenAllGears = true;     // decode every tempo, not just ours
    double scanRate = 4.0;          // visi-scope rows per second
    bool wideScope = false;         // show the duet's high voice too
    bool lens = true;               // the visi-scope's time lens: recent rows magnified, old ones kept
};

// What the console shows on its meters, gathered by the host.
struct GlissandoTelemetry
{
    bool haveReport = false;        // a frame has been heard at all
    double snrDb = 0.0;
    double dopplerHz = 0.0;
    int heardGear = 0;              // tempo of the last frame heard
    Glissando::Scale heardScale = Glissando::Scale::Pentatonic;    // and the scale it was sung in
    int heardScaleDegree = 0;
    int chorusParticipants = 0;
    int transmitScaleDegree = 0;
    double secondsSinceHeard = 0.0;
    int transmitGear = 3;           // what we would send with now
    int advisedGear = 0;            // what the last report recommends, 0 none
    bool receiving = false;         // a melody was heard just now
    bool transmitting = false;
    bool transmitBusy = false;      // a chat burst on the air, pauses included
    bool audioRunning = false;
    bool rigFrequencyKnown = false;
    double rigFrequencyHz = 0.0;
};

// The setup dialogs the console's Preferences button leads to.
enum class GlissandoSetup
{
    Options,
    AudioDevices,
    RigControl,
    Filters,
    EasySetup,
};

class IGlissandoHost
{
public:
    virtual ~IGlissandoHost() = default;

    virtual GlissandoTelemetry glissandoTelemetry() = 0;
    virtual void glissandoSettingsChanged(const GlissandoConsoleSettings& settings) = 0;

    // Averaged receive spectrum for the visi-scope; see GlissandoScope.
    virtual bool glissandoSpectrum(std::vector<float>& magnitudesDb, double& nyquistHz) = 0;

    // Frames decoded since the last call, to write on the visi-scope.
    virtual std::vector<GlissandoScopeFrame> glissandoHeardFrames() = 0;

    virtual void glissandoSetAudioRunning(bool running) = 0;

    // Stops what is being transmitted now: the radio unkeys at once and the
    // message is dropped, not retried. The audio keeps running.
    virtual void glissandoAbortTransmit() = 0;
    virtual void glissandoSetRigFrequency(double hz) = 0;

    // The operator's list of favourite dial frequencies, in Hz.
    virtual std::vector<double> glissandoFrequencyPresets() = 0;

    virtual void glissandoShowChat() = 0;

    // Every station's traffic, not only ours: shows or hides its window,
    // and says whether it is showing.
    virtual void glissandoShowSnoop(bool show) = 0;
    virtual bool glissandoSnoopShown() = 0;

    // Some setup can only change while the audio is stopped.
    virtual bool glissandoSetupAvailable(GlissandoSetup setup) = 0;
    virtual void glissandoOpenSetup(GlissandoSetup setup) = 0;

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
    void updateGearButtons();
    void refreshTelemetry();
    void selectGear(int gear);
    void selectScale(Glissando::Scale scale);
    void selectScaleDegree(int degree);
    void setTuning(double hz);
    void enterRigFrequency();
    void showFrequencyPresets();
    void showPreferences();

    void OnTimer(wxTimerEvent& event);
    void OnClose(wxCloseEvent& event);

    IGlissandoHost* host_;
    GlissandoConsoleSettings settings_;
    int sendingGear_;               // the tempo transmitting now, from telemetry
    int displayedScaleDegree_;
    wxTimer timer_;

    wxPanel* marquee_;
    GlissandoScope* scope_;
    Chaotica::Dial* tuningDial_;
    Chaotica::Dial* scanRateDial_;
    Chaotica::Readout* rigReadout_;
    Chaotica::Button* presetsButton_;
    Chaotica::Button* rigButton_;
    Chaotica::Meter* snrMeter_;
    Chaotica::Readout* dopplerReadout_;
    Chaotica::Readout* heardReadout_;
    Chaotica::Readout* heardScaleReadout_;
    Chaotica::Readout* tempoReadout_;
    Chaotica::Readout* frameReadout_;
    std::vector<Chaotica::Button*> gearButtons_;
    std::vector<Chaotica::Button*> scaleButtons_;
    Chaotica::Button* degreeButton_;
    Chaotica::Button* chorusButton_;
    Chaotica::Button* autoButton_;
    Chaotica::Button* listenAllButton_;
    Chaotica::Button* wideButton_;
    Chaotica::Button* lensButton_;
    Chaotica::Button* engageButton_;
    bool engageAborts_ = false;     // the button reads Abort
    Chaotica::Button* chatButton_;
    Chaotica::Button* snoopButton_;
    Chaotica::Button* preferencesButton_;
    Chaotica::Lamp* engagedLamp_;
    Chaotica::Lamp* receivingLamp_;
    Chaotica::Lamp* transmittingLamp_;
};

#endif // GUI_GLISSANDO__GLISSANDO_CONSOLE_H
