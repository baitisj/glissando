/*
  ongui.cpp

  The simpler GUI event handlers.
*/

#include <sstream>
#include <iomanip>
#include <locale>
#include <cmath>
#include <cstring>
#include <vector>

#include "main.h"
#include "gui/dialogs/dlg_text_messaging.h"
#include "text_messaging/TextMessagingSession.h"

#include "git_version.h"
#include "gui/dialogs/dlg_easy_setup.h"
#include "gui/dialogs/dlg_audiooptions.h"
#include "gui/dialogs/dlg_options.h"
#include "gui/dialogs/dlg_ptt.h"
#include "gui/dialogs/monitor_volume_adj.h"

#if defined(WIN32)
#include "rig_control/omnirig/OmniRigController.h"
#endif // defined(WIN32)

extern std::atomic<bool>   g_tx;
extern int g_txLevel;
extern std::atomic<float> g_txLevelScale;
extern int g_tuneLevel;
extern std::atomic<float> g_tuneLevelScale;

extern wxConfigBase *pConfig;
extern std::atomic<bool> endingTx;
extern std::atomic<int> g_outfifo1_empty;
extern paCallBackData* g_rxUserdata;

extern std::atomic<SNDFILE*>            g_sfRecFileFromModulator;
extern std::atomic<bool>                g_recFileFromModulator;
extern SNDFILE            *g_sfRecFile;
extern bool g_recFileFromRadio;

extern std::atomic<bool>                g_playFileFromRadio;


extern wxMutex g_mutexProtectingCallbackData;


static wxString bandNameForFilter(FilterFrequency band);

//-------------------------------------------------------------------------
// Forces redraw of main panels on window resize.
//-------------------------------------------------------------------------
void MainFrame::topFrame_OnSize( wxSizeEvent& event )
{
    m_auiNbookCtrl->Refresh();
    TopFrame::topFrame_OnSize(event);
}

//-------------------------------------------------------------------------
// OnExitClick()
//-------------------------------------------------------------------------
void MainFrame::OnExitClick(wxCommandEvent& event)
{
    OnExit(event);
}

//-------------------------------------------------------------------------
// OnToolsEasySetup()
//-------------------------------------------------------------------------
void MainFrame::OnToolsEasySetup(wxCommandEvent&)
{
    EasySetupDialog* dlg = new EasySetupDialog(this);
    if (dlg->ShowModal() == wxOK)
    {
        // Show/hide frequency box based on CAT control setup.
        m_freqBox->Show(isFrequencyControlEnabled_());


        // Relayout window so that the changes can take effect.
        m_panel->Layout();
    }
}

//-------------------------------------------------------------------------
// OnToolsEasySetupUI()
//-------------------------------------------------------------------------
void MainFrame::OnToolsEasySetupUI(wxUpdateUIEvent& event)
{
    event.Enable(!m_RxRunning);
}

//-------------------------------------------------------------------------
// OnToolsTextMessaging()
//-------------------------------------------------------------------------
void MainFrame::OnToolsTextMessaging(wxCommandEvent&)
{
    if (m_textMessagingDialog == nullptr)
    {
        m_textMessagingDialog = new TextMessagingDialog(this);
        applyGlissandoIcon(m_textMessagingDialog);
    }

    // Picks up history, the current callsign and anything heard while the
    // window was closed.
    m_textMessagingDialog->refreshFromSession();
    m_textMessagingDialog->SetTitle(_("Glissando Chat"));
    m_textMessagingDialog->Show();
    m_textMessagingDialog->Iconize(false);
    m_textMessagingDialog->Raise();
}

//-------------------------------------------------------------------------
// OnToolsTextMessagingUI()
//-------------------------------------------------------------------------
void MainFrame::OnToolsTextMessagingUI(wxUpdateUIEvent& event)
{
    // Chat needs the data modems; without them the window would have nothing
    // to say.
    event.Enable(TextMessaging::TextMessagingSession::instance().isStarted());
}

//-------------------------------------------------------------------------
// OnToolsAudio()
//-------------------------------------------------------------------------
void MainFrame::OnToolsAudio(wxCommandEvent& event)
{
    wxUnusedVar(event);
    int rv = 0;
    AudioOptsDialog *dlg = new AudioOptsDialog(NULL);
    rv = dlg->ShowModal();
    if(rv == wxOK)
    {
        dlg->ExchangeData(EXCHANGE_DATA_OUT);
    }
    delete dlg;
}

//-------------------------------------------------------------------------
// OnToolsAudioUI()
//-------------------------------------------------------------------------
void MainFrame::OnToolsAudioUI(wxUpdateUIEvent& event)
{
    event.Enable(!m_RxRunning);
}

