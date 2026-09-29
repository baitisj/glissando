//==========================================================================
// Name:            defines.h
// Purpose:         Definitions used by plots derived from plot class.
// Created:         August 27, 2012
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
#ifndef __FDMDV2_DEFINES__
#define __FDMDV2_DEFINES__

#include "wx/wx.h"
#include "logging/ulog.h"
#include "modem_stats.h" // MODEM_STATS_NSPEC, the width of the spectrum

// Spectrogram and Waterfall

#define MIN_MAG_DB        -40.0     // min of spectrogram/waterfall magnitude axis
#define MAX_MAG_DB          0.0     // max of spectrogram/waterfall magnitude axis
#define STEP_MAG_DB         5.0     // magnitude axis step
#define BETA                0.95    // constant for time averaging spectrum data
#define MIN_F_HZ            0       // min freq on Waterfall and Spectrum
#define MAX_F_HZ            3000    // max freq on Waterfall and Spectrum
#define STEP_F_HZ           500     // major (e.g. text legend) freq step on Waterfall and Spectrum graticule
#define STEP_MINOR_F_HZ     100     // minor (ticks) freq step on Waterfall and Spectrum graticule
#define WATERFALL_SECS_Y    30      // number of seconds represented by y axis of waterfall
#define WATERFALL_SECS_STEP 5       // graticule y axis steps of waterfall
#define DT                  0.10    // time between real time graphing updates
#define FS                  8000    // chat receiver and spectrum sample rate

// Waveform plotting constants

#define WAVEFORM_PLOT_FS    400                            // sample rate (points/s) of waveform plotted to screen
#define WAVEFORM_PLOT_TIME  5                              // length or entire waveform on screen
#define WAVEFORM_PLOT_BUF   ((int)(DT*WAVEFORM_PLOT_FS))   // number of new samples we plot per DT

// sample rate I/O & conversion constants

#define SAMPLE_RATE         48000                          // 48 kHz sampling rate rec. as we can trust accuracy of sound card
#define N8                  160                            // processing buffer size at 8 kHz
#define N48                 (N8*SAMPLE_RATE/FS)            // processing buffer size at 48 kHz
#define NUM_CHANNELS        2                              // I think most sound cards prefer stereo we will convert to mono
#define VOX_TONE_FREQ       1000.0                         // optional left channel vox tone freq
#define VOX_TONE_AMP        30000                          // optional left channel vox tone amp
#define TUNE_TONE_FREQ      1500.0                         // carrier the Tune button sends
#define FIFO_SIZE           640                            // default fifo size in ms
#define FRAME_DURATION_MS   20                             // default frame length of 20 mS = 0.02 seconds
#define MS_TO_SEC           1000
#define RECORD_FILE_SAMPLE_RATE (SAMPLE_RATE)

// Level Gauge
#define FROM_RADIO_MAX       0.8
#define FROM_MIC_MAX         0.8
#define LEVEL_BETA           0.99

// TX Attenuation (0.1 dB increments)
#define TX_ATTENUATION_MIN (-300) /* -30 dB */
#define TX_ATTENUATION_MAX (0)
#define TX_ATTENUATION_DEFAULT TX_ATTENUATION_MAX
#define TX_ATTENUATION_SMALL_STEP (2) /* 0.2 dB */
#define TX_ATTENUATION_LARGE_STEP (10) /* 1 dB */

// Real-time memory block size
#define CODEC2_REAL_TIME_MEMORY_SIZE (512*1024)
   
enum
{
    ID_ROTATE_LEFT = wxID_HIGHEST + 1,
    ID_ROTATE_RIGHT,
    ID_RESIZE,
    ID_PAINT_BG
};

#endif  //__FDMDV2_DEFINES__
