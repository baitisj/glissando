/*
    util.h
    
    Miscellaneous utility functions
*/

#include "main.h"

#ifdef _WIN32
#include <strsafe.h>
#endif

extern std::atomic<bool>             g_tx;

bool MainApp::CanAccessSerialPort(std::string const& portName)
{
    bool couldOpen = true;
    com_handle_t com_handle = COM_HANDLE_INVALID;
    
#ifdef _WIN32
    {
        if (portName.substr(0, 3) != "COM")
        {
            // assume we can open if we don't have a valid port name.
            return couldOpen;
        }
        
        TCHAR  nameWithStrangePrefix[100];
        StringCchPrintf(nameWithStrangePrefix, 100, TEXT("\\\\.\\%hs"), portName.c_str());
	
        if((com_handle=CreateFile(nameWithStrangePrefix
                                   ,GENERIC_READ | GENERIC_WRITE/* Access */
                                   ,0				/* Share mode */
                                   ,NULL		 	/* Security attributes */
                                   ,OPEN_EXISTING		/* Create access */
                                   ,0                           /* File attributes */
                                   ,NULL		        /* Template */
                                   ))==INVALID_HANDLE_VALUE) {
           couldOpen = false;
    	}
        else
        {
            CloseHandle(com_handle);
        }
    }
#else
	{
        if (portName.substr(0, 5) != "/dev/")
        {
            // assume we can open if we don't have a valid port name.
            return couldOpen;
        }
        
		if((com_handle=open(portName.c_str(), O_NONBLOCK|O_RDWR))== COM_HANDLE_INVALID)
        {
            couldOpen = false;
        }
        else
        {
            close(com_handle);
        }
	}
#endif
    
    if (!couldOpen)
    {
        std::string errorMessage = "Could not open serial port " + portName + ".";
        
        #ifdef _WIN32
        errorMessage += " Please ensure that no other applications are accessing the port.";
        #elif __linux
        errorMessage += " Please ensure that you have permission to access the port. Adding yourself to the 'dialout' group (and logging out/back in) along with reattaching your radio to your PC will typically ensure this.";
        #else
        errorMessage += " Please ensure that you have permission to access the port.";
        #endif
         
        CallAfter([&, errorMessage]() {
            wxMessageBox(
                errorMessage, 
                wxT("Error"), wxOK | wxICON_ERROR, GetTopWindow());
        });
    }
    
    return couldOpen;
}

//----------------------------------------------------------------
// isReceiveOnly()
//----------------------------------------------------------------

bool MainFrame::isReceiveOnly()
{
    return g_nSoundCards <= 1;
}

//----------------------------------------------------------------
// OpenSerialPort()
//----------------------------------------------------------------

void MainFrame::OpenSerialPort(void)
{
    if(!wxGetApp().appConfiguration.rigControlConfiguration.serialPTTPort->IsEmpty()) 
    {
        if (wxGetApp().CanAccessSerialPort((const char*)wxGetApp().appConfiguration.rigControlConfiguration.serialPTTPort->ToUTF8()))
        {
            wxGetApp().rigPttController = std::make_shared<SerialPortOutRigController>(
                    (const char*)wxGetApp().appConfiguration.rigControlConfiguration.serialPTTPort->c_str(),
                    wxGetApp().appConfiguration.rigControlConfiguration.serialPTTUseRTS,
                    wxGetApp().appConfiguration.rigControlConfiguration.serialPTTPolarityRTS,
                    wxGetApp().appConfiguration.rigControlConfiguration.serialPTTUseDTR,
                    wxGetApp().appConfiguration.rigControlConfiguration.serialPTTPolarityDTR);
            wxGetApp().rigFrequencyController = nullptr;
            
            wxGetApp().rigPttController->onRigError += [&](IRigController*, std::string const& err) {
                std::string fullErrMsg = "Couldn't open serial port for PTT output: " + err; 
                CallAfter([&]() 
                {
                    wxMessageBox(fullErrMsg, wxT("Error"), wxOK | wxICON_ERROR, this);
                });
            };

            wxGetApp().rigPttController->connect();
        }
    }
}

//----------------------------------------------------------------
// OpenPTTInPort()
//----------------------------------------------------------------

void MainFrame::OpenPTTInPort(void)
{
    if(!wxGetApp().appConfiguration.rigControlConfiguration.serialPTTInputPort->IsEmpty()) 
    {
        if (wxGetApp().CanAccessSerialPort((const char*)wxGetApp().appConfiguration.rigControlConfiguration.serialPTTInputPort->ToUTF8()))
        {
            wxGetApp().m_pttInSerialPort = std::make_shared<SerialPortInRigController>(
                (const char*)wxGetApp().appConfiguration.rigControlConfiguration.serialPTTInputPort->c_str(),
                wxGetApp().appConfiguration.rigControlConfiguration.serialPTTInputPolarityCTS);
            
            wxGetApp().m_pttInSerialPort->onRigError += [&](IRigController*, std::string const& err)
            {
                std::string fullErr = "Couldn't open PTT input port: " + err;
                CallAfter([&]() 
                {
                    wxMessageBox(fullErr, wxT("Error"), wxOK | wxICON_ERROR, this);
                });
            };

            wxGetApp().m_pttInSerialPort->onPttChange += [&](IRigController*, bool pttState)
            {
                log_info("PTT input state is now %d", pttState);
                GetEventHandler()->CallAfter([this, pttState]() {
                    if (pttState != m_btnTogPTT->GetValue())
                    {
                        m_btnTogPTT->SetValue(pttState);                        
                        togglePTT(); 
                    }
                });
            };

            wxGetApp().m_pttInSerialPort->connect();
        }
    }
}


//----------------------------------------------------------------
// ClosePTTInPort()
//----------------------------------------------------------------

void MainFrame::ClosePTTInPort(void)
{
    if (wxGetApp().m_pttInSerialPort)
    {
        wxGetApp().m_pttInSerialPort->disconnect();
        wxGetApp().m_pttInSerialPort = nullptr;
    }
}

// Decimates samples using an algorithm that produces nice plots of
// speech signals at a low sample rate.  We want a low sample rate so
// we don't hammer the graphics system too hard.  Saves decimated data
// to a fifo for plotting on screen.

void resample_for_plot(GenericFIFO<short> *plotFifo, short buf[], short* dec_samples, int length, int fs) FREEDV_NONBLOCKING
{
    int decimation = fs/WAVEFORM_PLOT_FS;
    int nSamples, sample;
    int i, st, en, max, min;

    nSamples = length/decimation;
    if (nSamples % 2) nSamples++; // dec_samples is populated in groups of two

    for(sample = 0; sample < nSamples; sample += 2)
    {
        st = decimation*sample;
        en = decimation*(sample+2);
        max = min = 0;
        for(i=st; i<en && i<length; i++ )
        {
            if (max < buf[i]) max = buf[i];
            if (min > buf[i]) min = buf[i];
        }
        dec_samples[sample] = max;
        dec_samples[sample+1] = min;
    }
    plotFifo->write(dec_samples, nSamples);
}

void MainFrame::executeOnUiThreadAndWait_(std::function<void()> fn)
{
    std::mutex funcMutex;
    std::condition_variable funcConditionVariable;
    std::unique_lock<std::mutex> funcLock(funcMutex);
    
    CallAfter([&]() {
        std::unique_lock<std::mutex> guiLock(funcMutex);
        
        fn();
        
        funcConditionVariable.notify_one();
    });
    
    funcConditionVariable.wait(funcLock);
}
