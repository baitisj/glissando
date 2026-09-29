/*

    playrec.cpp
    
    Playing and recording files.
*/

#include "main.h"


extern wxMutex g_mutexProtectingCallbackData;

SNDFILE            *g_sfRecFile;
bool                g_recFileFromRadio;
std::atomic<unsigned int> g_recFromRadioSamples;
int                 g_recFileFromRadioEventId;

std::atomic<SNDFILE*> g_sfPlayFileFromRadio;
std::atomic<bool>                g_playFileFromRadio;
std::atomic<int>    g_sfFs;
std::atomic<bool>   g_loopPlayFileFromRadio;
int                 g_playFileFromRadioEventId;

std::atomic<SNDFILE*>            g_sfRecFileFromModulator;
std::atomic<bool>                g_recFileFromModulator;

int                 g_recFromModulatorSamples;
int                 g_recFileFromModulatorEventId;

extern std::atomic<bool> g_tx;

// extra panel added to file open dialog to add loop checkbox
MyExtraPlayFilePanel::MyExtraPlayFilePanel(wxWindow *parent): wxPanel(parent)
{
    m_cb = new wxCheckBox(this, -1, wxT("Loop"));
    m_cb->SetToolTip(_("When checked file will repeat forever"));
    m_cb->SetValue(g_loopPlayFileFromRadio.load(std::memory_order_relaxed));

    // bug: I can't this to align right.....
    wxBoxSizer *sizerTop = new wxBoxSizer(wxHORIZONTAL);
    sizerTop->Add(m_cb, 0, 0, 0);
    SetSizerAndFit(sizerTop);
}

static wxWindow* createMyExtraPlayFilePanel(wxWindow *parent)
{
    return new MyExtraPlayFilePanel(parent);
}

void MainFrame::StopPlaybackFileFromRadio()
{
    g_mutexProtectingCallbackData.Lock();
    g_playFileFromRadio.store(false, std::memory_order_release);
    auto tmp = g_sfPlayFileFromRadio.load(std::memory_order_acquire);
    sf_close(tmp);
    g_sfPlayFileFromRadio.store(nullptr, std::memory_order_release);
    SetStatusText(wxT(""));
    m_menuItemPlayFileFromRadio->SetItemLabel(wxString(_("Start Play File - From Radio...")));
    g_mutexProtectingCallbackData.Unlock();
}

//-------------------------------------------------------------------------
// OnPlayFileFromRadio()
// This puppy "plays" a recorded file into the demodulator input, allowing us
// to replay off air signals.
//-------------------------------------------------------------------------
void MainFrame::OnPlayFileFromRadio(wxCommandEvent& event)
{
    wxUnusedVar(event);

    log_debug("OnPlayFileFromRadio:: %d", (int)g_playFileFromRadio.load(std::memory_order_acquire));
    if (g_playFileFromRadio.load(std::memory_order_acquire))
    {
        log_debug("OnPlayFileFromRadio:: Stop");
        StopPlaybackFileFromRadio();
    }
    else
    {
        wxString    soundFile;
        SF_INFO     sfInfo;

        wxFileDialog openFileDialog(
                                    this,
                                    wxT("Play File - From Radio"),
                                    wxGetApp().appConfiguration.playFileFromRadioPath,
                                    wxEmptyString,
                                    wxT("WAV and RAW files (*.wav;*.raw)|*.wav;*.raw|")
                                    wxT("All files (*.*)|*.*"),
                                    wxFD_OPEN | wxFD_FILE_MUST_EXIST
                                    );

        // add the loop check box
        openFileDialog.SetExtraControlCreator(&createMyExtraPlayFilePanel);

        if(openFileDialog.ShowModal() == wxID_CANCEL)
        {
            return;     // the user changed their mind...
        }

        wxString fileName, extension;
        soundFile = openFileDialog.GetPath();
        wxString tmpString = wxGetApp().appConfiguration.playFileFromRadioPath;
        wxFileName::SplitPath(soundFile, &tmpString, &fileName, &extension);
        sfInfo.format = 0;

        if(!extension.IsEmpty())
        {
            extension.LowerCase();
            if(extension == wxT("raw"))
            {
                sfInfo.format     = SF_FORMAT_RAW | SF_FORMAT_PCM_16;
                sfInfo.channels   = 1;
                sfInfo.samplerate = FS; // raw files are taken to be 8 kHz
            }
        }
        g_sfPlayFileFromRadio.store(sf_open(soundFile.c_str(), SFM_READ, &sfInfo), std::memory_order_release);
        g_sfFs.store(sfInfo.samplerate, std::memory_order_release);
        if(g_sfPlayFileFromRadio.load(std::memory_order_acquire) == NULL)
        {
            wxString strErr = sf_strerror(NULL);
            wxMessageBox(strErr, wxT("Couldn't open sound file"), wxOK);
            return;
        }
        
        // Save path for future use
        wxGetApp().appConfiguration.playFileFromRadioPath = tmpString;

        wxWindow * const ctrl = openFileDialog.GetExtraControl();

        // Huh?! I just copied wxWidgets-2.9.4/samples/dialogs ....
        g_loopPlayFileFromRadio.store(static_cast<MyExtraPlayFilePanel*>(ctrl)->getLoop(), std::memory_order_relaxed);

        wxString statusText = "";
        if(extension == wxT("raw")) {
            statusText = wxString::Format(wxT("Playing raw file %s as radio input (assuming Fs=%d)"), soundFile, (int)sfInfo.samplerate);
        }
        else
        {
            statusText = wxString::Format(wxT("Playing file %s as radio input"), soundFile);
        }
        SetStatusText(statusText, 0);
        log_debug("OnPlayFileFromRadio:: Playing File Fs = %d", (int)sfInfo.samplerate);
        m_menuItemPlayFileFromRadio->SetItemLabel(wxString(_("Stop Play File - From Radio...")));
        g_playFileFromRadio.store(true, std::memory_order_release);
    }
}

void MainFrame::StopRecFileFromRadio()
{
    if (g_sfRecFile != nullptr)
    {
        log_debug("Stopping Record....");
        g_mutexProtectingCallbackData.Lock();
        g_recFileFromRadio = false;
        g_recFileFromModulator = false;
        sf_close(g_sfRecFile);
        g_sfRecFile = nullptr;
        g_sfRecFileFromModulator = nullptr;
        SetStatusText(wxT(""));
        
        g_mutexProtectingCallbackData.Unlock();
        
    }
}


