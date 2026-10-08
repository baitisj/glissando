//==========================================================================
// Name:            main.cpp
//
// Purpose:         FreeDV main()
// Created:         Apr. 9, 2012
// Authors:         David Rowe, David Witten
//
// License:
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//==========================================================================

#include <algorithm>
#include <inttypes.h>
#include <time.h>
#include <ctime>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <deque>
#include <random>
#include <chrono>
#include <climits>
#include <wx/cmdline.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/tokenzr.h>
#include <wx/uiaction.h>

#if wxCHECK_VERSION(3,2,0)
#include <wx/uilocale.h>
#endif // wxCHECK_VERSION(3,2,0)

#include "defines.h"
#include "git_version.h"
#include "main.h"
#include "os/os_interface.h"
#include "audio/AudioEngineFactory.h"
#include "pipeline/TxRxThread.h"
#include "reporting/CsvReporter.h"

#include "gui/dialogs/dlg_options.h"
#include "gui/dialogs/dlg_easy_setup.h"
#include "gui/dialogs/dlg_snoop.h"
#include "gui/dialogs/dlg_text_messaging.h"
#include "pipeline/TextMessagingModem.h"
#include "pipeline/TextMessagingTransport.h"
#include "pipeline/TextMessagingTxQueue.h"
#include "text_messaging/Data2GTransport.h"
#include "text_messaging/TextMessagingSession.h"
#include "text_messaging/UsDataSegments.h"
#include "gui/util/WindowPositionRestore.h"

#include "util/logging/ulog.h"
#include "util/audio_spin_mutex.h"

#if defined(__WXGTK__) && defined(HAS_GTK3)
#include <gtk/gtk.h>
#endif // defined(__WXGTK__) && defined(HAS_GTK3)

using namespace std::chrono_literals;
using namespace std::placeholders;

#define wxUSE_FILEDLG   1
#define wxUSE_LIBPNG    1
#define wxUSE_LIBJPEG   1
#define wxUSE_GIF       1
#define wxUSE_PCX       1
#define wxUSE_LIBTIFF   1

extern "C" {
    extern void golay23_init(void);
}


//-------------------------------------------------------------------
// Bunch of globals used for communication with sound card call
// back functions
// ------------------------------------------------------------------

std::shared_ptr<TxRxThread> m_txThread;
std::shared_ptr<TxRxThread> m_rxThread;

// time averaged magnitude spectrum of the radio input, which the Glissando
// console's waterfall draws
GenericFIFO<float>  g_avmag(MODEM_STATS_NSPEC * 10 / DT); // 1s worth
float               g_avmag_waterfall[MODEM_STATS_NSPEC];

// TX level for attenuation
int g_txLevel = 0;
std::atomic<float> g_txLevelScale;
int g_tuneLevel = 0;
std::atomic<float> g_tuneLevelScale;

// GUI controls that affect rx and tx processes
std::atomic<bool>   g_tx;
std::atomic<bool>  g_half_duplex;

paCallBackData     *g_rxUserdata;

// FIFO used for plotting the radio's waveform
constexpr int PLOT_BUF_MULTIPLIER=8;
GenericFIFO<short>  g_plotDemodInFifo(PLOT_BUF_MULTIPLIER*WAVEFORM_PLOT_BUF);

// Soundcard config
int                 g_nSoundCards;

// PortAudio over/underflow counters

std::atomic<int>    g_infifo1_full;
std::atomic<int>    g_outfifo1_empty;
std::atomic<int>    g_infifo2_full;
std::atomic<int>    g_outfifo2_empty;
int                 g_AEstatus1[4];
int                 g_AEstatus2[4];

// playing and recording from sound files


extern SNDFILE            *g_sfRecFile;
extern bool                g_recFileFromRadio;
extern std::atomic<unsigned int> g_recFromRadioSamples;
extern int                 g_recFileFromRadioEventId;

extern std::atomic<SNDFILE*> g_sfPlayFileFromRadio;
extern std::atomic<bool>                g_playFileFromRadio;
extern std::atomic<int>    g_sfFs;
extern std::atomic<bool>   g_loopPlayFileFromRadio;
extern int                 g_playFileFromRadioEventId;

extern std::atomic<SNDFILE*>            g_sfRecFileFromModulator;
extern std::atomic<bool>                g_recFileFromModulator;
extern int                 g_recFileFromModulatorEventId;



wxWindow           *g_parent;

// experimental mutex to make sound card callbacks mutually exclusive
// TODO: review code and see if we need this any more, as fifos should
// now be thread safe

wxMutex g_mutexProtectingCallbackData(wxMUTEX_RECURSIVE);

// End of TX state control
std::atomic<bool> endingTx;

// Running state
std::atomic<bool> isModemRunning;

// Option test file to log samples

FILE *ftest;

// Config file management
wxConfigBase *pConfig = NULL;

// Glissando keeps its settings, chat history and logs under its own names, so
// it never reads or changes an installed FreeDV's. The application name also
// names the config file (~/.glissando.conf, or glissando/glissando.conf under
// XDG) and the user data folder (~/.glissando, or ~/.local/share/glissando).
static const wxChar* const GLISSANDO_APP_NAME = wxT("glissando");
static const wxChar* const GLISSANDO_VENDOR_NAME = wxT("Glissando");

// Name used for the separate state-store config object (distinct from the main
// app config so the last-used path is readable regardless of which backend is
// active).  On Windows this becomes HKCU\Software\Glissando\Glissando-State;
// on macOS/Linux it becomes a file in the per-user config directory.
static const wxChar* const GLISSANDO_STATE_APP_NAME    = wxT("Glissando-State");
static const wxChar* const LAST_USED_CONFIG_KEY     = wxT("/LastUsedConfigFile");

wxString getLastUsedConfigPath()
{
    wxConfig stateConfig(GLISSANDO_STATE_APP_NAME, GLISSANDO_VENDOR_NAME);
    wxString path;
    stateConfig.Read(LAST_USED_CONFIG_KEY, &path, wxEmptyString);
    return path;
}

void saveLastUsedConfigPath(const wxString& path)
{
    wxConfig stateConfig(GLISSANDO_STATE_APP_NAME, GLISSANDO_VENDOR_NAME);
    stateConfig.Write(LAST_USED_CONFIG_KEY, path);
    stateConfig.Flush();
}

void clearLastUsedConfigPath()
{
    wxConfig stateConfig(GLISSANDO_STATE_APP_NAME, GLISSANDO_VENDOR_NAME);
    stateConfig.Write(LAST_USED_CONFIG_KEY, wxEmptyString);
    stateConfig.Flush();
}

// WxWidgets - initialize the application

IMPLEMENT_APP(MainApp);

std::mutex logMutex;
static void LogLockFunction_(bool lock, void *)
{
    if (lock)
    {
        logMutex.lock();
    }
    else
    {
        logMutex.unlock();
    }
}

// g_nSoundCards counts the radio's audio ports in use: 0 with no radio
// input, 1 when receiving only, 2 when the radio output is there as well and
// the station can transmit.
static int radioSoundCardCount()
{
    auto& audio = wxGetApp().appConfiguration.audioConfiguration;
    if (audio.soundCard1In.deviceName == "none") return 0;
    return audio.soundCard1Out.deviceName == "none" ? 1 : 2;
}

template<int soundCardId, bool isOut>
void MainFrame::handleAudioDeviceChange_(std::string const& newDeviceName)
{
    wxString devName = wxString::FromUTF8(newDeviceName.c_str());
    if (soundCardId == 1)
    {
        if (isOut)
        {
            wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.deviceName = devName;
        }
        else
        {
            wxGetApp().appConfiguration.audioConfiguration.soundCard1In.deviceName = devName;
        }
    }
    else if (soundCardId == 2)
    {
        if (isOut)
        {
            wxGetApp().appConfiguration.audioConfiguration.soundCard2Out.deviceName = devName;
        }
        else
        {
            wxGetApp().appConfiguration.audioConfiguration.soundCard2In.deviceName = devName;
        }
    }
    wxGetApp().appConfiguration.save(pConfig);
}

void MainApp::OnInitCmdLine(wxCmdLineParser& parser)
{
    wxApp::OnInitCmdLine(parser);
    parser.AddOption("f", "config", "Use different configuration file instead of the default.");
    // The console is always the window now; the switch stays so older
    // scripts that pass it still start.
    parser.AddSwitch("g", "glissando", "Accepted for compatibility; the Glissando console always opens.");
}

