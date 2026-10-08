//==========================================================================
// Name:            main.h
//
// Purpose:         Declares simple wxWidgets application with GUI.
// Created:         Apr. 9, 2012
// Authors:         David Rowe, David Witten
//
// License:
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//==========================================================================
#ifndef __FDMDV2_MAIN__
#define __FDMDV2_MAIN__

#include "config.h"
#include <wx/wx.h>

#include <wx/tglbtn.h>
#include <wx/app.h>
#include "wx/rawbmp.h"
#include "wx/file.h"
#include "wx/filename.h"
#include "wx/config.h"
#include <wx/fileconf.h>
#include "wx/graphics.h"
#include "wx/mstream.h"
#include "wx/wfstream.h"
#include "wx/quantize.h"
#include "wx/scopedptr.h"
#include "wx/stopwatch.h"
#include "wx/versioninfo.h"
#include <wx/sound.h>
#include <wx/url.h>
#include <wx/sstream.h>
#include <wx/listbox.h>
#include <wx/textdlg.h>
#include <wx/regex.h>
#include <wx/socket.h>
#include <wx/numformatter.h>

#include <stdint.h>
#include <future>
#include <map>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#include <cpuid.h>
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#endif

#ifdef _MSC_VER
// used for AVX checking
#include <intrin.h>
#endif

#include "defines.h"

#include "topFrame.h"
#include "gui/dialogs/filter_frequency.h"
#include "gui/dialogs/tot_warning.h"
#include "gui/controls/plot.h"
#include "gui/controls/plot_scalar.h"
#include "gui/glissando/GlissandoConsole.h"
#include "text_messaging/Data2GTransport.h"
#include "text_messaging/TextMessagingTypes.h"
#include "sndfile.h"
#include "comp_prim.h"
#include "rig_control/DriveServo.h"
#include "rig_control/SmokeGauge.h"
#include "rig_control/HamlibRigController.h"
#include "rig_control/SerialPortOutRigController.h"
#include "rig_control/SerialPortInRigController.h"
#include "reporting/IReporter.h"
#include "audio/AudioEngineFactory.h"
#include "audio/IAudioDevice.h"
#include "config/FreeDVConfiguration.h"
#include "pipeline/paCallbackData.h"
#include "pipeline/LinkStep.h"
#include "freedv_sanitizers.h"

#define _USE_TIMER              1
#define _USE_ONIDLE             1
#define _DUMMY_DATA             1
//#define _AUDIO_PASSTHROUGH    1
#define _REFRESH_TIMER_PERIOD   (DT*1000)


//#define _USE_ABOUT_DIALOG       1

enum {
        ID_START = wxID_HIGHEST,
        ID_TIMER_DEMOD_IN,
        ID_TIMER_UPDATE_OTHER,
        ID_TIMER_UPD_FREQ,
        ID_TIMER_TOT,           // Time-Out Timer
        ID_TIMER_TOT_WARNING,   // Polls remaining TOT time to show warning
     };

#define EXCHANGE_DATA_IN    0
#define EXCHANGE_DATA_OUT   1

extern int                 g_nSoundCards;

// Last-used configuration file helpers.
// The path is stored in a platform-appropriate state store
// (registry on Windows, file on macOS/Linux) that is independent
// of the main application config so it can always be read and written
// regardless of which config backend is currently active.
wxString  getLastUsedConfigPath();
void      saveLastUsedConfigPath(const wxString& path);
void      clearLastUsedConfigPath();

// "Detect Sync" state machine states and constants

#define DS_IDLE           0
#define DS_SYNC_WAIT      1
#define DS_UNSYNC_WAIT    2
#define DS_SYNC_WAIT_TIME 5.0

class MainFrame;
class TextMessagingDialog;
class SnoopDialog;
class TextMessagingTransport;

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
// Class MainApp
//
// @class $(Name)
// @author $(User)
// @date $(Date)
// @file $(CurrentFileName).$(CurrentFileExt)
// @brief
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
class MainApp : public wxApp
{
    public:
        // Indicates which row was last selected (for auto-filling during logging)
        enum LastSelectedRow 
        {
            UNSELECTED,
            MAIN_WINDOW,
        };

        virtual bool        OnInit();
        virtual void        OnInitCmdLine(wxCmdLineParser& parser);
        virtual bool        OnCmdLineParsed(wxCmdLineParser& parser);
        virtual int         OnExit();


        bool                    CanAccessSerialPort(std::string const& portName);
        
        LastSelectedRow lastSelectedLoggingRow;

