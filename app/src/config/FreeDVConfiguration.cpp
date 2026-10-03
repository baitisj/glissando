//==========================================================================
// Name:            FreeDVConfiguration.cpp
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

#include <wx/stdpaths.h>
#include <wx/filename.h>

#include "../defines.h"
#include "FreeDVConfiguration.h"

FreeDVConfiguration::FreeDVConfiguration()
    /* First time configuration options */
    : firstTimeUse("/FirstTimeUse", true)
        
    /* Position and size of main window */
    , mainWindowLeft("/MainFrame/left", 20)
    , mainWindowTop("/MainFrame/top", 20)
    , mainWindowWidth("/MainFrame/width", 800)
    , mainWindowHeight("/MainFrame/height", 780)

    /* Position and size of Audio Config window */
    , audioConfigWindowLeft("/Windows/AudioConfig/left", -1)
    , audioConfigWindowTop("/Windows/AudioConfig/top", -1)
    , audioConfigWindowWidth("/Windows/AudioConfig/width", -1)
    , audioConfigWindowHeight("/Windows/AudioConfig/height", -1)

    /* Current tab view */
    , currentNotebookTab("/MainFrame/rxNbookCtrl", 0)
        
    /* Squelch configuration */
        
    /* Misc. audio settings */
    , fifoSizeMs("/Audio/fifoSize_ms", (int)FIFO_SIZE)
    , transmitLevel("/Audio/transmitLevel", 0)
    , tuneLevel("/Audio/tuneLevel", 0)
    , txAttenByBand("/Audio/transmitLevelByBand", {})
    , tuneAttenByBand("/Audio/tuneLevelByBand", {})
        
    /* Recording settings */
    , playFileToMicInPath("/File/playFileToMicInPath", _(""))
    , playFileFromRadioPath("/File/playFileFromRadioPath", _(""))
        
    , enableSpaceBarForPTT("/Rig/EnableSpacebarForPTT", true)
    , pttKeyCode("/Rig/PttKeyCode", WXK_SPACE)
    , pttMomentaryMode("/Rig/PttMomentaryMode", false)

        
    , halfDuplexMode("/Rig/HalfDuplex", true)
    , textChatUsDataSegmentsOnly("/TextChat/UsDataSegmentsOnly", true)
    , data2gEnabled("/Data2G/Enabled", false)
    , data2gHost("/Data2G/Host", "127.0.0.1")
    , data2gKissPort("/Data2G/KissPort", 8100)
    , data2gUseCommandPort("/Data2G/UseCommandPort", true)
    , data2gCommandPort("/Data2G/CommandPort", 8300)
    , glissandoGear("/Glissando/Gear", 3)
    , glissandoAutoGear("/Glissando/AutoGear", true)
    , glissandoScale("/Glissando/Scale", "pentatonic")
    , glissandoTuningDeciHz("/Glissando/TuningDeciHz", 0)
    , glissandoListenAllGears("/Glissando/ListenAllGears", true)
    , glissandoChords("/Glissando/Chords", true)
    , glissandoTail("/Glissando/Tail", 2)
    , glissandoCwText("/Glissando/CwText", "Gliss de <MYCALL>")
    , glissandoCwWpm("/Glissando/CwWpm", 20)
    , glissandoCwIdMinutes("/Glissando/CwIdMinutes", 10)
    , glissandoScanRateDeci("/Glissando/ScanRateDeci", 40)
    , glissandoScopeLens("/Glissando/ScopeLens", true)
    , glissandoTransmitShips("/Glissando/TransmitShips", true)
    , glissandoShowMarquee("/Glissando/ShowMarquee", true)
    , glissandoWindowLeft("/Glissando/WindowLeft", -1)
    , glissandoWindowTop("/Glissando/WindowTop", -1)
    , glissandoWindowWidth("/Glissando/WindowWidth", 1040)
    , glissandoWindowHeight("/Glissando/WindowHeight", 720)
        
        
        
        
    , debugConsoleEnabled("/Debug/console", false)
        
        
        
        
        
    

    , monitorTxAudio("/Monitor/TransmitAudio", false)
    , monitorTxAudioVol("/Monitor/TransmitAudioVol", 0)

    , txRxDelayMilliseconds("/Audio/TxRxDelayMilliseconds", 0)

    , autoStartOnLaunch("/Modem/autoStartOnLaunch", false)
{
    // empty
}

