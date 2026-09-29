//=========================================================================
// Name:            TxRxThread.h
// Purpose:         Implements the main processing thread for audio I/O.
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

#include <chrono>
#include <cstring>
#include <sstream>
using namespace std::chrono_literals;

#include "freedv_sanitizers.h"

// WebRTC uses FS, which is defined in defines.h. Thus, it needs to be included
// first.
#include "AgcStep.h"

// This forces us to use freedv-gui's version rather than another one.
// TBD -- may not be needed once we fully switch over to the audio pipeline.
#include "../defines.h"

#include "TxRxThread.h"
#include "paCallbackData.h"

#include "PlaybackStep.h"
#include "EitherOrStep.h"
#include "RNNoiseStep.h"
#include "ResamplePlotStep.h"
#include "ResampleStep.h"
#include "TapStep.h"
#include "LevelAdjustStep.h"
#include "FreeDVTransmitStep.h"
#include "RecordStep.h"
#include "ComputeRfSpectrumStep.h"
#include "FreeDVReceiveStep.h"
#include "MuteStep.h"
#include "LinkStep.h"
#include "BeepStep.h"
#include "MixStep.h"
#include "TextMessagingModem.h"
#include "TextMessagingReceiveStep.h"
#include "TextMessagingTxQueue.h"

#include "util/logging/ulog.h"
#include "os/os_interface.h"

#include "codec2_alloc.h"

// Experimental options for potential future release:
//
// * ENABLE_FASTER_PLOTS: This uses a faster resampling algorithm to reduce the CPU
//   usage required to generate various plots in the user interface. (Tech note: When
//   enabled, r8brain uses a wider transition band for the plot resampling.)
// * ENABLE_PROCESSING_STATS: This causes execution statistics to be collected for RX and TX
//   processing and output in the log after the user pushes Stop. (Define in .h file.)

#define ENABLE_FASTER_PLOTS

// External globals
// TBD -- work on fully removing the need for these.
extern paCallBackData* g_rxUserdata;
extern std::atomic<int> g_analog;
extern std::atomic<bool> g_half_duplex;
extern std::atomic<bool> g_tx;
extern std::atomic<bool> g_playFileToMicIn;
extern std::atomic<int> g_sfTxFs;
extern std::atomic<bool> g_loopPlayFileToMicIn;
extern std::atomic<float> g_TxFreqOffsetHz;
extern GenericFIFO<short> g_plotSpeechInFifo;
extern GenericFIFO<short> g_plotDemodInFifo;
extern GenericFIFO<short> g_plotSpeechOutFifo;
extern int g_mode;
extern int g_txLevel;
extern std::atomic<float> g_txLevelScale;
extern std::atomic<bool> g_queueResync;
extern int g_resyncs;
extern bool g_recFileFromRadio;
extern std::atomic<unsigned int> g_recFromRadioSamples;
extern std::atomic<bool> g_playFileFromRadio;
extern std::atomic<int> g_sfFs;
extern std::atomic<bool>     g_totBeepActive;
extern std::atomic<bool> g_loopPlayFileFromRadio;
extern int g_SquelchActive;
extern float g_SquelchLevel;
extern GenericFIFO<float> g_avmag;
extern std::atomic<int> g_State;
extern std::atomic<float> g_RxFreqOffsetHz;
extern float g_sig_pwr_av;

#include "../freedv_interface.h"
extern FreeDVInterface freedvInterface;

#include <wx/wx.h>
#include "../main.h"
extern wxWindow* g_parent;

static auto& NonblockingWxGetApp() FREEDV_NONBLOCKING
{
    // Note: wxWidgets implementation of wxGetApp() only returns the App object
    // and performs no other tasks. Verified RT safe as of wxWidgets version 3.3.1.
    FREEDV_BEGIN_VERIFIED_SAFE
    return wxGetApp();
    FREEDV_END_VERIFIED_SAFE
}