        FreeDVConfiguration appConfiguration;
        wxString customConfigFileName;
        wxString defaultConfigFilePath;

        
        // PTT -----------------------------------    
        int        m_intHamlibRig;
        std::shared_ptr<IRigFrequencyController> rigFrequencyController;
        std::shared_ptr<IRigPttController> rigPttController;

        // PTT Input
        std::shared_ptr<SerialPortInRigController> m_pttInSerialPort;
        
        // Logging

        wxRect              m_rTopWindow;

        std::vector<std::shared_ptr<IReporter> > m_reporters;
        
        bool                loadConfig();
        bool                saveConfig();

        // misc

        float      m_channel_snr_dB;

        MainFrame *frame;

        // 700 options

        // carrier attenuation


        // tone interferer simulation


        // debugging 700D audio break up


        
        std::shared_ptr<LinkStep> linkStep;

#if !wxCHECK_VERSION(3,2,0)
        wxLocale m_locale;
#endif // !wxCHECK_VERSION(3,2,0)

        int m_reportCounter;
    protected:
    private:
};

// declare global static function wxGetApp()
DECLARE_APP(MainApp)

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
// panel with custom loop checkbox for play file dialog
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
class MyExtraPlayFilePanel : public wxPanel
{
public:
    MyExtraPlayFilePanel(wxWindow *parent);
    void setLoopPlayFileToMicIn(bool checked) { m_cb->SetValue(checked); }
    bool getLoop(void) { return m_cb->GetValue(); }
private:
    wxCheckBox *m_cb;
};

class TxRxThread;

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
// Class MainFrame
//
// @class $(Name)
// @author $(User)
// @date $(Date)
// @file $(CurrentFileName).$(CurrentFileExt)
// @brief
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
class MainFrame : public TopFrame, public IGlissandoHost
{
    public:
        MainFrame(wxWindow *parent);
        virtual ~MainFrame();

        TextMessagingDialog*    m_textMessagingDialog;
        SnoopDialog*            m_snoopDialog;
        TextMessagingTransport* m_textMessagingTransport;
        TextMessaging::Data2GTransport* m_data2gTransport;
        GlissandoConsole*       m_glissandoConsole;
        bool                    m_chatUnread = false;
        PlotScalar*             m_panelDemodIn;

        bool                    m_RxRunning;
        bool                    txChangeoverOccurring_;

        
        bool                    OpenHamlibRig();
#if defined(WIN32)
        void                    OpenOmniRig();
#endif // defined(WIN32)
        void                    OpenSerialPort(void);
        void                    OpenPTTInPort(void);
        void                    ClosePTTInPort(void);

        bool                    m_modal;

#ifdef _USE_TIMER
        wxTimer                 m_plotTimer;

        // Not sure why we have the option to disable timers. TBD?
        wxTimer                 m_updFreqStatusTimer; //[UP]

        wxTimer                 m_plotScatterTimer;
        wxTimer                 m_plotDemodInTimer;

        // Time-Out Timer (TOT): stops TX after configured period
        wxTimer                 m_totTimer;
        wxTimer                 m_totWarningTimer;
#endif

        // TOT warning state
        std::chrono::time_point<std::chrono::high_resolution_clock> m_totTxStartTime;
        int                     m_totCurrentDurationMs{0};
        TotWarningDialog*       m_totWarningDialog_{nullptr};

        // TOT beep state

    void destroy_fifos(void);

    void togglePTT(void);

    // Keys and unkeys the radio for a text messaging burst. Called on the GUI
    // thread; see startTextMessaging_().
    void setTextMessagingPtt_(bool keyed);

    // Keeps text chat off the air where the operator's preference says it may
    // not send data. Call whenever the frequency or that preference changes.
    void updateTextChatTransmitPermission_();

    void startTextMessaging_();
    void stopTextMessaging_();

    // Points text chat at our own modem or at an external data2g-host, as
    // Preferences say. Call after start and whenever Preferences close.
    void applyChatModem_();

public:
    // One line for the chat window: which modem chat goes through and, for
    // Data2G, whether data2g-host is reachable. Empty for our own modem.
    wxString chatModemStatus();

    // How long a chat message of this text would take on the air, in
    // seconds, at the Glissando tempo chat would send it at now; 0 when that
    // is not known (codec2 modes, or Data2G carrying chat).
    double chatMessageAirSeconds(const std::string& text);

    // A chat message has come in while the COMMS window is closed; the
    // console's COMMS button flashes until the window is opened.
    void noteChatUnread() { m_chatUnread = true; }

    // Makes the COMMS window, hidden, so it hears messages even before it is
    // first opened.
    void createChatWindow();

