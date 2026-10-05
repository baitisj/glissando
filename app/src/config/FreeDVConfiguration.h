//==========================================================================
// Name:            FreeDVConfiguration.h
// Purpose:         Implements the configuration for FreeDV
// Created:         July 1, 2023
// Authors:         Mooneer Salem
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

#ifndef FREEDV_CONFIGURATION_H
#define FREEDV_CONFIGURATION_H

#include <inttypes.h>
#include <wx/string.h>
#include "WxWidgetsConfigStore.h"
#include "ConfigurationDataElement.h"
#include "AudioConfiguration.h"
#include "RigControlConfiguration.h"
#include "ReportingConfiguration.h"

class FreeDVConfiguration : public WxWidgetsConfigStore
{
public:
    FreeDVConfiguration();
    virtual ~FreeDVConfiguration() = default;
    
    AudioConfiguration audioConfiguration;
    RigControlConfiguration rigControlConfiguration;
    ReportingConfiguration reportingConfiguration;
    
    ConfigurationDataElement<bool> firstTimeUse;
    
    ConfigurationDataElement<long> mainWindowLeft;
    ConfigurationDataElement<long> mainWindowTop;
    ConfigurationDataElement<long> mainWindowWidth;
    ConfigurationDataElement<long> mainWindowHeight;

    ConfigurationDataElement<long> audioConfigWindowLeft;
    ConfigurationDataElement<long> audioConfigWindowTop;
    ConfigurationDataElement<long> audioConfigWindowWidth;
    ConfigurationDataElement<long> audioConfigWindowHeight;

    ConfigurationDataElement<long> currentNotebookTab;
    
    ConfigurationDataElement<int> fifoSizeMs;
    ConfigurationDataElement<int> transmitLevel;
    ConfigurationDataElement<int> tuneLevel;
    ConfigurationDataElement<std::map<wxString, int>> txAttenByBand;
    ConfigurationDataElement<std::map<wxString, int>> tuneAttenByBand;
    
    ConfigurationDataElement<wxString> playFileToMicInPath;
    ConfigurationDataElement<wxString> playFileFromRadioPath;
    
    ConfigurationDataElement<bool> enableSpaceBarForPTT;
    ConfigurationDataElement<int> pttKeyCode;
    ConfigurationDataElement<bool> pttMomentaryMode;

    
    ConfigurationDataElement<bool> halfDuplexMode;

    // Text chat transmits only where US rules permit a data emission (47 CFR
    // 97.305), and not at all while the operating frequency is unknown. On by
    // default: an operator elsewhere turns it off.
    ConfigurationDataElement<bool> textChatUsDataSegmentsOnly;

    // Text chat through a separately running data2g-host instead of our own
    // modem (docs/DATA2G.md): KISS for the frames, and the command port for
    // PTT and BUSY unless another client needs it.
    ConfigurationDataElement<bool> data2gEnabled;
    ConfigurationDataElement<wxString> data2gHost;
    ConfigurationDataElement<int> data2gKissPort;
    ConfigurationDataElement<bool> data2gUseCommandPort;
    ConfigurationDataElement<int> data2gCommandPort;

    // The Glissando console and the melodic chirp mode it drives. Tuning
    // offset and scan rate are stored in tenths (Hz, rows per second) so
    // they fit the integer config type.
    ConfigurationDataElement<int> glissandoGear;
    ConfigurationDataElement<bool> glissandoAutoGear;
    ConfigurationDataElement<wxString> glissandoScale;
    ConfigurationDataElement<int> glissandoTuningDeciHz;
    ConfigurationDataElement<bool> glissandoListenAllGears;
    ConfigurationDataElement<bool> glissandoChords;         // the opening chord
    ConfigurationDataElement<int> glissandoTail;            // 0 off, 1 chord, 2 CW glorified, 3 CW straight
    ConfigurationDataElement<wxString> glissandoCwText;     // <MYCALL> is the Station callsign
    ConfigurationDataElement<int> glissandoCwWpm;
    ConfigurationDataElement<int> glissandoCwIdMinutes;     // 0: every keying
    ConfigurationDataElement<int> glissandoScanRateDeci;
    ConfigurationDataElement<bool> glissandoScopeLens;
    ConfigurationDataElement<bool> glissandoTransmitShips;  // rockets and invaders on the scope while sending
    ConfigurationDataElement<bool> glissandoShowMarquee;    // the GLISSANDO title card above the scope
    ConfigurationDataElement<bool> glissandoChatOpen;       // the COMMS window was open when the app closed
    ConfigurationDataElement<bool> glissandoSnoopOpen;      // and the snooping window
    ConfigurationDataElement<bool> glissandoSmoke;          // the scope smokes on a long keying at high power
    ConfigurationDataElement<int> glissandoSmokeSeconds;    // keyed this long, it starts
    ConfigurationDataElement<long> glissandoWindowLeft;
    ConfigurationDataElement<long> glissandoWindowTop;
    ConfigurationDataElement<long> glissandoWindowWidth;
    ConfigurationDataElement<long> glissandoWindowHeight;
    
    
    
    
    ConfigurationDataElement<bool> debugConsoleEnabled; // note: Windows only
    
    
    
    
    
    

    ConfigurationDataElement<bool> monitorTxAudio;
    ConfigurationDataElement<float> monitorTxAudioVol;

    ConfigurationDataElement<int> txRxDelayMilliseconds;


    ConfigurationDataElement<bool> autoStartOnLaunch;

    virtual void load(wxConfigBase* config) override;
    virtual void save(wxConfigBase* config) override;
};

#endif // FREEDV_CONFIGURATION_H
