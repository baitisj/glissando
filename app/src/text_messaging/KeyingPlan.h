//=========================================================================
// Name:            KeyingPlan.h
// Purpose:         Cuts a chat keying that would outlast the transmit
//                  time-out timer into several shorter ones, only ever
//                  between whole modem frames.
//
// Written for Glissando. At Adagio one frame is 55 s and a single text
// fragment is six of them, so a keying held for all of it runs past the
// usual 180 s time-out and has a frame cut in half. Letting up on the
// transmitter between frames restarts the timer (the app's and the rig's)
// without losing anything: every Glissando frame carries its own sync, and
// the far end reassembles segments by order, not by timing.
//=========================================================================

#ifndef TEXT_MESSAGING__KEYING_PLAN_H
#define TEXT_MESSAGING__KEYING_PLAN_H

#include <cstddef>
#include <vector>

namespace TextMessaging
{

// frameEnds holds where each modem frame of the keying ends, as ascending
// sample offsets; the last one is the length of the keying. Returns where
// each separate keying ends, taking as many whole frames into each as fit in
// maxSamples. A frame longer than maxSamples on its own still goes out whole,
// in a keying of its own: splitting it would lose it. maxSamples of zero
// means no limit, and the result is the one keying.
std::vector<size_t> splitKeying(const std::vector<size_t>& frameEnds, size_t maxSamples);

} // namespace TextMessaging

#endif // TEXT_MESSAGING__KEYING_PLAN_H