//-------------------------------------------------------------------------
// OnToolsOptions()
//-------------------------------------------------------------------------
void MainFrame::OnToolsOptions(wxCommandEvent& event)
{
    bool oldFreqAsKHz = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz;

    wxUnusedVar(event);
    if (optionsDlg->ShowModal() == wxOK)
    {
        // Update reporting list.
        updateReportingFreqList_();

        // The text chat preferences may have changed.
        applyChatModem_();
        updateTextChatTransmitPermission_();
    
        // Show/hide frequency box based on CAT control configuration.
        m_freqBox->Show(isFrequencyControlEnabled_());
        
        // XXX - with really short windows, wxWidgets sometimes doesn't size
        // the components properly until the user resizes the window (even if only
        // by a pixel or two). As a really hacky workaround, we emulate this behavior
        // when restoring window sizing. These resize events also happen after configuration
        // is restored but I'm not sure this is necessary.
        wxSize size = GetSize();
        auto w = size.GetWidth();
        auto h = size.GetHeight();
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

        
        
        // Adjust frequency labels on main window
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

        // If the "Frequency as kHz" option has changed, update the frequencies
        // in the main window's callsign list.
        if (oldFreqAsKHz != wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
        {
            for (int index = 0; index < m_lastReportedCallsignListView->GetItemCount(); index++)
            {
                wxString newFreq = "";
                wxString freq = m_lastReportedCallsignListView->GetItemText(index, 1);
                double freqDouble = 0;
                wxNumberFormatter::FromString(freq, &freqDouble);

                if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
                {
                    freqDouble *= 1000.0;
                    newFreq = wxNumberFormatter::ToString(freqDouble, 1);
                }
                else
                {
                    freqDouble /= 1000.0;
                    newFreq = wxNumberFormatter::ToString(freqDouble, 4);
                }

                m_lastReportedCallsignListView->SetItem(index, 1, newFreq);
            }
        }

        // Relayout window so that the changes can take effect.
        m_panel->Layout();
    }
}

//-------------------------------------------------------------------------
// OnToolsOptionsUI()
//-------------------------------------------------------------------------
void MainFrame::OnToolsOptionsUI(wxUpdateUIEvent&)
{
}

//-------------------------------------------------------------------------
// OnToolsComCfg()
//-------------------------------------------------------------------------
void MainFrame::OnToolsComCfg(wxCommandEvent& event)
{
    wxUnusedVar(event);

    ComPortsDlg *dlg = new ComPortsDlg(NULL);

    if (dlg->ShowModal() == wxID_OK)
    {
        // Show/hide frequency box based on CAT control configuration.
        m_freqBox->Show(isFrequencyControlEnabled_());
    }

    delete dlg;
}

//-------------------------------------------------------------------------
// OnToolsComCfgUI()
//-------------------------------------------------------------------------
void MainFrame::OnToolsComCfgUI(wxUpdateUIEvent& event)
{
    event.Enable(!m_RxRunning);
}

void MainFrame::onFrequencyModeChange_(IRigFrequencyController*, uint64_t freq, IRigFrequencyController::Mode mode)
{
    CallAfter([&, mode, freq]() {
        // Update string value.
        switch(mode)
        {
            case IRigFrequencyController::USB:
                m_txtModeStatus->SetLabel(wxT("USB"));
                m_txtModeStatus->Enable(true);
                break;
            case IRigFrequencyController::DIGU:
                m_txtModeStatus->SetLabel(wxT("USB-D"));
                m_txtModeStatus->Enable(true);
                break;
            case IRigFrequencyController::LSB:
                m_txtModeStatus->SetLabel(wxT("LSB"));
                m_txtModeStatus->Enable(true);
                break;
            case IRigFrequencyController::DIGL:
                m_txtModeStatus->SetLabel(wxT("LSB-D"));
                m_txtModeStatus->Enable(true);
                break;
            case IRigFrequencyController::FM:
                m_txtModeStatus->SetLabel(wxT("FM"));
                m_txtModeStatus->Enable(true);
                break;
            case IRigFrequencyController::DIGFM:
                m_txtModeStatus->SetLabel(wxT("FM-D"));
                m_txtModeStatus->Enable(true);
                break;
            case IRigFrequencyController::AM:
                m_txtModeStatus->SetLabel(wxT("AM"));
                m_txtModeStatus->Enable(true);
                break;
            default:
                m_txtModeStatus->SetLabel(wxT("unk"));
                m_txtModeStatus->Enable(false);
                break;
        }

        // Round to the nearest 100 Hz.
        uint64_t wholeFreq = freq / 100;
        uint64_t remainder = freq % 100;

        if (remainder >= 50)
        {
            wholeFreq++;
        }

        auto newFreq = wholeFreq * 100;

        // Widest 60 meter allocation is 5.250-5.450 MHz per https://en.wikipedia.org/wiki/60-meter_band.
        bool is60MeterBand = newFreq >= 5250000 && newFreq <= 5450000;

        // Update color based on the mode and current frequency.
        bool isUsbFreq = newFreq >= 10000000 || is60MeterBand;
        bool isLsbFreq = newFreq < 10000000 && !is60MeterBand;

        bool isMatchingMode = 
            (isUsbFreq && (mode == IRigFrequencyController::USB || mode == IRigFrequencyController::DIGU)) ||
            (isLsbFreq && (mode == IRigFrequencyController::LSB || mode == IRigFrequencyController::DIGL));

        if (isMatchingMode)
        {
            m_txtModeStatus->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        }
        else
        {
            m_txtModeStatus->SetForegroundColour(wxColor(*wxRED));
        }

        // The radio is the source of truth for its frequency: whatever it
        // reports is what the app shows and uses, whether the operator turned
        // the dial or another program (rigctl, a logger) retuned it.
        if (!suppressFreqModeUpdates_)
        {
            // wxString::Format() doesn't respect locale but wxNumberFormatter should. Use the latter instead.
            wxString freqString;            
            if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
            {
                freqString = wxNumberFormatter::ToString(newFreq / 1000.0, 1);
            }
            else
            {
                freqString = wxNumberFormatter::ToString(newFreq / 1000.0 / 1000.0, 4);
            }
            
            // Set internal reporting frequency to ensure we don't immediately request
            // a frequency change from the radio (i.e. if rapidly changing frequency
            // on the radio side). Note that this also causes reporting to not fire 
            // by m_cboReportFrequency's change handler, so we should fire off reporting
            // here.
            auto oldFreq = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency;
            wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency = newFreq;
            updateTextChatTransmitPermission_();
            if (oldFreq != newFreq)
            {
                for (auto& ptr : wxGetApp().m_reporters)
                {
                    ptr->freqChange(wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency);
                }
            }
            
            m_cboReportFrequency->SetValue(freqString);
        }
        m_txtModeStatus->Refresh();

        // Auto-save outgoing band levels, then load the new band's levels
        auto newBandEnum = getFilterForFrequency(newFreq);
        if (newBandEnum != BAND_OTHER && newBandEnum != lastBand_)
        {
            autoSaveCurrentBandLevels_();
            lastBand_ = newBandEnum;
            loadTxAttenForBand_(newBandEnum);
            loadTuneAttenForBand_(newBandEnum);
            applyTxLevel(); // apply both loaded values in one call
        }
    });
}

void MainFrame::onRadioConnected_(IRigController*)
{
    // The radio keeps the frequency it is on, and the app picks it up from
    // the first poll. Only a frequency the operator chose in the app while
    // the radio was not connected is sent to it now. The mode is the
    // operator's to set on the radio.
    if (operatorFrequencyPending_.exchange(false) &&
        wxGetApp().rigFrequencyController && 
        (wxGetApp().appConfiguration.rigControlConfiguration.hamlibEnableFreqModeChanges || wxGetApp().appConfiguration.rigControlConfiguration.hamlibEnableFreqChangesOnly) &&
        wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency > 0)
    {
        wxGetApp().rigFrequencyController->setFrequency(wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency);
    }
}

void MainFrame::refreshRigFrequencyBeforeKeying_()
{
    // Commands to the radio run in order, so this read lands before the PTT
    // command queued right after it.
    if (wxGetApp().rigFrequencyController != nullptr && wxGetApp().rigFrequencyController->isConnected())
    {
        wxGetApp().rigFrequencyController->requestCurrentFrequencyMode();
    }
}

void MainFrame::onRadioDisconnected_(IRigController*)
{
    CallAfter([&]() {
        m_txtModeStatus->SetLabel(wxT("unk"));
        m_txtModeStatus->Enable(false);
    });
}

// Attempt to talk to rig using Hamlib

bool MainFrame::OpenHamlibRig() {
    if (wxGetApp().appConfiguration.rigControlConfiguration.hamlibUseForPTT != true)
       return false;
    
    int rig = wxGetApp().m_intHamlibRig;    
    if (rig == -1)
    {
        std::string fullErr = "The radio's model is empty. This is likely due to changes in Hamlib between FreeDV releases. Please click Stop Modem, double-check your CAT settings and push Start Modem again.";
        CallAfter([&, fullErr]() {
            wxMessageBox(fullErr, wxT("Error"), wxOK | wxICON_ERROR, this);
        });
        return false;
    }
    
    wxString port = wxGetApp().appConfiguration.rigControlConfiguration.hamlibSerialPort;
    wxString pttPort = wxGetApp().appConfiguration.rigControlConfiguration.hamlibPttSerialPort;
    auto pttType = (HamlibRigController::PttType)wxGetApp().appConfiguration.rigControlConfiguration.hamlibPTTType.get();
    
    int serial_rate = wxGetApp().appConfiguration.rigControlConfiguration.hamlibSerialRate;
    if (wxGetApp().CanAccessSerialPort((const char*)port.ToUTF8()) && (
        pttType == HamlibRigController::PTT_VIA_CAT || pttType == HamlibRigController::PTT_VIA_NONE || wxGetApp().CanAccessSerialPort((const char*)pttPort.ToUTF8())))
    {
        auto tmp = std::make_shared<HamlibRigController>(
            rig, (const char*)port.mb_str(wxConvUTF8), serial_rate, wxGetApp().appConfiguration.rigControlConfiguration.hamlibIcomCIVAddress,
            pttType, pttType == HamlibRigController::PTT_VIA_CAT || pttType == HamlibRigController::PTT_VIA_NONE ? (const char*)port.mb_str(wxConvUTF8) : (const char*)pttPort.mb_str(wxConvUTF8),
            false, // leave the radio where it is on stop, rather than on the frequency it had at start
            true, // frequency only: the mode is the operator's
            wxGetApp().appConfiguration.rigControlConfiguration.hamlibForceRTSOn,
            wxGetApp().appConfiguration.rigControlConfiguration.hamlibForceDTROn);

        // Hamlib also controls PTT.
        wxGetApp().rigFrequencyController = tmp;
        wxGetApp().rigPttController = tmp;
        
        wxGetApp().rigFrequencyController->onRigError += [this](IRigController*, std::string const& err)
        {
            std::string fullErr = "Couldn't connect to Radio with hamlib: " + err;
            CallAfter([&, fullErr]() {
                wxMessageBox(fullErr, wxT("Error"), wxOK | wxICON_ERROR, this);
            });
        };

        wxGetApp().rigFrequencyController->onRigConnected += [&](IRigController* ptr) {
            onRadioConnected_(ptr);
        };

        wxGetApp().rigFrequencyController->onRigDisconnected += [&](IRigController* ptr) {
            onRadioDisconnected_(ptr);
        };

        wxGetApp().rigFrequencyController->onFreqModeChange += [&](IRigFrequencyController* ptr, uint64_t freq, IRigFrequencyController::Mode mode) {
            onFrequencyModeChange_(ptr, freq, mode);
        };
        wxGetApp().rigFrequencyController->connect();
        return true;
    }
    else
    {
        return false;
    }
}

#if defined(WIN32)
// TBD -- a lot of this can be combined with the Hamlib logic above.
void MainFrame::OpenOmniRig() 
{
    auto tmp = std::make_shared<OmniRigController>(
        wxGetApp().appConfiguration.rigControlConfiguration.omniRigRigId,
        (wxGetApp().appConfiguration.rigControlConfiguration.hamlibEnableFreqModeChanges || wxGetApp().appConfiguration.rigControlConfiguration.hamlibEnableFreqChangesOnly),
        true); // frequency only: the mode is the operator's

    // OmniRig also controls PTT.
    wxGetApp().rigFrequencyController = tmp;
    if (!wxGetApp().rigPttController)
    {
        wxGetApp().rigPttController = tmp;
    }

    wxGetApp().rigFrequencyController->onRigError += [this](IRigController*, std::string err)
    {
        std::string fullErr = "Couldn't connect to Radio with OmniRig: " + err;
        CallAfter([&, fullErr]() {
            wxMessageBox(fullErr, wxT("Error"), wxOK | wxICON_ERROR, this);
        });
    };

    wxGetApp().rigFrequencyController->onRigConnected += [&](IRigController* ptr) {
        onRadioConnected_(ptr);
    };

    wxGetApp().rigFrequencyController->onRigDisconnected += [&](IRigController* ptr) {
        onRadioDisconnected_(ptr);
    };

    wxGetApp().rigFrequencyController->onFreqModeChange += [&](IRigFrequencyController* ptr, uint64_t freq, IRigFrequencyController::Mode mode) {
        onFrequencyModeChange_(ptr, freq, mode);
    };

    wxGetApp().rigFrequencyController->connect();
}
#endif // defined(WIN32)

//-------------------------------------------------------------------------
// OnCloseFrame()
//-------------------------------------------------------------------------
void MainFrame::OnCloseFrame(wxCloseEvent&)
{
    auto engine = AudioEngineFactory::GetAudioEngine();
    engine->stop();
    engine->setOnEngineError(nullptr, nullptr);
    
    Destroy();
}

//-------------------------------------------------------------------------
// OnTop()
//-------------------------------------------------------------------------
void MainFrame::OnTop(wxCommandEvent& event)
{
    auto style = GetWindowStyle();

    // Clear wxSTAY_ON_TOP first if it's already set.
    style &= ~wxSTAY_ON_TOP;
    
    // (Re)set wxSTAY_ON_TOP depending on whether the menu option is checked.
    if (event.IsChecked())
    {
        style |= wxSTAY_ON_TOP;
    }

    SetWindowStyle(style);
}

//-------------------------------------------------------------------------
// OnDeleteConfig()
//-------------------------------------------------------------------------
void MainFrame::OnDeleteConfig(wxCommandEvent&)
{
    wxMessageDialog messageDialog(
        this, "Would you like to restore configuration to defaults?", wxT("Restore Defaults"),
        wxYES_NO | wxICON_QUESTION | wxCENTRE);

    auto answer = messageDialog.ShowModal();
    if (answer == wxID_YES)
    {
        if(pConfig->DeleteAll())
        {
            wxLogMessage(wxT("Config file/registry key successfully deleted."));

            // Resets all configuration to defaults.
            loadConfiguration_();
        }
        else
        {
            wxLogError(wxT("Deleting config file/registry key failed."));
        }
    }
}

//-------------------------------------------------------------------------
// OnDeleteConfigUI()
//-------------------------------------------------------------------------
void MainFrame::OnDeleteConfigUI( wxUpdateUIEvent& event )
{
    event.Enable(!m_RxRunning);
}

//-------------------------------------------------------------------------
// Paint()
//-------------------------------------------------------------------------
void MainFrame::OnPaint(wxPaintEvent& WXUNUSED(event))
{
    wxPaintDC dc(this);

    if(GetMenuBar()->IsChecked(ID_PAINT_BG))
    {
        dc.Clear();
    }
    dc.SetUserScale(m_zoom, m_zoom);
}

//-------------------------------------------------------------------------
// bandNameForFilter() - maps FilterFrequency enum to config key string
//-------------------------------------------------------------------------
static wxString bandNameForFilter(FilterFrequency band)
{
    switch (band)
    {
        case BAND_160M:    return "160m";
        case BAND_80M:     return "80m";
        case BAND_60M:     return "60m";
        case BAND_40M:     return "40m";
        case BAND_30M:     return "30m";
        case BAND_20M:     return "20m";
        case BAND_17M:     return "17m";
        case BAND_15M:     return "15m";
        case BAND_12M:     return "12m";
        case BAND_10M:     return "10m";
        case BAND_VHF_UHF: return "vhfuhf";
        default:           return "";
    }
}

// autoSaveCurrentBandLevels_() - if the current band has saved per-band
// values, update them with the current live levels.
//-------------------------------------------------------------------------
void MainFrame::autoSaveCurrentBandLevels_(bool writeConfig)
{
    // Use lastBand_ (not the current reporting frequency) so that on a band
    // change we save the *outgoing* band's levels before loading the new one.
    wxString bandName = bandNameForFilter(lastBand_);
    if (bandName.IsEmpty())
        return;
    bool changed = false;
    auto& txAtten = wxGetApp().appConfiguration.txAttenByBand;
    if (txAtten->find(bandName) != txAtten->end())
    {
        txAtten->insert_or_assign(bandName, g_txLevel);
        changed = true;
    }
    auto& tuneAtten = wxGetApp().appConfiguration.tuneAttenByBand;
    if (tuneAtten->find(bandName) != tuneAtten->end())
    {
        tuneAtten->insert_or_assign(bandName, g_tuneLevel);
        changed = true;
    }
    if (changed && writeConfig)
        wxGetApp().appConfiguration.save(pConfig);
}

// loadTxAttenForBand_() - load saved TX attenuation for the given band
//-------------------------------------------------------------------------
void MainFrame::loadTxAttenForBand_(FilterFrequency band)
{
    wxString bandName = bandNameForFilter(band);
    auto& atten = wxGetApp().appConfiguration.txAttenByBand;
    auto it = atten->find(bandName);
    g_txLevel = (it != atten->end()) ? it->second : (int)wxGetApp().appConfiguration.transmitLevel;
    txLoadedLevel_ = g_txLevel; // snapshot for Restore
    // Caller is responsible for calling applyTxLevel() after all band values are loaded.
}

// loadTuneAttenForBand_() - load saved tune attenuation for the given band
//-------------------------------------------------------------------------
void MainFrame::loadTuneAttenForBand_(FilterFrequency band)
{
    wxString bandName = bandNameForFilter(band);
    auto& atten = wxGetApp().appConfiguration.tuneAttenByBand;
    auto it = atten->find(bandName);
    g_tuneLevel = (it != atten->end()) ? it->second : (int)wxGetApp().appConfiguration.tuneLevel;
    tuneLoadedLevel_ = g_tuneLevel; // snapshot for Restore
    // Caller is responsible for calling applyTxLevel() after all band values are loaded.
}

// applyTxLevel() - shared helper to apply g_txLevel and update the UI
//-------------------------------------------------------------------------
void MainFrame::applyTxLevel()
{
    bool isTuning = m_btnTogTune->GetValue();
    wxString fmtString;

    if (g_txLevel < TX_ATTENUATION_MIN) g_txLevel = TX_ATTENUATION_MIN;
    if (g_txLevel > TX_ATTENUATION_MAX) g_txLevel = TX_ATTENUATION_MAX;
    g_txLevelScale.store(exp(g_txLevel / 10.0 / 20.0 * log(10.0)), std::memory_order_release);

    if (g_tuneLevel < TX_ATTENUATION_MIN) g_tuneLevel = TX_ATTENUATION_MIN;
    if (g_tuneLevel > TX_ATTENUATION_MAX) g_tuneLevel = TX_ATTENUATION_MAX;
    g_tuneLevelScale.store(exp(g_tuneLevel / 10.0 / 20.0 * log(10.0)), std::memory_order_release);

    if (isTuning)
        fmtString = wxString::Format(MIC_SPKR_LEVEL_FORMAT_STR, wxNumberFormatter::ToString((double)g_tuneLevel/10.0, 1), DECIBEL_STR);
    else
        fmtString = wxString::Format(MIC_SPKR_LEVEL_FORMAT_STR, wxNumberFormatter::ToString((double)g_txLevel/10.0, 1), DECIBEL_STR);

    m_txtTxLevelNum->SetLabel(fmtString);

    // Only update the global defaults when the current band has no per-band
    // override, so that per-band values don't contaminate the global default.
    // Use the current reporting frequency directly rather than lastBand_ to
    // avoid stale state before the first band-change event fires.
    wxString bandName = bandNameForFilter(getFilterForFrequency(
        wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency));
    if (isTuning)
    {
        if (bandName.IsEmpty() || wxGetApp().appConfiguration.tuneAttenByBand->find(bandName) == wxGetApp().appConfiguration.tuneAttenByBand->end())
            wxGetApp().appConfiguration.tuneLevel = g_tuneLevel;
    }
    else
    {
        if (bandName.IsEmpty() || wxGetApp().appConfiguration.txAttenByBand->find(bandName) == wxGetApp().appConfiguration.txAttenByBand->end())
            wxGetApp().appConfiguration.transmitLevel = g_txLevel;
    }
}

void MainFrame::OnTxLevelDecrBig( wxCommandEvent& ) { if (m_btnTogTune->GetValue()) g_tuneLevel -= TX_ATTENUATION_LARGE_STEP; else g_txLevel -= TX_ATTENUATION_LARGE_STEP; applyTxLevel(); }
void MainFrame::OnTxLevelDecr( wxCommandEvent& )    { if (m_btnTogTune->GetValue()) g_tuneLevel -= TX_ATTENUATION_SMALL_STEP;  else g_txLevel -= TX_ATTENUATION_SMALL_STEP;  applyTxLevel(); }
void MainFrame::OnTxLevelIncr( wxCommandEvent& )    { if (m_btnTogTune->GetValue()) g_tuneLevel += TX_ATTENUATION_SMALL_STEP;  else g_txLevel += TX_ATTENUATION_SMALL_STEP;  applyTxLevel(); }
void MainFrame::OnTxLevelIncrBig( wxCommandEvent& ) { if (m_btnTogTune->GetValue()) g_tuneLevel += TX_ATTENUATION_LARGE_STEP; else g_txLevel += TX_ATTENUATION_LARGE_STEP; applyTxLevel(); }

void MainFrame::OnTxLevelMouseWheel( wxMouseEvent& event )
{
    int delta = (event.GetWheelRotation() > 0) ? TX_ATTENUATION_SMALL_STEP : -TX_ATTENUATION_SMALL_STEP;
    if (m_btnTogTune->GetValue()) g_tuneLevel += delta; else g_txLevel += delta;
    applyTxLevel();
}

void MainFrame::OnTxLevelContextMenu( wxContextMenuEvent& )
{
    uint64_t freq = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency;
    FilterFrequency bandEnum = getFilterForFrequency(freq);
    wxString bandName = bandNameForFilter(bandEnum); // string used for display labels and map keys

    if (bandName.IsEmpty())
        return;

    auto& atten = wxGetApp().appConfiguration.txAttenByBand;
    bool hasSaved = (atten->find(bandName) != atten->end());

    wxMenu menu;
    wxString toggleLabel = hasSaved
        ? wxString::Format(_("Disable auto-save of TX atten for %s"), bandName)
        : wxString::Format(_("Enable auto-save of TX atten for %s"),  bandName);
    auto toggleItem  = menu.Append(wxID_ANY, toggleLabel);
    auto restoreItem = menu.Append(wxID_ANY, wxString::Format(_("Restore TX atten level for %s"), bandName));
    restoreItem->Enable(hasSaved);

    // Toggle: Enable saves the current level and opts the band into auto-save;
    // Disable removes the entry so the band reverts to the global default.
    menu.Bind(wxEVT_MENU, [this, bandName, hasSaved](wxCommandEvent&) {
        if (hasSaved)
            wxGetApp().appConfiguration.txAttenByBand->erase(bandName);
        else
        {
            txLoadedLevel_ = g_txLevel; // record restore point at Enable time
            wxGetApp().appConfiguration.txAttenByBand->insert_or_assign(bandName, g_txLevel);
        }
        wxGetApp().appConfiguration.save(pConfig);
    }, toggleItem->GetId());
    // Restore reverts to txLoadedLevel_: the value active when this band was
    // entered or when Enable was last clicked, whichever is more recent.
    // Using a snapshot avoids the case where auto-save on band departure has
    // already overwritten the map entry with the adjusted value.
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        g_txLevel = txLoadedLevel_;
        applyTxLevel();
    }, restoreItem->GetId());

    PopupMenu(&menu);
}