void FreeDVConfiguration::load(wxConfigBase* config)
{
    audioConfiguration.load(config);
    rigControlConfiguration.load(config);
    reportingConfiguration.load(config);
    
    load_(config, firstTimeUse);
    
    load_(config, mainWindowLeft);
    load_(config, mainWindowTop);
    load_(config, mainWindowWidth);
    load_(config, mainWindowHeight);

    load_(config, audioConfigWindowLeft);
    load_(config, audioConfigWindowTop);
    load_(config, audioConfigWindowWidth);
    load_(config, audioConfigWindowHeight);

    load_(config, currentNotebookTab);
    
    
    load_(config, fifoSizeMs);
    load_(config, transmitLevel);
    load_(config, tuneLevel);
    
    load_(config, playFileToMicInPath);
    load_(config, playFileFromRadioPath);
    
    load_(config, enableSpaceBarForPTT);
    load_(config, pttKeyCode);
    load_(config, pttMomentaryMode);

    
    load_(config, halfDuplexMode);
    load_(config, textChatUsDataSegmentsOnly);
    load_(config, data2gEnabled);
    load_(config, data2gHost);
    load_(config, data2gKissPort);
    load_(config, data2gUseCommandPort);
    load_(config, data2gCommandPort);
    load_(config, glissandoGear);
    load_(config, glissandoAutoGear);
    load_(config, glissandoScale);
    load_(config, glissandoTuningDeciHz);
    load_(config, glissandoListenAllGears);
    load_(config, glissandoChords);
    // Before the tail had its own setting, turning the chords off turned
    // off the closing one too; keep such a station as quiet as it was.
    bool tailSaved = config->HasEntry("/Glissando/Tail");
    load_(config, glissandoTail);
    if (!tailSaved && !glissandoChords) glissandoTail = 0;
    load_(config, glissandoCwText);
    // The first default spelled the mode out in full; a text still equal to
    // it takes the shorter one.
    if ((wxString)glissandoCwText == "Glissando de <MYCALL>") glissandoCwText = wxString("Gliss de <MYCALL>");
    load_(config, glissandoCwWpm);
    load_(config, glissandoCwIdMinutes);
    load_(config, glissandoScanRateDeci);
    load_(config, glissandoScopeLens);
    load_(config, glissandoTransmitShips);
    load_(config, glissandoShowMarquee);
    load_(config, glissandoWindowLeft);
    load_(config, glissandoWindowTop);
    load_(config, glissandoWindowWidth);
    load_(config, glissandoWindowHeight);
    
    
    
    load_(config, debugConsoleEnabled);
    
    
    
    
    
    
    load_(config, monitorTxAudio);
    load_(config, monitorTxAudioVol);
    
    

    load_(config, txRxDelayMilliseconds);

    load_(config, autoStartOnLaunch);

    load_(config, txAttenByBand);
    load_(config, tuneAttenByBand);
}

void FreeDVConfiguration::save(wxConfigBase* config)
{
    audioConfiguration.save(config);
    rigControlConfiguration.save(config);
    reportingConfiguration.save(config);
    
    save_(config, firstTimeUse);
    
    save_(config, mainWindowLeft);
    save_(config, mainWindowTop);
    save_(config, mainWindowWidth);
    save_(config, mainWindowHeight);

    save_(config, audioConfigWindowLeft);
    save_(config, audioConfigWindowTop);
    save_(config, audioConfigWindowWidth);
    save_(config, audioConfigWindowHeight);

    save_(config, currentNotebookTab);
    
    
    save_(config, fifoSizeMs);
    save_(config, transmitLevel);
    save_(config, tuneLevel);
    
    save_(config, playFileToMicInPath);
    save_(config, playFileFromRadioPath);
    
    save_(config, enableSpaceBarForPTT);
    save_(config, pttKeyCode);
    save_(config, pttMomentaryMode);

    
    save_(config, halfDuplexMode);
    save_(config, textChatUsDataSegmentsOnly);
    save_(config, data2gEnabled);
    save_(config, data2gHost);
    save_(config, data2gKissPort);
    save_(config, data2gUseCommandPort);
    save_(config, data2gCommandPort);
    save_(config, glissandoGear);
    save_(config, glissandoAutoGear);
    save_(config, glissandoScale);
    save_(config, glissandoTuningDeciHz);
    save_(config, glissandoListenAllGears);
    save_(config, glissandoChords);
    save_(config, glissandoTail);
    save_(config, glissandoCwText);
    save_(config, glissandoCwWpm);
    save_(config, glissandoCwIdMinutes);
    save_(config, glissandoScanRateDeci);
    save_(config, glissandoScopeLens);
    save_(config, glissandoTransmitShips);
    save_(config, glissandoShowMarquee);
    save_(config, glissandoWindowLeft);
    save_(config, glissandoWindowTop);
    save_(config, glissandoWindowWidth);
    save_(config, glissandoWindowHeight);
    
    
    
    
    save_(config, debugConsoleEnabled);
    
    
    
    
    
    

    save_(config, monitorTxAudio);
    save_(config, monitorTxAudioVol);

    save_(config, txRxDelayMilliseconds);

    save_(config, autoStartOnLaunch);

    save_(config, txAttenByBand);
    save_(config, tuneAttenByBand);

    config->Flush();
}