#include <sndfile.h>
extern std::atomic<SNDFILE*> g_sfPlayFile;
extern std::atomic<SNDFILE*>            g_sfRecFileFromModulator;
extern std::atomic<bool>                g_recFileFromModulator;
extern SNDFILE* g_sfRecFile;
extern std::atomic<SNDFILE*> g_sfRecMicFile;
extern SNDFILE* g_sfRecDecoderFile;
extern std::atomic<SNDFILE*> g_sfPlayFileFromRadio;

extern std::atomic<bool> g_recFileFromMic;
extern bool g_recFileFromDecoder;


void TxRxThread::initializePipeline_()
{
    if (m_tx)
    {
        // Chat bursts are all the transmit side sends. They come already
        // modulated, and only need the operator's transmit level applied.
        dataTxPipeline_ = std::make_unique<AudioPipeline>(FS, outputSampleRate_);
        dataTxPipeline_->appendPipelineStep(new LevelAdjustStep(FS, +[]() FREEDV_NONBLOCKING {
            return g_txLevelScale.load(std::memory_order_acquire);
        }));
        dataTxSamples_ = std::make_unique<short[]>((FS * FRAME_DURATION_MS) / MS_TO_SEC);
    }
    else
    {
        pipeline_ = std::make_unique<AudioPipeline>(inputSampleRate_, outputSampleRate_);

        auto activeRxPipeline = new AudioPipeline(inputSampleRate_, outputSampleRate_);

        // Record from radio step (optional)
        auto recordRadioStep = new RecordStep(
            RECORD_FILE_SAMPLE_RATE, 
            []() { return g_sfRecFile; }, 
            [](int numSamples) {
                g_recFromRadioSamples.fetch_sub(numSamples, std::memory_order_relaxed);
                if (g_recFromRadioSamples.load(std::memory_order_relaxed) <= 0)
                {
                    // call stop record menu item, should be thread safe
                    g_parent->CallAfter(&MainFrame::StopRecFileFromRadio);
                }
            }
        );
        auto recordRadioPipeline = new AudioPipeline(inputSampleRate_, recordRadioStep->getOutputSampleRate());
        recordRadioPipeline->appendPipelineStep(recordRadioStep);
        
        auto recordRadioTap = new TapStep(inputSampleRate_, recordRadioPipeline);
        auto bypassRecordRadio = new AudioPipeline(inputSampleRate_, inputSampleRate_);
        
        auto eitherOrRecordRadio = new EitherOrStep(
            +[]() FREEDV_NONBLOCKING { return g_recFileFromRadio && (g_sfRecFile != NULL); },
            recordRadioTap,
            bypassRecordRadio
        );
        activeRxPipeline->appendPipelineStep(eitherOrRecordRadio);
        
        // Play from radio step (optional)
        auto eitherOrBypassPlayRadio = new AudioPipeline(inputSampleRate_, inputSampleRate_);
        auto eitherOrPlayRadio = new AudioPipeline(inputSampleRate_, inputSampleRate_);
        auto playRadio = new PlaybackStep(
            inputSampleRate_, 
            []() { return g_sfFs.load(std::memory_order_acquire); },
            []() { return g_playFileFromRadio.load(std::memory_order_acquire) ? g_sfPlayFileFromRadio.load(std::memory_order_acquire) : nullptr; },
            []() {
                if (g_loopPlayFileFromRadio.load(std::memory_order_relaxed))
                    sf_seek(g_sfPlayFileFromRadio.load(std::memory_order_acquire), 0, SEEK_SET);
                else {
                    log_info("playFileFromRadio finished, issuing event!");
                    ((MainFrame*)g_parent)->executeOnUiThreadAndWait_([]() { ((MainFrame*)g_parent)->StopPlaybackFileFromRadio();});
                }
            }
        );
        eitherOrPlayRadio->appendPipelineStep(playRadio);
        
        auto eitherOrPlayRadioStep = new EitherOrStep(
            +[]() FREEDV_NONBLOCKING {
                auto result = g_playFileFromRadio.load(std::memory_order_acquire) && (g_sfPlayFileFromRadio.load(std::memory_order_acquire) != NULL);
                return result;
            },
            eitherOrPlayRadio,
            eitherOrBypassPlayRadio);
        activeRxPipeline->appendPipelineStep(eitherOrPlayRadioStep);

        // Text messaging data demodulation. This is a tap so the DATAC13 and
        // DATAC4 demodulators run on the tap's own thread, leaving the voice
        // path untouched whether or not anyone is chatting.
        auto textMessagingPipeline = new AudioPipeline(inputSampleRate_, FS);
        textMessagingPipeline->appendPipelineStep(new TextMessagingReceiveStep(&textMessagingModem()));
        auto textMessagingTap = new TapStep(inputSampleRate_, textMessagingPipeline);
        activeRxPipeline->appendPipelineStep(textMessagingTap);

        // Resample for plot step (demod in)
        auto resampleForPlotStep = new ResampleForPlotStep(&g_plotDemodInFifo);
        auto resampleForPlotPipeline = new AudioPipeline(inputSampleRate_, resampleForPlotStep->getOutputSampleRate());
#if defined(ENABLE_FASTER_PLOTS)
        auto resampleForPlotResampler = new ResampleStep(inputSampleRate_, resampleForPlotStep->getInputSampleRate(), true); // need to create manually to get access to "plot only" optimizations
        resampleForPlotPipeline->appendPipelineStep(resampleForPlotResampler);
#endif // defined(ENABLE_FASTER_PLOTS)
        resampleForPlotPipeline->appendPipelineStep(resampleForPlotStep);

        auto resampleForPlotTap = new TapStep(inputSampleRate_, resampleForPlotPipeline);
        activeRxPipeline->appendPipelineStep(resampleForPlotTap);

        // RF spectrum computation step
        auto computeRfSpectrumStep = new ComputeRfSpectrumStep(
            +[]() FREEDV_NONBLOCKING { return freedvInterface.getCurrentRxModemStats(); },
            +[]() FREEDV_NONBLOCKING { return &g_avmag; }
        );
        auto computeRfSpectrumPipeline = new AudioPipeline(
            inputSampleRate_, computeRfSpectrumStep->getOutputSampleRate());
#if defined(ENABLE_FASTER_PLOTS)
        auto resampleForRfSpectrum = new ResampleStep(inputSampleRate_, computeRfSpectrumStep->getInputSampleRate(), true); // need to create manually to get access to "plot only" optimizations
        computeRfSpectrumPipeline->appendPipelineStep(resampleForRfSpectrum);
#endif // defined(ENABLE_FASTER_PLOTS)
        computeRfSpectrumPipeline->appendPipelineStep(computeRfSpectrumStep);
        
        auto computeRfSpectrumTap = new TapStep(inputSampleRate_, computeRfSpectrumPipeline);
        activeRxPipeline->appendPipelineStep(computeRfSpectrumTap);
        
        // RX demodulation step
        auto bypassRfDemodulationPipeline = new AudioPipeline(inputSampleRate_, outputSampleRate_);
        auto rfDemodulationPipeline = new AudioPipeline(inputSampleRate_, outputSampleRate_);
        auto rfDemodulationStep = freedvInterface.createReceivePipeline(
            inputSampleRate_, outputSampleRate_,
            +[]() FREEDV_NONBLOCKING { return &g_State; },
            +[]() FREEDV_NONBLOCKING { return g_RxFreqOffsetHz.load(std::memory_order_relaxed); },
            +[]() FREEDV_NONBLOCKING { return &g_sig_pwr_av; },
            helper_
        );
        rfDemodulationPipeline->appendPipelineStep(rfDemodulationStep);

        // Resample for plot step (speech out)
        auto resampleForPlotOutStep = new ResampleForPlotStep(&g_plotSpeechOutFifo);
        auto resampleForPlotOutPipeline = new AudioPipeline(outputSampleRate_, resampleForPlotOutStep->getOutputSampleRate());
#if defined(ENABLE_FASTER_PLOTS)
        auto resampleForPlotOutResampler = new ResampleStep(outputSampleRate_, resampleForPlotOutStep->getInputSampleRate(), true); // need to create manually to get access to "plot only" optimizations
        resampleForPlotOutPipeline->appendPipelineStep(resampleForPlotOutResampler);
#endif // defined(ENABLE_FASTER_PLOTS)
        resampleForPlotOutPipeline->appendPipelineStep(resampleForPlotOutStep);

        auto resampleForPlotOutTap = new TapStep(outputSampleRate_, resampleForPlotOutPipeline);
        rfDemodulationPipeline->appendPipelineStep(resampleForPlotOutTap);
        
        auto eitherOrRfDemodulationStep = new EitherOrStep(
            +[]() FREEDV_NONBLOCKING { return g_analog.load(std::memory_order_relaxed) != 0; },
            bypassRfDemodulationPipeline,
            rfDemodulationPipeline);

        activeRxPipeline->appendPipelineStep(eitherOrRfDemodulationStep);

        // Record from decoder step (optional)
        auto recordDecoderStep = new RecordStep(
            outputSampleRate_, 
            []() { return g_sfRecDecoderFile; }, 
            [](int) {
                // Recording stops when the user explicitly tells us to,
                // no action required here.
            }
        );
        auto recordDecoderPipeline = new AudioPipeline(outputSampleRate_, outputSampleRate_);
        recordDecoderPipeline->appendPipelineStep(recordDecoderStep);
        
        auto recordDecoderTap = new TapStep(outputSampleRate_, recordDecoderPipeline);
        auto bypassRecordDecoder = new AudioPipeline(outputSampleRate_, outputSampleRate_);
        
        auto eitherOrRecordDecoder = new EitherOrStep(
            +[]() FREEDV_NONBLOCKING { return (g_recFileFromDecoder) && (g_sfRecDecoderFile != NULL); },
            recordDecoderTap,
            bypassRecordDecoder
        );
        activeRxPipeline->appendPipelineStep(eitherOrRecordDecoder);

        auto activeRxMutePipeline = new AudioPipeline(inputSampleRate_, outputSampleRate_);
        auto activeRxMuteStep = new MuteStep(inputSampleRate_, outputSampleRate_);
        activeRxMutePipeline->appendPipelineStep(activeRxMuteStep);

        // TOT beep step: emits a warning beep during countdown
        auto totBeepBypass = new AudioPipeline(inputSampleRate_, outputSampleRate_);
        auto totBeepMuteStep = new MuteStep(inputSampleRate_, outputSampleRate_);
        totBeepBypass->appendPipelineStep(totBeepMuteStep);

        auto totBeepActivePath = new AudioPipeline(inputSampleRate_, outputSampleRate_);
        beepStep_ = new BeepStep(
            outputSampleRate_, 750, 80, 5, 
            +[]() FREEDV_NONBLOCKING {
                return g_totBeepActive.load(std::memory_order_acquire);
            },
            +[](BeepStep& thisStep) FREEDV_NONBLOCKING {
                g_totBeepActive.store(false, std::memory_order_release);
                thisStep.reset();
            }
        );
        totBeepActivePath->appendPipelineStep(beepStep_);
        auto totBeepEitherOr = new EitherOrStep(
            +[]() FREEDV_NONBLOCKING {
                return g_totBeepActive.load(std::memory_order_acquire);
            },
            totBeepActivePath,
            totBeepBypass
        );

        auto activeRxEitherOr = new EitherOrStep(
            +[]() FREEDV_NONBLOCKING {
                bool tmpTx = g_tx.load(std::memory_order_acquire);
                bool tmpHalfDuplex = g_half_duplex.load(std::memory_order_acquire);
                return
                    (tmpTx && NonblockingWxGetApp().appConfiguration.monitorTxAudio.getWithoutProcessing()) ||
                    (tmpHalfDuplex && !tmpTx) || !tmpHalfDuplex;
            },
            activeRxPipeline,
            activeRxMutePipeline
        );
        
        auto totMixStep = new MixStep(
            activeRxEitherOr,
            totBeepEitherOr
        );
        pipeline_->appendPipelineStep(totMixStep);
       
        // Clear anything in the FIFO before resuming decode.
        clearFifos_();
    }
}