void MainFrame::OnTuneAttenContextMenu( wxContextMenuEvent& )
{
    wxMenu menu;

    auto minItem = menu.Append(wxID_ANY, _("Set tune output to minimum (-30 dB)"));
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        g_tuneLevel = TX_ATTENUATION_MIN;
        applyTxLevel();
    }, minItem->GetId());

    uint64_t freq = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency;
    FilterFrequency bandEnum = getFilterForFrequency(freq);
    wxString bandName = bandNameForFilter(bandEnum);

    if (!bandName.IsEmpty())
    {
        menu.AppendSeparator();
        auto& atten = wxGetApp().appConfiguration.tuneAttenByBand;
        bool hasSaved = (atten->find(bandName) != atten->end());

        wxString toggleLabel = hasSaved
            ? wxString::Format(_("Disable auto-save of tune atten for %s"), bandName)
            : wxString::Format(_("Enable auto-save of tune atten for %s"),  bandName);
        auto toggleItem  = menu.Append(wxID_ANY, toggleLabel);
        auto restoreItem = menu.Append(wxID_ANY, wxString::Format(_("Restore tune atten level for %s"), bandName));
        restoreItem->Enable(hasSaved);

        menu.Bind(wxEVT_MENU, [this, bandName, hasSaved](wxCommandEvent&) {
            if (hasSaved)
                wxGetApp().appConfiguration.tuneAttenByBand->erase(bandName);
            else
            {
                tuneLoadedLevel_ = g_tuneLevel;
                wxGetApp().appConfiguration.tuneAttenByBand->insert_or_assign(bandName, g_tuneLevel);
            }
            wxGetApp().appConfiguration.save(pConfig);
        }, toggleItem->GetId());
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            g_tuneLevel = tuneLoadedLevel_;
            applyTxLevel();
        }, restoreItem->GetId());
    }

    PopupMenu(&menu);
}

