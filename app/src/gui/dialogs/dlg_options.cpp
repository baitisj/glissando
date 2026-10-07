//==========================================================================
// Name:            dlg_options.cpp
// Purpose:         Dialog for controlling misc FreeDV options
// Date:            May 24 2013
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
#include <wx/gbsizer.h>
#include <wx/numformatter.h>
#include "GlissandoCw.h"
#include "pipeline/TextMessagingModem.h"
#include "dlg_options.h"


// F13-F24 on Linux (and possibly other platforms) return values different
// than how they're defined in wxWidgets. These constants are so we can
// check for these as well and do the right thing for key->name mapping.
constexpr int WXK_F13_LINUX = 436;
constexpr int WXK_F24_LINUX = WXK_F13_LINUX + 11;

// Returns a human-readable name for a PTT key code.
static wxString getPTTKeyName(int keyCode)
{
    if (keyCode >= 'A' && keyCode <= 'Z')
    {
        return wxString((char)keyCode);
    }
    else if (keyCode >= '0' && keyCode <= '9')
    {
        return wxString((char)keyCode);
    }
    else if (keyCode >= WXK_F1 && keyCode <= WXK_F24)
    {
        return wxString::Format(_("F%d"), (keyCode - WXK_F1) + 1);
    }
    else if (keyCode >= WXK_F13_LINUX && keyCode <= WXK_F24_LINUX)
    {
        return wxString::Format(_("F%d"), (keyCode - WXK_F13_LINUX) + 13);
    }

    switch (keyCode)
    {
        case WXK_SPACE:    return _("Space");
        case WXK_TAB:      return _("Tab");
        case WXK_RETURN:   return _("Enter");
        case WXK_ESCAPE:   return _("Escape");
        case WXK_BACK:     return _("Backspace");
        case WXK_DELETE:   return _("Delete");
        case WXK_INSERT:   return _("Insert");
        case WXK_HOME:     return _("Home");
        case WXK_END:      return _("End");
        case WXK_PAGEUP:   return _("Page Up");
        case WXK_PAGEDOWN: return _("Page Down");
        case WXK_UP:       return _("Up");
        case WXK_DOWN:     return _("Down");
        case WXK_LEFT:     return _("Left");
        case WXK_RIGHT:    return _("Right");
        default:
            if (keyCode > 32 && keyCode < 127)
                return wxString((char)keyCode);
            return wxString::Format(_("Key(%d)"), keyCode);
    }
}

// PortAudio over/underflow counters