void* TxRxThread::Entry() noexcept
{
    // Get raw pointer so we don't need to constantly access the shared_ptr
    // and thus constantly increment/decrement refcounts.
    IRealtimeHelper* helper = helper_.get();

    // Ensure that O(1) memory allocator is used for Codec2
    // instead of standard malloc().
    codec2_initialize_realtime(CODEC2_REAL_TIME_MEMORY_SIZE);
    
    initializePipeline_();
    
    // Request real-time scheduling from the operating system.
    helper->setHelperRealTime();

#if defined(ENABLE_PROCESSING_STATS)
    processingStats_.reset();
    waitStats_.reset();
#endif // defined(ENABLE_PROCESSING_STATS)

    // Set thread name for debugging
    const char* threadName = nullptr;
    if (m_tx) threadName = "txThread";
    else threadName = "rxThread";
    SetThreadName(threadName);

    // Make sure we don't start processing until
    // the main thread is ready.
    readySem_.signal();
    startSem_.wait();
    clearFifos_();

    while (m_run.load(std::memory_order_acquire))
    {
        if (!m_run.load(std::memory_order_acquire)) break;

#if defined(ENABLE_PROCESSING_STATS)
        // Closes out the interval opened by waitStats_.start() below, at
        // the end of the *previous* iteration -- i.e. how long this thread
        // actually spent inside stopRealTimeWork()'s semaphore wait (a
        // scheduling delay would show up here even if processing itself,
        // measured separately in txProcessing_/rxProcessing_, looks fine).
        waitStats_.end();
#endif // defined(ENABLE_PROCESSING_STATS)

        //log_info("thread woken up: m_tx=%d", (int)m_tx);
        helper->startRealTimeWork();

        if (m_tx) txProcessing_(helper);
        else rxProcessing_(helper);

        // Determine whether we need to pause for a shorter amount
        // of time to avoid dropouts.
        // The transmit side hurries while the radio's output FIFO runs low.
        // The receive side has no output to keep fed and drains all its
        // input each time round, so it just waits for more.
        bool fastMode = false;
        if (m_tx)
        {
            auto outFifo = g_rxUserdata->outfifo1;
            fastMode = outFifo->numUsed() < outFifo->capacity() / 2;
        }

#if defined(ENABLE_PROCESSING_STATS)
        waitStats_.start();
#endif // defined(ENABLE_PROCESSING_STATS)
        helper->stopRealTimeWork(fastMode);
    }

#if defined(ENABLE_PROCESSING_STATS)
    processingStats_.report(m_tx, "processing");
    waitStats_.report(m_tx, "wait");
#endif // defined(ENABLE_PROCESSING_STATS)

    // Force pipeline to delete itself when we're done with the thread.
    pipeline_ = nullptr;
    
    // Return to normal scheduling
    helper->clearHelperRealTime();
    
    codec2_disable_realtime();
    
    return NULL;
}

