//=========================================================================
// Name:            IRigSwrMeter.h
// Purpose:         Interface for reading a radio's SWR meter.
//
// License:
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU Lesser General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//=========================================================================

#ifndef I_RIG_SWR_METER_H
#define I_RIG_SWR_METER_H

#include "EventHandler.h"

class IRigSwrMeter
{
public:
    virtual ~IRigSwrMeter() = default;

    // A reading as the radio's meter gives it (1.0 is a perfect match).
    EventHandler<IRigSwrMeter*, double> onSwrReading;

    // Whether the radio connected now says it can report SWR.
    virtual bool canReadSwr() = 0;

    // Asks the radio for its SWR; the answer comes back on onSwrReading.
    // Meant for while transmitting: a request made while one is still
    // waiting for the radio is dropped rather than queued behind it.
    virtual void requestSwr() = 0;

protected:
    IRigSwrMeter() = default;
};

#endif // I_RIG_SWR_METER_H