    // The Glissando tempo a chat keying would go out at now, from the
    // console or Auto (with Data2G, the tempo that picks its mode); 0 when
    // chat goes over codec2.
    int chatTransmitGear();

    // How much of the chat transmission on the air has been sent, 0 to 1;
    // negative when none is, or when Data2G carries chat and does not say.
    double chatSendProgress();

    // The keying length past which the time-out timer cuts in: the app's,
    // or 180 s, the usual setting on a rig, when the app's is off.
    int chatTimeOutSeconds();
    // The smoke off the visi-scope, 0 for none to 1 (see SmokeGauge).
    double smokeLevel() const { return smokeGauge_.level(); }

    // True when chat goes through our own modem and the console is
    // disengaged: there is no transmitter, so anything queued waits for the
    // operator to press Engage. Data2G keys its own radio and never waits.
    bool chatWaitsForEngage();

    // Stops the chat keying on the air now, for the operator aborting the
    // message in it, and leaves the rest of the queue alone. Data2G keys
    // its own radio, so nothing can be stopped there.
    void chatStopKeying();

private:

public:
    // The Glissando console (glissando_host.cpp), the application's window;
    // this frame stays hidden behind it. Opening it switches text chat to the
    // Glissando mode, and closing it quits.
    void openGlissandoConsole();
    // Gives a window the Glissando icon (Windows; elsewhere the desktop
    // entry supplies it).
    static void applyGlissandoIcon(wxTopLevelWindow* window);

    // IGlissandoHost
    virtual GlissandoTelemetry glissandoTelemetry() override;
    virtual void glissandoSettingsChanged(const GlissandoConsoleSettings& settings) override;
    virtual bool glissandoSpectrum(std::vector<float>& magnitudesDb, double& nyquistHz) override;
    virtual std::vector<GlissandoScopeFrame> glissandoHeardFrames() override;
    virtual std::vector<GlissandoScopeSent> glissandoSentFrames() override;
    virtual void glissandoSetAudioRunning(bool running) override;
    virtual void glissandoAbortTransmit() override;
    virtual void glissandoSetRigFrequency(double hz) override;
    virtual void glissandoSetDrive(double db) override;
    virtual void glissandoSetDriveAuto(bool automatic) override;
    virtual std::vector<double> glissandoFrequencyPresets() override;
    virtual void glissandoShowChat(bool show) override;
    virtual bool glissandoChatShown() override;
    virtual void glissandoShowSnoop(bool show) override;
    virtual bool glissandoSnoopShown() override;
    virtual bool glissandoSetupAvailable(GlissandoSetup setup) override;
    virtual void glissandoOpenSetup(GlissandoSetup setup) override;
    virtual void glissandoConsoleClosed(const wxRect& lastPosition) override;

private:
    // Pushes the console's choices to the chat modem and the protocol's
    // timers to match; called on every change and every refresh.
    void applyGlissandoToModem_(bool enabled);
    void closeGlissandoConsole_();
    GlissandoConsoleSettings loadGlissandoSettings_() const;
    // The Glissando tempo chat sends at now: the console's, or Auto's pick.
    // With Data2G carrying chat it picks the Data2G mode.
    int chatTempo_();
    TextMessaging::AirTiming appliedAirTiming_;

    // Set while text chat goes through data2g-host (applyChatModem_()).
    std::atomic<bool> data2gChatActive_{false};

    // Adds a station whose chat was heard to the stations heard log. Called
    // from the receive threads; the log is written on the UI thread.
    void logStationHeard_(std::string const& callsign, float snr, std::string const& modem);
    // When each station was last logged, on which frequency, so that one
    // exchange is one line in the log rather than one per frame.
    std::map<std::string, std::pair<std::chrono::steady_clock::time_point, int64_t>> stationsHeardLogged_;
    TextMessaging::Data2GTransport::Settings appliedData2GSettings_;

    bool                    m_schedule_restore;


        void StopPlaybackFileFromRadio();
        void StopRecFileFromRadio();
        
        bool isReceiveOnly();
        
    protected:

        // protected event handlers
        virtual void topFrame_OnSize( wxSizeEvent& event ) override;
        virtual void topFrame_OnClose( wxCloseEvent& event ) override;
        virtual void OnCloseFrame(wxCloseEvent& event);
        void OnExitClick(wxCommandEvent& event);

        void startTxStream();
        void startRxStream();
        void stopTxStream();
        void stopRxStream();
        void abortTxStream();
        void abortRxStream();

        void OnTop(wxCommandEvent& event) override;
        void OnExit( wxCommandEvent& event ) override;