static bool PttKeyDown_ = false;
int MainApp::FilterEvent(wxEvent& event)
{
    if ((event.GetEventType() == wxEVT_KEY_DOWN) &&
        (((wxKeyEvent&)event).GetKeyCode() == wxGetApp().appConfiguration.pttKeyCode))
        {
            bool pttKeyChanging = !PttKeyDown_;
            PttKeyDown_ = true;

            // only use space to toggle PTT if we are running and no modal dialogs (like options) up
            bool mainWindowActive = frame->IsActive();
            bool totWarningActive = frame->m_totWarningDialog_ != nullptr && frame->m_totWarningDialog_->IsActive();
            bool tuneActive = frame->m_btnTogTune->GetValue();

            // m_pttKeyRequireRelease_ blocks a key held through a forced TX stop
            // (e.g. TOT) from immediately restarting TX -- see main.h.
            if (frame->m_RxRunning && !tuneActive && !frame->m_pttKeyRequireRelease_ &&
                (mainWindowActive || totWarningActive) &&
                wxGetApp().appConfiguration.enableSpaceBarForPTT && !frame->isReceiveOnly()) {

                {
                    if (wxGetApp().appConfiguration.pttMomentaryMode) {
                        // Momentary mode: start TX only on the initial key press (not repeated events).
                        if (!g_tx.load(std::memory_order_acquire)) {
                            frame->m_btnTogPTT->SetValue(true);
                            frame->m_btnTogPTT->SetBackgroundColour(*wxRED);
                            frame->togglePTT();
                        }
                    } else if (pttKeyChanging) {
                        // Latching mode: toggle TX state on each key press.
                        if (frame->m_btnTogPTT->GetValue())
                            frame->m_btnTogPTT->SetValue(false);
                        else
                            frame->m_btnTogPTT->SetValue(true);

                        frame->togglePTT();
                    }
                }

                return Event_Processed; // absorb key so we don't toggle control with focus (e.g. Start)

            }
        }

    // In momentary mode, stop TX when the PTT key is released.
    if ((event.GetEventType() == wxEVT_KEY_UP) &&
        (((wxKeyEvent&)event).GetKeyCode() == wxGetApp().appConfiguration.pttKeyCode))
        {
            PttKeyDown_ = false;

            bool mainWindowActive = frame->IsActive();
            bool totWarningActive = frame->m_totWarningDialog_ != nullptr && frame->m_totWarningDialog_->IsActive();
            if (frame->m_RxRunning && (mainWindowActive || totWarningActive) &&
                wxGetApp().appConfiguration.enableSpaceBarForPTT && !frame->isReceiveOnly() &&
                wxGetApp().appConfiguration.pttMomentaryMode) {

                {
                    if (g_tx.load(std::memory_order_acquire)) {
                        frame->m_btnTogPTT->SetValue(false);
                        frame->m_btnTogPTT->SetBackgroundColour(wxNullColour);
                        frame->togglePTT();
                    } else if (frame->m_btnTogPTT->GetValue()) {
                        // Key released before g_tx caught up -- likely still inside
                        // togglePTT()'s TX/RX delay loop for the start that's in
                        // progress. Calling togglePTT() here would no-op against its
                        // re-entrancy guard, so remember the release and let
                        // togglePTT() action it once the start finishes -- see
                        // m_momentaryKeyReleasedDuringChangeover_ in main.h.
                        log_info("PTT key released mid-changeover -- deferring momentary stop");
                        frame->m_momentaryKeyReleasedDuringChangeover_ = true;
                    }
                }
                return Event_Processed;
            }
        }

    return Event_Skip;
}