#if defined(ENABLE_PROCESSING_STATS)
void TxRxThread::TimingStats::reset()
{
    started = false;
    numSamples = 0;
    minDuration = 1e9;
    maxDuration = 0;
    sumDuration = 0;
    sumDoubleDuration = 0;
    memset(histogramCounts, 0, sizeof(histogramCounts));
}

void TxRxThread::TimingStats::start()
{
    started = true;
    timeStart = std::chrono::high_resolution_clock::now();
}

void TxRxThread::TimingStats::end()
{
    if (!started) return;
    started = false;

    auto e = std::chrono::high_resolution_clock::now();
    auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(e - timeStart).count();
    numSamples++;
    if (d < minDuration)
    {
        minDuration = d;
        minTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    }
    if (d > maxDuration)
    {
        maxDuration = d;
        maxTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    }
    sumDuration += d; sumDoubleDuration += pow(d, 2);

    double dMs = d / 1.0e6;
    int bucket = 0;
    while (bucket < NUM_HISTOGRAM_BUCKETS - 1 && dMs >= HISTOGRAM_BUCKET_BOUNDS_MS[bucket])
    {
        bucket++;
    }
    histogramCounts[bucket]++;
}

void TxRxThread::TimingStats::report(bool m_tx, const char* label) const
{
    if (numSamples > 0)
    {
        // localtime() isn't thread-safe (shared static buffer); use the
        // reentrant variant since TX and RX threads can call this concurrently.
        std::tm minTm{};
        std::tm maxTm{};
#if defined(_WIN32)
        localtime_s(&minTm, &minTime);
        localtime_s(&maxTm, &maxTime);
#else
        localtime_r(&minTime, &minTm);
        localtime_r(&maxTime, &maxTm);
#endif // defined(_WIN32)
        char bufMin[32];
        char bufMax[32];
        std::strftime(bufMin, 32, "%H:%M:%S", &minTm);
        std::strftime(bufMax, 32, "%H:%M:%S", &maxTm);

        log_info("m_tx = %d, %s: min = %f ns [%s], max = %f ns [%s], mean = %f ns, stdev = %f ns (n = %d)", m_tx, label, minDuration, bufMin, maxDuration, bufMax, sumDuration / numSamples, sqrt((sumDoubleDuration - pow(sumDuration, 2)/numSamples) / (numSamples - 1)), numSamples);

        // Histogram bucketed by upper bound in ms. Distinguishes "one freak
        // outlier" (a lone hit in a high bucket) from "a real cluster of
        // slow samples" (many hits spread across the middle/high buckets)
        // -- neither is visible in the min/max/mean/stdev line above once
        // averaged over a large n.
        std::stringstream histSs;
        for (int i = 0; i < NUM_HISTOGRAM_BUCKETS; i++)
        {
            if (i > 0) histSs << " ";
            if (i < NUM_HISTOGRAM_BUCKETS - 1)
            {
                histSs << "<" << HISTOGRAM_BUCKET_BOUNDS_MS[i] << "ms:" << histogramCounts[i];
            }
            else
            {
                histSs << ">=" << HISTOGRAM_BUCKET_BOUNDS_MS[i - 1] << "ms:" << histogramCounts[i];
            }
        }
        log_info("m_tx = %d, %s histogram: %s", m_tx, label, histSs.str().c_str());
    }
}
#endif // defined(ENABLE_PROCESSING_STATS)

