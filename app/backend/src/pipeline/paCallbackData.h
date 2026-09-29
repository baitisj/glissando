#ifndef AUDIO_PIPELINE_PA_CALLBACK_DATA_H
#define AUDIO_PIPELINE_PA_CALLBACK_DATA_H

#include "../util/GenericFIFO.h"
#include "../util/audio_spin_mutex.h"

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
// paCallBackData
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=
typedef struct paCallBackData
{
    paCallBackData()
        : infifo1(nullptr)
        , outfifo1(nullptr)
        , infifo2(nullptr)
        , outfifo2(nullptr)
        , leftChannelVoxTone(false)
        , voxTonePhase(0.0)
        , isTuning(false)
        , tuneSineWaveSampleNumber(0)
    {
        // empty
    }

    // FIFOs attached to first sound card
    GenericFIFO<short>    *infifo1;
    GenericFIFO<short>    *outfifo1;

    // FIFOs attached to second sound card
    GenericFIFO<short>    *infifo2;
    GenericFIFO<short>    *outfifo2;

    // optional loud tone on left channel to reliably trigger vox
    std::atomic<bool> leftChannelVoxTone;
    float             voxTonePhase;

    // Temporary buffers for reading and writing
    std::unique_ptr<short[]> tmpReadRxBuffer_;
    std::unique_ptr<short[]> tmpReadTxBuffer_;
    std::unique_ptr<short[]> tmpWriteRxBuffer_;
    std::unique_ptr<short[]> tmpWriteTxBuffer_;

    // Tune state
    std::atomic<bool> isTuning;
    std::atomic<int> tuneSineWaveSampleNumber;
} paCallBackData;

#endif // AUDIO_PIPELINE_PA_CALLBACK_DATA_H