        void OnToolsEasySetup( wxCommandEvent& event ) override;
        void OnToolsEasySetupUI( wxUpdateUIEvent& event ) override;
        void OnToolsTextMessaging( wxCommandEvent& event ) override;
        void OnToolsGlissando( wxCommandEvent& event ) override;
        void OnToolsTextMessagingUI( wxUpdateUIEvent& event ) override;
        void OnToolsAudio( wxCommandEvent& event ) override;
        void OnToolsAudioUI( wxUpdateUIEvent& event ) override;
        void OnToolsComCfg( wxCommandEvent& event ) override;
        void OnToolsComCfgUI( wxUpdateUIEvent& event ) override;
        void OnToolsOptions(wxCommandEvent& event) override;
        void OnToolsOptionsUI(wxUpdateUIEvent& event) override;

        void OnPlayFileFromRadio( wxCommandEvent& event ) override;
        void OnToolsExportConfig( wxCommandEvent& event ) override;
        void OnToolsExportConfigUI( wxUpdateUIEvent& event ) override;
        void OnToolsImportConfig( wxCommandEvent& event ) override;
        void OnToolsImportConfigUI( wxUpdateUIEvent& event ) override;
        void OnToolsLoadDefaultConfig( wxCommandEvent& event ) override;
        void OnToolsLoadDefaultConfigUI( wxUpdateUIEvent& event ) override;


        // Toggle Buttons
        void OnTogBtnSplitClick(wxCommandEvent& event);
        void OnTogBtnPTT( wxCommandEvent& event ) override;
        void OnTogBtnPTTRightClick( wxContextMenuEvent& event ) override;

        // NOTE: sets TX colour on press to avoid a GTK blue-flash during the TX delay.
        // Upstream may prefer a different approach (e.g. true press-to-start TX).
        void OnTogBtnPTTMouseDown( wxMouseEvent& event );
        void OnTogBtnPTTMouseLeave( wxMouseEvent& event );

        

        void OnTogBtnOnOff( wxCommandEvent& event ) override;

        
        void OnCallSignReset( wxCommandEvent& event ) override;

        //System Events
        void OnPaint(wxPaintEvent& event);
        void OnSize( wxSizeEvent& event );
        void OnUpdateUI( wxUpdateUIEvent& event );
        void OnDeleteConfig(wxCommandEvent&);
        void OnDeleteConfigUI( wxUpdateUIEvent& event );
#ifdef _USE_TIMER
        void OnTimer(wxTimerEvent &evt);
#endif
#ifdef _USE_ONIDLE
        void OnIdle(wxIdleEvent &evt);
#endif


        void applyTxLevel();
        void OnTxLevelDecrBig( wxCommandEvent& event ) override;
        void OnTxLevelDecr( wxCommandEvent& event ) override;
        void OnTxLevelIncr( wxCommandEvent& event ) override;
        void OnTxLevelIncrBig( wxCommandEvent& event ) override;
        void OnTxLevelMouseWheel( wxMouseEvent& event ) override;
        void OnTxLevelContextMenu( wxContextMenuEvent& event ) override;
        void OnTuneAttenContextMenu( wxContextMenuEvent& event ) override;
        void loadTxAttenForBand_(FilterFrequency band);
        void loadTuneAttenForBand_(FilterFrequency band);
        void autoSaveCurrentBandLevels_(bool writeConfig = true);
        
        
        void OnChangeReportFrequency( wxCommandEvent& event ) override;
        void OnChangeReportFrequencyVerify( wxCommandEvent& event ) override;
        
        void OnReportFrequencySetFocus(wxFocusEvent& event) override;
        void OnReportFrequencyKillFocus(wxFocusEvent& event) override;

        void OnSystemColorChanged(wxSysColourChangedEvent& event) override;
        

        void OnTOTTimer(wxTimerEvent& evt);
        void OnTOTWarningTimer(wxTimerEvent& evt);
        
        void OnSetMonitorTxAudio( wxCommandEvent& event );
        
        void OnSetMonitorTxAudioVol( wxCommandEvent& event );
        

        void OnRightClickCallsignList(wxMouseEvent& event) override;

        void OnOpenCallsignList( wxCommandEvent& event ) override;
        void OnCloseCallsignList( wxCommandEvent& event ) override;

        void OnTogBtnTune(wxCommandEvent& event) override;
        
    private:
        const wxString EMPTY_STR;
        const wxString MIC_SPKR_LEVEL_FORMAT_STR;
        const wxString DECIBEL_STR;
        const wxString CURRENT_TIME_FORMAT_STR;
        const wxString SNR_FORMAT_STR_NO_DB;
        const wxString CALLSIGN_FORMAT_RGX;

