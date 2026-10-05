//=========================================================================
// Name:            IRigTransmitMeters.h
// Purpose:         Interface for reading a radio's SWR and ALC meters.
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

#ifndef I_RIG_TRANSMIT_METERS_H
#define I_RIG_TRANSMIT_METERS_H

#include "EventHandler.h"

class IRigTransmitMeters
{
public:
    virtual ~IRigTransmitMeters() = default;

    // A reading as the radio's meter gives it (1.0 is a perfect match).
    EventHandler<IRigTransmitMeters*, double> onSwrReading;

    // The ALC as Hamlib scales it, 0 to 1 (on Icoms, the ALC zone of the
    // radio's meter).
    EventHandler<IRigTransmitMeters*, double> onAlcReading;

    // Whether the radio connected now says it can report SWR, or ALC.
    virtual bool canReadSwr() = 0;
    virtual bool canReadAlc() = 0;

    // Asks the radio for the meters wanted; each answer comes back on its
    // event. Meant for while transmitting: a request made while one is still
    // waiting for the radio is dropped rather than queued behind it.
    virtual void requestMeters(bool swr, bool alc) = 0;

protected:
    IRigTransmitMeters() = default;
};

#endif // I_RIG_TRANSMIT_METERS_H