void MainFrame::OnSetMonitorTxAudio( wxCommandEvent& event )
{
    wxGetApp().appConfiguration.monitorTxAudio = event.IsChecked();
    adjustMonitorPttVolMenuItem_->Enable(wxGetApp().appConfiguration.monitorTxAudio);
}

void MainFrame::OnSetMonitorTxAudioVol( wxCommandEvent& )
{
    auto popup = new MonitorVolumeAdjPopup(this, wxGetApp().appConfiguration.monitorTxAudioVol);
    popup->Popup();
}

//-------------------------------------------------------------------------
// OnTogBtnPTTRightClick(): show right-click menu for PTT button
//-------------------------------------------------------------------------
void MainFrame::OnTogBtnPTTRightClick( wxContextMenuEvent& )
{
    m_btnTogPTT->PopupMenu(pttPopupMenu_, LeftOffsetContextMenuPosition(m_btnTogPTT));
}

//-------------------------------------------------------------------------
// OnTogBtnPTTMouseDown()
// Set TX colour immediately on mouse press when going RX->TX, avoiding a GTK
// blue-flash during the TX delay before togglePTT() sets it on release.
// Only fires when the pipeline is genuinely in RX (g_tx false); during the
// TX->RX drain g_tx is still true, so clicks there are correctly ignored.
// NOTE for upstream: this is a simple cosmetic fix. A fuller alternative would
// be to start TX here on press and suppress the togglePTT() call on release.
//-------------------------------------------------------------------------
void MainFrame::OnTogBtnPTTMouseDown(wxMouseEvent& event)
{
    if (txChangeoverOccurring_) return;
    event.Skip();
}

//-------------------------------------------------------------------------
// OnTogBtnPTTMouseLeave()
// Reset premature TX colour if mouse leaves button before release and TX
// has not actually started, preventing a stuck-red button.
//-------------------------------------------------------------------------
void MainFrame::OnTogBtnPTTMouseLeave(wxMouseEvent& event)
{
    if (!m_btnTogPTT->GetValue() && !g_tx.load(std::memory_order_acquire))
    {
        m_btnTogPTT->SetBackgroundColour(wxNullColour);
#if !defined(__APPLE__)
        // macOS limitations prevent the foreground color of toggle buttons from being 
        // reliably set, so don't mess with it in the first place.
        m_btnTogPTT->SetForegroundColour(wxNullColour);
#endif // !defined(__APPLE__)
        m_btnTogPTT->Refresh();
    }
    event.Skip();
}

//-------------------------------------------------------------------------
// OnTogBtnPTT ()
//-------------------------------------------------------------------------
void MainFrame::OnTogBtnPTT (wxCommandEvent&)
{
    togglePTT();
}

//-------------------------------------------------------------------------
// OnTOTTimer()
// Time-Out Timer handler: fires when the configured TX time limit expires.
//-------------------------------------------------------------------------
void MainFrame::OnTOTTimer(wxTimerEvent&)
{
    if (!g_tx.load(std::memory_order_acquire))
        return;

    log_info("Time-Out Timer (TOT) expired — stopping transmit");

    if (m_totWarningTimer.IsRunning())
        m_totWarningTimer.Stop();

    if (m_totWarningDialog_)
    {
        auto dlg = m_totWarningDialog_;
        m_totWarningDialog_ = nullptr;
        dlg->Destroy();
    }
    m_totCurrentDurationMs = 0;

    {
        m_btnTogPTT->SetValue(false);
        endingTx.store(true, std::memory_order_release);
        togglePTT();

        // If the spacebar PTT key is still physically held down (e.g. something
        // resting on the keyboard), holding it through the timeout must not be
        // able to immediately re-key the rig -- that would defeat the whole
        // point of the TOT. wxEVT_KEY_UP/DOWN aren't reliable for detecting
        // "still held" here: on some platforms, a held key generates real
        // key-up/key-down event pairs at the OS repeat rate rather than a
        // single sustained key-down, so we poll the actual OS key state
        // instead and keep the spacebar disabled until it genuinely goes up.
        if (wxGetApp().appConfiguration.pttMomentaryMode &&
            PttKeyDown_)
        {
            log_info("TOT fired while PTT key still held -- blocking restart until key is released");
            m_pttKeyRequireRelease_ = true;
            m_pttKeyPollTimer.Start(30, wxTIMER_CONTINUOUS);
        }
    }
}

void MainFrame::OnPttKeyPollTimer(wxTimerEvent&)
{
    if (!PttKeyDown_)
    {
        log_info("PTT key released -- spacebar PTT re-armed");
        m_pttKeyRequireRelease_ = false;
        m_pttKeyPollTimer.Stop();
    }
}

void MainFrame::OnTOTWarningTimer(wxTimerEvent&)
{
    if (!g_tx.load(std::memory_order_acquire) || m_totCurrentDurationMs <= 0)
        return;

    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_totTxStartTime).count();
    int remaining = m_totCurrentDurationMs - (int)elapsed;

    if (remaining > 0 && remaining <= 15000)
    {
        if (!m_totWarningDialog_)
        {
            m_totWarningDialog_ = new TotWarningDialog(
                this, remaining,
                [this]() {
                    m_totWarningDialog_->Destroy();
                    m_totWarningDialog_ = nullptr;
                    
                    if (!g_tx.load(std::memory_order_acquire) || m_totCurrentDurationMs <= 0)
                        return;

                    m_totCurrentDurationMs = wxGetApp().appConfiguration.rigControlConfiguration.totTimerSecs * 1000;
                    m_totTxStartTime = std::chrono::high_resolution_clock::now();
                    m_totTimer.Start(m_totCurrentDurationMs, wxTIMER_ONE_SHOT);
                    log_info("Time-Out Timer (TOT) extended — %d ms remaining", m_totCurrentDurationMs);
                }
            );
            m_totWarningDialog_->Show();
            m_totWarningDialog_->Iconize(false); // undo minimize if required
            m_totWarningDialog_->Raise(); // brings from background to foreground if required
        }
        else
        {
            m_totWarningDialog_->updateRemainingTime(remaining);
        }
    }
    else if (remaining > 15000 && m_totWarningDialog_)
    {
        auto dlg = m_totWarningDialog_;
        m_totWarningDialog_ = nullptr;
        dlg->Destroy();
    }
}

