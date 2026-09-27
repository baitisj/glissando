//==========================================================================
// Name:            AudioConfiguration.cpp
// Purpose:         Implements the audio device configuration for FreeDV
// Created:         July 2, 2023
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

#include "AudioConfiguration.h"

template<>
const char* AudioConfiguration::AudioDeviceConfigName<1, AudioConfiguration::DIR_IN>::GetDeviceNameConfigName()
{
    return "/Audio/soundCard1InDeviceName";
}

template<>
const char* AudioConfiguration::AudioDeviceConfigName<1, AudioConfiguration::DIR_OUT>::GetDeviceNameConfigName()
{
    return "/Audio/soundCard1OutDeviceName";
}

template<>
const char* AudioConfiguration::AudioDeviceConfigName<2, AudioConfiguration::DIR_IN>::GetDeviceNameConfigName()
{
    return "/Audio/soundCard2InDeviceName";
}

template<>
const char* AudioConfiguration::AudioDeviceConfigName<2, AudioConfiguration::DIR_OUT>::GetDeviceNameConfigName()
{
    return "/Audio/soundCard2OutDeviceName";
}

template<>
const char* AudioConfiguration::AudioDeviceConfigName<1, AudioConfiguration::DIR_IN>::GetSampleRateConfigName()
{
    return "/Audio/soundCard1InSampleRate";
}

template<>
const char* AudioConfiguration::AudioDeviceConfigName<1, AudioConfiguration::DIR_OUT>::GetSampleRateConfigName()
{
    return "/Audio/soundCard1OutSampleRate";
}

template<>
const char* AudioConfiguration::AudioDeviceConfigName<2, AudioConfiguration::DIR_IN>::GetSampleRateConfigName()
{
    return "/Audio/soundCard2InSampleRate";
}

template<>
const char* AudioConfiguration::AudioDeviceConfigName<2, AudioConfiguration::DIR_OUT>::GetSampleRateConfigName()
{
    return "/Audio/soundCard2OutSampleRate";
}

void AudioConfiguration::load(wxConfigBase* config)
{
    // Migration -- grab old sample rates from older FreeDV configuration
    int oldSoundCard1SampleRate = config->Read(wxT("/Audio/soundCard1SampleRate"), -1);
    int oldSoundCard2SampleRate = config->Read(wxT("/Audio/soundCard2SampleRate"), -1);
    soundCard1In.sampleRate.setDefaultVal(oldSoundCard1SampleRate);
    soundCard1Out.sampleRate.setDefaultVal(oldSoundCard1SampleRate);
    soundCard2In.sampleRate.setDefaultVal(oldSoundCard2SampleRate);
    soundCard2Out.sampleRate.setDefaultVal(oldSoundCard2SampleRate);
    
    soundCard1In.load(config);
    soundCard1Out.load(config);
    soundCard2In.load(config);
    soundCard2Out.load(config);

    // Glissando uses only the radio's audio: soundCard1In from the radio and
    // soundCard1Out to it. FreeDV's layouts also had a microphone and
    // speaker, and its receive-only layout put the speaker in soundCard1Out.
    // Convert a FreeDV layout once, so that speaker is never taken for the
    // radio.
    if (!config->Read(wxT("/Audio/radioOnlyLayout"), false))
    {
        bool hadSecondCard = soundCard2In.deviceName != "none" || soundCard2Out.deviceName != "none";
        if (!hadSecondCard)
        {
            // Receive only: soundCard1Out was the speaker.
            soundCard1Out.deviceName = "none";
        }
        soundCard2In.deviceName = "none";
        soundCard2Out.deviceName = "none";

        save(config);
    }
}

void AudioConfiguration::save(wxConfigBase* config)
{
    soundCard1In.save(config);
    soundCard1Out.save(config);
    soundCard2In.save(config);
    soundCard2Out.save(config);
    config->Write(wxT("/Audio/radioOnlyLayout"), true);
}
