//=========================================================================
// Name:            ComputeRfSpectrumStep.h
// Purpose:         Describes a RF spectrum computation step step in the audio pipeline.
//
// Authors:         Mooneer Salem
// License:
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//=========================================================================

#ifndef AUDIO_PIPELINE__COMPUTE_RF_SPECTRUM_STEP_H
#define AUDIO_PIPELINE__COMPUTE_RF_SPECTRUM_STEP_H

#include <memory>
#include <functional>
#include <vector>

#include "modem_stats.h" // MODEM_STATS_NSPEC, the width every display uses
#include "GlissandoFft.h"
#include "IPipelineStep.h"
#include "../util/realtime_fp.h"
#include "../util/GenericFIFO.h"

class ComputeRfSpectrumStep : public IPipelineStep
{
public:
    // Note: only supports 8 kHz, so needs to be inserted into an AudioPipeline
    // in order to downconvert properly.
    //
    // The waterfall's own spectrum of the radio input: MODEM_STATS_NSPEC
    // bins from 0 to 4 kHz in dB, from the same Hann windowed
    // 1024 point FFT over the latest samples that codec2's
    // modem_stats_get_rx_spectrum() used, scaled the same way. It used to take
    // its FFT state from the voice modem.
    explicit ComputeRfSpectrumStep(realtime_fp<GenericFIFO<float>*()> const& getAvMagFn);
    virtual ~ComputeRfSpectrumStep();
    
    virtual int getInputSampleRate() const FREEDV_NONBLOCKING override;
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING override;
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING override;
    
private:
    static constexpr int FFT_SIZE = 2 * MODEM_STATS_NSPEC;

    realtime_fp<GenericFIFO<float>*()> getAvMagFn_;
    Glissando::Fft fft_;
    std::vector<float> window_;     // Hann
    std::vector<float> history_;    // the latest FFT_SIZE samples, oldest first
    std::vector<double> fftData_;   // interleaved re, im
    std::vector<float> rxSpectrum_;
};

#endif // AUDIO_PIPELINE__COMPUTE_RF_SPECTRUM_STEP_H