void MainFrame::togglePTT(void) {
    // Guard against re-entrant calls during the TX drain (Yield() processes events).
    // This is necessary because we are not disabling the button during the changeover,
    // as doing so causes the text on the button to be unreadable.
    if (txChangeoverOccurring_) 
    {
        return;
    }
    txChangeoverOccurring_ = true;

    // If we're playing a RX file, we want to stop it before TX.
    if (g_playFileFromRadio.load(std::memory_order_acquire))
    {
        StopPlaybackFileFromRadio();
    }

    std::chrono::high_resolution_clock highResClock;

    // Record direction now; button value may be toggled by a stray click during
    // the drain loops below, which would corrupt newTx at the end if not checked.
    const bool wasInTx = g_tx.load(std::memory_order_acquire);

    if (wasInTx)
    {
        // Amber during TX->RX drain: distinct from TX (red) and RX (default),
        // black text readable throughout. Foreground is pinned explicitly
        // (not just left to the GTK theme) since some themes/window states
        // - e.g. backdrop while the TOT warning dialog has focus - would
        // otherwise dim or recolour the default text away from black.
        m_btnTogPTT->SetBackgroundColour(wxColour(255, 165, 0));
#if !defined(__APPLE__)
        // macOS limitations prevent the foreground color of toggle buttons from being 
        // reliably set, so don't mess with it in the first place.
        m_btnTogPTT->SetForegroundColour(*wxBLACK);
#endif // !defined(__APPLE__)
        m_btnTogPTT->SetLabel("TX Ending");
        m_btnTogPTT->Refresh();

        // Stop Time-Out Timer on TX->RX transition (user stopped, VK finished, or TOT fired).
        if (m_totTimer.IsRunning())
            m_totTimer.Stop();
        if (m_totWarningTimer.IsRunning())
            m_totWarningTimer.Stop();
        if (m_totWarningDialog_)
        {
            auto dlg = m_totWarningDialog_;
            m_totWarningDialog_ = nullptr;
            dlg->Destroy();
        }
        m_totCurrentDurationMs = 0;

        // If PTT input is enabled, suspend further changes until after EOO is sent.
        if (wxGetApp().m_pttInSerialPort)
        {
            wxGetApp().m_pttInSerialPort->suspendChanges(true);
        }
        
        // Give the tail of the burst time to leave the transmit FIFO. The
        // wait is the 160 ms it was when it was set by the voice modem's
        // frame, which chat bursts have always been timed against.
        constexpr int msSleep = 160;
        log_debug("Sleeping for %d ms prior to ending TX", msSleep);

        auto before = highResClock.now();

        while(true)
        {
            auto diff = highResClock.now() - before;
            if (diff >= std::chrono::milliseconds(msSleep))
            {
                break;
            }

            wxThread::Sleep(1);
            wxGetApp().Yield(true);
        }
        
        // Wait for the remaining samples to flow through the system before
        // toggling PTT. Note that there is a 1000ms timeout as backup.
        int sample = g_outfifo1_empty.load(std::memory_order_relaxed);
        before = highResClock.now();
        while(true)
        {
            auto diff = highResClock.now() - before;
            auto tmp = g_outfifo1_empty.load(std::memory_order_relaxed);
            if (diff >= std::chrono::milliseconds(1000) || (tmp != sample))
            {
                log_info("All TX finished (diff = %d ms, fifo_empty = %d, sample = %d), going out of PTT", (int)std::chrono::duration_cast<std::chrono::milliseconds>(diff).count(), tmp, sample);
                break;
            }

            wxThread::Sleep(1);

            // Yield() used to avoid lack of UI responsiveness during delay.
            wxGetApp().Yield(true);
        }
        
        // Wait for a minimum amount of time before stopping TX to ensure that
        // remaining audio gets piped to the radio from the operating system.
        auto outDevice = txOutSoundDevice;
        auto latency = 0;
        if (outDevice)
        {
            latency = outDevice->getLatencyInMicroseconds();
        }

        log_info(
            "Pausing for a minimum of %d us before TX->RX to allow remaining audio to go out", 
            latency);
        before = highResClock.now();
        while(true)
        {
            auto diff = highResClock.now() - before;
            if (diff >= std::chrono::microseconds(latency))
            {
                break;
            }

            wxThread::Sleep(1);

            // Yield() used to avoid lack of UI responsiveness during delay.
            wxGetApp().Yield(true);
        }
                
        // Wait an additional configured timeframe before actually clearing PTT (below)
        if (wxGetApp().appConfiguration.txRxDelayMilliseconds > 0)
        {
            // Delay outbound TX audio if going into TX.
            // Yield() used to avoid lack of UI responsiveness during delay.
            before = highResClock.now();
            while(true)
            {
                auto diff = highResClock.now() - before;
                if (diff >= std::chrono::milliseconds(wxGetApp().appConfiguration.txRxDelayMilliseconds.get()))
                {
                    break;
                }

                wxThread::Sleep(1);
                wxGetApp().Yield(true);
            }
        }
        g_tx.store(false, std::memory_order_release);
        endingTx.store(false, std::memory_order_release);
        
        if (wxGetApp().m_pttInSerialPort)
        {
            wxGetApp().m_pttInSerialPort->suspendChanges(false);
        }
        
        // Re-enable buttons.
        m_togBtnOnOff->Enable(true);
    }
    else
    {
        // Force PTT button colors ASAP to avoid latency after mouse up.
        m_btnTogPTT->SetBackgroundColour(*wxRED);
#if !defined(__APPLE__)
        // macOS limitations prevent the foreground color of toggle buttons from being 
        // reliably set, so don't mess with it in the first place.
        m_btnTogPTT->SetForegroundColour(*wxBLACK);
#endif // !defined(__APPLE__)
        wxGetApp().Yield(true);

        // If PTT input is enabled, suspend further changes until we actually start TX.
        if (wxGetApp().m_pttInSerialPort)
        {
            wxGetApp().m_pttInSerialPort->suspendChanges(true);
        }
        // Disable On/Off button.
        m_togBtnOnOff->Enable(false);
    }

    // Use wasInTx to determine direction: don't let a stray click during the drain
    // flip newTx and leave the radio keyed with the pipeline in the wrong state.
    auto newTx = !wasInTx;
    if (wxGetApp().rigPttController != nullptr && wxGetApp().rigPttController->isConnected())
    {
        // The radio is not polled while it transmits, so read its frequency
        // once more just before keying: a retune in the last second before
        // a burst would otherwise not show until the burst ends.
        if (newTx) refreshRigFrequencyBeforeKeying_();
        wxGetApp().rigPttController->ptt(newTx);
    }

    // reset level gauge

    m_maxLevel = 0;
    m_gaugeLevel->SetValue(0);
    
    // Report TX change to registered reporters
    for (auto& obj : wxGetApp().m_reporters)
    {
        obj->transmit("GLISSANDO", newTx);
    }
    
    // If we're recording, switch to/from modulator and radio.
    if (g_sfRecFile != nullptr)
    {
        if (!newTx)
        {
            g_recFileFromModulator = false;
            g_recFileFromRadio = true;
        }
        else
        {
            g_recFileFromRadio = false;
            g_recFileFromModulator = true;
        }
    }

    if (newTx)
    {
        endingTx.store(false, std::memory_order_release);
            
        if (wxGetApp().appConfiguration.txRxDelayMilliseconds > 0)
        {
            // Delay outbound TX audio if going into TX.
            // Note: Yield() used here to avoid lack of UI responsiveness during delay.
            auto before = highResClock.now();
            while(true)
            {
                auto diff = highResClock.now() - before;
                if (diff >= std::chrono::milliseconds(wxGetApp().appConfiguration.txRxDelayMilliseconds.get()))
                {
                    break;
                }

                wxThread::Sleep(1);
                wxGetApp().Yield(true);
            }
        }

        // g_tx governs when audio actually goes out during TX, so don't set to true until
        // after the delay occurs.
        g_tx.store(true, std::memory_order_release);

        // Start Time-Out Timer if enabled.
        if (wxGetApp().appConfiguration.rigControlConfiguration.totTimerEnabled &&
            wxGetApp().appConfiguration.rigControlConfiguration.totTimerSecs > 0)
        {
            int totMs = wxGetApp().appConfiguration.rigControlConfiguration.totTimerSecs * 1000;
            log_info("Starting Time-Out Timer (%d seconds)", wxGetApp().appConfiguration.rigControlConfiguration.totTimerSecs.get());
            m_totTimer.Start(totMs, wxTIMER_ONE_SHOT);

            m_totTxStartTime = std::chrono::high_resolution_clock::now();
            m_totCurrentDurationMs = totMs;
            m_totWarningTimer.Start(500, wxTIMER_CONTINUOUS);
        }

        if (wxGetApp().m_pttInSerialPort)
        {
            wxGetApp().m_pttInSerialPort->suspendChanges(false);
        }
    }
    
    // wxWidgets should already be doing the below logic,
    // but for some reason this intermittently doesn't happen
    // on Windows. Just to be sure, we force the correct state
    // here (similar to what's already done for ending TX while
    // using the voice keyer).
    m_btnTogPTT->SetValue(newTx);
    m_btnTogPTT->SetLabel(_("&XMIT"));
    m_btnTogPTT->SetBackgroundColour(m_btnTogPTT->GetValue() ? *wxRED : wxNullColour);
#if !defined(__APPLE__)
    // macOS limitations prevent the foreground color of toggle buttons from being 
    // reliably set, so don't mess with it in the first place.
    m_btnTogPTT->SetForegroundColour(m_btnTogPTT->GetValue() ? *wxBLACK : wxNullColour);
#endif // !defined(__APPLE__)
    
    // The Report Frequency drop-down should not be modifiable during TX.
    // Additionally, tuning during normal TX is verboten.
    m_cboReportFrequency->Enable(!newTx);
    m_btnTogTune->Enable(!newTx);

    CallAfter([&]() {
        txChangeoverOccurring_ = false;
    });

    if (newTx && m_momentaryKeyReleasedDuringChangeover_)
    {
        // The momentary PTT key was released while this start was still in
        // progress (see the wxEVT_KEY_UP handler in FilterEvent). Queued
        // after the CallAfter() above so it runs once txChangeoverOccurring_
        // has cleared, rather than no-opping against it.
        m_momentaryKeyReleasedDuringChangeover_ = false;
        log_info("Momentary PTT key was released while TX was starting -- stopping now");
        CallAfter([this]() {
            m_btnTogPTT->SetValue(false);
            m_btnTogPTT->SetBackgroundColour(wxNullColour);
            togglePTT();
        });
    }
}