extern std::atomic<int>    g_infifo1_full;
extern std::atomic<int>    g_outfifo1_empty;
extern std::atomic<int>    g_infifo2_full;
extern std::atomic<int>    g_outfifo2_empty;
extern int                 g_AEstatus1[4];
extern int                 g_AEstatus2[4];
extern wxDatagramSocket    *g_sock;
extern wxConfigBase *pConfig;

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
// Class OptionsDlg
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
OptionsDlg::OptionsDlg(wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style) : wxDialog(parent, id, title, pos, size, style)
{
    // XXX - FreeDV only supports English but makes a best effort to at least use regional formatting
    // for e.g. numbers. Thus, we only need to override layout direction.
    SetLayoutDirection(wxLayout_LeftToRight);
    
    if (wxGetApp().customConfigFileName != "")
    {
        SetTitle(wxString::Format("%s (%s)", title, wxGetApp().customConfigFileName));
    }
    
    sessionActive_ = false;
    
    wxPanel* panel = new wxPanel(this);
    
    wxBoxSizer* bSizer30;
    bSizer30 = new wxBoxSizer(wxVERTICAL);
    
    // Create notebook and tabs.
    m_notebook = new wxNotebook(panel, wxID_ANY);
    m_reportingTab = new wxPanel(m_notebook, wxID_ANY);
    m_rigControlTab = new wxPanel(m_notebook, wxID_ANY);
    m_modemTab = new wxPanel(m_notebook, wxID_ANY);
    m_uiTab = new wxPanel(m_notebook, wxID_ANY);
    m_debugTab = new wxPanel(m_notebook, wxID_ANY);
    
    m_notebook->AddPage(m_reportingTab, _("Station"));
    m_notebook->AddPage(m_rigControlTab, _("Rig Control"));
    m_notebook->AddPage(m_modemTab, _("Modem"));
    m_notebook->AddPage(m_uiTab, _("UI Options"));
    m_notebook->AddPage(m_debugTab, _("Debugging"));
    
    bSizer30->Add(m_notebook, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 3);
    
    // Station tab: the callsign chat goes out under, and where stations
    // heard are logged.
    wxBoxSizer* sizerReporting = new wxBoxSizer(wxVERTICAL);

    wxStaticBox* sbStation = new wxStaticBox(m_reportingTab, wxID_ANY, _("Station"));
    wxStaticBoxSizer* sbSizerStationRows = new wxStaticBoxSizer(sbStation, wxVERTICAL);

    wxBoxSizer* sbSizerCallsign = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* labelCallsign = new wxStaticText(sbStation, wxID_ANY, wxT("Callsign:"), wxDefaultPosition, wxDefaultSize, 0);
    sbSizerCallsign->Add(labelCallsign, 0,  static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    m_txt_callsign = new wxTextCtrl(sbStation, wxID_ANY,  wxEmptyString, wxDefaultPosition, wxSize(180,-1), 0, wxTextValidator(wxFILTER_ALPHANUMERIC));
    m_txt_callsign->SetToolTip(_("The callsign text chat sends under."));
    sbSizerCallsign->Add(m_txt_callsign, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);

    sbSizerCallsign->AddSpacer(10);
    wxStaticText* labelGridSquare = new wxStaticText(sbStation, wxID_ANY, _("Grid square:"));
    sbSizerCallsign->Add(labelGridSquare, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    m_txtGridSquare = new wxTextCtrl(sbStation, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(90, -1));
    m_txtGridSquare->SetToolTip(_("Your Maidenhead locator, such as CN87 or CN87ux. Chat sends the first four "
                                  "characters, which place you to within about 100 km."));
    sbSizerCallsign->Add(m_txtGridSquare, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    m_ckboxSendGridSquare = new wxCheckBox(sbStation, wxID_ANY, _("Send it with chat"), wxDefaultPosition,
                                           wxDefaultSize, wxCHK_2STATE);
    m_ckboxSendGridSquare->SetToolTip(
        _("Your grid square rides at the end of your first message to each station that can read it, "
          "until that station says it has it, and fills the spare voice of a duet for free."));
    sbSizerCallsign->Add(m_ckboxSendGridSquare, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    sbSizerStationRows->Add(sbSizerCallsign, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);

    // CSV log file path
    wxBoxSizer* sbSizerCsvLog = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* labelCsvLogPath = new wxStaticText(sbStation, wxID_ANY, wxT("Stations Heard Log File:"), wxDefaultPosition, wxDefaultSize, 0);
    sbSizerCsvLog->Add(labelCsvLogPath, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    m_txtCtrlCsvLogFilePath = new wxTextCtrl(sbStation, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(200, -1), 0);
    m_txtCtrlCsvLogFilePath->SetToolTip(_("While engaged, each station whose chat is heard is added to this CSV file: time, callsign, modem, frequency and SNR."));
    sbSizerCsvLog->Add(m_txtCtrlCsvLogFilePath, 1, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    m_buttonChooseCsvLogFilePath = new wxButton(sbStation, wxID_ANY, _("Choose"), wxDefaultPosition, wxSize(-1, -1), 0);
    m_buttonChooseCsvLogFilePath->SetMinSize(wxSize(120, -1));
    sbSizerCsvLog->Add(m_buttonChooseCsvLogFilePath, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    sbSizerStationRows->Add(sbSizerCsvLog, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);

    sizerReporting->Add(sbSizerStationRows, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);
    
    m_reportingTab->SetSizer(sizerReporting);
    
    // Rig Control tab
    wxBoxSizer* sizerRigControl = new wxBoxSizer(wxVERTICAL);
    
    //------------------------------
    // Rig Control options
    //------------------------------
    
    wxStaticBoxSizer* sbSizer_ptt;
    wxStaticBox *sb_ptt = new wxStaticBox(m_rigControlTab, wxID_ANY, _("PTT Options"));
    sbSizer_ptt = new wxStaticBoxSizer(sb_ptt, wxVERTICAL);
    
    wxSizer* pttKeySizer = new wxBoxSizer(wxHORIZONTAL);
    m_ckboxEnableSpacebarForPTT = new wxCheckBox(sb_ptt, wxID_ANY, _("Enable key for PTT:"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    pttKeySizer->Add(m_ckboxEnableSpacebarForPTT, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);

    m_txtPTTKeyName = new wxTextCtrl(sb_ptt, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(120, -1), wxTE_READONLY | wxTE_PROCESS_ENTER);
    m_txtPTTKeyName->SetToolTip(_("The key currently assigned to PTT."));
    pttKeySizer->Add(m_txtPTTKeyName, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);

    m_btnSetPTTKey = new wxButton(sb_ptt, wxID_ANY, _("Change..."), wxDefaultPosition, wxDefaultSize);
    pttKeySizer->Add(m_btnSetPTTKey, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);

    sbSizer_ptt->Add(pttKeySizer, 0, static_cast<int>(wxALL), 0);

    m_ckboxPTTMomentaryMode = new wxCheckBox(sb_ptt, wxID_ANY, _("Momentary PTT (hold key to transmit)"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxPTTMomentaryMode->SetToolTip(_("When enabled, you must hold the PTT button or key to keep transmitting. Releasing it returns to receive."));
    sbSizer_ptt->Add(m_ckboxPTTMomentaryMode, 0, static_cast<int>(wxALL), 5);

    wxSizer* txRxDelaySizer = new wxBoxSizer(wxHORIZONTAL);

    auto txRxDelayLabel = new wxStaticText(sb_ptt, wxID_ANY, _("TX/RX Delay (milliseconds): "));
    txRxDelaySizer->Add(txRxDelayLabel, 0, static_cast<int>(wxALL) | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 5);

    m_txtTxRxDelayMilliseconds = new wxTextCtrl(sb_ptt, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80,-1), 0, wxTextValidator(wxFILTER_DIGITS));
    m_txtTxRxDelayMilliseconds->SetToolTip(_("The amount of time to wait between toggling PTT and stopping/starting TX audio in milliseconds."));
    txRxDelaySizer->Add(m_txtTxRxDelayMilliseconds, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);

    sbSizer_ptt->Add(txRxDelaySizer, 0, static_cast<int>(wxALL), 0);

    wxSizer* totTimerSizer = new wxBoxSizer(wxHORIZONTAL);

    m_ckboxTOTTimerEnabled = new wxCheckBox(sb_ptt, wxID_ANY, _("Enable Time-Out Timer (TOT):"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxTOTTimerEnabled->SetToolTip(_("When enabled, FreeDV will automatically stop transmitting after the configured time period has elapsed."));
    totTimerSizer->Add(m_ckboxTOTTimerEnabled, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);

    m_txtTOTTimerSecs = new wxTextCtrl(sb_ptt, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1), 0, wxTextValidator(wxFILTER_DIGITS));
    m_txtTOTTimerSecs->SetToolTip(_("The number of seconds FreeDV will transmit before automatically dropping back to receive."));
    totTimerSizer->Add(m_txtTOTTimerSecs, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);

    auto totTimerSecsLabel = new wxStaticText(sb_ptt, wxID_ANY, _("seconds"));
    totTimerSizer->Add(totTimerSecsLabel, 0, static_cast<int>(wxALL) | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 5);

    sbSizer_ptt->Add(totTimerSizer, 0, static_cast<int>(wxALL), 0);

    m_ckboxTOTTimerEnabled->Connect(wxEVT_CHECKBOX, wxCommandEventHandler(OptionsDlg::OnTOTTimerEnable), NULL, this);

    sizerRigControl->Add(sbSizer_ptt,0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);

    // Read while transmitting, so these can change during a session.
    wxStaticBox* sb_swr = new wxStaticBox(m_rigControlTab, wxID_ANY, _("SWR"));
    wxStaticBoxSizer* sbSizer_swr = new wxStaticBoxSizer(sb_swr, wxVERTICAL);

    m_ckboxSwrMeter = new wxCheckBox(sb_swr, wxID_ANY, _("Show SWR on the console's meter while transmitting"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxSwrMeter->SetToolTip(_("Asks the radio for its SWR once a second while transmitting, and the console's Signal meter reads SWR instead. Only radios whose Hamlib backend reports SWR are asked. Turn this off if your radio stumbles when spoken to while transmitting."));
    sbSizer_swr->Add(m_ckboxSwrMeter, 0, static_cast<int>(wxALL), 5);

    m_ckboxSwrAutoAbort = new wxCheckBox(sb_swr, wxID_ANY, _("Abort transmitting when SWR goes over 3:1"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxSwrAutoAbort->SetToolTip(_("Stops the transmission at the first reading over 3:1, as the console's Abort button would: the message is dropped, not retried."));
    sbSizer_swr->Add(m_ckboxSwrAutoAbort, 0, static_cast<int>(wxALL), 5);

    m_ckboxSwrMeter->Connect(wxEVT_CHECKBOX, wxCommandEventHandler(OptionsDlg::OnSwrMeterEnable), NULL, this);

    sizerRigControl->Add(sbSizer_swr, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);
    
    wxStaticBoxSizer* sbSizer_hamlib;
    wxStaticBox *sb_hamlib = new wxStaticBox(m_rigControlTab, wxID_ANY, _("Frequency Control Options"));
    sbSizer_hamlib = new wxStaticBoxSizer(sb_hamlib, wxVERTICAL);
    
    // The radio's mode (USB, LSB, DIGU...) is the operator's to set; only the
    // frequency is ever changed from here.
    wxSizer* freqModeSizer = new wxBoxSizer(wxHORIZONTAL);
    m_rbFrequencyControl = new wxRadioButton(sb_hamlib, wxID_ANY, _("Set the radio's frequency"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    freqModeSizer->Add(m_rbFrequencyControl, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);
    
    m_rbNoFrequencyControl = new wxRadioButton(sb_hamlib, wxID_ANY, _("Leave the radio's frequency alone"), wxDefaultPosition, wxDefaultSize);
    freqModeSizer->Add(m_rbNoFrequencyControl, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);
    
    sbSizer_hamlib->Add(freqModeSizer, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);
    
    m_ckboxFrequencyEntryAsKHz = new wxCheckBox(sb_hamlib, wxID_ANY, _("Frequency entry in kHz"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    sbSizer_hamlib->Add(m_ckboxFrequencyEntryAsKHz, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);

    sizerRigControl->Add(sbSizer_hamlib,0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);
    
    wxStaticBoxSizer* sbSizer_freqList;
    wxStaticBox *sb_freqList = new wxStaticBox(m_rigControlTab, wxID_ANY, _("Predefined Frequencies"));
    sbSizer_freqList = new wxStaticBoxSizer(sb_freqList, wxVERTICAL);
    
    wxGridBagSizer* gridSizer = new wxGridBagSizer(5, 5);
    
    m_freqList = new wxListBox(sb_freqList, wxID_ANY, wxDefaultPosition, wxSize(350,150), 0, NULL, wxLB_SINGLE | wxLB_NEEDED_SB);
    gridSizer->Add(m_freqList, wxGBPosition(0, 0), wxGBSpan(5, 2), static_cast<int>(wxEXPAND));

    const int FREQ_LIST_BUTTON_WIDTH = 100; 
    const int FREQ_LIST_BUTTON_HEIGHT = -1;
    m_freqListAdd = new wxButton(sb_freqList, wxID_ANY, _("Add"), wxDefaultPosition, wxSize(FREQ_LIST_BUTTON_WIDTH,FREQ_LIST_BUTTON_HEIGHT), 0);
    gridSizer->Add(m_freqListAdd, wxGBPosition(0, 2), wxDefaultSpan, static_cast<int>(wxEXPAND));
    m_freqListRemove = new wxButton(sb_freqList, wxID_ANY, _("Remove"), wxDefaultPosition, wxSize(FREQ_LIST_BUTTON_WIDTH,FREQ_LIST_BUTTON_HEIGHT), 0);
    gridSizer->Add(m_freqListRemove, wxGBPosition(1, 2), wxDefaultSpan, static_cast<int>(wxEXPAND));
    m_freqListMoveUp = new wxButton(sb_freqList, wxID_ANY, _("Move Up"), wxDefaultPosition, wxSize(FREQ_LIST_BUTTON_WIDTH,FREQ_LIST_BUTTON_HEIGHT), 0);
    gridSizer->Add(m_freqListMoveUp, wxGBPosition(2, 2), wxDefaultSpan, static_cast<int>(wxEXPAND));
    m_freqListMoveDown = new wxButton(sb_freqList, wxID_ANY, _("Move Down"), wxDefaultPosition, wxSize(FREQ_LIST_BUTTON_WIDTH,FREQ_LIST_BUTTON_HEIGHT), 0);
    gridSizer->Add(m_freqListMoveDown, wxGBPosition(3, 2), wxDefaultSpan, static_cast<int>(wxEXPAND));
    
    if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
    {
        m_labelEnterFreq = new wxStaticText(sb_freqList, wxID_ANY, wxT("Enter frequency (kHz):"), wxDefaultPosition, wxDefaultSize, 0);
    }
    else
    {
        m_labelEnterFreq = new wxStaticText(sb_freqList, wxID_ANY, wxT("Enter frequency (MHz):"), wxDefaultPosition, wxDefaultSize, 0);
    }
    gridSizer->Add(m_labelEnterFreq, wxGBPosition(5, 0), wxDefaultSpan, wxALIGN_CENTER_VERTICAL);
    
    m_txtCtrlNewFrequency = new wxTextCtrl(sb_freqList, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    gridSizer->Add(m_txtCtrlNewFrequency, wxGBPosition(5, 1), wxGBSpan(1, 2), static_cast<int>(wxEXPAND));
    
    sbSizer_freqList->Add(gridSizer, 0, static_cast<int>(wxALL), 5);
    
    sizerRigControl->Add(sbSizer_freqList,0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);
    
    m_rigControlTab->SetSizer(sizerRigControl);
        
    // Modem tab
    wxBoxSizer* sizerModem = new wxBoxSizer(wxVERTICAL);
    
    //------------------------------
    // Operation
    //------------------------------

    wxStaticBox *sb_operation = new wxStaticBox(m_modemTab, wxID_ANY, _("Operation"));
    wxStaticBoxSizer* sbSizer_operation = new wxStaticBoxSizer(sb_operation, wxHORIZONTAL);

    m_ckboxAutoStartOnLaunch = new wxCheckBox(sb_operation, wxID_ANY, _("Start Automatically on Launch"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    sbSizer_operation->Add(m_ckboxAutoStartOnLaunch, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);

    m_ckHalfDuplex = new wxCheckBox(sb_operation, wxID_ANY, _("Half Duplex"), wxDefaultPosition, wxSize(-1,-1), 0);
    m_ckHalfDuplex->SetToolTip(_("Mutes the receiver while transmitting, so Glissando does not hear itself."));
    sbSizer_operation->Add(m_ckHalfDuplex, 0, static_cast<int>(wxALL) | wxALIGN_LEFT|wxALIGN_CENTER_VERTICAL, 5);

    sizerModem->Add(sbSizer_operation, 0, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 5);

    //------------------------------
    // Text chat
    //------------------------------
    wxStaticBox *sb_textChat = new wxStaticBox(m_modemTab, wxID_ANY, _("Text Chat"));
    wxStaticBoxSizer* sbSizer_textChat = new wxStaticBoxSizer(sb_textChat, wxVERTICAL);

    m_ckboxTextChatUsDataSegmentsOnly = new wxCheckBox(
        sb_textChat, wxID_ANY, _("Transmit only where US rules permit data (47 CFR 97.305)"),
        wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxTextChatUsDataSegmentsOnly->SetToolTip(
        _("Text chat is sent as data, and US rules permit data only in certain segments of "
          "the amateur bands, not the phone segments. "
          "While this is checked, text chat transmits only with the dial at least 3 kHz inside "
          "a US amateur data segment, and not at all while Glissando does not know the operating "
          "frequency; it still receives."));
    sbSizer_textChat->Add(m_ckboxTextChatUsDataSegmentsOnly, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);

    m_ckboxGlissandoChords = new wxCheckBox(
        sb_textChat, wxID_ANY, _("Open each Glissando transmission with a chord"),
        wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxGlissandoChords->SetToolTip(
        _("E4 and D5 together for 0.6 s, which other stations hear as the channel being "
          "taken long before a frame decodes."));
    sbSizer_textChat->Add(m_ckboxGlissandoChords, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);

    // The tail: how each transmission ends (docs/CW_TAIL.md).
    wxFlexGridSizer* tailSizer = new wxFlexGridSizer(2, 5, 5);
    tailSizer->AddGrowableCol(1);

    tailSizer->Add(new wxStaticText(sb_textChat, wxID_ANY, _("Tail:")), 0, wxALIGN_CENTER_VERTICAL);
    wxString tailChoices[] = {_("Off"), _("Chord"), _("CW, glorified"), _("CW, straight on E4+D5")};
    m_choiceGlissandoTail = new wxChoice(sb_textChat, wxID_ANY, wxDefaultPosition, wxDefaultSize, 4, tailChoices);
    m_choiceGlissandoTail->SetToolTip(
        _("How each Glissando transmission ends. Chord: every note of the scale for one bar. "
          "CW: the text below in Morse, so a listener knows what to search for; it also identifies "
          "the station. Glorified sings each dit and dah on a note of the scale; straight keys them on "
          "E4 and D5 together, which a CW decoder tuned to either note can copy. The CW tail plays at most once "
          "every so many minutes, and the chord ends the transmissions in between."));
    tailSizer->Add(m_choiceGlissandoTail, 0, wxALIGN_LEFT);

    tailSizer->Add(new wxStaticText(sb_textChat, wxID_ANY, _("CW text:")), 0, wxALIGN_CENTER_VERTICAL);
    m_txtGlissandoCwText = new wxTextCtrl(sb_textChat, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(260, -1));
    m_txtGlissandoCwText->SetToolTip(_("<MYCALL> is replaced by the callsign on the Station tab."));
    tailSizer->Add(m_txtGlissandoCwText, 1, static_cast<int>(wxEXPAND));

    tailSizer->Add(new wxStaticText(sb_textChat, wxID_ANY, _("CW speed:")), 0, wxALIGN_CENTER_VERTICAL);
    wxBoxSizer* speedSizer = new wxBoxSizer(wxHORIZONTAL);
    m_spinGlissandoCwWpm = new wxSpinCtrl(sb_textChat, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                          wxSP_ARROW_KEYS, Glissando::CW_MIN_WPM, Glissando::CW_MAX_WPM,
                                          Glissando::CW_DEFAULT_WPM);
    m_spinGlissandoCwWpm->SetToolTip(
        _("US rules (47 CFR 97.119) allow an automatic CW identification at up to 20 WPM."));
    speedSizer->Add(m_spinGlissandoCwWpm, 0, wxALIGN_CENTER_VERTICAL);
    speedSizer->Add(new wxStaticText(sb_textChat, wxID_ANY, _("WPM, at most once every")), 0,
                    static_cast<int>(wxLEFT) | static_cast<int>(wxRIGHT) | wxALIGN_CENTER_VERTICAL, 5);
    m_spinGlissandoCwIdMinutes = new wxSpinCtrl(sb_textChat, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                                wxSize(80, -1), wxSP_ARROW_KEYS, 0, 60, 10);
    m_spinGlissandoCwIdMinutes->SetToolTip(
        _("Ten minutes is the US station identification interval. 0 plays the CW tail on every transmission."));
    speedSizer->Add(m_spinGlissandoCwIdMinutes, 0, wxALIGN_CENTER_VERTICAL);
    speedSizer->Add(new wxStaticText(sb_textChat, wxID_ANY, _("minutes")), 0,
                    static_cast<int>(wxLEFT) | wxALIGN_CENTER_VERTICAL, 5);
    tailSizer->Add(speedSizer, 0, wxALIGN_LEFT);

    tailSizer->AddSpacer(0);
    m_textGlissandoCwTail = new wxStaticText(sb_textChat, wxID_ANY, wxEmptyString);
    tailSizer->Add(m_textGlissandoCwTail, 0, wxALIGN_LEFT);

    sbSizer_textChat->Add(tailSizer, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);

    m_ckboxGlissandoTransmitShips = new wxCheckBox(
        sb_textChat, wxID_ANY, _("Draw rocket ships and invaders on the visi-scope while sending"),
        wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxGlissandoTransmitShips->SetToolTip(
        _("The notes being sent, printed into the waterfall as ships in their columns: "
          "rockets for the pentatonic scale, invaders for the tritone scales."));
    sbSizer_textChat->Add(m_ckboxGlissandoTransmitShips, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);

    // No tooltips, on purpose: it's an easter egg.
    wxBoxSizer* smokeSizer = new wxBoxSizer(wxHORIZONTAL);
    m_ckboxGlissandoSmoke = new wxCheckBox(
        sb_textChat, wxID_ANY, _("Smoke"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    smokeSizer->Add(m_ckboxGlissandoSmoke, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    m_txtGlissandoSmokeSeconds = new wxTextCtrl(sb_textChat, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                                wxSize(60, -1), 0, wxTextValidator(wxFILTER_DIGITS));
    smokeSizer->Add(m_txtGlissandoSmokeSeconds, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    smokeSizer->Add(new wxStaticText(sb_textChat, wxID_ANY, _("seconds")), 0, wxALIGN_CENTER_VERTICAL);
    sbSizer_textChat->Add(smokeSizer, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);

    // Data2G: an external modem program the operator runs; chat reaches it
    // over TCP (docs/DATA2G.md).
    m_ckboxData2G = new wxCheckBox(
        sb_textChat, wxID_ANY, _("Send chat through Data2G (a data2g-host you run separately)"),
        wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxData2G->SetToolTip(
        _("Chat goes out through data2g-host instead of Glissando. Start data2g-host yourself, "
          "with its own sound card and rigctld PTT settings; this program only connects to it, "
          "and does not key the radio for chat while this is checked."));
    sbSizer_textChat->Add(m_ckboxData2G, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);

    wxBoxSizer* data2gSizer = new wxBoxSizer(wxHORIZONTAL);
    data2gSizer->Add(new wxStaticText(sb_textChat, wxID_ANY, _("Host:")), 0,
                     static_cast<int>(wxLEFT) | wxALIGN_CENTER_VERTICAL, 25);
    m_txtData2GHost = new wxTextCtrl(sb_textChat, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(140, -1));
    data2gSizer->Add(m_txtData2GHost, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    data2gSizer->Add(new wxStaticText(sb_textChat, wxID_ANY, _("KISS port:")), 0, wxALIGN_CENTER_VERTICAL, 0);
    m_txtData2GKissPort = new wxTextCtrl(sb_textChat, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                         wxSize(70, -1), 0, wxTextValidator(wxFILTER_DIGITS));
    data2gSizer->Add(m_txtData2GKissPort, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    data2gSizer->Add(new wxStaticText(sb_textChat, wxID_ANY, _("Command port:")), 0,
                     static_cast<int>(wxLEFT) | wxALIGN_CENTER_VERTICAL, 10);
    m_txtData2GCommandPort = new wxTextCtrl(sb_textChat, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                            wxSize(70, -1), 0, wxTextValidator(wxFILTER_DIGITS));
    m_txtData2GCommandPort->SetToolTip(
        _("Chat opens the GLISS broadcast group here, picks each tempo's Data2G mode, and reads "
          "data2g-host's PTT and BUSY reports. Sessions use the next port up for their data. "
          "data2g-host serves one command client at a time."));
    data2gSizer->Add(m_txtData2GCommandPort, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    sbSizer_textChat->Add(data2gSizer, 0, wxALIGN_LEFT, 0);

    m_ckboxData2GSessions = new wxCheckBox(
        sb_textChat, wxID_ANY, _("Connect a session for messages to one station"), wxDefaultPosition,
        wxDefaultSize, wxCHK_2STATE);
    m_ckboxData2GSessions->SetToolTip(
        _("A message to a station picked in the call roster goes through a connected Data2G session, "
          "which negotiates its own speed and retries. When that station does not answer, the "
          "message goes to the GLISS group instead. Unchecked, everything goes to the group."));
    sbSizer_textChat->Add(m_ckboxData2GSessions, 0, static_cast<int>(wxLEFT) | wxALIGN_LEFT, 25);

    m_ckboxData2G->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { updateData2GControls_(); });
    m_choiceGlissandoTail->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { updateCwTailControls_(); });
    m_txtGlissandoCwText->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { updateCwTailControls_(); });
    m_spinGlissandoCwWpm->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { updateCwTailControls_(); });
    m_txt_callsign->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) { updateCwTailControls_(); event.Skip(); });

    sizerModem->Add(sbSizer_textChat, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);
    
    m_modemTab->SetSizer(sizerModem);

    // UI Options tab: how the console looks.
    wxBoxSizer* sizerUi = new wxBoxSizer(wxVERTICAL);
    wxStaticBox* sb_uiConsole = new wxStaticBox(m_uiTab, wxID_ANY, _("Console"));
    wxStaticBoxSizer* sbSizer_uiConsole = new wxStaticBoxSizer(sb_uiConsole, wxVERTICAL);

    m_ckboxGlissandoShowMarquee = new wxCheckBox(
        sb_uiConsole, wxID_ANY, _("Show the GLISSANDO title above the visi-scope"),
        wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxGlissandoShowMarquee->SetToolTip(
        _("Turn off on a small screen: the console gets shorter by the title's height."));
    sbSizer_uiConsole->Add(m_ckboxGlissandoShowMarquee, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);
    sizerUi->Add(sbSizer_uiConsole, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);

    wxStaticBox* sb_mapBall = new wxStaticBox(m_uiTab, wxID_ANY, _("Map Ball"));
    wxStaticBoxSizer* sbSizer_mapBall = new wxStaticBoxSizer(sb_mapBall, wxVERTICAL);

    m_ckboxGlissandoMapBorders = new wxCheckBox(
        sb_mapBall, wxID_ANY, _("Country borders"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxGlissandoMapBorders->SetToolTip(
        _("Faint lines between countries on the map ball, smoothed to a few clean strokes."));
    sbSizer_mapBall->Add(m_ckboxGlissandoMapBorders, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);

    m_ckboxGlissandoMapBrushedMetal = new wxCheckBox(
        sb_mapBall, wxID_ANY, _("Brushed metal"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    m_ckboxGlissandoMapBrushedMetal->SetToolTip(
        _("The land on the map ball as metal brushed along the lines of latitude, catching the light "
          "as it turns. Turn off on a slow computer: it takes the most drawing."));
    sbSizer_mapBall->Add(m_ckboxGlissandoMapBrushedMetal, 0, static_cast<int>(wxALL) | wxALIGN_LEFT, 5);

    sizerUi->Add(sbSizer_mapBall, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);
    m_uiTab->SetSizer(sizerUi);
    
    // Debug tab
    wxBoxSizer* sizerDebug = new wxBoxSizer(wxVERTICAL);
    
#ifdef __WXMSW__
    //------------------------------
    // debug console, for WIndows build make console pop up for debug messages
    //------------------------------

    wxStaticBoxSizer* sbSizer_console;
    wxStaticBox *sb_console = new wxStaticBox(m_debugTab, wxID_ANY, _("Debug: Windows"));
    sbSizer_console = new wxStaticBoxSizer(sb_console, wxHORIZONTAL);

    m_ckboxDebugConsole = new wxCheckBox(sb_console, wxID_ANY, _("Show Console"), wxDefaultPosition, wxDefaultSize, wxCHK_2STATE);
    sbSizer_console->Add(m_ckboxDebugConsole, 0, wxALIGN_LEFT, 5);

    sizerDebug->Add(sbSizer_console,0, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 5);

#endif // __WXMSW__
    
    //----------------------------------------------------------
    // FIFO and under/overflow counters used for debug
    //----------------------------------------------------------

    wxStaticBox* sb_fifo = new wxStaticBox(m_debugTab, wxID_ANY, _("Debug: FIFO and Under/Over Flow Counters"));
    wxStaticBoxSizer* sbSizer_fifo = new wxStaticBoxSizer(sb_fifo, wxVERTICAL);

    wxBoxSizer* sbSizer_fifo1 = new wxBoxSizer(wxHORIZONTAL);

    // FIFO size in ms

    wxStaticText *m_staticTextFifo1 = new wxStaticText(sb_fifo, wxID_ANY, _("Fifo Size (ms):"), wxDefaultPosition, wxDefaultSize, 0);
    sbSizer_fifo1->Add(m_staticTextFifo1, 0, static_cast<int>(wxALL) | wxALIGN_CENTER_VERTICAL, 5);
    m_txtCtrlFifoSize = new wxTextCtrl(sb_fifo, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80,-1), 0);
    sbSizer_fifo1->Add(m_txtCtrlFifoSize, 0, static_cast<int>(wxALL), 5);

    // Reset stats button
    
    m_BtnFifoReset = new wxButton(sb_fifo, wxID_ANY, _("Reset"), wxDefaultPosition, wxDefaultSize, 0);
    sbSizer_fifo1->Add(m_BtnFifoReset, 0,  wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 5);
    sbSizer_fifo->Add(sbSizer_fifo1);

    // text lines with fifo counters
    
    m_textPA1 = new wxStaticText(sb_fifo, wxID_ANY, wxT(""), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    sbSizer_fifo->Add(m_textPA1, 0, wxALIGN_LEFT, 1);
    m_textPA2 = new wxStaticText(sb_fifo, wxID_ANY, wxT(""), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    sbSizer_fifo->Add(m_textPA2, 0, wxALIGN_LEFT, 1);

    m_textFifos = new wxStaticText(sb_fifo, wxID_ANY, wxT(""), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    sbSizer_fifo->Add(m_textFifos, 0, wxALIGN_LEFT, 1);

    sizerDebug->Add(sbSizer_fifo,0, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 3);

    m_debugTab->SetSizer(sizerDebug);

    //------------------------------
    // OK - Cancel - Apply Buttons 
    //------------------------------

    wxBoxSizer* bSizer31 = new wxBoxSizer(wxHORIZONTAL);

    m_sdbSizer5OK = new wxButton(panel, wxID_OK);
    bSizer31->Add(m_sdbSizer5OK, 0, static_cast<int>(wxALL), 2);

    m_sdbSizer5Cancel = new wxButton(panel, wxID_CANCEL);
    bSizer31->Add(m_sdbSizer5Cancel, 0, static_cast<int>(wxALL), 2);

    m_sdbSizer5Apply = new wxButton(panel, wxID_APPLY);
    bSizer31->Add(m_sdbSizer5Apply, 0, static_cast<int>(wxALL), 2);

    bSizer30->Add(bSizer31, 0, static_cast<int>(wxALL) | wxALIGN_CENTER, 5);

    panel->SetSizer(bSizer30);
    
    wxBoxSizer* winSizer = new wxBoxSizer(wxVERTICAL);
    winSizer->Add(panel, 0, static_cast<int>(wxEXPAND));
    
    this->SetSizerAndFit(winSizer);
    this->Layout();
    this->Centre(wxBOTH);

    
    // Connect Events -------------------------------------------------------

    this->Connect(wxEVT_INIT_DIALOG, wxInitDialogEventHandler(OptionsDlg::OnInitDialog));

    m_sdbSizer5OK->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnOK), NULL, this);
    m_sdbSizer5Cancel->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnCancel), NULL, this);
    m_sdbSizer5Apply->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnApply), NULL, this);



#ifdef __WXMSW__
    m_ckboxDebugConsole->Connect(wxEVT_COMMAND_CHECKBOX_CLICKED, wxScrollEventHandler(OptionsDlg::OnDebugConsole), NULL, this);
#endif



    m_buttonChooseCsvLogFilePath->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnChooseCsvLogFilePath), NULL, this);

    m_BtnFifoReset->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnFifoReset), NULL, this);

    
    

    m_ckboxEnableSpacebarForPTT->Connect(wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler(OptionsDlg::OnEnableSpacebarForPTT), NULL, this);
    m_btnSetPTTKey->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnSetPTTKey), NULL, this);
    m_txtPTTKeyName->Bind(wxEVT_KEY_DOWN, &OptionsDlg::OnPTTKeyCapture, this);
    m_txtPTTKeyName->Bind(wxEVT_CHAR, &OptionsDlg::OnPTTKeyCapture, this);
    this->Bind(wxEVT_CHAR_HOOK, &OptionsDlg::OnDialogCharHook, this);
    
    m_freqList->Connect(wxEVT_LISTBOX, wxCommandEventHandler(OptionsDlg::OnReportingFreqSelectionChange), NULL, this);
    m_txtCtrlNewFrequency->Connect(wxEVT_TEXT, wxCommandEventHandler(OptionsDlg::OnReportingFreqTextChange), NULL, this);
    m_freqListAdd->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnReportingFreqAdd), NULL, this);
    m_freqListRemove->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnReportingFreqRemove), NULL, this);
    m_freqListMoveUp->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnReportingFreqMoveUp), NULL, this);
    m_freqListMoveDown->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnReportingFreqMoveDown), NULL, this);
    
    event_in_serial = 0;
    event_out_serial = 0;
}

//-------------------------------------------------------------------------
// ~OptionsDlg()
//-------------------------------------------------------------------------
OptionsDlg::~OptionsDlg()
{

    // Disconnect Events

    this->Disconnect(wxEVT_INIT_DIALOG, wxInitDialogEventHandler(OptionsDlg::OnInitDialog));

    m_sdbSizer5OK->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnOK), NULL, this);
    m_sdbSizer5Cancel->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnCancel), NULL, this);
    m_sdbSizer5Apply->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnApply), NULL, this);


    m_buttonChooseCsvLogFilePath->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnChooseCsvLogFilePath), NULL, this);

    m_BtnFifoReset->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnFifoReset), NULL, this);

#ifdef __WXMSW__
    m_ckboxDebugConsole->Disconnect(wxEVT_COMMAND_CHECKBOX_CLICKED, wxScrollEventHandler(OptionsDlg::OnDebugConsole), NULL, this);
#endif
    
    
    

    m_ckboxEnableSpacebarForPTT->Disconnect(wxEVT_COMMAND_CHECKBOX_CLICKED, wxCommandEventHandler(OptionsDlg::OnEnableSpacebarForPTT), NULL, this);
    m_btnSetPTTKey->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnSetPTTKey), NULL, this);
    m_txtPTTKeyName->Unbind(wxEVT_KEY_DOWN, &OptionsDlg::OnPTTKeyCapture, this);
    m_txtPTTKeyName->Unbind(wxEVT_CHAR, &OptionsDlg::OnPTTKeyCapture, this);
    this->Unbind(wxEVT_CHAR_HOOK, &OptionsDlg::OnDialogCharHook, this);
    
    m_freqList->Disconnect(wxEVT_LISTBOX, wxCommandEventHandler(OptionsDlg::OnReportingFreqSelectionChange), NULL, this);
    m_txtCtrlNewFrequency->Disconnect(wxEVT_TEXT, wxCommandEventHandler(OptionsDlg::OnReportingFreqTextChange), NULL, this);
    m_freqListAdd->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnReportingFreqAdd), NULL, this);
    m_freqListRemove->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnReportingFreqRemove), NULL, this);
    m_freqListMoveUp->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnReportingFreqMoveUp), NULL, this);
    m_freqListMoveDown->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(OptionsDlg::OnReportingFreqMoveDown), NULL, this);
}

