//==========================================================================
// Name:            dlg_options.h
// Purpose:         Dialog for controlling misc FreeDV options
// Created:         Nov 25 2012
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

#ifndef __OPTIONS_DIALOG__
#define __OPTIONS_DIALOG__

#include "../../main.h"
#include "defines.h"
#include <wx/spinctrl.h>

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
// Class OptionsDlg
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
class OptionsDlg : public wxDialog
{
    public:
    OptionsDlg( wxWindow* parent,
                wxWindowID id = wxID_ANY, const wxString& title = _("Options"), 
                const wxPoint& pos = wxDefaultPosition, 
                const wxSize& size = wxDefaultSize, 
                long style = wxDEFAULT_DIALOG_STYLE|wxTAB_TRAVERSAL );
        ~OptionsDlg();

        void    ExchangeData(int inout, bool storePersistent);

        void DisplayFifoPACounters();
        
        void setSessionActive(bool active) 
        {
            sessionActive_ = active; 
        
            updateReportingState();
        }
        
    protected:

        // Handlers for events.

        void    OnOK(wxCommandEvent& event);
        void    OnCancel(wxCommandEvent& event);
        void    OnApply(wxCommandEvent& event);
        void    OnClose(wxCloseEvent& event);
        void    OnInitDialog(wxInitDialogEvent& event);
 
        void    OnDebugConsole(wxScrollEvent& event);

        void    OnFifoReset(wxCommandEvent& event);
        
        void    OnEnableSpacebarForPTT(wxCommandEvent& event);
        void    OnSetPTTKey(wxCommandEvent& event);
        void    OnTOTTimerEnable(wxCommandEvent& event);
        void    OnSwrMeterEnable(wxCommandEvent& event);
        void    OnDialogCharHook(wxKeyEvent& event);
        void    OnPTTKeyCapture(wxKeyEvent& event);
        void    enterPTTCaptureMode_();
        void    exitPTTCaptureMode_(bool accept, int keyCode = 0);

        wxCheckBox* m_ckHalfDuplex;

        wxNotebook  *m_notebook;
        wxNotebookPage *m_reportingTab; // Station: callsign, stations heard log
        wxNotebookPage *m_rigControlTab; // Rig Control
        wxNotebookPage *m_modemTab; // 700/OFDM/duplex
        wxNotebookPage *m_debugTab; // Debug
        
        /* Hamlib options */
        wxRadioButton *m_rbFrequencyControl;
        wxRadioButton *m_rbNoFrequencyControl;
        wxCheckBox    *m_ckboxEnableSpacebarForPTT;
        wxCheckBox    *m_ckboxPTTMomentaryMode;
        wxTextCtrl    *m_txtPTTKeyName;
        wxButton      *m_btnSetPTTKey;
        int            m_selectedPTTKeyCode;
        bool           m_capturingPTTKey;
        wxTextCtrl    *m_txtTxRxDelayMilliseconds;
        wxCheckBox    *m_ckboxFrequencyEntryAsKHz;

        /* Time-Out Timer options */
        wxCheckBox    *m_ckboxTOTTimerEnabled;
        wxTextCtrl    *m_txtTOTTimerSecs;

        /* SWR meter */
        wxCheckBox    *m_ckboxSwrMeter;
        wxCheckBox    *m_ckboxSwrAutoAbort;
        
        /* test frames, other simulated channel impairments */



        wxCheckBox   *m_ckboxAutoStartOnLaunch;

        wxTextCtrl    *m_txt_callsign;
        wxTextCtrl    *m_txtCtrlCsvLogFilePath;
        wxButton      *m_buttonChooseCsvLogFilePath;
        
        wxButton*     m_BtnFifoReset;
        wxStaticText  *m_textFifos;
        wxStaticText  *m_textPA1;
        wxStaticText  *m_textPA2;
        wxTextCtrl    *m_txtCtrlFifoSize;
        
        wxButton*     m_sdbSizer5OK;
        wxButton*     m_sdbSizer5Cancel;
        wxButton*     m_sdbSizer5Apply;

        wxCheckBox   *m_ckboxDebugConsole;

        wxCheckBox*  m_ckboxTextChatUsDataSegmentsOnly;
        wxCheckBox*  m_ckboxGlissandoChords;
        wxChoice*    m_choiceGlissandoTail;
        wxTextCtrl*  m_txtGlissandoCwText;
        wxSpinCtrl*  m_spinGlissandoCwWpm;
        wxSpinCtrl*  m_spinGlissandoCwIdMinutes;
        wxStaticText* m_textGlissandoCwTail;
        void updateCwTailControls_();
        wxCheckBox*  m_ckboxGlissandoTransmitShips;
        wxCheckBox*  m_ckboxGlissandoShowMarquee;
        wxCheckBox*  m_ckboxGlissandoSmoke;
        wxTextCtrl*  m_txtGlissandoSmokeSeconds;
        wxCheckBox*  m_ckboxData2G;
        wxTextCtrl*  m_txtData2GHost;
        wxTextCtrl*  m_txtData2GKissPort;
        wxCheckBox*  m_ckboxData2GCommandPort;
        wxTextCtrl*  m_txtData2GCommandPort;
        void updateData2GControls_();
        
        wxListBox*  m_freqList;
        wxStaticText* m_labelEnterFreq;
        wxTextCtrl* m_txtCtrlNewFrequency;
        wxButton*   m_freqListAdd;
        wxButton*   m_freqListRemove;
        wxButton*   m_freqListMoveUp;
        wxButton*   m_freqListMoveDown;
        
        unsigned int  event_in_serial, event_out_serial;

        void OnChooseCsvLogFilePath(wxCommandEvent& event);
        
        void OnReportingFreqSelectionChange(wxCommandEvent& event);
        void OnReportingFreqTextChange(wxCommandEvent& event);
        void OnReportingFreqAdd(wxCommandEvent& event);
        void OnReportingFreqRemove(wxCommandEvent& event);
        void OnReportingFreqMoveUp(wxCommandEvent& event);
        void OnReportingFreqMoveDown(wxCommandEvent& event);
        
     private:
         void updateReportingState();
         void updateRigControlState();
         
         bool sessionActive_;
};

#endif // __OPTIONS_DIALOG__