void MainFrame::OnTogBtnTune(wxCommandEvent&)
{
    // Ensure background is correct for button.
    bool newTx = m_btnTogTune->GetValue();
    m_btnTogTune->SetBackgroundColour(newTx ? *wxRED : wxNullColour);

    // Update PTT state
    if (wxGetApp().rigPttController != nullptr && wxGetApp().rigPttController->isConnected()) 
    {
        if (newTx) refreshRigFrequencyBeforeKeying_();
        wxGetApp().rigPttController->ptt(newTx);
    }

    // Disable actual TX controls if needed
    m_btnTogPTT->Enable(!newTx);
    m_cboReportFrequency->Enable(!newTx);

    // Enable tuning carrier
    g_rxUserdata->tuneSineWaveSampleNumber.store(0, std::memory_order_release);
    g_rxUserdata->isTuning.store(newTx, std::memory_order_release);

    wxString fmtString;
    if (newTx)
    {
        fmtString = wxString::Format(MIC_SPKR_LEVEL_FORMAT_STR, wxNumberFormatter::ToString((double)g_tuneLevel/10.0, 1), DECIBEL_STR);
        m_txLevelBox->SetLabel("Tune &Attenuation");
    }
    else
    {
        fmtString = wxString::Format(MIC_SPKR_LEVEL_FORMAT_STR, wxNumberFormatter::ToString((double)g_txLevel/10.0, 1), DECIBEL_STR);
        m_txLevelBox->SetLabel("TX &Attenuation");
    }

    m_txtTxLevelNum->SetLabel(fmtString);

    // Make sure focus on Tune button is actually cleared once tune state is switched.
    // Seems to be a common problem on some Linux systems for some reason.
    m_auiNbookCtrl->SetFocus();
}

void MainFrame::OnCallSignReset(wxCommandEvent&)
{
    m_txtCtrlCallSign->SetValue(wxEmptyString);
    
    m_lastReportedCallsignListView->DeleteAllItems();
    m_cboLastReportedCallsigns->SetText(_(""));
}

void MainFrame::OnRightClickCallsignList(wxMouseEvent&)
{
    auto index = m_lastReportedCallsignListView->GetFirstSelected();
    while (index != -1)
    {
        wxGetApp().lastSelectedLoggingRow = MainApp::UNSELECTED;
        m_lastReportedCallsignListView->Select(index, false);
        index = m_lastReportedCallsignListView->GetFirstSelected();
    }
    // Deselect only -- leave the displayed callsign text alone.
    // See OnCloseCallsignList() for why this is deferred and followed by a
    // forced repaint.
    CallAfter([this]() {
        m_BtnCallSignReset->SetFocus();
        m_cboLastReportedCallsigns->Refresh();
        m_cboLastReportedCallsigns->Update();
    });
}

void MainFrame::OnOpenCallsignList( wxCommandEvent& event )
{
    wxGetApp().lastSelectedLoggingRow = MainApp::MAIN_WINDOW;
    event.Skip();
}

void MainFrame::OnCloseCallsignList( wxCommandEvent& event )
{
    auto index = m_lastReportedCallsignListView->GetFirstSelected();
    if (index == -1)
    {
        // Make sure we're not selected if no callsigns selected.
        wxGetApp().lastSelectedLoggingRow = MainApp::UNSELECTED;

        // Deferred via CallAfter so it runs once the popup's own dismissal
        // processing has fully finished.
        CallAfter([this]() {
            m_BtnCallSignReset->SetFocus();

            // The popup window overlaps the combo's own value area while
            // open; on dismiss GTK doesn't always damage/repaint that
            // region, so the combo can be left showing its stale
            // "focused/selected" look from before the popup covered it.
            // Force a fresh, synchronous repaint.
            m_cboLastReportedCallsigns->Refresh();
            m_cboLastReportedCallsigns->Update();
        });
    }
    event.Skip();
}

void MainFrame::OnChangeReportFrequencyVerify( wxCommandEvent& event )
{
    if (wxGetApp().rigFrequencyController != nullptr && suppressFreqModeUpdates_)
    {
        return;
    }
    
    OnChangeReportFrequency(event);
}