        friend class MainApp; // needed for unit tests
        friend class TxRxThread; // XXX - needed for execOnUiThreadAndWait_().

        std::shared_ptr<IAudioDevice> rxInSoundDevice;
        std::shared_ptr<IAudioDevice> txOutSoundDevice;
        
        bool        m_useMemory;
        wxTextCtrl* m_tc;
        int         m_zoom;

        // Events
        void        processTxtEvent(char event[]);
        class OptionsDlg *optionsDlg;

        // level Gauge
        float       m_maxLevel;


        bool suppressFreqModeUpdates_;
        // The operator picked a frequency in the app while no radio was
        // connected; the radio is tuned to it once it connects. Otherwise the
        // radio's own frequency stands: the app never retunes it on its own.
        std::atomic<bool> operatorFrequencyPending_;
        void refreshRigFrequencyBeforeKeying_();

        // The radio's SWR and ALC while transmitting (see glissando_host.cpp).
        // Touched on the GUI thread only.
        void pollRigMeters_();
        void onRigSwrReading_(double swr);
        void onRigAlcReading_(double alc);
        double rigSwr_ = NAN;               // NaN until a reading comes back on this keying
        double rigRfPower_ = NAN;           // the radio's power setting as last read, NaN never read
        uint64_t keyedAtMs_ = 0;            // when the radio was keyed, as the meter poll saw it; 0 unkeyed
        SmokeGauge smokeGauge_;             // the easter egg: smoke on a long keying at high power
        uint64_t rigSwrAbortAtMs_ = 0;      // when high SWR last aborted a transmission
        DriveServo driveServo_{TX_ATTENUATION_MIN, 0.5};
        uint64_t alcOverAtMs_ = 0;          // when the ALC last read over the DRIVE target
        FilterFrequency lastBand_;
        // Restore-point: the TX/tune level that was active when we entered the
        // current band (or when Enable was first clicked for that band). Restore
        // reverts to this rather than re-reading the map, which may already have
        // been overwritten by auto-save on a prior band departure.
        // Initialised to -20 dB (units are tenths of dB) as a safe default in
        // case Restore is invoked before any band load or Enable has occurred.
        int txLoadedLevel_{-200};
        int tuneLoadedLevel_{-200};
        
        
        wxMenu* pttPopupMenu_;
        wxMenuItem* adjustMonitorPttVolMenuItem_;

        bool terminating_; // used for terminating FreeDV

        // Signalled once the detached rig PTT/frequency controller disconnect
        // threads (see performFreeDVOff_()) finish tearing down. Only waited on,
        // with a bounded timeout, when terminating_ is set -- lets a responsive
        // rig disconnect cleanly before the process exits out from under the
        // detached thread, without reintroducing an unbounded hang against an
        // unresponsive one.
        std::future<void> rigPttDisconnectFuture_;
        std::future<void> rigFreqDisconnectFuture_;

        
        int         getSoundCardIDFromName(wxString& name, bool input);
        bool        validateSoundCardSetup(bool silent = false);
        
        void loadConfiguration_();
        void restoreCallsignListFromCsv_();
        void exportConfiguration_(wxConfigBase* config);
        void setConfiguration_(wxConfigBase* config);

        
        void performFreeDVOn_();
        void performFreeDVOff_();
        
        void executeOnUiThreadAndWait_(std::function<void()> fn);
        
        void updateReportingFreqList_();
        
        
        void onFrequencyModeChange_(IRigFrequencyController*, uint64_t freq, IRigFrequencyController::Mode mode);
        void onRadioConnected_(IRigController* ptr);
        void onRadioDisconnected_(IRigController* ptr);

        // Audio error handlers
        void onAudioEngineError_(IAudioEngine&, std::string const& error, void* state);
        void onAudioDeviceError_(std::string error);
        static void OnAudioDeviceError_(IAudioDevice&, std::string const& error, void* state);

        // Audio device change handling
        template<int soundCardId, bool isOut>
        void handleAudioDeviceChange_(std::string const& newDeviceName);

        // Audio device data handlers
        static void OnTxOutAudioData_(IAudioDevice& dev, void* data, size_t size, void* state) FREEDV_NONBLOCKING;
        static void OnRxInAudioData_(IAudioDevice& dev, void* data, size_t size, void* state) FREEDV_NONBLOCKING;

        bool isFrequencyControlEnabled_()
        {
            return true;
        }
        
        int getIdealStationsHeardColumnLength_(int col);
};

void resample_for_plot(GenericFIFO<short> *plotFifo, short buf[], short* dec_samples, int length, int fs) FREEDV_NONBLOCKING;


#endif //__FDMDV2_MAIN__
