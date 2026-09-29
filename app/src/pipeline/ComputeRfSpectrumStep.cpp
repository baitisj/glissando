//=========================================================================
// Name:            ComputeRfSpectrumStep.cpp
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

#include <algorithm>
#include <cmath>

#include "ComputeRfSpectrumStep.h"
#include "../defines.h"

ComputeRfSpectrumStep::ComputeRfSpectrumStep(realtime_fp<GenericFIFO<float>*()> const& getAvMagFn)
    : getAvMagFn_(getAvMagFn)
    , fft_(FFT_SIZE)
    , window_(FFT_SIZE)
    , history_(FFT_SIZE, 0.0f)
    , fftData_(2 * FFT_SIZE)
    , rxSpectrum_(MODEM_STATS_NSPEC)
{
    for (int i = 0; i < FFT_SIZE; i++)
    {
        window_[i] = 0.5f - 0.5f * cosf((float)i * 2.0f * (float)M_PI / FFT_SIZE);
    }
}

ComputeRfSpectrumStep::~ComputeRfSpectrumStep() = default;

int ComputeRfSpectrumStep::getInputSampleRate() const FREEDV_NONBLOCKING
{
    return FS;
}

int ComputeRfSpectrumStep::getOutputSampleRate() const FREEDV_NONBLOCKING
{
    return FS;
}

short* ComputeRfSpectrumStep::execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
{
    // Nothing below allocates: every buffer was sized in the constructor.
    FREEDV_BEGIN_VERIFIED_SAFE

    // Slide the newest samples into the history. A block longer than the
    // FFT keeps only its tail.
    int keep = std::max(0, FFT_SIZE - numInputSamples);
    int skip = std::max(0, numInputSamples - FFT_SIZE);
    std::copy(history_.begin() + (FFT_SIZE - keep), history_.end(), history_.begin());
    for (int i = skip; i < numInputSamples; i++)
    {
        history_[keep + (i - skip)] = inputSamples[i];
    }

    for (int i = 0; i < FFT_SIZE; i++)
    {
        fftData_[2 * i] = history_[i] * window_[i];
        fftData_[2 * i + 1] = 0.0;
    }
    fft_.forward(fftData_.data());

    // Scaled exactly as codec2's plot was, so the waterfall's colours don't
    // move: 0 dB is MODEM_STATS_NSPEC times its nominal 16 bit modem level.
    constexpr float FULL_SCALE_DB = 112.514478f; // 20 log10(512 * 825)
    for (int i = 0; i < MODEM_STATS_NSPEC; i++)
    {
        double re = fftData_[2 * i];
        double im = fftData_[2 * i + 1];
        rxSpectrum_[i] = 10.0f * log10f((float)(re * re + im * im) + 1E-12f) - FULL_SCALE_DB;
    }

    FREEDV_END_VERIFIED_SAFE

    auto avMagPtr = getAvMagFn_();
    avMagPtr->write(rxSpectrum_.data(), MODEM_STATS_NSPEC);
    
    // Tap only, no output.
    *numOutputSamples = 0;

    return nullptr;
}