void MainFrame::OnChangeReportFrequency( wxCommandEvent& event )
{    
    wxString freqStr = m_cboReportFrequency->GetValue();
    auto oldFreq = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency;

    wxString oldFreqString;            
    if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
    {
        oldFreqString = wxNumberFormatter::ToString(oldFreq / 1000.0, 1);
    }
    else
    {
        oldFreqString = wxNumberFormatter::ToString(oldFreq / 1000.0 / 1000.0, 4);
    }

    if (freqStr.Length() > 0)
    {
        double tmp = 0;
        wxNumberFormatter::FromString(freqStr, &tmp);
        
        if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
        {
            wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency = round(tmp * 1000);
        }
        else
        {
            wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency = round(tmp * 1000 * 1000);
        }

        if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency > 0)
        {
            m_cboReportFrequency->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));

            // Round to the nearest 100 Hz.
            uint64_t wholeFreq = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency / 100;
            uint64_t remainder = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency % 100;

            if (remainder >= 50)
            {
                wholeFreq++;
            }

            wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency = wholeFreq * 100;
        }
        else
        {
            m_cboReportFrequency->SetForegroundColour(wxColor(*wxRED));
        }
    }
    else
    {
        wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency = 0;
        m_cboReportFrequency->SetForegroundColour(wxColor(*wxRED));
    }

    // Text chat follows a frequency once it has been entered: on Enter, on a
    // choice from the list, or when the box loses focus. Not on each keystroke
    // while it is being typed -- "14.080" passes through 1, 14 and 14.0 MHz on
    // the way, and each of those would stop chat and discard what it had
    // queued. With rig control the radio is not retuned until then either.
    bool stillTyping = event.GetEventType() == wxEVT_TEXT && suppressFreqModeUpdates_;
    if (!stillTyping) updateTextChatTransmitPermission_();

    if (freqStr != oldFreqString)
    {
        log_info("Request frequency change to %" PRIu64 " Hz", wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency.get());

        // Report current frequency to reporters
        for (auto& ptr : wxGetApp().m_reporters)
        {
            ptr->freqChange(wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency);
        }
    }

    // Only a frequency the operator entered or picked retunes the radio.
    // The box also changes when the radio reports a new frequency and
    // when it loses focus (as it does when transmitting disables it);
    // sending those back could put the radio on a frequency it has
    // since been turned away from. An operator's choice goes to the radio even
    // when the box already shows it, in case the radio moved meanwhile.
    bool operatorChoice = event.GetEventType() == wxEVT_TEXT_ENTER || event.GetEventType() == wxEVT_COMBOBOX;
    if (operatorChoice &&
        wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency > 0 && 
        (wxGetApp().appConfiguration.rigControlConfiguration.hamlibEnableFreqModeChanges || wxGetApp().appConfiguration.rigControlConfiguration.hamlibEnableFreqChangesOnly))
    {
        if (wxGetApp().rigFrequencyController != nullptr && wxGetApp().rigFrequencyController->isConnected())
        {
            // Request the frequency change on the radio side; the mode is
            // left as the operator set it.
            wxGetApp().rigFrequencyController->setFrequency(wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency);
        }
        else
        {
            operatorFrequencyPending_ = true;
        }
    }

    // Auto-save outgoing band levels, then load the new band's levels
    if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency > 0)
    {
        auto newBandEnum = getFilterForFrequency(
            wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency);
        if (newBandEnum != BAND_OTHER && newBandEnum != lastBand_)
        {
            autoSaveCurrentBandLevels_();
            lastBand_ = newBandEnum;
            loadTxAttenForBand_(newBandEnum);
            loadTuneAttenForBand_(newBandEnum);
            applyTxLevel(); // apply both loaded values in one call
        }
    }
}

void MainFrame::OnReportFrequencySetFocus(wxFocusEvent& event)
{
    // Hold off the radio's reports only while someone can type in the box.
    // The main window stays hidden behind the Glissando console, and a box
    // that took focus there would otherwise stop the app following the
    // radio until it next lost focus.
    suppressFreqModeUpdates_ = IsShown();
    TopFrame::OnReportFrequencySetFocus(event);
}

void MainFrame::OnReportFrequencyKillFocus(wxFocusEvent& event)
{
    suppressFreqModeUpdates_ = false;
    
    // Handle any frequency changes as appropriate.
    wxCommandEvent tmpEvent;
    OnChangeReportFrequency(tmpEvent);

    TopFrame::OnReportFrequencyKillFocus(event);
}

void MainFrame::OnSystemColorChanged(wxSysColourChangedEvent& event)
{
    // Works around issues on wxWidgets with certain controls not changing colors
    // when the user switches between light and dark mode.
    if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency > 0)
    {
        m_cboReportFrequency->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    }
    else
    {
        m_cboReportFrequency->SetForegroundColour(*wxRED);
    }
    TopFrame::OnSystemColorChanged(event);
}

void MainFrame::updateReportingFreqList_()
{
    uint64_t prevFreqInt = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequency;

    double freq =  ((double)prevFreqInt)/1000.0;
    if (!wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
    {
        freq /= 1000.0;
    }

    wxString prevSelected;    
    if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
    {
        prevSelected = wxNumberFormatter::ToString(freq, 1);
    }
    else
    {
        prevSelected = wxNumberFormatter::ToString(freq, 4);
    }
    m_cboReportFrequency->Clear();
    
    for (auto& item : wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyList.get())
    {
        m_cboReportFrequency->Append(item);
    }
    
    auto idx = m_cboReportFrequency->FindString(prevSelected);
    if (idx >= 0)
    {
        m_cboReportFrequency->SetSelection(idx);
    }
    m_cboReportFrequency->SetValue(prevSelected);
    
    // Update associated label if the units have changed
    if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
    {
        m_freqBox->SetLabel(_("Radio Freq. (kHz)"));
    }
    else
    {
        m_freqBox->SetLabel(_("Radio Freq. (MHz)"));
    }
}

void MainFrame::OnToolsExportConfigUI(wxUpdateUIEvent& event)
{
    event.Enable(!m_RxRunning);
}

void MainFrame::OnToolsImportConfigUI(wxUpdateUIEvent& event)
{
    event.Enable(!m_RxRunning);
}

void MainFrame::OnToolsExportConfig(wxCommandEvent& event)
{
    wxUnusedVar(event);

    wxFileDialog saveFileDialog(
        this,
        _("Export Glissando Configuration"),
        wxGetApp().defaultConfigFilePath,
        wxEmptyString,
        wxT("Glissando configuration files (*.conf)|*.conf|All files (*.*)|*.*"),
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (saveFileDialog.ShowModal() == wxID_CANCEL)
        return;

    wxString path = saveFileDialog.GetPath();
    wxFileConfig* exportConfig = new wxFileConfig(wxT("glissando"), wxT("Glissando"), path, path, wxCONFIG_USE_LOCAL_FILE);
    exportConfiguration_(exportConfig);
    exportConfig->Flush();
    delete exportConfig;
}

void MainFrame::OnToolsImportConfig(wxCommandEvent& event)
{
    wxUnusedVar(event);

    wxFileDialog openFileDialog(
        this,
        _("Import Glissando Configuration"),
        wxGetApp().defaultConfigFilePath,
        wxEmptyString,
        wxT("Glissando configuration files (*.conf)|*.conf|All files (*.*)|*.*"),
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    if (openFileDialog.ShowModal() == wxID_CANCEL)
        return;

    wxString path = openFileDialog.GetPath();

    if (!wxFileExists(path))
    {
        wxMessageBox(_("The selected file does not exist."), _("Import Error"), wxOK | wxICON_ERROR, this);
        return;
    }

    wxFileConfig* importConfig = new wxFileConfig(wxT("glissando"), wxT("Glissando"), path, path, wxCONFIG_USE_LOCAL_FILE);

    if (importConfig->GetNumberOfGroups() == 0 && importConfig->GetNumberOfEntries() == 0)
    {
        delete importConfig;
        wxMessageBox(_("The selected file could not be parsed as a Glissando configuration."), _("Import Error"), wxOK | wxICON_ERROR, this);
        return;
    }

    // On Linux/macOS, this replaces $HOME with "~" to shorten the title a bit.
    wxFileName fn(path);
    wxGetApp().customConfigFileName = fn.GetFullName();

    SetTitle(wxString::Format("%s (%s)", _("Glissando ") + wxString::FromUTF8(GetFreeDVVersion().c_str()), wxGetApp().customConfigFileName));
    setConfiguration_(importConfig);

    // Remember this file so it is automatically restored on the next startup.
    saveLastUsedConfigPath(path);
}

void MainFrame::OnToolsLoadDefaultConfigUI(wxUpdateUIEvent& event)
{
    event.Enable(!m_RxRunning);
}

void MainFrame::OnToolsLoadDefaultConfig(wxCommandEvent& event)
{
    wxUnusedVar(event);

    wxMessageDialog messageDialog(
        this, _("This will load the default FreeDV configuration. Are you sure?"),
        _("Load Default Configuration"),
        wxYES_NO | wxICON_QUESTION | wxCENTRE);

    if (messageDialog.ShowModal() != wxID_YES)
        return;

    // Create a platform-appropriate default config:
    // On Windows this uses the registry (wxRegConfig); on macOS/Linux it
    // uses the default file location (wxFileConfig).  This becomes the
    // active pConfig going forward — no need to restore the old one.
    wxConfigBase* defaultConfig = wxConfigBase::Create();
    
    setConfiguration_(defaultConfig);

    // Remove the last-used config path so startup reverts to the default next time.
    clearLastUsedConfigPath();

    // Clear any custom config file indicator from the title bar.
    wxGetApp().customConfigFileName = wxEmptyString;
    SetTitle(_("Glissando ") + wxString::FromUTF8(GetFreeDVVersion().c_str()));
}