void TxRxThread::clearFifos_() FREEDV_NONBLOCKING
{
    paCallBackData  *cbData = g_rxUserdata;
    
    if (m_tx)
    {
        cbData->outfifo1->reset();
    }
    else
    {
        cbData->infifo1->reset();
    }
}

//---------------------------------------------------------------------------------------------
// Main real time processing for tx and rx of FreeDV signals, run in its own threads
//---------------------------------------------------------------------------------------------

bool TxRxThread::transmitTextMessagingAudio_(IRealtimeHelper* helper) FREEDV_NONBLOCKING
{
    auto& queue = textMessagingTxQueue();
    if (dataTxPipeline_ == nullptr) return false;

    paCallBackData* cbData = g_rxUserdata;
    const int nsamIn = (FS * FRAME_DURATION_MS) / MS_TO_SEC;
    const int nsamOut = (nsamIn * outputSampleRate_) / FS;

    if (!queue.ownsTransmitter() && !dataTxInProgress_) return false;

    // The transport can conclude a burst without us: its watchdog clears the
    // queue and the transmitting flag itself. Follow it, because otherwise
    // this latch stays set, the next burst never gets its transmitting flag
    // raised, and the transport unkeys it early and clips it off the air.
    if (dataTxInProgress_ && !queue.isTransmitting() && queue.isEmpty())
    {
        dataTxInProgress_ = false;
    }

    // PTT takes a moment to engage; pushing samples at the radio before it
    // does would clip the front of the burst, so wait for it.
    if (!g_tx.load(std::memory_order_acquire)) return true;

    if (!dataTxInProgress_ && !queue.isEmpty())
    {
        dataTxInProgress_ = true;
        queue.setTransmitting(true);
    }

    int nout = 0;
    while (!helper->mustStopWork() && queue.numUsed() > 0 && cbData->outfifo1->numFree() >= nsamOut)
    {
        int numRead = queue.read(dataTxSamples_.get(), nsamIn);
        if (numRead <= 0) break;

        auto outputSamples = dataTxPipeline_->execute(dataTxSamples_.get(), numRead, &nout);
        if (outputSamples != nullptr && nout > 0)
        {
            if (cbData->outfifo1->write(outputSamples, nout) != 0)
            {
                FREEDV_BEGIN_VERIFIED_SAFE
                log_warn("TX outfifo1 full, dropped %d text messaging samples (free=%d)", nout, cbData->outfifo1->numFree());
                FREEDV_END_VERIFIED_SAFE
            }
        }
    }

    // Hold the transmitter until the sound card has actually played out the
    // burst; unkeying earlier would clip the last frame off the air.
    if (queue.isEmpty() && cbData->outfifo1->numUsed() <= nsamOut)
    {
        dataTxInProgress_ = false;
        queue.setTransmitting(false);
    }

    return true;
}