bool MainApp::OnCmdLineParsed(wxCmdLineParser& parser)
{
    // Before anything below looks for a config file or a data folder.
    SetVendorName(GLISSANDO_VENDOR_NAME);
    SetAppName(GLISSANDO_APP_NAME);
    SetAppDisplayName(wxT("Glissando"));

    ulog_set_lock(&LogLockFunction_, nullptr);
    ulog_set_prefix_fn([](ulog_Event *, char *prefix, size_t prefix_size) {
        static unsigned int counter = 0;
        snprintf(prefix, prefix_size, " [%u]", ++counter);
    });
        
    log_info("Glissando version %s starting", GetFreeDVVersion().c_str());

    if (!wxApp::OnCmdLineParsed(parser))
    {
        return false;
    }


    wxString configPath;
    if (parser.Found("f", &configPath))
    {
        log_info("Loading configuration from %s", (const char*)configPath.ToUTF8());
        pConfig = new wxFileConfig(GLISSANDO_APP_NAME, GLISSANDO_VENDOR_NAME, configPath, configPath, wxCONFIG_USE_LOCAL_FILE);
        wxConfigBase::Set(pConfig);
        
        // On Linux/macOS, this replaces $HOME with "~" to shorten the title a bit.
        wxFileName fn(configPath);        
        customConfigFileName = fn.GetFullName();
        defaultConfigFilePath = fn.GetPath();
    }
    else
    {
        // ~/.glissando.conf rather than wxWidgets' default of ~/.glissando,
        // which is also the user data folder that holds the chat history.
        wxString oldFileLocation = wxFileConfig::GetLocalFile(wxT("glissando.conf"), 0).GetFullPath();
        wxFileName tempOldFile(oldFileLocation);

#if wxCHECK_VERSION(3,3,0) && defined(__linux__)
        // Execute this early during the application startup, before the
        // global wxConfig object is created.
        bool migrateSuccess = true;
        wxString newFileLocation = wxFileConfig::GetLocalFile(GLISSANDO_APP_NAME, wxCONFIG_USE_XDG | wxCONFIG_USE_SUBDIR).GetFullPath();
        wxString newFileDir = wxFileConfig::GetLocalFile(GLISSANDO_APP_NAME, wxCONFIG_USE_XDG | wxCONFIG_USE_SUBDIR).GetPath();
        log_info("Determining if we need to migrate config file to standard location...");
        log_info("   Old location: %s", (const char*)oldFileLocation.ToUTF8());
        log_info("   New location: %s", (const char*)newFileLocation.ToUTF8());

        wxFileName tempNewFile(newFileLocation);
        if (!tempNewFile.IsFileReadable() && tempOldFile.IsFileReadable())
        {
            // Migration hasn't happened yet, try to copy to new location.
            // Note that the return value from Mkdir isn't reliable despite actually
            // creating the folder, so we should check for its existence separately.
            wxFileName::Mkdir(newFileDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
            if (!wxDirExists(newFileDir))
            {
                log_warn("Could not create folder %s. Migration will not proceed.", (const char*)newFileDir.ToUTF8());
                migrateSuccess = false;
            }
            else
            {
                bool copyResult = wxCopyFile(oldFileLocation, newFileLocation);
                if (!copyResult)
                {
                    log_warn("Could not copy %s to %s. Migration will not proceed.", (const char*)oldFileLocation.ToUTF8(), (const char*)newFileLocation.ToUTF8());
                    migrateSuccess = false;
                }
            }
        }
 
        // Prefer doing it only after successfully calling MigrateLocalFile(),
        // otherwise, i.e. if it failed, the old config file wouldn't be used.
        if (migrateSuccess)
        {
            wxStandardPaths::Get().SetFileLayout(wxStandardPaths::FileLayout_XDG);

            // Need to explicitly create the wxFileConfig on Linux so that we can force wxWidgets
            // to load configuration files under a subdirectory. Otherwise, simply FileLayout_XDG
            // above will use ~/.config/glissando.conf.
            pConfig = new wxFileConfig(GLISSANDO_APP_NAME, GLISSANDO_VENDOR_NAME, newFileLocation, newFileLocation, wxCONFIG_USE_LOCAL_FILE | wxCONFIG_USE_SUBDIR | wxCONFIG_USE_XDG);

            wxConfigBase::Set(pConfig);
            defaultConfigFilePath = tempNewFile.GetPath();
        }
#else
#if defined(__linux__)
        pConfig = new wxFileConfig(GLISSANDO_APP_NAME, GLISSANDO_VENDOR_NAME, oldFileLocation, oldFileLocation, wxCONFIG_USE_LOCAL_FILE);
        wxConfigBase::Set(pConfig);
#endif // defined(__linux__)
        defaultConfigFilePath = tempOldFile.GetPath();
#endif // wxCHECK_VERSION(3,3,0) && defined(__linux__)

        // If a config file was previously loaded via "Use Configuration...",
        // restore it now so the user's last session is preserved.
        wxString lastUsedPath = getLastUsedConfigPath();
        if (!lastUsedPath.IsEmpty())
        {
            if (wxFileExists(lastUsedPath))
            {
                log_info("Restoring last-used configuration from %s",
                         (const char*)lastUsedPath.ToUTF8());
                pConfig = new wxFileConfig(GLISSANDO_APP_NAME, GLISSANDO_VENDOR_NAME,
                                           lastUsedPath, lastUsedPath,
                                           wxCONFIG_USE_LOCAL_FILE);
                wxConfigBase::Set(pConfig);
                wxFileName fn(lastUsedPath);
                customConfigFileName = fn.GetFullName();
                defaultConfigFilePath = fn.GetPath();
            }
            else
            {
                log_warn("Last-used config file '%s' no longer exists; reverting to default.",
                         (const char*)lastUsedPath.ToUTF8());
                clearLastUsedConfigPath();
            }
        }
    }

    pConfig = wxConfigBase::Get();
    pConfig->SetRecordDefaults();
    
    return true;
}

//-------------------------------------------------------------------------
#if defined(__WXGTK__) && defined(HAS_GTK3)
// Suppress the GTK theme :active (button-press) colour flash app-wide.
// Queries the theme's normal button background and installs a screen-level
// CSS rule so button:active renders identically to the resting state.
// Fails gracefully if the named colour variables are absent (non-Breeze themes).
static bool TryLookupThemeColour_(const char* name, GdkRGBA& out)
{
    GtkWidget* tmpButton = gtk_button_new();
    GtkWidget* tmpWindow = gtk_offscreen_window_new();
    gtk_container_add(GTK_CONTAINER(tmpWindow), tmpButton);
    gtk_widget_show_all(tmpWindow);
    GtkStyleContext* ctx = gtk_widget_get_style_context(tmpButton);
    bool found = gtk_style_context_lookup_color(ctx, name, &out);
    gtk_widget_destroy(tmpWindow);
    return found;
}

static void SuppressButtonPressFlicker_()
{
    GdkRGBA bg;
    bool found = false;
    // theme_button_background_normal is Breeze-specific; theme_bg_color is
    // a broader fallback. theme_base_color is deliberately excluded — it
    // resolves to the text-field background (white in light themes).
    const char* names[] = { "theme_button_background_normal", "theme_bg_color", nullptr };
    for (int i = 0; names[i] && !found; i++)
        found = TryLookupThemeColour_(names[i], bg);
    if (!found)
        return;

    gchar* cssColour = gdk_rgba_to_string(&bg);
    gchar* css = g_strdup_printf(
        "button:active {"
        "  background-color: %s;"
        "  background-image: none;"
        "  box-shadow: none;"
        "}", cssColour);
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_free(css);
    g_free(cssColour);
    g_object_unref(provider);
}

#endif // defined(__WXGTK__) && defined(HAS_GTK3)

// OnInit()
//-------------------------------------------------------------------------
bool MainApp::OnInit()
{
    // Initialize locale.
#if wxCHECK_VERSION(3,2,0)
    wxUILocale::UseDefault();
#else
    m_locale.Init();
#endif // wxCHECK_VERSION(3,2,0)

    lastSelectedLoggingRow = LastSelectedRow::UNSELECTED;
    m_reporters.clear();
    m_reportCounter = 0;
    
    if(!wxApp::OnInit())
    {
        return false;
    }
#if defined(__WXGTK__) && defined(HAS_GTK3)
    SuppressButtonPressFlicker_();
#endif // defined(__WXGTK__) && defined(HAS_GTK3)
    SetVendorName(GLISSANDO_VENDOR_NAME);
    SetAppName(GLISSANDO_APP_NAME);
    
    golay23_init();

    
    m_rTopWindow = wxRect(0, 0, 0, 0);

     // Create the main application window

    frame = new MainFrame(NULL);
    SetTopWindow(frame);

    // The frame keeps the radio, audio and chat running but is never shown:
    // the Glissando console is the application's window, with the chat and
    // snooping windows floating beside it.
    frame->Layout();
    g_parent = frame;
    frame->openGlissandoConsole();
    frame->CallAfter([]() {
        // The COMMS and snooping windows as they were when the app last
        // closed. COMMS is made either way, so it hears what comes in.
        MainFrame* mainFrame = wxGetApp().frame;
        mainFrame->createChatWindow();
        if (wxGetApp().appConfiguration.glissandoChatOpen) mainFrame->glissandoShowChat(true);
        if (wxGetApp().appConfiguration.glissandoSnoopOpen) mainFrame->glissandoShowSnoop(true);
    });
    
    return true;
}

//-------------------------------------------------------------------------
// OnExit()
//-------------------------------------------------------------------------
int MainApp::OnExit()
{
    return 0;
}

//-------------------------------------------------------------------------
// loadConfiguration_(): Loads or sets default configuration options.
//-------------------------------------------------------------------------
void MainFrame::loadConfiguration_()
{
    wxGetApp().appConfiguration.load(pConfig);
    
    // restore frame position and size
    int x = wxGetApp().appConfiguration.mainWindowLeft;
    int y = wxGetApp().appConfiguration.mainWindowTop;
    int w = wxGetApp().appConfiguration.mainWindowWidth;
    int h = wxGetApp().appConfiguration.mainWindowHeight;

    // sanitise frame position as a first pass at Win32 registry bug

    if (x < 0 || x > 2048) x = 20;
    if (y < 0 || y > 2048) y = 20;
    if (w < 0 || w > 2048) w = 800;
    if (h < 0 || h > 2048) h = 780;

    
    wxSize size = GetMinSize();

    if (w < size.GetWidth()) w = size.GetWidth();
    if (h < size.GetHeight()) h = size.GetHeight();

    RestoreWindowPosition(this, x, y);

    // XXX - with really short windows, wxWidgets sometimes doesn't size
    // the components properly until the user resizes the window (even if only
    // by a pixel or two). As a really hacky workaround, we emulate this behavior
    // when restoring window sizing. These resize events also happen after configuration
    // is restored but I'm not sure this is necessary.
    CallAfter([=, this]()
    {
        SetSize(w, h);
    });
    CallAfter([=, this]()
    {
        SetSize(w + 1, h + 1);
    });
    CallAfter([=, this]()
    {
        SetSize(w, h);
    });
    
    g_txLevel = wxGetApp().appConfiguration.transmitLevel;
    float dbLoss = g_txLevel / 10.0;
    float scaleFactor = exp(dbLoss/20.0 * log(10.0));
    g_txLevelScale.store(scaleFactor, std::memory_order_release);

    wxString fmtString = wxString::Format(MIC_SPKR_LEVEL_FORMAT_STR, wxNumberFormatter::ToString((double)dbLoss, 1), DECIBEL_STR);
    m_txtTxLevelNum->SetLabel(fmtString);

    g_tuneLevel = wxGetApp().appConfiguration.tuneLevel;
    dbLoss = g_tuneLevel / 10.0;
    scaleFactor = exp(dbLoss/20.0 * log(10.0));
    g_tuneLevelScale.store(scaleFactor, std::memory_order_release);

    // Brings a saved level pushed in above its ceiling down to it.
    applyTxLevel();

    // Adjust frequency entry labels
    wxListItem colInfo;
    m_lastReportedCallsignListView->GetColumn(1, colInfo);
    if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
    {
        m_freqBox->SetLabel(_("Radio Freq. (kHz)"));
        colInfo.SetText(_("kHz"));
    }
    else
    {
        m_freqBox->SetLabel(_("Radio Freq. (MHz)"));
        colInfo.SetText(_("MHz"));
    }
    m_lastReportedCallsignListView->SetColumn(1, colInfo);
    
    // PTT -------------------------------------------------------------------
    
    // Note: we're no longer using RigName but we need to bring over the old data
    // for backwards compatibility.    
    if (wxGetApp().appConfiguration.rigControlConfiguration.hamlibRigName == wxT(""))
    {
        wxGetApp().m_intHamlibRig = pConfig->ReadLong("/Hamlib/RigName", -1);
        if (wxGetApp().m_intHamlibRig >= 0)
        {
            wxGetApp().appConfiguration.rigControlConfiguration.hamlibRigName = HamlibRigController::RigIndexToName(wxGetApp().m_intHamlibRig);
        }
    }
    else
    {
        wxGetApp().m_intHamlibRig = HamlibRigController::RigNameToIndex(std::string(wxGetApp().appConfiguration.rigControlConfiguration.hamlibRigName->ToUTF8()));
    }
    
    // -----------------------------------------------------------------------

    ulog_set_level(LOG_INFO);

    // General reporting parameters

    // wxString::Format() doesn't respect locale but C++ iomanip should. Use the latter instead.
    if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency > 0)
    {
        double freqFactor = 1000.0;
        
        if (!wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
        {
            freqFactor *= 1000.0;
        }
        
        double freq =  ((double)wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency) / freqFactor;

        wxString sVal;
        if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
        {
            sVal = wxNumberFormatter::ToString(freq, 1);
        }
        else
        {
            sVal = wxNumberFormatter::ToString(freq, 4);
        }
        m_cboReportFrequency->SetValue(sVal);
    }

    pConfig->SetPath(wxT("/"));
    
    m_btnTogPTT->Disable();
    
    // Show/hide frequency box based on CAT control status
    m_freqBox->Show(isFrequencyControlEnabled_());

    restoreCallsignListFromCsv_();


    // Ensure that sound card count is correct. Otherwise the Audio Options won't show
    // the correct devices prior to start.
    g_nSoundCards = radioSoundCardCount();
    
    // Update the reporting list as needed.
    updateReportingFreqList_();
    
    // Relayout window so that the changes can take effect.
    auto currentSizer = m_panel->GetSizer();
    m_panel->SetSizerAndFit(currentSizer, false);
    m_panel->Layout();
}

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
// Class MainFrame(wxFrame* pa->ent) : TopFrame(parent)
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
MainFrame::MainFrame(wxWindow *parent) : TopFrame(parent, wxID_ANY, _("Glissando ") + wxString::FromUTF8(GetFreeDVVersion().c_str())),

    // Create needed strings in advance so we don't need to continually 
    // reallocate memory every time through OnTimer() below.
    EMPTY_STR(""),
    MIC_SPKR_LEVEL_FORMAT_STR("%s%s"),
    DECIBEL_STR("dB"),
    CURRENT_TIME_FORMAT_STR("%s %s"),
    SNR_FORMAT_STR_NO_DB("%0.1f"),
    CALLSIGN_FORMAT_RGX("(([A-Za-z0-9]+/)?[A-Za-z0-9]{1,3}[0-9][A-Za-z0-9]*[A-Za-z](/[A-Za-z0-9]+)?)")
{
    SetThreadName("GUI");

    terminating_ = false;
    txChangeoverOccurring_ = false;

    // Add config file name to title bar if provided at the command line.
    if (wxGetApp().customConfigFileName != "")
    {
        SetTitle(wxString::Format("%s (%s)", _("Glissando ") + wxString::FromUTF8(GetFreeDVVersion().c_str()), wxGetApp().customConfigFileName));
    }
    
    
    m_textMessagingDialog = nullptr;
    m_snoopDialog = nullptr;
    m_textMessagingTransport = nullptr;
    m_data2gTransport = nullptr;
    m_glissandoConsole = nullptr;

    // Initialize panel pointers to null before creation since "page changed" 
    // events fire as we're adding these (and we compare against these pointers
    // inside the handler).
    m_panelDemodIn = nullptr;

    m_zoom              = 1.;
    suppressFreqModeUpdates_ = false;
    operatorFrequencyPending_ = false;
    lastBand_ = BAND_OTHER;
    
    tools->AppendSeparator();
    wxMenuItem* m_menuItemToolsConfigDelete;
    m_menuItemToolsConfigDelete = new wxMenuItem(tools, wxID_ANY, wxString(_("&Restore defaults")) , wxT("Delete config file/keys and restore defaults"), wxITEM_NORMAL);
    this->Connect(m_menuItemToolsConfigDelete->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(MainFrame::OnDeleteConfig));
    this->Connect(m_menuItemToolsConfigDelete->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler(MainFrame::OnDeleteConfigUI));

    tools->Append(m_menuItemToolsConfigDelete);
    
    // Add Demod Input window
    m_panelDemodIn = new PlotScalar(m_auiNbookCtrl, WAVEFORM_PLOT_TIME, 1.0/WAVEFORM_PLOT_FS, -1, 1, 1, 0.2, "%2.1f", 0);
    m_auiNbookCtrl->AddPage(m_panelDemodIn, _("Frm Radio"), true, wxNullBitmap);

    m_togBtnOnOff->Connect(wxEVT_UPDATE_UI, wxUpdateUIEventHandler(MainFrame::OnTogBtnOnOffUI), NULL, this);
    m_btnTogPTT->Bind(wxEVT_LEFT_DOWN, &MainFrame::OnTogBtnPTTMouseDown, this);
    m_btnTogPTT->Bind(wxEVT_LEFT_DCLICK, &MainFrame::OnTogBtnPTTMouseDown, this);
    m_btnTogPTT->Bind(wxEVT_LEAVE_WINDOW, &MainFrame::OnTogBtnPTTMouseLeave, this);

    loadConfiguration_();
    
#ifdef _USE_TIMER
    Bind(wxEVT_TIMER, &MainFrame::OnTimer, this);       // ID_MY_WINDOW);
    m_plotDemodInTimer.SetOwner(this, ID_TIMER_DEMOD_IN);

    m_plotTimer.SetOwner(this, ID_TIMER_UPDATE_OTHER);
    m_updFreqStatusTimer.SetOwner(this,ID_TIMER_UPD_FREQ);
    m_totTimer.SetOwner(this, ID_TIMER_TOT);
    Bind(wxEVT_TIMER, &MainFrame::OnTOTTimer, this, ID_TIMER_TOT);
    m_totWarningTimer.SetOwner(this, ID_TIMER_TOT_WARNING);
    Bind(wxEVT_TIMER, &MainFrame::OnTOTWarningTimer, this, ID_TIMER_TOT_WARNING);
#endif
    
    // Create PTT popup menu
    pttPopupMenu_ = new wxMenu();
    assert(pttPopupMenu_ != nullptr);
    
    auto monitorMenuItem = pttPopupMenu_->AppendCheckItem(wxID_ANY, _("Monitor transmitted audio"));
    pttPopupMenu_->Check(monitorMenuItem->GetId(), wxGetApp().appConfiguration.monitorTxAudio);
    pttPopupMenu_->Connect(
        monitorMenuItem->GetId(), wxEVT_COMMAND_MENU_SELECTED, 
        wxCommandEventHandler(MainFrame::OnSetMonitorTxAudio),
        NULL,
        this);
        
    adjustMonitorPttVolMenuItem_ = pttPopupMenu_->Append(wxID_ANY, _("Adjust Monitor Volume..."));
    adjustMonitorPttVolMenuItem_->Enable(wxGetApp().appConfiguration.monitorTxAudio);
    pttPopupMenu_->Connect(
        adjustMonitorPttVolMenuItem_->GetId(), wxEVT_COMMAND_MENU_SELECTED,
        wxCommandEventHandler(MainFrame::OnSetMonitorTxAudioVol),
        NULL,
        this
        );

    m_RxRunning = false;

    m_txThread = nullptr;
    m_rxThread = nullptr;
    wxGetApp().linkStep = nullptr;
    
#ifdef _USE_ONIDLE
    Connect(wxEVT_IDLE, wxIdleEventHandler(MainFrame::OnIdle), NULL, this);
#endif //_USE_ONIDLE

    g_sfRecFile = NULL;
    g_recFileFromRadio = false;

    g_sfPlayFileFromRadio.store(NULL, std::memory_order_release);
    g_playFileFromRadio.store(false, std::memory_order_release);
    g_loopPlayFileFromRadio.store(false, std::memory_order_relaxed);

    g_sfRecFileFromModulator = NULL;
    g_recFileFromModulator = false;

    g_tx.store(false, std::memory_order_release);

    optionsDlg = new OptionsDlg(NULL);
    m_schedule_restore = false;

    // Init optional Windows debug console so we can see all those printfs

#ifdef __WXMSW__
    if (wxGetApp().appConfiguration.debugConsoleEnabled || wxGetApp().appConfiguration.firstTimeUse) {
        // somewhere to send printfs while developing
        int ret = AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        log_info("AllocConsole: %d m_debug_console: %d", ret, wxGetApp().appConfiguration.debugConsoleEnabled.get());
    }
#endif
    
    if (wxGetApp().appConfiguration.firstTimeUse)
    {
        // Initial setup. Display Easy Setup dialog.
        CallAfter([&]() {
            EasySetupDialog* dlg = new EasySetupDialog(this);
            if (dlg->ShowModal() == wxOK)
            {
                // Show/hide frequency box based on CAT control configuration.
                m_freqBox->Show(isFrequencyControlEnabled_());


                // Relayout window so that the changes can take effect.
                m_panel->Layout();
            }
        });
    }
    else if (wxGetApp().appConfiguration.autoStartOnLaunch)
    {
        // Simulate a press of the Start button once the main window is up,
        // but only if the audio configuration is actually valid. Checking
        // silently first avoids popping up error dialogs (or the Easy Setup
        // dialog) at launch for users who haven't fully configured audio yet.
        CallAfter([&]() {
            if (!validateSoundCardSetup(true)) return;

            m_togBtnOnOff->SetValue(true);
            wxCommandEvent onEvent(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, m_togBtnOnOff->GetId());
            onEvent.SetEventObject(m_togBtnOnOff);
            OnTogBtnOnOff(onEvent);
        });
    }

    wxGetApp().appConfiguration.firstTimeUse = false;

    //#define FTEST
    #ifdef FTEST
    ftest = fopen("ftest.raw", "wb");
    assert(ftest != NULL);
    #endif

    /* experimental checkbox control of thread priority, used
       to helpo debug 700D windows sound break up */


    startTextMessaging_();
}

void MainFrame::restoreCallsignListFromCsv_()
{
    auto csvPath = wxGetApp().appConfiguration.reportingConfiguration.csvLogFilePath.get();
    if (csvPath.IsEmpty())
        return;

    std::ifstream file(csvPath.ToStdString());
    if (!file.is_open())
        return;

    std::vector<std::string> lines;
    std::string line;
    bool firstLine = true;
    while (std::getline(file, line))
    {
        if (firstLine) { firstLine = false; continue; } // skip CSV header
        if (!line.empty())
            lines.push_back(line);
    }

    const int MAX_RESTORE_ROWS = 100;
    if ((int)lines.size() > MAX_RESTORE_ROWS)
        lines.erase(lines.begin(), lines.begin() + ((int)lines.size() - MAX_RESTORE_ROWS));

    bool freqAsKhz = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz;

    for (auto& csvLine : lines)
    {
        // CSV columns: date,time,callsign,mode,frequency_hz,snr_db
        std::istringstream ss(csvLine);
        std::string date, time, callsign, mode, freqStr, snrStr;
        if (!std::getline(ss, date, ',')) continue;
        if (!std::getline(ss, time, ',')) continue;
        if (!std::getline(ss, callsign, ',')) continue;
        if (!std::getline(ss, mode, ',')) continue;
        if (!std::getline(ss, freqStr, ',')) continue;
        if (!std::getline(ss, snrStr)) continue;

        uint64_t freqHz = 0;
        try { freqHz = std::stoull(freqStr); } catch (...) { continue; }

        wxString freqDisplay;
        if (freqAsKhz)
            freqDisplay = wxNumberFormatter::ToString(freqHz / 1000.0, 1);
        else
            freqDisplay = wxNumberFormatter::ToString(freqHz / 1000000.0, 4);

        wxString dateTime = wxString::Format("%s %s", date, time);

        int snrInt = 0;
        try { snrInt = std::stoi(snrStr); } catch (...) { continue; }
        wxString snrDisplay;
        snrDisplay.Printf(SNR_FORMAT_STR_NO_DB, (float)snrInt);

        auto index = m_lastReportedCallsignListView->InsertItem(0, wxString(callsign), 0);
        m_lastReportedCallsignListView->SetItem(index, 1, freqDisplay);
        m_lastReportedCallsignListView->SetItem(index, 2, dateTime);
        m_lastReportedCallsignListView->SetItem(index, 3, snrDisplay);
        m_lastReportedCallsignListView->SetItemTextColour(index, wxColour(160, 160, 160));
    }

    for (int col = 0; col < 4; col++)
        m_lastReportedCallsignListView->SetColumnWidth(col, getIdealStationsHeardColumnLength_(col));
}

static std::recursive_mutex stoppingMutex;

void MainFrame::setConfiguration_(wxConfigBase* config)
{
    pConfig = config;
    wxConfigBase::Set(pConfig);
        
    // Resets all configuration to defaults.
    loadConfiguration_();
}

void MainFrame::exportConfiguration_(wxConfigBase* config)
{
    if (!IsIconized()) {
        int w = 0;
        int h = 0;
        int x = 0;
        int y = 0;
        GetSize(&w, &h);
        GetPosition(&x, &y);
        
        wxGetApp().appConfiguration.mainWindowLeft = x;
        wxGetApp().appConfiguration.mainWindowTop = y;
        wxGetApp().appConfiguration.mainWindowWidth = w;
        wxGetApp().appConfiguration.mainWindowHeight = h;
    }



    wxGetApp().appConfiguration.transmitLevel = g_txLevel;
    autoSaveCurrentBandLevels_(false);

    wxGetApp().appConfiguration.save(config);
}

// The transmit time-out timer, in seconds, or 0 when it is off.
static int timeOutTimerSeconds()
{
    auto& rig = wxGetApp().appConfiguration.rigControlConfiguration;
    return rig.totTimerEnabled && rig.totTimerSecs > 0 ? rig.totTimerSecs.get() : 0;
}

//-------------------------------------------------------------------------
// startTextMessaging_(): brings up the chat session for the whole run of the
// application, so retries and incoming messages keep working with the chat
// window closed.
//-------------------------------------------------------------------------
void MainFrame::startTextMessaging_()
{
    if (!textMessagingModem().open())
    {
        log_warn("Text messaging unavailable: the data modems could not be opened");
        return;
    }

    m_textMessagingTransport = new TextMessagingTransport(&textMessagingModem());
    m_textMessagingTransport->setPttFunction([this](bool keyed) {
        // Called from the session thread; PTT belongs to the GUI thread.
        CallAfter([this, keyed]() { setTextMessagingPtt_(keyed); });
    });
    m_textMessagingTransport->setVoiceTransmitCheck([]() {
        return g_tx.load(std::memory_order_acquire);
    });

    m_textMessagingTransport->setTransmitAllowedCheck([]() {
        // Sending needs the transmit thread, which only exists when FreeDV is
        // running with a transmit sound device.
        return m_txThread != nullptr;
    });

    m_textMessagingTransport->setKeyingLimitFunction([]() {
        // A chat keying runs the same time-out timer as voice does.
        return timeOutTimerSeconds() * 1000;
    });

    textMessagingModem().setFrameCallback([this](const TextMessaging::Frame& frame, float snr) {
        // With Data2G carrying chat, what our own modem hears is not part of
        // the conversation: its replies would go out on the other modem.
        // The snooping window hears it all the same.
        auto& session = TextMessaging::TextMessagingSession::instance();
        session.snoop().onFrame(frame, snr, TextMessaging::SnoopSource::Glissando, std::time(nullptr));
        logStationHeard_(frame.originCallsign, snr, "GLISSANDO");
        if (data2gChatActive_.load(std::memory_order_acquire)) return;
        session.protocol().onFrameReceived(frame, snr);
    });

    m_data2gTransport = new TextMessaging::Data2GTransport();
    m_data2gTransport->setLogFunction([](const std::string& line) { log_info("%s", line.c_str()); });
    m_data2gTransport->setFrameCallback([this](const TextMessaging::Frame& frame, float snr, bool viaSession) {
        auto& session = TextMessaging::TextMessagingSession::instance();
        session.snoop().onFrame(frame, snr, TextMessaging::SnoopSource::Data2G, std::time(nullptr));
        logStationHeard_(frame.originCallsign, snr, "DATA2G");
        // A session's own acknowledgements answer what comes through it.
        session.protocol().onFrameReceived(frame, snr, viaSession);
    });

    wxString databasePath = wxStandardPaths::Get().GetUserDataDir();
    if (!wxDirExists(databasePath)) wxMkdir(databasePath);
    databasePath += wxFileName::GetPathSeparator();
    databasePath += "text_messaging.db";

    auto& session = TextMessaging::TextMessagingSession::instance();
    if (!session.start(databasePath.ToStdString(), m_textMessagingTransport))
    {
        log_warn("Could not start text messaging: %s", session.lastError().c_str());
        return;
    }

    session.protocol().setMyCallsign(
        wxGetApp().appConfiguration.reportingConfiguration.reportingCallsign->ToStdString());
    session.snoop().setMyCallsign(session.protocol().myCallsign());

    applyChatModem_();
    updateTextChatTransmitPermission_();
}

//-------------------------------------------------------------------------
// logStationHeard_(): the stations heard log (Preferences, Options, Station)
// gets a line for each station whose chat frames are heard, at most one per
// station and frequency every ten minutes.
//-------------------------------------------------------------------------
void MainFrame::logStationHeard_(std::string const& callsign, float snr, std::string const& modem)
{
    if (callsign.empty()) return;

    CallAfter([this, callsign, snr, modem]() {
        if (wxGetApp().m_reporters.empty()) return;

        constexpr auto RELOG_AFTER = std::chrono::minutes(10);
        auto now = std::chrono::steady_clock::now();
        int64_t frequency = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency;
        auto last = stationsHeardLogged_.find(callsign);
        if (last != stationsHeardLogged_.end() && last->second.second == frequency &&
            now - last->second.first < RELOG_AFTER)
        {
            return;
        }
        stationsHeardLogged_[callsign] = {now, frequency};

        signed char snrDb = (signed char)std::max(-128.0f, std::min(127.0f, std::round(snr)));
        for (auto& reporter : wxGetApp().m_reporters)
        {
            reporter->addReceiveRecord(callsign, modem, frequency > 0 ? (uint64_t)frequency : 0, snrDb);
        }
    });
}

//-------------------------------------------------------------------------
// applyChatModem_(): hands text chat to our own modem or to data2g-host.
// Data2G's transport keeps its connection only while it is chosen; a change
// of host or port reconnects.
//-------------------------------------------------------------------------
void MainFrame::applyChatModem_()
{
    if (m_textMessagingTransport == nullptr || m_data2gTransport == nullptr) return;

    auto& config = wxGetApp().appConfiguration;
    auto& protocol = TextMessaging::TextMessagingSession::instance().protocol();

    protocol.setMyLocator(config.reportingConfiguration.reportingGridSquare->ToStdString(),
                          config.reportingConfiguration.reportingSendGridSquare);

    // data2g-host's MYCALL, BCAST FROM and sessions use the chat callsign.
    m_data2gTransport->setMyCallsign(config.reportingConfiguration.reportingCallsign->ToStdString());

    // Files from the stations on the auto-accept list go straight into the
    // received files folder.
    std::vector<std::string> autoAccept;
    wxStringTokenizer calls(config.data2gAutoAcceptFilesFrom.get(), " ,;\t");
    while (calls.HasMoreTokens()) autoAccept.push_back(calls.GetNextToken().ToStdString());
    m_data2gTransport->setFileAutoAccept(std::string(chatReceivedFilesFolder().utf8_str()), autoAccept);

    if (config.data2gEnabled)
    {
        TextMessaging::Data2GTransport::Settings settings;
        settings.host = ((wxString)config.data2gHost).ToStdString();
        settings.kissPort = config.data2gKissPort;
        settings.commandPort = config.data2gCommandPort;
        settings.useSessions = config.data2gSessions;

        bool changed = !data2gChatActive_.load() || settings.host != appliedData2GSettings_.host ||
                       settings.kissPort != appliedData2GSettings_.kissPort ||
                       settings.commandPort != appliedData2GSettings_.commandPort ||
                       settings.useSessions != appliedData2GSettings_.useSessions;
        if (!changed) return;

        log_info("Text chat now goes through data2g-host at %s (KISS %d, commands %d)%s", settings.host.c_str(),
                 settings.kissPort, settings.commandPort,
                 settings.useSessions ? "" : ", group only");
        // Anything our own transmitter still had queued for chat goes.
        m_textMessagingTransport->abort();
        m_data2gTransport->setGear(chatTempo_());
        m_data2gTransport->start(settings);
        appliedData2GSettings_ = settings;
        data2gChatActive_.store(true, std::memory_order_release);
        protocol.setTransport(m_data2gTransport);

        // The timers follow the tempo's Data2G mode from here on
        // (applyGlissandoToModem_()).
        TextMessaging::AirTiming timing = m_data2gTransport->airTiming();
        protocol.setAirTiming(timing);
        appliedAirTiming_ = timing;
    }
    else
    {
        if (!data2gChatActive_.load()) return;

        log_info("Text chat back on our own modem");
        data2gChatActive_.store(false, std::memory_order_release);
        protocol.setTransport(m_textMessagingTransport);
        m_data2gTransport->stop();

        TextMessaging::AirTiming timing = textMessagingModem().airTiming();
        protocol.setAirTiming(timing);
        appliedAirTiming_ = timing;
    }
}

double MainFrame::chatMessageAirSeconds(const std::string& text)
{
    // Not estimated for Data2G, whose burst length depends on how the host
    // packs the frames into codewords.
    if (data2gChatActive_.load()) return 0.0;
    return textMessagingModem().glissandoMessageSeconds(
        text, wxGetApp().appConfiguration.reportingConfiguration.reportingCallsign->ToStdString());
}

int MainFrame::chatTransmitGear()
{
    // With Data2G each tempo is a Data2G mode, so a message can still take
    // a tempo of its own.
    if (data2gChatActive_.load()) return chatTempo_();
    if (!textMessagingModem().glissandoConfig().enabled) return 0;
    return textMessagingModem().glissandoStatus().transmitGear;
}

double MainFrame::chatSendProgress()
{
    if (data2gChatActive_.load() || m_textMessagingTransport == nullptr) return -1.0;
    return m_textMessagingTransport->keyingProgress();
}

int MainFrame::chatTimeOutSeconds()
{
    // With the app's timer off, the rig's own is still likely there, and
    // 180 s is the usual setting.
    int seconds = timeOutTimerSeconds();
    return seconds > 0 ? seconds : 180;
}

bool MainFrame::chatWaitsForEngage()
{
    return m_textMessagingTransport != nullptr && !data2gChatActive_.load() && !m_RxRunning;
}

void MainFrame::chatStopKeying()
{
    if (data2gChatActive_.load() || m_textMessagingTransport == nullptr) return;

    log_info("Chat keying stopped by the operator");
    m_textMessagingTransport->abort();

    // What the sound card has not played yet goes too, as for Abort on the
    // console.
    if (g_rxUserdata != nullptr && g_rxUserdata->outfifo1 != nullptr) g_rxUserdata->outfifo1->reset();
}

bool MainFrame::chatCanSendFiles(wxString& why)
{
    if (m_data2gTransport == nullptr || !data2gChatActive_.load())
    {
        why = _("Files go through a Data2G session. Choose Send chat through Data2G, with sessions, "
                "in Preferences.");
        return false;
    }
    if (!appliedData2GSettings_.useSessions)
    {
        why = _("Files go through a Data2G session. Turn on Connect a session for messages to one "
                "station in Preferences.");
        return false;
    }
    if (!m_data2gTransport->sendsFiles())
    {
        why = _("data2g-host's command and session ports are not connected yet.");
        return false;
    }
    why.clear();
    return true;
}

uint64_t MainFrame::chatSendFile(const std::string& callsign, const wxString& path, wxString& error)
{
    wxString why;
    if (!chatCanSendFiles(why))
    {
        error = why;
        return 0;
    }
    std::string reason;
    uint64_t id = m_data2gTransport->sendFile(callsign, std::string(path.utf8_str()), reason);
    if (id == 0) error = wxString::FromUTF8(reason);
    else log_info("File %s queued for %s through Data2G", (const char*)path.utf8_str(), callsign.c_str());
    return id;
}

bool MainFrame::chatAcceptFile(uint64_t id, const wxString& path, wxString& error)
{
    if (m_data2gTransport == nullptr) return false;
    std::string reason;
    if (m_data2gTransport->acceptFile(id, std::string(path.utf8_str()), reason)) return true;
    error = wxString::FromUTF8(reason);
    return false;
}

void MainFrame::chatDeclineFile(uint64_t id)
{
    if (m_data2gTransport != nullptr) m_data2gTransport->declineFile(id);
}

bool MainFrame::chatCancelFile(uint64_t id)
{
    return m_data2gTransport != nullptr && m_data2gTransport->cancelFile(id);
}

std::vector<TextMessaging::Data2G::FileTransfer> MainFrame::chatFileTransfers()
{
    if (m_data2gTransport == nullptr) return {};
    return m_data2gTransport->fileTransfers();
}

uint64_t MainFrame::chatFileTransferChanges()
{
    return m_data2gTransport != nullptr ? m_data2gTransport->fileTransferChanges() : 0;
}

wxString MainFrame::chatReceivedFilesFolder()
{
    wxString folder = wxGetApp().appConfiguration.data2gReceivedFilesFolder;
    if (!folder.IsEmpty()) return folder;
    return wxStandardPaths::Get().GetDocumentsDir() + wxFileName::GetPathSeparator() +
           wxString("Glissando received files");
}

void MainFrame::chatSetReceivedFilesFolder(const wxString& folder)
{
    if (folder.IsEmpty() || folder == wxGetApp().appConfiguration.data2gReceivedFilesFolder.get()) return;
    wxGetApp().appConfiguration.data2gReceivedFilesFolder = folder;
    wxGetApp().appConfiguration.save(pConfig);
    applyChatModem_();
}

wxString MainFrame::chatModemStatus()
{
    if (!data2gChatActive_.load() || m_data2gTransport == nullptr) return wxEmptyString;

    TextMessaging::Data2GTransport::Status status = m_data2gTransport->status();
    wxString where = wxString::Format("%s:%d", wxString::FromUTF8(appliedData2GSettings_.host),
                                      appliedData2GSettings_.kissPort);
    if (!status.kissConnected)
    {
        return wxString::Format(_("Data2G: no data2g-host at %s yet; retrying."), where);
    }

    wxString line = wxString::Format(_("Data2G at %s"), where);
    if (!status.commandConnected)
    {
        line += _(", command port not connected");
    }
    else if (status.groupPort == 0)
    {
        line += _(", opening the GLISS group");
    }
    else if (!status.groupMode.empty())
    {
        line += wxString::Format(_(", group GLISS in %s"), wxString::FromUTF8(status.groupMode));
    }
    if (!status.sessionPeer.empty())
    {
        line += wxString::Format(_(", session with %s"), wxString::FromUTF8(status.sessionPeer));
        if (status.sessionUnacked > 0 && status.sessionUnackedExact)
        {
            line += wxString::Format(_(" (%lld bytes not yet acknowledged)"), (long long)status.sessionUnacked);
        }
        else if (status.sessionUnacked > 0)
        {
            line += _(" (not all acknowledged yet)");
        }
    }
    else if (status.sessionConnecting)
    {
        line += _(", calling for a session");
    }
    if (!status.error.empty() && !status.commandConnected)
    {
        line += wxString::Format(" (%s)", wxString::FromUTF8(status.error));
    }
    return line + ".";
}

//-------------------------------------------------------------------------
// updateTextChatTransmitPermission_(): with the preference on, text chat
// transmits only where US rules permit data. The frequency is the one FreeDV
// already keeps: the rig's when rig control reports it, otherwise the one
// typed into the main window, and zero while neither has said.
//-------------------------------------------------------------------------
void MainFrame::updateTextChatTransmitPermission_()
{
    // The frequency box passes through zero while the window is being built,
    // before text chat has started; startTextMessaging_() applies the real
    // frequency when it does.
    if (m_textMessagingTransport == nullptr) return;

    std::string reason;
    if (wxGetApp().appConfiguration.textChatUsDataSegmentsOnly)
    {
        reason = TextMessaging::usDataTransmitRestriction(
            wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency);
    }

    auto& protocol = TextMessaging::TextMessagingSession::instance().protocol();
    if (reason == protocol.transmitInhibitedReason()) return;

    if (reason.empty())
    {
        log_info("Text chat may transmit again");
    }
    else
    {
        log_info("Text chat transmit inhibited: %s", reason.c_str());
    }

    protocol.setTransmitInhibited(reason);
}

//-------------------------------------------------------------------------
// stopTextMessaging_()
//-------------------------------------------------------------------------
void MainFrame::stopTextMessaging_()
{
    if (m_textMessagingTransport != nullptr) m_textMessagingTransport->abort();

    // Its thread hands frames to the protocol, so it goes first.
    if (m_data2gTransport != nullptr) m_data2gTransport->stop();
    data2gChatActive_.store(false, std::memory_order_release);

    TextMessaging::TextMessagingSession::instance().stop();

    textMessagingModem().setFrameCallback(nullptr);
    textMessagingModem().close();

    delete m_data2gTransport;
    m_data2gTransport = nullptr;

    delete m_textMessagingTransport;
    m_textMessagingTransport = nullptr;
}

//-------------------------------------------------------------------------
// setTextMessagingPtt_(): keys and unkeys the radio for one chat burst,
// through the same path the voice keyer uses.
//-------------------------------------------------------------------------
void MainFrame::setTextMessagingPtt_(bool keyed)
{
    if (keyed)
    {
        // Normally already set by the transport. But a burst sent as several
        // keyings lets the radio up in between, and that unkeying released
        // ownership below; running after it on this thread, this takes it
        // back before the radio is keyed again.
        textMessagingTxQueue().setOwnsTransmitter(true);

        if (!m_btnTogPTT->GetValue())
        {
            m_btnTogPTT->SetValue(true);
            togglePTT();
        }

        return;
    }

    if (m_btnTogPTT->GetValue())
    {
        m_btnTogPTT->SetValue(false);
        endingTx.store(true, std::memory_order_release);
        togglePTT();
    }

    // Ownership is released only now: the transmit thread has to keep
    // microphone audio off the air for the whole changeover, not just until
    // the burst has run out of samples.
    textMessagingTxQueue().setOwnsTransmitter(false);
}

//-------------------------------------------------------------------------
// ~MainFrame()
//-------------------------------------------------------------------------
MainFrame::~MainFrame()
{
    // Stops the chat session before the modems it uses go away.
    stopTextMessaging_();

    if (m_snoopDialog != nullptr)
    {
        m_snoopDialog->Destroy();
        m_snoopDialog = nullptr;
    }

    if (m_textMessagingDialog != nullptr)
    {
        m_textMessagingDialog->Close();
        m_textMessagingDialog->Destroy();
        m_textMessagingDialog = nullptr;
    }

#ifdef FTEST
    fclose(ftest);
    #endif

    wxGetApp().rigPttController = nullptr;
    wxGetApp().rigFrequencyController = nullptr;
    wxGetApp().m_pttInSerialPort = nullptr;
    
    exportConfiguration_(pConfig);

    m_togBtnOnOff->Disconnect(wxEVT_UPDATE_UI, wxUpdateUIEventHandler(MainFrame::OnTogBtnOnOffUI), NULL, this);

    {
        std::unique_lock<std::recursive_mutex> lk(stoppingMutex);
        if (m_RxRunning)
        {
            stopRxStream();
        }  
    }

    if (g_sfRecFile != NULL)
    {
        sf_close(g_sfRecFile);
        g_sfRecFile = NULL;
    }
    if (g_sfRecFileFromModulator != NULL)
    {
        sf_close(g_sfRecFileFromModulator);
        g_sfRecFileFromModulator = NULL;
    }
#ifdef _USE_TIMER
    if(m_plotTimer.IsRunning())
    {
        m_plotTimer.Stop();
        m_plotDemodInTimer.Stop();
        Unbind(wxEVT_TIMER, &MainFrame::OnTimer, this);
    }
#endif //_USE_TIMER

#ifdef _USE_ONIDLE
    Disconnect(wxEVT_IDLE, wxIdleEventHandler(MainFrame::OnIdle), NULL, this);
#endif // _USE_ONIDLE

    if (optionsDlg != NULL) {
        delete optionsDlg;
        optionsDlg = NULL;
    }

    wxGetApp().rigFrequencyController = nullptr;
    wxGetApp().rigPttController = nullptr;
    wxGetApp().m_reporters.clear();

    auto engine = AudioEngineFactory::GetAudioEngine();
    engine->stop();
    engine->setOnEngineError(nullptr, nullptr);
}


#ifdef _USE_ONIDLE
void MainFrame::OnIdle(wxIdleEvent &) {
}
#endif

int MainFrame::getIdealStationsHeardColumnLength_(int col)
{
    int curColWidth = m_lastReportedCallsignListView->GetColumnWidth(col);
    
    for (int index = 0; index < m_lastReportedCallsignListView->GetItemCount(); index++)
    {
        auto itemText = m_lastReportedCallsignListView->GetItemText(index, col);
        wxSize itemSize = m_lastReportedCallsignListView->GetTextExtent(itemText);
        
        curColWidth = std::max(curColWidth, itemSize.GetWidth() + 10); // 10px buffer around text
    }
    
    return curColWidth;
}

#ifdef _USE_TIMER
//----------------------------------------------------------------
// OnTimer()
//
// when the timer fires every DT seconds we update the GUI displays.
// the tabs only the plot that is visible actually gets updated, this
// keeps CPU load reasonable
//----------------------------------------------------------------
void MainFrame::OnTimer(wxTimerEvent &evt)
{
    short demodInPlotSamples[WAVEFORM_PLOT_BUF];
    bool txState = false;
    bool halfDuplexState = false;

    auto& timer = evt.GetTimer();
    auto timerId = timer.GetId();
    if (!m_RxRunning || !timer.IsRunning())
    {
        return;
    }
    
    if (timerId == ID_TIMER_UPDATE_OTHER || timerId == ID_TIMER_DEMOD_IN)
    {
        txState = g_tx.load(std::memory_order_relaxed);
        halfDuplexState = g_half_duplex.load(std::memory_order_relaxed);
    }

    if (timerId == ID_TIMER_UPD_FREQ)
    {
        // show freq. and mode [UP]
        if (wxGetApp().rigFrequencyController && wxGetApp().rigFrequencyController->isConnected()) 
        {
            log_debug("update freq and mode ...."); 
            wxGetApp().rigFrequencyController->requestCurrentFrequencyMode();
        }
        pollRigMeters_();
     }
      else if (timerId == ID_TIMER_DEMOD_IN)
      {
          if (g_plotDemodInFifo.read(demodInPlotSamples, WAVEFORM_PLOT_BUF)) {
              memset(demodInPlotSamples, 0, WAVEFORM_PLOT_BUF*sizeof(short));
          }
          m_panelDemodIn->add_new_short_samples(demodInPlotSamples, WAVEFORM_PLOT_BUF, 32767);
          m_panelDemodIn->refreshData();
      }
      else
      {
         // Update average magnitudes. The Glissando console's waterfall
         // draws g_avmag_waterfall (see glissandoSpectrum()).
         float rxSpectrum[MODEM_STATS_NSPEC];
         memset(rxSpectrum, 0, sizeof(float) * MODEM_STATS_NSPEC);
         bool txNotInFullDuplex = halfDuplexState && txState;
         if (!txNotInFullDuplex)
         {
             while (g_avmag.numUsed() >= MODEM_STATS_NSPEC)
             {
                 g_avmag.read(rxSpectrum, MODEM_STATS_NSPEC);
                 for (int index = 0; index < MODEM_STATS_NSPEC; index++)
                 {
                     g_avmag_waterfall[index] = BETA * g_avmag_waterfall[index] + (1.0 - BETA) * rxSpectrum[index];
                 }
              }
         }
         else
         {
            // Assume zero spectrum to avoid waterfall artifacts. Note:
            // this display's dB scale runs 0 (loudest) to MIN_MAG_DB
            // (quietest) -- memset()'ing to zero bytes sets every bin to
            // 0.0 dB, the *loudest* possible value, not silence, which
            // painted the waterfall solid instead of blanking it. Fill
            // with MIN_MAG_DB instead so it actually reads as quiet/black.
            std::fill_n(g_avmag_waterfall, MODEM_STATS_NSPEC, MIN_MAG_DB);
         }

        /* FIFO and PortAudio under/overflow debug counters */
        optionsDlg->DisplayFifoPACounters();

        // command from UDP thread that is best processed in main thread to avoid seg faults

        if (m_schedule_restore) {
            if (IsIconized())
                Restore();
            m_schedule_restore = false;
        }
    }
    
    if (timerId == ID_TIMER_DEMOD_IN && !txState && m_RxRunning)
    {
        // Level Gauge: From Radio peaks -------------------------------------
        int maxDemodIn = 0;
        for(int i=0; i<WAVEFORM_PLOT_BUF; i++)
            if (maxDemodIn < abs(demodInPlotSamples[i]))
                maxDemodIn = abs(demodInPlotSamples[i]);

        // peak from last second
        if (maxDemodIn > m_maxLevel)
            m_maxLevel = maxDemodIn;

        // Peak Reading meter: updates peaks immediately, then slowly decays
        int maxScaled = (int)(100.0 * ((float)m_maxLevel/32767.0));
        m_gaugeLevel->SetValue(maxScaled);
        m_maxLevel *= LEVEL_BETA;
    }
}
#endif

void MainFrame::topFrame_OnClose( wxCloseEvent& event )
{
    // The console goes with this frame.
    if (!terminating_ && m_glissandoConsole != nullptr) closeGlissandoConsole_();

    if (terminating_)
    {
        // A previous close request already kicked off the async RX/PTT
        // shutdown below, which calls Destroy() itself once done (see
        // OnTogBtnOnOff()) -- nothing left to do here. This guards more than
        // just the shutdown below: m_RxRunning isn't
        // cleared until deep inside that shutdown (stopRxStream(), called
        // after the rig-disconnect code), so a repeat close request arriving
        // first would otherwise fall into the `if (m_RxRunning)` block again
        // and re-enter OnTogBtnOnOff() a second time concurrently with the
        // shutdown already in progress.
        return;
    }

    if (m_RxRunning)
    {
        if (m_btnTogPTT->GetValue())
        {
            // Stop PTT first
            togglePTT();
        }
        
        // Stop execution.
        terminating_ = true;
        wxCommandEvent* offEvent = new wxCommandEvent(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, m_togBtnOnOff->GetId());
        offEvent->SetEventObject(m_togBtnOnOff);
        m_togBtnOnOff->SetValue(false);
        OnTogBtnOnOff(*offEvent);
        delete offEvent;
    } 
    else
    {
        TopFrame::topFrame_OnClose(event);
    }
}

//-------------------------------------------------------------------------
// OnExit()
//-------------------------------------------------------------------------
void MainFrame::OnExit(wxCommandEvent&)
{
    if (m_RxRunning)
    {
        if (m_btnTogPTT->GetValue())
        {
            // Stop PTT first
            togglePTT();
        }
        
        // Stop execution.
        terminating_ = true;
        wxCommandEvent* offEvent = new wxCommandEvent(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, m_togBtnOnOff->GetId());
        offEvent->SetEventObject(m_togBtnOnOff);
        m_togBtnOnOff->SetValue(false);
        OnTogBtnOnOff(*offEvent);
        delete offEvent;
    } 
    else
    {
        Destroy();
    }
}

void MainFrame::performFreeDVOn_()
{
    log_debug("Start .....");
    isModemRunning.store(false, std::memory_order_release);
    endingTx.store(false, std::memory_order_release);
    g_tx.store(false, std::memory_order_release);

    executeOnUiThreadAndWait_([&]() 
    {
        // Blank out the spectrum. Note: this display's dB scale runs 0
        // (loudest) to MIN_MAG_DB (quietest) -- memset()'ing to zero bytes
        // sets every bin to 0.0 dB, the *loudest* possible value, not
        // silence. Fill with MIN_MAG_DB instead so it actually reads as
        // quiet/black.
        std::fill_n(g_avmag_waterfall, MODEM_STATS_NSPEC, MIN_MAG_DB);

        // Reset plot FIFO
        g_plotDemodInFifo.reset();
    });
    
    //
    // Start Running -------------------------------------------------
    //

    g_half_duplex.store(wxGetApp().appConfiguration.halfDuplexMode, std::memory_order_release);

    m_maxLevel = 0;
    executeOnUiThreadAndWait_([&]() 
    {
        m_gaugeLevel->SetValue(0);
        
    });
    
    // attempt to start sound cards and tx/rx processing
    std::promise<bool> tmpPromise;
    std::future<bool> tmpFuture = tmpPromise.get_future();

    // Note: this executes on the UI thread as macOS may need to display popups
    // to process this request.
    CallAfter([&]() {
        VerifyMicrophonePermissions(tmpPromise);
    });

    tmpFuture.wait();
    
    if (tmpFuture.get())
    {
        bool soundCardSetupValid = false;
        executeOnUiThreadAndWait_([&]() {
            soundCardSetupValid = validateSoundCardSetup();
        });
        
        if (soundCardSetupValid)
        {
            wxGetApp().m_reporters.clear();
            
            startRxStream();

            if (m_RxRunning)
            {
                // Enable Tune button if TX is valid.
                if (g_nSoundCards > 1)
                {
                    executeOnUiThreadAndWait_([&]() {
                        m_btnTogTune->Enable(true);
                    });
                }

                // attempt to start PTT ......            
                if (wxGetApp().appConfiguration.rigControlConfiguration.hamlibUseForPTT)
                {
                    OpenHamlibRig();
                }
                else if (wxGetApp().appConfiguration.rigControlConfiguration.useSerialPTT) 
                {
                    OpenSerialPort();
                }
                else
                {
                    wxGetApp().rigPttController = nullptr;
                }
                
    #if defined(WIN32)
                if (wxGetApp().appConfiguration.rigControlConfiguration.useOmniRig)
                {
                    // OmniRig can be anbled along with serial port PTT.
                    // The logic below will ensure we don't overwrite the serial PTT
                    // handler.
                    OpenOmniRig();
                }
    #endif // defined(WIN32)
                
                // The radio keeps whatever frequency it is on; see
                // onRadioConnected_() for the one exception.

                // The stations heard log is the one reporter left: each
                // station whose chat is heard goes into it (see
                // logStationHeard_()). PSK Reporter and the UDP reporters
                // are parked.
                wxGetApp().m_reporters.clear();
                {
                    auto csvPath = wxGetApp().appConfiguration.reportingConfiguration.csvLogFilePath.get();
                    if (!csvPath.IsEmpty())
                    {
                        wxGetApp().m_reporters.push_back(std::make_shared<CsvReporter>(csvPath.ToStdString()));
                    }
                }

                if (wxGetApp().appConfiguration.rigControlConfiguration.useSerialPTTInput)
                {
                    OpenPTTInPort();
                }

                executeOnUiThreadAndWait_([&]() 
                {

        #ifdef _USE_TIMER
                    m_plotTimer.Start(_REFRESH_TIMER_PERIOD, wxTIMER_CONTINUOUS);
                    m_plotDemodInTimer.Start(_REFRESH_TIMER_PERIOD, wxTIMER_CONTINUOUS);

                    m_updFreqStatusTimer.Start(1000); // every 1 second[UP]
        #endif // _USE_TIMER
                });

                isModemRunning.store(true, std::memory_order_release);
            }
        }
    }
    else
    {
        executeOnUiThreadAndWait_([&]() 
        {
            wxMessageBox(wxString("Microphone permissions must be granted to FreeDV for it to function properly."), wxT("Error"), wxOK | wxICON_ERROR, this);
        });
    }
}

void MainFrame::performFreeDVOff_()
{
    log_debug("Stop .....");
    
    //
    // Stop Running -------------------------------------------------
    //

    isModemRunning.store(false, std::memory_order_release);

#ifdef _USE_TIMER
    executeOnUiThreadAndWait_([&]() 
    {
        // Disable Tune mode if needed
        if (m_btnTogTune->GetValue())
        {
            m_btnTogTune->SetValue(false);
            
            // Ensures that Tune button actions are actually run to stop transmitting the tone.
            wxCommandEvent tmpEvent;
            OnTogBtnTune(tmpEvent);
        }
        
        m_btnTogTune->Enable(false);

        m_plotTimer.Stop();
        m_plotDemodInTimer.Stop();
        m_updFreqStatusTimer.Stop(); // [UP]
    });
#endif // _USE_TIMER
    
    // always end with PTT in rx state
    if (wxGetApp().rigPttController != nullptr && wxGetApp().rigPttController->isConnected())
    {
        wxGetApp().rigPttController->ptt(false);
        wxGetApp().rigPttController->disconnect();
    }
    // Dropping the last shared_ptr reference below runs the controller's
    // destructor, which blocks until the rig actually finishes
    // disconnecting. Against an unresponsive radio (e.g. powered off, via
    // rigctld) that can take far longer than Hamlib's own client-side
    // timeout/retry settings suggest, since those don't bound however long
    // rigctld itself waits on the physical radio it's talking to. Move the
    // last reference onto a detached thread so that wait can't hold up
    // turning the modem off (or app shutdown) -- the controller stays
    // alive exactly as long as it needs to, just off to the side rather
    // than blocking here. The associated future lets a terminating shutdown
    // (see OnTogBtnOnOff()) wait a bounded amount of time for this to finish
    // before the process exits out from under the thread.
    std::promise<void> pttDisconnectProm;
    rigPttDisconnectFuture_ = pttDisconnectProm.get_future();
    std::thread([ptr = std::move(wxGetApp().rigPttController), prom = std::move(pttDisconnectProm)]() mutable
    {
        SetThreadName("RigPttDisconnect");
        ptr = nullptr;
        prom.set_value();
    }).detach();

    if (wxGetApp().rigFrequencyController != nullptr && wxGetApp().rigFrequencyController->isConnected())
    {
        wxGetApp().rigFrequencyController->disconnect();
    }
    std::promise<void> freqDisconnectProm;
    rigFreqDisconnectFuture_ = freqDisconnectProm.get_future();
    std::thread([ptr = std::move(wxGetApp().rigFrequencyController), prom = std::move(freqDisconnectProm)]() mutable
    {
        SetThreadName("RigFreqDisconnect");
        ptr = nullptr;
        prom.set_value();
    }).detach();

    if (wxGetApp().appConfiguration.rigControlConfiguration.useSerialPTTInput)
    {
        ClosePTTInPort();
    }
    
    executeOnUiThreadAndWait_([&]() 
    {
        m_btnTogPTT->SetValue(false);
    });
    
    stopRxStream();
         
    wxGetApp().m_reporters.clear();

    executeOnUiThreadAndWait_([&]() 
    {
        m_btnTogPTT->Disable();
    });
}

//-------------------------------------------------------------------------
// OnTogBtnOnOff()
//-------------------------------------------------------------------------
void MainFrame::OnTogBtnOnOff(wxCommandEvent&)
{
    if (!m_togBtnOnOff->IsEnabled()) return;

    m_togBtnOnOff->SetFocus();
    
    // Disable buttons while on/off is occurring
    m_togBtnOnOff->Enable(false);
    m_btnTogPTT->Enable(false);
        
    // we are attempting to start

    if (!m_RxRunning)
    {
        std::thread onOffExec([this]() 
        {
            SetThreadName("TurningOn");

            performFreeDVOn_();
            
            if (!m_RxRunning)
            {
                // Startup failed.
                performFreeDVOff_();
            }

            // On/Off actions complete, re-enable button.
            executeOnUiThreadAndWait_([&]() {
                bool txEnabled = 
                    m_RxRunning &&
                    (g_nSoundCards == 2);
                
                m_btnTogPTT->Enable(txEnabled);
                optionsDlg->setSessionActive(m_RxRunning);

                if (m_RxRunning)
                {
                    m_togBtnOnOff->SetLabel(wxT("&Stop Modem"));
                }
                m_togBtnOnOff->SetValue(m_RxRunning);
                m_togBtnOnOff->Enable(true);

                // On some systems the Report Frequency box ends up getting
                // focus after clicking on Start. This causes the frequency
                // to never update. To avoid this, we force focus to be elsewhere
                // in the window.
                m_auiNbookCtrl->SetFocus();
            });
        });
        onOffExec.detach();
    }
    else
    {
        std::thread onOffExec([this]() 
        {
            SetThreadName("TurningOff");

            performFreeDVOff_();

            if (terminating_)
            {
                // We're about to destroy the frame and let the process exit,
                // which would kill the detached rig-disconnect threads from
                // performFreeDVOff_() mid-flight if a slow/unresponsive rig
                // hasn't finished with them yet -- e.g. a queued ptt(false)
                // might never reach the radio. Give them a bounded chance to
                // finish first; an unresponsive rig still can't hang shutdown
                // indefinitely, it just gets abandoned after the grace period.
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                if (rigPttDisconnectFuture_.valid())
                {
                    rigPttDisconnectFuture_.wait_until(deadline);
                }
                if (rigFreqDisconnectFuture_.valid())
                {
                    rigFreqDisconnectFuture_.wait_until(deadline);
                }
            }

            // On/Off actions complete, re-enable button.
            executeOnUiThreadAndWait_([&]() {
                m_btnTogPTT->Enable(m_RxRunning);
                optionsDlg->setSessionActive(m_RxRunning);
                m_togBtnOnOff->SetValue(m_RxRunning);
                m_togBtnOnOff->SetLabel(wxT("&Start Modem"));
                m_togBtnOnOff->Enable(true);

                if (terminating_)
                {
                    CallAfter([&]() { Destroy(); });
                }
            });
        });
        onOffExec.detach();
    }
}

//-------------------------------------------------------------------------
// stopRxStream()
//-------------------------------------------------------------------------
void MainFrame::stopRxStream()
{
    std::unique_lock<std::recursive_mutex> lk(stoppingMutex);

    if(m_RxRunning)
    {
        StopLowLatencyActivity();

        m_RxRunning = false;

        // Nothing can play a burst once the audio threads are gone.
        if (m_textMessagingTransport != nullptr) m_textMessagingTransport->abort();

        if (m_txThread)
        {
            m_txThread->stop();
            
            if (txOutSoundDevice)
            {
                txOutSoundDevice->stop();
                txOutSoundDevice.reset();
            }
            
            m_txThread = nullptr;
        }

        if (m_rxThread)
        {
            m_rxThread->stop();
            
            if (rxInSoundDevice)
            {
                rxInSoundDevice->stop();
                rxInSoundDevice.reset();
            }

            m_rxThread = nullptr;
        }

        wxGetApp().linkStep = nullptr;
        destroy_fifos();
        
        delete g_rxUserdata;
        
        auto engine = AudioEngineFactory::GetAudioEngine();
        engine->stop();
        engine->setOnEngineError(nullptr, nullptr);
    }
}

void MainFrame::destroy_fifos(void)
{
    if (g_rxUserdata->infifo1) delete g_rxUserdata->infifo1;
    if (g_rxUserdata->outfifo1) delete g_rxUserdata->outfifo1;
    if (g_rxUserdata->infifo2) delete g_rxUserdata->infifo2;
    if (g_rxUserdata->outfifo2) delete g_rxUserdata->outfifo2;
    
    g_rxUserdata->infifo1 = nullptr;
    g_rxUserdata->infifo2 = nullptr;
    g_rxUserdata->outfifo1 = nullptr;
    g_rxUserdata->outfifo2 = nullptr;
}

//-------------------------------------------------------------------------
// startRxStream()
//-------------------------------------------------------------------------
void MainFrame::startRxStream()
{
    log_debug("startRxStream .....");
    if(!m_RxRunning) {
        m_RxRunning = true;
        
        auto engine = AudioEngineFactory::GetAudioEngine();
        engine->setOnEngineError([](IAudioEngine& dev, std::string const& error, void* state) { 
            MainFrame* castedThis = (MainFrame*)state;
            castedThis->onAudioEngineError_(dev, error, state); 
        }, this);
        engine->start();

        IAudioDevice::AudioErrorCallbackFn errorCallback = &MainFrame::OnAudioDeviceError_;
        
        // Init call back data structure ----------------------------------------------

        g_rxUserdata = new paCallBackData;
                
        // create FIFOs used to interface between IAudioEngine and txRx
        // processing loop, which iterates about once every 10-40ms
        // (depending on platform/audio library). Sample rate conversion, 
        // stats for spectral plots, and transmit processng are all performed 
        // in the tx/rxProcessing loop.
        //
        // Note that soundCard[12]InFifoSizeSamples are significantly larger than
        // the other FIFO sizes. This is to better handle PulseAudio/pipewire
        // behavior on some devices, where the system sends multiple *seconds*
        // of audio samples at once followed by long periods with no samples at
        // all. Without a very large FIFO size (or a way to dynamically change
        // FIFO sizes, which isn't recommended for real-time operation), we will
        // definitely lose audio.
        constexpr int MAX_INCOMING_AUDIO_SEC = 75;
        int m_fifoSize_ms = wxGetApp().appConfiguration.fifoSizeMs;
        int soundCard1InFifoSizeSamples = MAX_INCOMING_AUDIO_SEC * wxGetApp().appConfiguration.audioConfiguration.soundCard1In.sampleRate;

        // Glissando only talks to the radio: audio in from it, and, on a
        // station that transmits, audio out to it. There is no microphone or
        // speaker stream, so only the radio's two FIFOs exist.
        g_rxUserdata->infifo1 = new GenericFIFO<short>(soundCard1InFifoSizeSamples);
        g_rxUserdata->tmpReadRxBuffer_ = std::make_unique<short[]>(soundCard1InFifoSizeSamples);

        // The radio output FIFO holds at least 480 ms: three of the voice
        // modem's 160 ms frames, which is what sized it before that modem
        // went and what the chat transmitter's timing was tuned against.
        constexpr int MIN_TX_OUT_FIFO_MS = 480;
        int soundCard1OutFifoSizeSamples = 0;
        if (g_nSoundCards == 2)
        {
            soundCard1OutFifoSizeSamples = std::max(MIN_TX_OUT_FIFO_MS, m_fifoSize_ms) *
                wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.sampleRate / 1000;
            g_rxUserdata->outfifo1 = new GenericFIFO<short>(soundCard1OutFifoSizeSamples);
            g_rxUserdata->tmpWriteTxBuffer_ = std::make_unique<short[]>(soundCard1OutFifoSizeSamples);
        }

        log_debug("fifoSize_ms: %d infifo1: %d/outfifo1 %d",
                wxGetApp().appConfiguration.fifoSizeMs.get(), soundCard1InFifoSizeSamples, soundCard1OutFifoSizeSamples);

        if (g_nSoundCards == 0) 
        {
            executeOnUiThreadAndWait_([&]() {
                wxMessageBox(wxT("No radio sound device configured, use Preferences - Sound cards to configure"), wxT("Error"), wxOK);
            });
            
            m_RxRunning = false;
            
            engine->stop();
            engine->setOnEngineError(nullptr, nullptr);
            destroy_fifos();
            delete g_rxUserdata;
            return;
        }

        bool failed = false;

        // Note: we assume 2 channels, but IAudioEngine will automatically downgrade to 1 channel if needed.
        // The radio output is started first as the input's sample rate could change as a result (e.g. Bluetooth on macOS).
        if (g_nSoundCards == 2)
        {
            txOutSoundDevice = engine->getAudioDevice(wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.deviceName, IAudioEngine::AUDIO_ENGINE_OUT, wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.sampleRate, 2);

            if (!txOutSoundDevice)
            {
                executeOnUiThreadAndWait_([]() {
                    wxMessageBox(wxString::Format("Could not find TX output sound device '%s'. Please check settings and try again.", wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.deviceName.get()), wxT("Error"), wxOK);
                });
                failed = true;
            }
            else
            {
                txOutSoundDevice->setDescription("Glissando to Radio");
                txOutSoundDevice->setOnAudioDeviceChanged([](IAudioDevice&, std::string newDeviceName, void* state) {
                    MainFrame* castedThis = (MainFrame*)state;
                    castedThis->CallAfter(&MainFrame::handleAudioDeviceChange_<1, true>, std::move(newDeviceName));
                }, this);
                txOutSoundDevice->setOnAudioData(&OnTxOutAudioData_, g_rxUserdata);

                txOutSoundDevice->setOnAudioOverflow([](IAudioDevice&, void*)
                {
                    g_AEstatus2[3]++;
                }, nullptr);

                txOutSoundDevice->setOnAudioUnderflow([](IAudioDevice&, void*)
                {
                    g_AEstatus2[2]++;
                }, nullptr);

                txOutSoundDevice->setOnAudioError(errorCallback, this);
                txOutSoundDevice->start();
            }
        }

        if (!failed)
        {
            rxInSoundDevice = engine->getAudioDevice(wxGetApp().appConfiguration.audioConfiguration.soundCard1In.deviceName, IAudioEngine::AUDIO_ENGINE_IN, wxGetApp().appConfiguration.audioConfiguration.soundCard1In.sampleRate, 2);

            if (!rxInSoundDevice)
            {
                executeOnUiThreadAndWait_([]() {
                    wxMessageBox(wxString::Format("Could not find RX input sound device '%s'. Please check settings and try again.", wxGetApp().appConfiguration.audioConfiguration.soundCard1In.deviceName.get()), wxT("Error"), wxOK);
                });
                failed = true;
            }
            else
            {
                rxInSoundDevice->setDescription("Radio to Glissando");
                rxInSoundDevice->setOnAudioDeviceChanged([](IAudioDevice&, std::string newDeviceName, void* state) {
                    MainFrame* castedThis = (MainFrame*)state;
                    castedThis->CallAfter(&MainFrame::handleAudioDeviceChange_<1, false>, std::move(newDeviceName));
                }, this);
            }
        }

        if (failed)
        {
            rxInSoundDevice.reset();

            if (txOutSoundDevice)
            {
                txOutSoundDevice->stop();
                txOutSoundDevice.reset();
            }

            m_RxRunning = false;

            engine->stop();
            engine->setOnEngineError(nullptr, nullptr);
            destroy_fifos();
            delete g_rxUserdata;
            return;
        }

        // Re-save sample rates in case they were somehow invalid before
        // device creation.
        wxGetApp().appConfiguration.audioConfiguration.soundCard1In.sampleRate = rxInSoundDevice->getSampleRate();
        if (txOutSoundDevice)
        {
            wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.sampleRate = txOutSoundDevice->getSampleRate();
        }

        // reset debug stats for FIFOs

        g_infifo1_full.store(0, std::memory_order_relaxed);
        g_outfifo1_empty.store(0, std::memory_order_relaxed);
        g_infifo2_full.store(0, std::memory_order_relaxed);
        g_outfifo2_empty.store(0, std::memory_order_relaxed);
        for (int i=0; i<4; i++) {
            g_AEstatus1[i] = g_AEstatus2[i] = 0;
        }

        // optional tone in left channel to reliably trigger vox

        g_rxUserdata->leftChannelVoxTone = wxGetApp().appConfiguration.rigControlConfiguration.leftChannelVoxTone;
        g_rxUserdata->voxTonePhase = 0;

        // Set sound card callbacks
        rxInSoundDevice->setOnAudioData(&OnRxInAudioData_, g_rxUserdata);
        
        rxInSoundDevice->setOnAudioOverflow([](IAudioDevice&, void*)
        {
            g_AEstatus1[1]++;
        }, nullptr);
        
        rxInSoundDevice->setOnAudioUnderflow([](IAudioDevice&, void*)
        {
            g_AEstatus1[0]++;
        }, nullptr);
        
        rxInSoundDevice->setOnAudioError(errorCallback, this);
        
        // Nothing monitors the transmit audio any more.
        wxGetApp().linkStep = nullptr;
        
        // start tx/rx processing thread
        if (txOutSoundDevice)
        {
            // The transmit side has no input device: it only plays what the
            // chat modem queues, so it runs at the radio output's rate
            // throughout and takes its timing from that device.
            m_txThread = std::make_shared<TxRxThread>(true, txOutSoundDevice->getSampleRate(), txOutSoundDevice->getSampleRate(), txOutSoundDevice);

            if (!txOutSoundDevice->isRunning())
            {
                rxInSoundDevice.reset();
                txOutSoundDevice.reset();
                m_txThread = nullptr;
                m_RxRunning = false;
                return;
            }

            m_txThread->start();
        }

        // Decoded audio has nowhere to go, so the receive side's output rate
        // just matches its input.
        m_rxThread = std::make_shared<TxRxThread>(false, rxInSoundDevice->getSampleRate(), rxInSoundDevice->getSampleRate(), rxInSoundDevice);

        rxInSoundDevice->start();
        if (!rxInSoundDevice->isRunning())
        {
            if (txOutSoundDevice) txOutSoundDevice->stop();
            
            rxInSoundDevice.reset();
            txOutSoundDevice.reset();
            m_RxRunning = false;
            return;
        }

        m_rxThread->start();

        // Logic to ensure that both TX/RX threads start work at the 
        // same time. This makes sure there are no dropouts at the beginning
        // of full duplex TX.
        m_rxThread->waitForReady();
        if (m_txThread != nullptr)
        {
            m_txThread->waitForReady();
        }
        m_rxThread->signalToStart();
        if (m_txThread != nullptr)
        {
            m_txThread->signalToStart();
        }
    
        log_debug("starting tx/rx processing thread");

        // Work around an issue where the buttons stay disabled even if there
        // is an error opening one or more audio device(s).
        bool txDevicesRunning = 
            (!txOutSoundDevice || txOutSoundDevice->isRunning());
        bool rxDevicesRunning = 
            (rxInSoundDevice && rxInSoundDevice->isRunning());
        m_RxRunning = txDevicesRunning && rxDevicesRunning;
    }

    if (m_RxRunning)
    {
        StartLowLatencyActivity();
    }
}

bool MainFrame::validateSoundCardSetup(bool silent)
{
    bool canRun = true;

    // Translate device names to IDs
    auto engine = AudioEngineFactory::GetAudioEngine();
    engine->setOnEngineError([this, silent](IAudioEngine&, std::string error, void*) {
        if (silent) return;
        CallAfter([this, error = std::move(error)]() {
            wxMessageBox(wxString::Format(
                "Error encountered while initializing the audio engine: %s.",
                error), wxT("Error"), wxOK, this);
        });
    }, nullptr);
    engine->start();
    
    auto defaultInputDevice = engine->getDefaultAudioDevice(IAudioEngine::AUDIO_ENGINE_IN);
    auto defaultOutputDevice = engine->getDefaultAudioDevice(IAudioEngine::AUDIO_ENGINE_OUT);
    
    g_nSoundCards = radioSoundCardCount();
    
    // For the purposes of validation, number of channels isn't necessary.
    auto soundCard1InDevice = engine->getAudioDevice(wxGetApp().appConfiguration.audioConfiguration.soundCard1In.deviceName, IAudioEngine::AUDIO_ENGINE_IN, wxGetApp().appConfiguration.audioConfiguration.soundCard1In.sampleRate, 1);
    auto soundCard1OutDevice = engine->getAudioDevice(wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.deviceName, IAudioEngine::AUDIO_ENGINE_OUT, wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.sampleRate, 1);

    wxString failedDeviceName;
    if (wxGetApp().appConfiguration.audioConfiguration.soundCard1In.deviceName != "none" && !soundCard1InDevice)
    {
        failedDeviceName = wxGetApp().appConfiguration.audioConfiguration.soundCard1In.deviceName.get();
        canRun = false;
    }
    else if (canRun && wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.deviceName != "none" && !soundCard1OutDevice)
    {
        failedDeviceName = wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.deviceName.get();
        canRun = false;
    }
    
    if (canRun && g_nSoundCards == 0)
    {
        if (!silent)
        {
            // Initial setup. Display Easy Setup dialog.
            CallAfter([&]() {
                EasySetupDialog* dlg = new EasySetupDialog(this);
                if (dlg->ShowModal() == wxOK)
                {
                    // Show/hide frequency box based on CAT control status
                    m_freqBox->Show(isFrequencyControlEnabled_());


                    // Relayout window so that the changes can take effect.
                    m_panel->Layout();
                }
            });
        }
        canRun = false;
    }
    else if (!canRun)
    {
        if (!silent)
        {
            wxMessageBox(wxString::Format(
                "Your %s device cannot be found and may have been removed from your system. Please reattach this device, close this message box and retry. If this fails, go to Tools->Audio Config... to check your settings.",
                failedDeviceName), wxT("Sound Device Not Found"), wxOK, this);
        }
    }
    else
    {
        const int MIN_SAMPLE_RATE_RADIO = 8000;
        int failedSampleRate = 0;
        int expectedSampleRate = 0;
        int expectedSampleRate1Out = MIN_SAMPLE_RATE_RADIO;
        
        // Validate sample rates
        if (wxGetApp().appConfiguration.audioConfiguration.soundCard1In.deviceName != "none" && wxGetApp().appConfiguration.audioConfiguration.soundCard1In.sampleRate < MIN_SAMPLE_RATE_RADIO)
        {
            failedDeviceName = wxGetApp().appConfiguration.audioConfiguration.soundCard1In.deviceName.get();
            failedSampleRate = wxGetApp().appConfiguration.audioConfiguration.soundCard1In.sampleRate;
            expectedSampleRate = MIN_SAMPLE_RATE_RADIO;
            canRun = false;
        }
        else if (wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.deviceName != "none" && wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.sampleRate < expectedSampleRate1Out)
        {
            failedDeviceName = wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.deviceName.get();
            failedSampleRate = wxGetApp().appConfiguration.audioConfiguration.soundCard1Out.sampleRate;
            expectedSampleRate = expectedSampleRate1Out;
            canRun = false;
        }
        
        if (!canRun && !silent)
        {
            wxMessageBox(wxString::Format(
                "Your %s device is set to use a sample rate of %d, which is less than the minimum of %d. Please go to Tools->Audio Config... to check your settings.",
                failedDeviceName, failedSampleRate, expectedSampleRate), wxT("Sample Rate Too Low"), wxOK, this);
        }
    }
    
    engine->stop();
    engine->setOnEngineError(nullptr, nullptr);
    
    return canRun;
}

void MainFrame::onAudioEngineError_(IAudioEngine&, std::string const& error, void*)
{
     executeOnUiThreadAndWait_([&, error]() {
         wxMessageBox(wxString::Format(
                          "Error encountered while initializing the audio engine: %s.", 
                          error), wxT("Error"), wxOK, this); 
     });
}

void MainFrame::onAudioDeviceError_(std::string error)
{
    wxMessageBox(wxString::Format("Error encountered while processing audio: %s", std::move(error)), wxT("Error"), wxOK);
}

void MainFrame::OnAudioDeviceError_(IAudioDevice&, std::string const& error, void* state)
{
    MainFrame* castedState = (MainFrame*)state;
    log_error("%s", error.c_str());
    castedState->CallAfter(&MainFrame::onAudioDeviceError_, error);
}

void MainFrame::OnTxOutAudioData_(IAudioDevice& dev, void* data, size_t size, void* state) FREEDV_NONBLOCKING
{
    paCallBackData* cbData = static_cast<paCallBackData*>(state);
    short* audioData = static_cast<short*>(data);
    short* tmpOutput = cbData->tmpWriteTxBuffer_.get();

    auto toRead = std::min((size_t)cbData->outfifo1->numUsed(), size);
    auto isTuning = cbData->isTuning.load(std::memory_order_acquire);

    // Only whole buffers are normally played, so a short read waits for more.
    // But nothing follows the end of a chat burst, and its last few samples,
    // less than a buffer, would sit in the FIFO for good: the transmit thread
    // waits for them to play before it confirms the burst, and the transport
    // gave up waiting and unkeyed a second late. Play them out, padded with
    // silence.
    auto& chatQueue = textMessagingTxQueue();
    bool chatTail = toRead > 0 && toRead < size && chatQueue.isTransmitting() && chatQueue.isEmpty();

    if (toRead < size && !isTuning && !chatTail)
    {
        g_outfifo1_empty.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        size_t readCount = toRead >= size ? size : (chatTail ? toRead : 0);
        if (readCount > 0 && cbData->outfifo1->read(tmpOutput, readCount) != 0)
        {
            // Raced with a concurrent reset(); nothing was actually copied
            // into tmpOutput, so fall back to silence below instead of
            // replaying stale samples from the previous callback.
            toRead = 0;
        }

        auto numChannels = dev.getNumChannels();
        auto enableVoxTone = g_tx.load(std::memory_order_acquire) && cbData->leftChannelVoxTone.load(std::memory_order_acquire);
        auto sr = dev.getSampleRate();

        if (isTuning)
        {
            // This may be better as a pipeline step but would also add additional
            // complexity (i.e. additional decision steps to let through the sine wave
            // vs. regular TX).
            auto txLevel = g_tuneLevelScale.load(std::memory_order_acquire) * (SHRT_MAX / 2);

            // Load once before the loop and store once after to avoid per-sample atomic
            // memory barriers, which can cause the audio callback to overrun its deadline.
            auto sineWaveSampleNumber = cbData->tuneSineWaveSampleNumber.load(std::memory_order_acquire);
            const double phaseIncrement = 2.0 * M_PI * TUNE_TONE_FREQ / sr;
            for (unsigned long index = 0; index < size; index++)
            {
                auto carrierSample = txLevel * sin(phaseIncrement * sineWaveSampleNumber);
                for (int i = 0; i < numChannels; i++)
                {
                    *audioData++ = carrierSample;
                }
                // Conditional branch is cheaper than integer division (% sr).
                if (++sineWaveSampleNumber >= (int)sr)
                    sineWaveSampleNumber = 0;
            }
            cbData->tuneSineWaveSampleNumber.store(sineWaveSampleNumber, std::memory_order_release);
        }
        else
        {
            for (size_t count = 0; count < size; count++, audioData += numChannels)
            {
                 auto output = (count < toRead) ? tmpOutput[count] : 0;

                // write signal to all channels to start. This is so that
                // the compiler can optimize for the most common case.
                for (auto j = 0; j < numChannels; j++)
                {
                    audioData[j] = output;
                }
                        
                // If VOX tone is enabled, go back through and add the VOX tone
                // on the left channel.
                if (enableVoxTone)
                {
                    cbData->voxTonePhase += 2.0*M_PI*VOX_TONE_FREQ/sr;
                    cbData->voxTonePhase -= 2.0*M_PI*floor(cbData->voxTonePhase/(2.0*M_PI));
                    audioData[0] = VOX_TONE_AMP*cos(cbData->voxTonePhase);
                }
            }
        }
    }
}

void MainFrame::OnRxInAudioData_(IAudioDevice& dev, void* data, size_t size, void* state) FREEDV_NONBLOCKING
{
    paCallBackData* cbData = static_cast<paCallBackData*>(state);
    short* audioData = static_cast<short*>(data);
    short* tmpInput = cbData->tmpReadRxBuffer_.get();

    auto numChannels = dev.getNumChannels();
    for (size_t i = 0; i < size; i++, audioData += numChannels)
    {
        tmpInput[i] = audioData[0];
    }
    if (isModemRunning.load(std::memory_order_acquire) && cbData->infifo1->write(tmpInput, size)) 
    {
        g_infifo1_full.fetch_add(1, std::memory_order_relaxed);
    }
}