//-------------------------------------------------------------------------
// ExchangeData()
//-------------------------------------------------------------------------
void OptionsDlg::ExchangeData(int inout, bool storePersistent)
{
    if(inout == EXCHANGE_DATA_IN)
    {
        // Populate reporting frequency list.
        for (auto& item : wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyList.get())
        {
            m_freqList->Append(item);
        }
        
        m_ckboxEnableSpacebarForPTT->SetValue(wxGetApp().appConfiguration.enableSpaceBarForPTT);
        m_ckboxPTTMomentaryMode->SetValue(wxGetApp().appConfiguration.pttMomentaryMode);
        m_selectedPTTKeyCode = wxGetApp().appConfiguration.pttKeyCode;
        m_capturingPTTKey = false;
        m_txtPTTKeyName->SetValue(getPTTKeyName(m_selectedPTTKeyCode));
        m_txtPTTKeyName->SetEditable(false);
        bool pttEnabled = wxGetApp().appConfiguration.enableSpaceBarForPTT;
        m_txtPTTKeyName->Enable(pttEnabled);
        m_btnSetPTTKey->Enable(pttEnabled);
        m_txtTxRxDelayMilliseconds->SetValue(wxString::Format("%d", wxGetApp().appConfiguration.txRxDelayMilliseconds.get()));

        m_ckboxTOTTimerEnabled->SetValue(wxGetApp().appConfiguration.rigControlConfiguration.totTimerEnabled);
        m_txtTOTTimerSecs->SetValue(wxString::Format("%d", wxGetApp().appConfiguration.rigControlConfiguration.totTimerSecs.get()));
        m_txtTOTTimerSecs->Enable(wxGetApp().appConfiguration.rigControlConfiguration.totTimerEnabled);

        m_ckboxSwrMeter->SetValue(wxGetApp().appConfiguration.rigControlConfiguration.swrMeter);
        m_ckboxSwrAutoAbort->SetValue(wxGetApp().appConfiguration.rigControlConfiguration.swrAutoAbort);
        m_ckboxSwrAutoAbort->Enable(wxGetApp().appConfiguration.rigControlConfiguration.swrMeter);

        // A FreeDV-era "frequency and mode changes" setting now means
        // frequency changes: the mode is never touched.
        bool frequencyControl =
            wxGetApp().appConfiguration.rigControlConfiguration.hamlibEnableFreqModeChanges ||
            wxGetApp().appConfiguration.rigControlConfiguration.hamlibEnableFreqChangesOnly;
        m_rbFrequencyControl->SetValue(frequencyControl);
        m_rbNoFrequencyControl->SetValue(!frequencyControl);
        m_ckboxFrequencyEntryAsKHz->SetValue(wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz);
        
        m_ckHalfDuplex->SetValue(wxGetApp().appConfiguration.halfDuplexMode);

        m_ckboxTextChatUsDataSegmentsOnly->SetValue(wxGetApp().appConfiguration.textChatUsDataSegmentsOnly);
        m_ckboxGlissandoChords->SetValue(wxGetApp().appConfiguration.glissandoChords);
        m_choiceGlissandoTail->SetSelection(std::min(std::max(wxGetApp().appConfiguration.glissandoTail.get(), 0), 3));
        m_txtGlissandoCwText->ChangeValue(wxGetApp().appConfiguration.glissandoCwText);
        m_spinGlissandoCwWpm->SetValue(wxGetApp().appConfiguration.glissandoCwWpm);
        m_spinGlissandoCwIdMinutes->SetValue(wxGetApp().appConfiguration.glissandoCwIdMinutes);
        m_ckboxGlissandoTransmitShips->SetValue(wxGetApp().appConfiguration.glissandoTransmitShips);
        m_ckboxGlissandoShowMarquee->SetValue(wxGetApp().appConfiguration.glissandoShowMarquee);
        m_ckboxGlissandoSmoke->SetValue(wxGetApp().appConfiguration.glissandoSmoke);
        m_ckboxGlissandoMapBorders->SetValue(wxGetApp().appConfiguration.glissandoMapBorders);
        m_ckboxGlissandoMapBrushedMetal->SetValue(wxGetApp().appConfiguration.glissandoMapBrushedMetal);
        m_txtGlissandoSmokeSeconds->SetValue(
            wxString::Format("%d", wxGetApp().appConfiguration.glissandoSmokeSeconds.get()));
        m_ckboxData2G->SetValue(wxGetApp().appConfiguration.data2gEnabled);
        m_txtData2GHost->SetValue(wxGetApp().appConfiguration.data2gHost);
        m_txtData2GKissPort->SetValue(wxString::Format("%d", wxGetApp().appConfiguration.data2gKissPort.get()));
        m_txtData2GCommandPort->SetValue(wxString::Format("%d", wxGetApp().appConfiguration.data2gCommandPort.get()));
        m_ckboxData2GSessions->SetValue(wxGetApp().appConfiguration.data2gSessions);
        updateData2GControls_();
        updateCwTailControls_();
        




        m_txtCtrlFifoSize->SetValue(wxString::Format(wxT("%i"),wxGetApp().appConfiguration.fifoSizeMs.get()));


        m_ckboxAutoStartOnLaunch->SetValue(wxGetApp().appConfiguration.autoStartOnLaunch);

#ifdef __WXMSW__
        m_ckboxDebugConsole->SetValue(wxGetApp().appConfiguration.debugConsoleEnabled);
#endif
        
        m_txt_callsign->SetValue(wxGetApp().appConfiguration.reportingConfiguration.reportingCallsign);
        m_txtGridSquare->SetValue(wxGetApp().appConfiguration.reportingConfiguration.reportingGridSquare);
        m_ckboxSendGridSquare->SetValue(wxGetApp().appConfiguration.reportingConfiguration.reportingSendGridSquare);

        // CSV log file path
        m_txtCtrlCsvLogFilePath->SetValue(wxGetApp().appConfiguration.reportingConfiguration.csvLogFilePath);

        // Stats reset time
        
        if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
        {
            m_labelEnterFreq->SetLabel(wxT("Enter frequency (kHz):"));
        }
        else
        {
            m_labelEnterFreq->SetLabel(wxT("Enter frequency (MHz):"));
        }
        
        // Update control state based on checkbox state.
        updateReportingState();
        updateRigControlState();

        wxCommandEvent tmpEvent;
        OnReportingFreqTextChange(tmpEvent);
    }

    if(inout == EXCHANGE_DATA_OUT)
    {
        // Save new reporting frequency list.
        std::vector<wxString> tmpList;
        tmpList.reserve(m_freqList->GetCount());
        for (unsigned int index = 0; index < m_freqList->GetCount(); index++)
        {
            tmpList.push_back(m_freqList->GetString(index));
        }
        wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyList = tmpList;
        
        wxGetApp().appConfiguration.enableSpaceBarForPTT = m_ckboxEnableSpacebarForPTT->GetValue();
        wxGetApp().appConfiguration.pttKeyCode = m_selectedPTTKeyCode;
        wxGetApp().appConfiguration.pttMomentaryMode = m_ckboxPTTMomentaryMode->GetValue();

        wxGetApp().appConfiguration.txRxDelayMilliseconds = wxAtoi(m_txtTxRxDelayMilliseconds->GetValue());

        wxGetApp().appConfiguration.rigControlConfiguration.totTimerEnabled = m_ckboxTOTTimerEnabled->GetValue();
        {
            long totSecs;
            m_txtTOTTimerSecs->GetValue().ToLong(&totSecs);
            if (totSecs < 1) totSecs = 1;
            wxGetApp().appConfiguration.rigControlConfiguration.totTimerSecs = (int)totSecs;
        }

        wxGetApp().appConfiguration.rigControlConfiguration.swrMeter = m_ckboxSwrMeter->GetValue();
        wxGetApp().appConfiguration.rigControlConfiguration.swrAutoAbort = m_ckboxSwrAutoAbort->GetValue();

        wxGetApp().appConfiguration.rigControlConfiguration.hamlibEnableFreqModeChanges = false;
        wxGetApp().appConfiguration.rigControlConfiguration.hamlibEnableFreqChangesOnly = m_rbFrequencyControl->GetValue();
        
        wxGetApp().appConfiguration.halfDuplexMode = m_ckHalfDuplex->GetValue();
        wxGetApp().appConfiguration.textChatUsDataSegmentsOnly = m_ckboxTextChatUsDataSegmentsOnly->GetValue();
        wxGetApp().appConfiguration.glissandoChords = m_ckboxGlissandoChords->GetValue();
        wxGetApp().appConfiguration.glissandoTail = m_choiceGlissandoTail->GetSelection();
        wxGetApp().appConfiguration.glissandoCwText = m_txtGlissandoCwText->GetValue();
        wxGetApp().appConfiguration.glissandoCwWpm = m_spinGlissandoCwWpm->GetValue();
        wxGetApp().appConfiguration.glissandoCwIdMinutes = m_spinGlissandoCwIdMinutes->GetValue();
        wxGetApp().appConfiguration.glissandoTransmitShips = m_ckboxGlissandoTransmitShips->GetValue();
        wxGetApp().appConfiguration.glissandoShowMarquee = m_ckboxGlissandoShowMarquee->GetValue();
        wxGetApp().appConfiguration.glissandoSmoke = m_ckboxGlissandoSmoke->GetValue();
        wxGetApp().appConfiguration.glissandoMapBorders = m_ckboxGlissandoMapBorders->GetValue();
        wxGetApp().appConfiguration.glissandoMapBrushedMetal = m_ckboxGlissandoMapBrushedMetal->GetValue();
        long smokeSeconds = 0;
        if (m_txtGlissandoSmokeSeconds->GetValue().ToLong(&smokeSeconds))
            wxGetApp().appConfiguration.glissandoSmokeSeconds = (int)std::min(smokeSeconds, 100000L);
        wxGetApp().appConfiguration.data2gEnabled = m_ckboxData2G->GetValue();
        wxString data2gHost = m_txtData2GHost->GetValue().Strip(wxString::both);
        wxGetApp().appConfiguration.data2gHost = data2gHost.IsEmpty() ? wxString("127.0.0.1") : data2gHost;
        auto port = [](wxTextCtrl* box, int fallback) {
            int value = wxAtoi(box->GetValue());
            return value > 0 && value < 65536 ? value : fallback;
        };
        wxGetApp().appConfiguration.data2gKissPort = port(m_txtData2GKissPort, 8100);
        wxGetApp().appConfiguration.data2gCommandPort = port(m_txtData2GCommandPort, 8300);
        wxGetApp().appConfiguration.data2gSessions = m_ckboxData2GSessions->GetValue();
        
        long FifoSize_ms;
        m_txtCtrlFifoSize->GetValue().ToLong(&FifoSize_ms);
        wxGetApp().appConfiguration.fifoSizeMs = (int)FifoSize_ms;


        wxGetApp().appConfiguration.autoStartOnLaunch = m_ckboxAutoStartOnLaunch->GetValue();

#ifdef __WXMSW__
        wxGetApp().appConfiguration.debugConsoleEnabled = m_ckboxDebugConsole->GetValue();
#endif

        wxGetApp().appConfiguration.reportingConfiguration.reportingCallsign = m_txt_callsign->GetValue();
        wxGetApp().appConfiguration.reportingConfiguration.reportingGridSquare =
            m_txtGridSquare->GetValue().Strip(wxString::both);
        wxGetApp().appConfiguration.reportingConfiguration.reportingSendGridSquare = m_ckboxSendGridSquare->GetValue();

        // CSV log file path
        wxGetApp().appConfiguration.reportingConfiguration.csvLogFilePath = m_txtCtrlCsvLogFilePath->GetValue();

        if (storePersistent) {
            wxGetApp().appConfiguration.save(pConfig);
            
            // Save reporting frequency units last due to how the frequency list is stored.
            // Then reload the configuration to ensure that the displayed frequencies are correct.
            wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz = m_ckboxFrequencyEntryAsKHz->GetValue();
            wxGetApp().appConfiguration.save(pConfig);
            wxGetApp().appConfiguration.load(pConfig);
        }
    }
}

