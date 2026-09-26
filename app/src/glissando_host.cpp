//=========================================================================
// Name:            glissando_host.cpp
// Purpose:         MainFrame's side of the Glissando console: opening and
//                  closing it, and answering what it asks of the radio,
//                  the audio and the chat modem.
//=========================================================================

#include <algorithm>
#include <cmath>
#include <cstring>

#include <wx/numformatter.h>

#include "main.h"
#include "gui/dialogs/dlg_text_messaging.h"
#include "pipeline/TextMessagingModem.h"
#include "text_messaging/TextMessagingSession.h"
#include "util/logging/ulog.h"

extern std::atomic<bool> g_tx;
extern FreeDVInterface freedvInterface;
extern float g_avmag_waterfall[MODEM_STATS_NSPEC];

namespace
{

// A melody counts as "being received" on the console's lamp for this long
// after a frame decodes; frames of one burst decode a frame length apart,
// so this is only the lamp, not carrier sense (the modem does that).
constexpr double RECEIVING_LAMP_SECONDS = 4.0;

// Set while the main window closes the console on its way out, so the
// console closing does not in turn try to close the main window.
bool closingWithMainWindow = false;

uint64_t steadyNowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool sameTiming(const TextMessaging::AirTiming& a, const TextMessaging::AirTiming& b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

} // namespace

GlissandoConsoleSettings MainFrame::loadGlissandoSettings_() const
{
    auto& config = wxGetApp().appConfiguration;
    GlissandoConsoleSettings settings;
    settings.gear = std::min(Glissando::MAX_GEAR, std::max(Glissando::MIN_GEAR, (int)config.glissandoGear));
    settings.autoGear = config.glissandoAutoGear;
    Glissando::Scale scale;
    if (Glissando::scaleFromName(((wxString)config.glissandoScale).ToStdString(), scale)) settings.scale = scale;
    settings.tuningOffsetHz = config.glissandoTuningDeciHz / 10.0;
    settings.listenAllGears = config.glissandoListenAllGears;
    settings.scanRate = std::max(0.5, config.glissandoScanRateDeci / 10.0);
    return settings;
}

void MainFrame::openGlissandoConsole(bool hideMainWindow)
{
    if (m_glissandoConsole == nullptr)
    {
        auto& config = wxGetApp().appConfiguration;
        wxRect position(config.glissandoWindowLeft, config.glissandoWindowTop,
                        config.glissandoWindowWidth, config.glissandoWindowHeight);
        if (position.x < 0 || position.y < 0) position.SetPosition(wxDefaultPosition);

        m_glissandoConsole = new GlissandoConsole(this, this, loadGlissandoSettings_(), position);
        config.glissandoEnabled = true;
        applyGlissandoToModem_(true);
        log_info("Glissando console opened; text chat now uses Glissando");
    }

    m_glissandoConsole->Show();
    m_glissandoConsole->Raise();

    // Hidden once the event loop is running: at startup the main window has
    // only just been shown, and hiding it before GTK has mapped it leaves it
    // unmapped but still claiming to be shown.
    if (hideMainWindow) CallAfter([this]() { Hide(); });
}

void MainFrame::OnToolsGlissando(wxCommandEvent&)
{
    openGlissandoConsole(false);
}

void MainFrame::closeGlissandoConsole_()
{
    if (m_glissandoConsole == nullptr) return;
    // Close() lands in glissandoConsoleClosed(), which does the bookkeeping.
    closingWithMainWindow = true;
    m_glissandoConsole->Close(true);
    closingWithMainWindow = false;
}

void MainFrame::glissandoConsoleClosed(const wxRect& lastPosition)
{
    auto& config = wxGetApp().appConfiguration;
    config.glissandoWindowLeft = lastPosition.x;
    config.glissandoWindowTop = lastPosition.y;
    config.glissandoWindowWidth = lastPosition.width;
    config.glissandoWindowHeight = lastPosition.height;
    config.glissandoEnabled = false;

    m_glissandoConsole = nullptr;
    applyGlissandoToModem_(false);
    log_info("Glissando console closed; text chat back on the codec2 data modes");

    // Standing in for the main window, the console was the application.
    if (!IsShown() && !terminating_ && !closingWithMainWindow) Close();
}

void MainFrame::applyGlissandoToModem_(bool enabled)
{
    auto& config = wxGetApp().appConfiguration;

    TextMessagingModem::GlissandoConfig modemConfig;
    modemConfig.enabled = enabled;
    modemConfig.gear = config.glissandoGear;
    modemConfig.autoGear = config.glissandoAutoGear;
    Glissando::Scale scale;
    if (Glissando::scaleFromName(((wxString)config.glissandoScale).ToStdString(), scale)) modemConfig.scale = scale;
    modemConfig.tuningOffsetHz = config.glissandoTuningDeciHz / 10.0;
    modemConfig.listenAllGears = config.glissandoListenAllGears;
    textMessagingModem().setGlissando(modemConfig);

    // Automatic shifting can change the tempo at any frame heard, and every
    // protocol timer scales with it.
    TextMessaging::AirTiming timing = textMessagingModem().airTiming();
    if (!sameTiming(timing, appliedAirTiming_))
    {
        TextMessaging::TextMessagingSession::instance().protocol().setAirTiming(timing);
        appliedAirTiming_ = timing;
        log_info("Text chat timers now: ack %d ms, fragment %d ms", timing.ackTimeoutMs,
                 timing.textFragmentAirMs);
    }
}

void MainFrame::glissandoSettingsChanged(const GlissandoConsoleSettings& settings)
{
    auto& config = wxGetApp().appConfiguration;
    config.glissandoGear = settings.gear;
    config.glissandoAutoGear = settings.autoGear;
    config.glissandoScale = wxString(Glissando::scaleName(settings.scale));
    config.glissandoTuningDeciHz = (int)std::lround(settings.tuningOffsetHz * 10.0);
    config.glissandoListenAllGears = settings.listenAllGears;
    config.glissandoScanRateDeci = (int)std::lround(settings.scanRate * 10.0);
    applyGlissandoToModem_(m_glissandoConsole != nullptr);
}

GlissandoTelemetry MainFrame::glissandoTelemetry()
{
    // Polled by the console a few times a second; keeps the protocol's
    // timers in step with automatic gear shifts as a side effect.
    if (m_glissandoConsole != nullptr) applyGlissandoToModem_(true);

    GlissandoTelemetry telemetry;
    TextMessagingModem::GlissandoStatus status = textMessagingModem().glissandoStatus();
    telemetry.haveReport = status.haveReport;
    telemetry.snrDb = status.report.snrDb;
    telemetry.dopplerHz = status.report.dopplerHz;
    telemetry.heardGear = status.heardGear;
    telemetry.secondsSinceHeard = status.haveReport ? (steadyNowMs() - status.heardAtMs) / 1000.0 : 0.0;
    telemetry.transmitGear = status.transmitGear;
    telemetry.advisedGear = status.advisedGear;
    telemetry.receiving = status.haveReport && telemetry.secondsSinceHeard < RECEIVING_LAMP_SECONDS;
    telemetry.transmitting = m_RxRunning && g_tx.load(std::memory_order_relaxed);
    telemetry.audioRunning = m_RxRunning;

    int64_t frequency = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency;
    telemetry.rigFrequencyKnown = frequency > 0;
    telemetry.rigFrequencyHz = (double)frequency;
    return telemetry;
}

bool MainFrame::glissandoSpectrum(std::vector<float>& magnitudesDb, double& nyquistHz)
{
    // The same averaged spectrum the main waterfall draws, refreshed by the
    // main window's timers while audio runs. Blank while transmitting, as
    // the main waterfall is.
    if (!m_RxRunning || g_tx.load(std::memory_order_relaxed)) return false;
    magnitudesDb.assign(g_avmag_waterfall, g_avmag_waterfall + MODEM_STATS_NSPEC);
    nyquistHz = freedvInterface.getTxModemSampleRate() / 2.0;
    return nyquistHz > 0.0;
}

void MainFrame::glissandoSetAudioRunning(bool running)
{
    if (running == m_RxRunning || !m_togBtnOnOff->IsEnabled()) return;
    m_togBtnOnOff->SetValue(running);
    wxCommandEvent event(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, m_togBtnOnOff->GetId());
    event.SetEventObject(m_togBtnOnOff);
    OnTogBtnOnOff(event);
}

void MainFrame::glissandoSetRigFrequency(double hz)
{
    // Goes through the main window's frequency box, so rig control, the
    // reporters and text chat's band check all hear about it the usual way.
    bool asKhz = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz;
    m_cboReportFrequency->SetValue(asKhz ? wxNumberFormatter::ToString(hz / 1000.0, 1)
                                         : wxNumberFormatter::ToString(hz / 1000000.0, 4));
    wxCommandEvent event(wxEVT_TEXT_ENTER, m_cboReportFrequency->GetId());
    OnChangeReportFrequency(event);
}

void MainFrame::glissandoShowChat()
{
    wxCommandEvent event;
    OnToolsTextMessaging(event);
}

void MainFrame::glissandoShowMainWindow(bool show)
{
    Show(show);
    if (show)
    {
        Iconize(false);
        Raise();
    }
}

bool MainFrame::glissandoMainWindowShown()
{
    return IsShown();
}
