//=========================================================================
// Name:            glissando_host.cpp
// Purpose:         MainFrame's side of the Glissando console: opening and
//                  closing it, and answering what it asks of the radio,
//                  the audio and the chat modem. The console is the
//                  application's window; MainFrame stays hidden behind it.
//=========================================================================

#include <algorithm>
#include <cmath>
#include <cstring>

#include <wx/numformatter.h>
#if defined(__WXMSW__)
#include <wx/msw/private.h>
#endif // defined(__WXMSW__)

#include "main.h"
#include "git_version.h"
#include "gui/dialogs/dlg_snoop.h"
#include "gui/dialogs/dlg_text_messaging.h"
#include "pipeline/TextMessagingModem.h"
#include "pipeline/TextMessagingTransport.h"
#include "text_messaging/FrameAnnotation.h"
#include "text_messaging/FrameCodec.h"
#include "text_messaging/TextMessagingSession.h"
#include "util/logging/ulog.h"

extern paCallBackData* g_rxUserdata;

extern std::atomic<bool> g_tx;
extern std::atomic<bool> endingTx;
extern float g_avmag_waterfall[MODEM_STATS_NSPEC];

namespace
{

// A melody counts as "being received" on the console's lamp for this long
// after a frame decodes; frames of one burst decode a frame length apart,
// so this is only the lamp, not carrier sense (the modem does that).
constexpr double RECEIVING_LAMP_SECONDS = 4.0;

// Preferences > Rig Control can have a transmission aborted over this SWR.
constexpr double SWR_ABORT_ABOVE = 3.0;

// After high SWR aborts, the meter keeps showing it this long, so the
// operator sees why the radio let go.
constexpr uint64_t SWR_ABORT_HOLD_MS = 5000;

// Set while MainFrame closes the console on its way out, so the console
// closing does not in turn try to close MainFrame.
bool closingWithMainWindow = false;

uint64_t steadyNowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

wxString fromAir(const std::string& text)
{
    // Chat text is UTF-8, but a frame can end part way through a character;
    // show what there is rather than nothing.
    wxString s = wxString::FromUTF8(text.data(), text.size());
    return s.empty() && !text.empty() ? wxString::From8BitData(text.data(), text.size()) : s;
}

GlissandoScopeFrame::Role scopeRole(TextMessaging::AnnotationToken::Role role)
{
    using Role = TextMessaging::AnnotationToken::Role;
    switch (role)
    {
        case Role::Kind: return GlissandoScopeFrame::Role::Kind;
        case Role::Station: return GlissandoScopeFrame::Role::Station;
        case Role::Field: return GlissandoScopeFrame::Role::Field;
        case Role::Text: return GlissandoScopeFrame::Role::Text;
        case Role::Unknown: return GlissandoScopeFrame::Role::Unknown;
    }
    return GlissandoScopeFrame::Role::Field;
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
    settings.lens = config.glissandoScopeLens;
    return settings;
}

void MainFrame::applyGlissandoIcon(wxTopLevelWindow* window)
{
#if defined(__WXMSW__)
    // The "glissando" icon in contrib/glissando.rc, at every size it has.
    static const wxIconBundle icons(wxS("glissando"), wxGetInstance());
    window->SetIcons(icons);
#else
    wxUnusedVar(window);
#endif // defined(__WXMSW__)
}

void MainFrame::openGlissandoConsole()
{
    if (m_glissandoConsole == nullptr)
    {
        auto& config = wxGetApp().appConfiguration;
        wxRect position(config.glissandoWindowLeft, config.glissandoWindowTop,
                        config.glissandoWindowWidth, config.glissandoWindowHeight);
        if (position.x < 0 || position.y < 0) position.SetPosition(wxDefaultPosition);

        m_glissandoConsole = new GlissandoConsole(this, this, loadGlissandoSettings_(), position);
        applyGlissandoIcon(m_glissandoConsole);
        m_glissandoConsole->SetTitle(_("Glissando ") + wxString::FromUTF8(GetFreeDVVersion().c_str()));
        applyGlissandoToModem_(true);
        log_info("Glissando console opened; text chat now uses Glissando");
    }

    m_glissandoConsole->Show();
    m_glissandoConsole->Raise();
}

void MainFrame::OnToolsGlissando(wxCommandEvent&)
{
    openGlissandoConsole();
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

    m_glissandoConsole = nullptr;
    applyGlissandoToModem_(false);
    log_info("Glissando console closed; text chat back on the codec2 data modes");

    // The console is the application: closing it quits.
    if (!terminating_ && !closingWithMainWindow) Close();
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
    modemConfig.chords = config.glissandoChords;
    switch (config.glissandoTail)
    {
    case 0: modemConfig.tail = TextMessagingModem::GlissandoConfig::Tail::Off; break;
    case 1: modemConfig.tail = TextMessagingModem::GlissandoConfig::Tail::Chord; break;
    case 3: modemConfig.tail = TextMessagingModem::GlissandoConfig::Tail::CwStraight; break;
    default: modemConfig.tail = TextMessagingModem::GlissandoConfig::Tail::Cw; break;
    }
    modemConfig.cwText = TextMessagingModem::cwTailText(
        ((wxString)config.glissandoCwText).ToStdString(),
        config.reportingConfiguration.reportingCallsign->ToStdString());
    modemConfig.cwWpm = config.glissandoCwWpm;
    modemConfig.cwIdMinutes = config.glissandoCwIdMinutes;
    textMessagingModem().setGlissando(modemConfig);

    // Automatic shifting can change the tempo at any frame heard, and every
    // protocol timer scales with it. Not while chat goes through Data2G,
    // whose timers applyChatModem_() set.
    if (data2gChatActive_.load(std::memory_order_acquire)) return;
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
    config.glissandoScopeLens = settings.lens;
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
    telemetry.heardScale = status.heardScale;
    telemetry.secondsSinceHeard = status.haveReport ? (steadyNowMs() - status.heardAtMs) / 1000.0 : 0.0;
    telemetry.transmitGear = status.transmitGear;
    telemetry.advisedGear = status.advisedGear;
    telemetry.receiving = status.haveReport && telemetry.secondsSinceHeard < RECEIVING_LAMP_SECONDS;
    telemetry.transmitting = m_RxRunning && g_tx.load(std::memory_order_relaxed);
    telemetry.transmitBusy = telemetry.transmitting ||
                             (m_RxRunning && m_textMessagingTransport != nullptr &&
                              m_textMessagingTransport->isTransmitting());
    telemetry.audioRunning = m_RxRunning;
    telemetry.engageToSend =
        chatWaitsForEngage() && TextMessaging::TextMessagingSession::instance().protocol().hasQueuedTransmissions();

    int64_t frequency = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency;
    telemetry.rigFrequencyKnown = frequency > 0;
    telemetry.rigFrequencyHz = (double)frequency;

    auto swrMeter = std::dynamic_pointer_cast<IRigSwrMeter>(wxGetApp().rigFrequencyController);
    bool holding = rigSwrAbortAtMs_ != 0 && steadyNowMs() - rigSwrAbortAtMs_ < SWR_ABORT_HOLD_MS;
    telemetry.showSwr = wxGetApp().appConfiguration.rigControlConfiguration.swrMeter && swrMeter &&
                        swrMeter->canReadSwr() && (telemetry.transmitting || holding);
    telemetry.swrKnown = !std::isnan(rigSwr_);
    telemetry.swr = telemetry.swrKnown ? rigSwr_ : 0.0;
    return telemetry;
}

void MainFrame::pollRigSwr_()
{
    // Called once a second while audio runs. Only while transmitting: the
    // reading means nothing otherwise, and a new keying starts with a blank
    // meter rather than the last one's reading.
    bool transmitting = g_tx.load(std::memory_order_acquire);
    if (!transmitting)
    {
        bool holding = rigSwrAbortAtMs_ != 0 && steadyNowMs() - rigSwrAbortAtMs_ < SWR_ABORT_HOLD_MS;
        if (!holding) rigSwr_ = NAN;
        return;
    }

    auto swrMeter = std::dynamic_pointer_cast<IRigSwrMeter>(wxGetApp().rigFrequencyController);
    if (!wxGetApp().appConfiguration.rigControlConfiguration.swrMeter || !swrMeter) return;
    swrMeter->requestSwr();
}

void MainFrame::onRigSwrReading_(double swr)
{
    // A reading asked for just before the radio let go can land after it.
    if (!g_tx.load(std::memory_order_acquire)) return;

    // Under 1:1 is no reading at all: some radios give 0 until power is up.
    if (!(swr >= 1.0)) return;
    rigSwr_ = swr;

    auto& rig = wxGetApp().appConfiguration.rigControlConfiguration;
    if (!rig.swrMeter || !rig.swrAutoAbort || swr <= SWR_ABORT_ABOVE) return;

    log_warn("SWR %.1f:1 is over %.0f:1: aborting the transmission", swr, SWR_ABORT_ABOVE);
    rigSwrAbortAtMs_ = steadyNowMs();

    bool chatOnAir = m_textMessagingTransport != nullptr && m_textMessagingTransport->isTransmitting();
    glissandoAbortTransmit();

    // Keyed some other way (the PTT key, say): let go as the time-out does.
    if (!chatOnAir && m_btnTogPTT->GetValue())
    {
        m_btnTogPTT->SetValue(false);
        endingTx.store(true, std::memory_order_release);
        togglePTT();
    }
}

bool MainFrame::glissandoSpectrum(std::vector<float>& magnitudesDb, double& nyquistHz)
{
    // The averaged spectrum of the radio input, refreshed by the main
    // window's timers while audio runs. Blank while transmitting.
    if (!m_RxRunning || g_tx.load(std::memory_order_relaxed)) return false;
    magnitudesDb.assign(g_avmag_waterfall, g_avmag_waterfall + MODEM_STATS_NSPEC);
    nyquistHz = FS / 2.0;
    return nyquistHz > 0.0;
}

std::vector<GlissandoScopeFrame> MainFrame::glissandoHeardFrames()
{
    std::vector<GlissandoScopeFrame> frames;
    std::vector<TextMessagingModem::GlissandoHeard> heard = textMessagingModem().takeGlissandoHeard();
    if (heard.empty()) return frames;

    // Destinations are sent as a CRC of the callsign: name the ones we can,
    // ourselves and the stations we have heard.
    auto& session = TextMessaging::TextMessagingSession::instance();
    std::string me = session.protocol().myCallsign();
    uint32_t myCrc = me.empty() ? 0 : TextMessaging::FrameCodec::callsignCrc24(me);
    std::vector<TextMessaging::HeardStation> stations = session.stations().stations();
    TextMessaging::CallsignForCrc nameFor = [&](uint32_t crc) -> std::string {
        if (myCrc != 0 && crc == myCrc) return "YOU";
        for (const TextMessaging::HeardStation& station : stations)
        {
            if (TextMessaging::FrameCodec::callsignCrc24(station.callsign) == crc) return station.callsign;
        }
        return std::string();
    };

    for (const TextMessagingModem::GlissandoHeard& h : heard)
    {
        const Glissando::GearInfo& gear = Glissando::gearInfo(h.gear);
        const Glissando::SegmentProgress& segment = h.segment;

        GlissandoScopeFrame frame;
        // Both are on the steady clock.
        frame.startSeconds = h.startMs / 1000.0;
        frame.symbolSeconds = gear.symbolSeconds;
        frame.glide = gear.glide;
        frame.voice = h.voice;
        frame.notesHz = h.notesHz;
        frame.melody.assign(h.melody.begin(), h.melody.end());
        for (int k = 0; k < Glissando::SYMBOLS_PER_FRAME; k++) frame.motif.push_back(Glissando::isMotifSymbol(k));
        frame.completed = segment.completed;

        wxString part = segment.filler ? wxString(_("FILLER"))
                                       : wxString::Format(segment.text ? _("TEXT %d") : _("SIGNAL %d"),
                                                          segment.index + 1);
        frame.title = wxString::Format("%s  %s  %+.0f dB", wxString(gear.tempo).Upper(), part, h.snrDb);

        if (!segment.filler)
        {
            for (const TextMessaging::AnnotationToken& token :
                 TextMessaging::describeSegment(segment.bytes, segment.index * Glissando::SEGMENT_DATA_BYTES,
                                                segment.knownFrom, segment.text, nameFor))
            {
                frame.tokens.push_back({scopeRole(token.role), fromAir(token.text)});
            }
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::vector<GlissandoScopeSent> MainFrame::glissandoSentFrames()
{
    std::vector<GlissandoScopeSent> frames;
    // Taken either way, so the modem's queue does not fill while the ships
    // are switched off.
    std::vector<TextMessagingModem::GlissandoSent> sentFrames = textMessagingModem().takeGlissandoSent();
    if (!wxGetApp().appConfiguration.glissandoTransmitShips) return frames;
    for (const TextMessagingModem::GlissandoSent& sent : sentFrames)
    {
        GlissandoScopeSent frame;
        frame.symbolSeconds = Glissando::gearInfo(sent.gear).symbolSeconds;
        frame.voice = sent.voice;
        frame.heroes = sent.scale == Glissando::Scale::Pentatonic;
        std::copy(sent.notesHz.begin(), sent.notesHz.end(), frame.notesHz.begin());
        frame.melody.assign(sent.melody.begin(), sent.melody.end());
        frame.leadSeconds = sent.leadSeconds;
        frame.tailSeconds = sent.tailSeconds;
        frames.push_back(frame);
        if (!sent.tailMelody.empty())
        {
            // The CW tail follows as a tune of its own, a Morse unit a note.
            GlissandoScopeSent tail = frame;
            tail.symbolSeconds = sent.tailUnitSeconds;
            tail.voice = 0;
            tail.melody = sent.tailMelody;
            tail.leadSeconds = 0.0;
            tail.tailSeconds = 0.0;
            frames.push_back(tail);
            if (!sent.tailHarmony.empty())
            {
                // A straight tail's second note, sounding with the first.
                tail.voice = 1;
                tail.melody = sent.tailHarmony;
                frames.push_back(std::move(tail));
            }
        }
    }
    return frames;
}

void MainFrame::glissandoSetAudioRunning(bool running)
{
    if (running == m_RxRunning || !m_togBtnOnOff->IsEnabled()) return;
    m_togBtnOnOff->SetValue(running);
    wxCommandEvent event(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, m_togBtnOnOff->GetId());
    event.SetEventObject(m_togBtnOnOff);
    OnTogBtnOnOff(event);
}

void MainFrame::glissandoAbortTransmit()
{
    log_info("Transmission aborted by the operator");

    // The protocol first, so that it cannot take the cut-off burst for a
    // finished one and wait for an acknowledgement, then retry it.
    TextMessaging::TextMessagingSession::instance().protocol().abortTransmission();

    // Drops the rest of the burst, keyings still to come included, and
    // unkeys.
    if (m_textMessagingTransport != nullptr) m_textMessagingTransport->abort();

    // And what the sound card has not played yet: the radio lets go as soon
    // as the output runs dry, rather than after up to a second of tune.
    if (g_rxUserdata != nullptr && g_rxUserdata->outfifo1 != nullptr) g_rxUserdata->outfifo1->reset();
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

std::vector<double> MainFrame::glissandoFrequencyPresets()
{
    auto& reporting = wxGetApp().appConfiguration.reportingConfiguration;
    double scale = reporting.reportingFrequencyAsKhz ? 1000.0 : 1000000.0;

    std::vector<double> presets;
    for (const wxString& item : reporting.reportingFrequencyList.get())
    {
        double value = 0.0;
        if (wxNumberFormatter::FromString(item, &value) && value > 0.0) presets.push_back(std::round(value * scale));
    }
    return presets;
}

void MainFrame::glissandoShowChat(bool show)
{
    if (!show)
    {
        // Hidden, not destroyed, as its own close box does.
        if (m_textMessagingDialog != nullptr) m_textMessagingDialog->Hide();
        return;
    }

    wxCommandEvent event;
    OnToolsTextMessaging(event);
}

bool MainFrame::glissandoChatShown()
{
    return m_textMessagingDialog != nullptr && m_textMessagingDialog->IsShown();
}

void MainFrame::glissandoShowSnoop(bool show)
{
    if (!show)
    {
        // Hidden, not destroyed: it keeps listening for when it comes back.
        if (m_snoopDialog != nullptr) m_snoopDialog->Hide();
        return;
    }

    if (m_snoopDialog == nullptr)
    {
        m_snoopDialog = new SnoopDialog(this);
        applyGlissandoIcon(m_snoopDialog);

        // Beside the chat window rather than on top of it.
        if (m_textMessagingDialog != nullptr && m_textMessagingDialog->IsShown())
        {
            wxRect chat = m_textMessagingDialog->GetScreenRect();
            m_snoopDialog->Move(chat.GetLeft() + 60, chat.GetTop() + 60);
        }
    }

    m_snoopDialog->Show();
    m_snoopDialog->Iconize(false);
    m_snoopDialog->Raise();
}

bool MainFrame::glissandoSnoopShown()
{
    return m_snoopDialog != nullptr && m_snoopDialog->IsShown();
}

bool MainFrame::glissandoSetupAvailable(GlissandoSetup setup)
{
    switch (setup)
    {
        case GlissandoSetup::Options:
            return true;
        case GlissandoSetup::AudioDevices:
        case GlissandoSetup::RigControl:
        case GlissandoSetup::EasySetup:
            return !m_RxRunning;
    }
    return false;
}

void MainFrame::glissandoOpenSetup(GlissandoSetup setup)
{
    if (!glissandoSetupAvailable(setup)) return;

    wxCommandEvent event;
    switch (setup)
    {
        case GlissandoSetup::Options: OnToolsOptions(event); break;
        case GlissandoSetup::AudioDevices: OnToolsAudio(event); break;
        case GlissandoSetup::RigControl: OnToolsComCfg(event); break;
        case GlissandoSetup::EasySetup: OnToolsEasySetup(event); break;
    }
}