//-------------------------------------------------------------------------
// OnOK()
//-------------------------------------------------------------------------
void OptionsDlg::OnOK(wxCommandEvent&)
{
    ExchangeData(EXCHANGE_DATA_OUT, true);
    //this->EndModal(wxID_OK);
    EndModal(wxOK);
    
    // Clear frequency list to prevent sizing issues on re-display.
    // EXCHANGE_DATA_IN will repopulate then.
    m_freqList->Clear();
}

//-------------------------------------------------------------------------
// OnCancel()
//-------------------------------------------------------------------------
void OptionsDlg::OnCancel(wxCommandEvent&)
{
    //this->EndModal(wxID_CANCEL);
    EndModal(wxCANCEL);
    
    // Clear frequency list to prevent sizing issues on re-display.
    // EXCHANGE_DATA_IN will repopulate then.
    m_freqList->Clear();
}

//-------------------------------------------------------------------------
// OnApply()
//-------------------------------------------------------------------------
void OptionsDlg::OnApply(wxCommandEvent&)
{
    bool oldFreqAsKHz = wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz;
    
    ExchangeData(EXCHANGE_DATA_OUT, true);
    
    bool khzChanged = 
        wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz != oldFreqAsKHz;
    
    // If there's something in the frequency entry textbox (i.e. if the user pushes Apply),
    // convert to the correct units.
    auto freqString = m_txtCtrlNewFrequency->GetValue();
    if (freqString.Length() > 0 && khzChanged)
    {
        double freqDouble = 0;
        wxNumberFormatter::FromString(freqString, &freqDouble);
        
        if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
        {
            freqDouble *= 1000;
            m_txtCtrlNewFrequency->SetValue(wxNumberFormatter::ToString(freqDouble, 1));
        }
        else
        {
            freqDouble /= 1000.0;
            m_txtCtrlNewFrequency->SetValue(wxNumberFormatter::ToString(freqDouble, 1));
        }
    }
        
    // Reload saved data to ensure the frequency list is properly displayed.
    m_freqList->Clear();
    ExchangeData(EXCHANGE_DATA_IN, false);
}