void TxRxThread::txProcessing_(IRealtimeHelper* helper) FREEDV_NONBLOCKING
{
    // Chat bursts are all Glissando transmits. There is no microphone, so
    // between bursts, and whenever the radio is keyed without one (Tune plays
    // its own tone in the sound card callback), the radio gets silence.
    transmitTextMessagingAudio_(helper);
}

void TxRxThread::rxProcessing_(IRealtimeHelper* helper) FREEDV_NONBLOCKING
{
    paCallBackData  *cbData = g_rxUserdata;

    // Buffers reused by tx and rx processing.  We take samples from
    // the sound card, and resample them for the freedv modem input
    // sample rate.  Typically the sound card is running at 48 or 44.1
    // kHz, and the modem at 8kHz.

    //
    //  RX side processing --------------------------------------------
    //
    
    if (g_queueResync.load(std::memory_order_acquire))
    {
        g_queueResync.store(false, std::memory_order_release);
        freedvInterface.setSync(FREEDV_SYNC_UNSYNC);
        g_resyncs++;
    }

    // Make sure we reset 
    if (!g_totBeepActive.load(std::memory_order_acquire))
    {
        beepStep_->reset();
    }
    
    // Attempt to read one processing frame (about 20ms) of receive samples,  we 
    // keep this frame duration constant across modes and sound card sample rates
    int nsam = (inputSampleRate_ * FRAME_DURATION_MS) / MS_TO_SEC;
    assert(nsam > 0);

    int             nout;

    // While enough input samples are available. The pipeline feeds the chat
    // receiver and the displays from taps along the way; what comes out of
    // its end is decoded audio for a speaker, which Glissando doesn't have.
    while (!helper->mustStopWork()) {
        if (cbData->infifo1->read(inputSamples_.get(), nsam) != 0)
        {
            break;
        }

#if defined(ENABLE_PROCESSING_STATS)
        processingStats_.start();
#endif // defined(ENABLE_PROCESSING_STATS)
        
        // send latest squelch level to FreeDV API, as it handles squelch internally
        freedvInterface.setSquelch(g_SquelchActive, g_SquelchLevel);

        pipeline_->execute(inputSamples_.get(), nsam, &nout);

#if defined(ENABLE_PROCESSING_STATS)
        processingStats_.end();
#endif // defined(ENABLE_PROCESSING_STATS)
    }
}