//-------------------------------------------------------------------------
// OnInitDialog()
//-------------------------------------------------------------------------
void OptionsDlg::OnInitDialog(wxInitDialogEvent&)
{
    ExchangeData(EXCHANGE_DATA_IN, false);
}

// immediately change flags rather using ExchangeData() so we can switch on and off at run time

void OptionsDlg::OnChooseCsvLogFilePath(wxCommandEvent&) {
    wxFileDialog fileDialog(
        this,
        wxT("Choose CSV log file location"),
        wxPathOnly(m_txtCtrlCsvLogFilePath->GetValue()),
        wxFileNameFromPath(m_txtCtrlCsvLogFilePath->GetValue()),
        wxT("CSV files (*.csv)|*.csv|All files (*.*)|*.*"),
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (fileDialog.ShowModal() == wxID_CANCEL) {
        return;
    }

    m_txtCtrlCsvLogFilePath->SetValue(fileDialog.GetPath());
}

void OptionsDlg::OnDebugConsole(wxScrollEvent&) {
    wxGetApp().appConfiguration.debugConsoleEnabled = m_ckboxDebugConsole->GetValue();
#ifdef __WXMSW__
    // somewhere to send printfs while developing, causes conmsole to pop up on Windows
    if (wxGetApp().appConfiguration.debugConsoleEnabled) {
        int ret = AllocConsole();
        freopen("CONOUT$", "w", stdout); 
        freopen("CONOUT$", "w", stderr); 
        log_info("AllocConsole: %d m_debug_console: %d", ret, wxGetApp().appConfiguration.debugConsoleEnabled.get());
    } 
#endif
}

void OptionsDlg::OnFifoReset(wxCommandEvent&)
{
    g_infifo1_full.store(0, std::memory_order_relaxed);
    g_outfifo1_empty.store(0, std::memory_order_relaxed);
    g_infifo2_full.store(0, std::memory_order_relaxed);
    g_outfifo2_empty.store(0, std::memory_order_relaxed);
    for (int i=0; i<4; i++) {
        g_AEstatus1[i] = g_AEstatus2[i] = 0;
    }
}

void OptionsDlg::updateReportingState()
{
    // The callsign cannot be changed during a session.
    m_txt_callsign->Enable(!sessionActive_);
}

void OptionsDlg::updateRigControlState()
{
    if (!sessionActive_)
    {
        m_rbFrequencyControl->Enable(true);
        m_rbNoFrequencyControl->Enable(true);
        m_ckboxEnableSpacebarForPTT->Enable(true);
        m_txtTxRxDelayMilliseconds->Enable(true);
    }
    else
    {
        // Rig control settings cannot be updated during a session.
        m_rbFrequencyControl->Enable(false);
        m_rbNoFrequencyControl->Enable(false);
        m_ckboxEnableSpacebarForPTT->Enable(false);
        m_txtTxRxDelayMilliseconds->Enable(false);
    }
}
    
void OptionsDlg::OnEnableSpacebarForPTT(wxCommandEvent&)
{
    bool enabled = m_ckboxEnableSpacebarForPTT->GetValue();
    m_txtPTTKeyName->Enable(enabled);
    m_btnSetPTTKey->Enable(enabled);
    if (!enabled)
        exitPTTCaptureMode_(false);
}

void OptionsDlg::OnTOTTimerEnable(wxCommandEvent&)
{
    m_txtTOTTimerSecs->Enable(m_ckboxTOTTimerEnabled->GetValue());
}

void OptionsDlg::OnSwrMeterEnable(wxCommandEvent&)
{
    // The abort works from the readings the meter asks for.
    m_ckboxSwrAutoAbort->Enable(m_ckboxSwrMeter->GetValue());
}

void OptionsDlg::OnSetPTTKey(wxCommandEvent&)
{
    if (m_capturingPTTKey)
        exitPTTCaptureMode_(false);
    else
        enterPTTCaptureMode_();
}

void OptionsDlg::OnDialogCharHook(wxKeyEvent& event)
{
    // wxEVT_CHAR_HOOK reaches the dialog before wxEVT_KEY_DOWN reaches any child
    // control, so this is the only place to intercept Escape while in capture mode
    // — otherwise wxDialog's built-in handler closes the dialog first.
    if (m_capturingPTTKey && event.GetKeyCode() == WXK_ESCAPE)
    {
        exitPTTCaptureMode_(false);
        return; // consume — do not let the dialog treat Escape as Cancel
    }
    event.Skip();
}

void OptionsDlg::OnPTTKeyCapture(wxKeyEvent& event)
{
    if (!m_capturingPTTKey) { event.Skip(); return; }

    int keyCode = event.GetKeyCode();
    // Normalize lowercase letters to match wxEVT_KEY_DOWN uppercase convention.
    if (keyCode >= 'a' && keyCode <= 'z')
        keyCode -= ('a' - 'A');

    if (keyCode == WXK_TAB)
    {
        exitPTTCaptureMode_(false);
        event.Skip();
        return;
    }
    // Ignore bare modifier keys.
    if (keyCode == WXK_SHIFT || keyCode == WXK_CONTROL || keyCode == WXK_ALT ||
        keyCode == WXK_CAPITAL || keyCode == WXK_NUMLOCK || keyCode == WXK_SCROLL ||
        keyCode == WXK_NONE || keyCode == WXK_WINDOWS_LEFT || keyCode == WXK_WINDOWS_RIGHT ||
        keyCode == WXK_WINDOWS_MENU || keyCode == WXK_COMMAND)
    {
        return;
    }
    exitPTTCaptureMode_(true, keyCode);
    // Don't Skip() — prevents the key from typing into the text field.
}

void OptionsDlg::enterPTTCaptureMode_()
{
    m_capturingPTTKey = true;
    m_txtPTTKeyName->SetEditable(true);
    m_txtPTTKeyName->SetValue(_("Press any key..."));
    m_txtPTTKeyName->SetFocus();
    m_btnSetPTTKey->SetLabel(_("Cancel"));
}

void OptionsDlg::exitPTTCaptureMode_(bool accept, int keyCode)
{
    m_capturingPTTKey = false;
    if (accept)
        m_selectedPTTKeyCode = keyCode;
    m_txtPTTKeyName->SetValue(getPTTKeyName(m_selectedPTTKeyCode));
    m_txtPTTKeyName->SetEditable(false);
    m_btnSetPTTKey->SetLabel(_("Change..."));
}

void OptionsDlg::DisplayFifoPACounters() {
    if (IsShownOnScreen())
    {
        wxString fifo_counters = wxString::Format(wxT("Fifos: infull1: %d outempty1: %d infull2: %d outempty2: %d"), g_infifo1_full.load(std::memory_order_relaxed), g_outfifo1_empty.load(std::memory_order_relaxed), g_infifo2_full.load(std::memory_order_relaxed), g_outfifo2_empty.load(std::memory_order_relaxed));
        m_textFifos->SetLabel(fifo_counters);

        // input: underflow overflow output: underflow overflow
        wxString pa_counters_1 = wxString::Format(wxT("Audio1: inUnderflow: %d inOverflow: %d outUnderflow %d outOverflow %d"), g_AEstatus1[0], g_AEstatus1[1], g_AEstatus1[2], g_AEstatus1[3]);
        m_textPA1->SetLabel(pa_counters_1);

        // input: underflow overflow output: underflow overflow
        wxString pa_counters_2 = wxString::Format(wxT("Audio2: inUnderflow: %d inOverflow: %d outUnderflow %d outOverflow %d"), g_AEstatus2[0], g_AEstatus2[1], g_AEstatus2[2], g_AEstatus2[3]);
        m_textPA2->SetLabel(pa_counters_2);
    }
}

void OptionsDlg::OnReportingFreqSelectionChange(wxCommandEvent&)
{
    auto sel = m_freqList->GetSelection();
    if (sel >= 0)
    {
        m_txtCtrlNewFrequency->SetValue(m_freqList->GetString(sel));
    }
    else
    {
        m_txtCtrlNewFrequency->SetValue("");
    }
}

void OptionsDlg::OnReportingFreqTextChange(wxCommandEvent&)
{
    double tmpValue = 0.0;
    bool validNumber = wxNumberFormatter::FromString(m_txtCtrlNewFrequency->GetValue(), &tmpValue);

    auto idx = m_freqList->FindString(m_txtCtrlNewFrequency->GetValue());
    if (idx != wxNOT_FOUND)
    {
        m_freqListAdd->Enable(false);
        m_freqListRemove->Enable(true);
        
        if (idx == 0)
        {
            m_freqListMoveUp->Enable(false);
            m_freqListMoveDown->Enable(true);
        }
        else if ((unsigned)idx == m_freqList->GetCount() - 1)
        {
            m_freqListMoveUp->Enable(true);
            m_freqListMoveDown->Enable(false);
        }
        else
        {
            m_freqListMoveUp->Enable(true);
            m_freqListMoveDown->Enable(true);
        }
    }
    else if (validNumber && tmpValue > 0)
    {
        m_freqListAdd->Enable(true);
        m_freqListRemove->Enable(false);
        m_freqListMoveUp->Enable(false);
        m_freqListMoveDown->Enable(false);
    }
    else
    {
        m_freqListAdd->Enable(false);
        m_freqListRemove->Enable(false);
        m_freqListMoveUp->Enable(false);
        m_freqListMoveDown->Enable(false);
    }
}

void OptionsDlg::OnReportingFreqAdd(wxCommandEvent&)
{
    auto val = m_txtCtrlNewFrequency->GetValue();
    
    double dVal = 0;
    wxNumberFormatter::FromString(val, &dVal);
    if (wxGetApp().appConfiguration.reportingConfiguration.reportingFrequencyAsKhz)
    {
        val = wxNumberFormatter::ToString(dVal, 1);
    }
    else
    {
        val = wxNumberFormatter::ToString(dVal, 4);
    }
    m_freqList->Append(val);
    m_txtCtrlNewFrequency->SetValue("");
}

void OptionsDlg::OnReportingFreqRemove(wxCommandEvent&)
{
    auto idx = m_freqList->FindString(m_txtCtrlNewFrequency->GetValue());
    if (idx >= 0)
    {
        m_freqList->Delete(idx);
    }
    m_txtCtrlNewFrequency->SetValue("");
}

void OptionsDlg::OnReportingFreqMoveUp(wxCommandEvent&)
{
    auto prevStr = m_txtCtrlNewFrequency->GetValue();
    auto idx = m_freqList->FindString(m_txtCtrlNewFrequency->GetValue());
    if (idx != wxNOT_FOUND && idx > 0)
    {
        m_freqList->Delete(idx);
        m_freqList->Insert(prevStr, idx - 1);
        m_freqList->SetSelection(idx - 1);
    }
    
    // Refresh button status
    m_txtCtrlNewFrequency->SetValue(prevStr);
}

void OptionsDlg::OnReportingFreqMoveDown(wxCommandEvent&)
{
    auto prevStr = m_txtCtrlNewFrequency->GetValue();
    auto idx = m_freqList->FindString(m_txtCtrlNewFrequency->GetValue());
    if (idx != wxNOT_FOUND && (unsigned int)idx < m_freqList->GetCount() - 1)
    {
        m_freqList->Delete(idx);
        m_freqList->Insert(prevStr, idx + 1);
        m_freqList->SetSelection(idx + 1);
    }
    
    // Refresh button status
    m_txtCtrlNewFrequency->SetValue(prevStr);
}

//-------------------------------------------------------------------------
// updateData2GControls_(): the connection settings only matter with Data2G
// chosen.
//-------------------------------------------------------------------------
void OptionsDlg::updateData2GControls_()
{
    bool on = m_ckboxData2G->GetValue();
    m_txtData2GHost->Enable(on);
    m_txtData2GKissPort->Enable(on);
    m_txtData2GCommandPort->Enable(on);
    m_ckboxData2GSessions->Enable(on);
}

//-------------------------------------------------------------------------
// updateCwTailControls_(): the CW settings only matter with the CW tail
// chosen; beneath them, what will be sent and for how long, or why the
// chord will close the transmission instead.
//-------------------------------------------------------------------------
void OptionsDlg::updateCwTailControls_()
{
    bool cw = m_choiceGlissandoTail->GetSelection() >= 2;
    m_txtGlissandoCwText->Enable(cw);
    m_spinGlissandoCwWpm->Enable(cw);
    m_spinGlissandoCwIdMinutes->Enable(cw);
    m_textGlissandoCwTail->Show(cw);
    if (!cw) return;

    std::string format = m_txtGlissandoCwText->GetValue().ToStdString();
    std::string text = TextMessagingModem::cwTailText(format, m_txt_callsign->GetValue().ToStdString());
    std::string sendable = Glissando::cwSendable(text);
    int wpm = m_spinGlissandoCwWpm->GetValue();
    wxString status;
    if (text.empty() && !format.empty())
    {
        status = _("Set a callsign on the Station tab; until then the chord ends each transmission.");
    }
    else if (sendable.empty())
    {
        status = _("Nothing to send; the chord ends each transmission.");
    }
    else if (!Glissando::cwTailFits(text, wpm))
    {
        status = wxString::Format(_("%.1f s is too long (%.0f s at most); the chord ends each transmission."),
                                  Glissando::cwTailSeconds(text, wpm), Glissando::CW_TAIL_MAX_SECONDS);
    }
    else
    {
        status = wxString::Format(_("Sends \"%s\", %.1f s."), wxString(sendable), Glissando::cwTailSeconds(text, wpm));
    }
    m_textGlissandoCwTail->SetLabel(status);
    m_textGlissandoCwTail->GetParent()->Layout();
}
