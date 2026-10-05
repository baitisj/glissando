//=========================================================================
// Name:            SmokeGauge.h
// Purpose:         How much smoke the visi-scope lets off on a long keying.
//
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

#ifndef SMOKE_GAUGE_H
#define SMOKE_GAUGE_H

#include <algorithm>
#include <cmath>

// An easter egg: a transmitter held keyed near full power for most of the
// time-out timer starts to smoke. The smoke starts two thirds of the way to
// the time-out (120 s of 180), is at its thickest eight ninths of the way
// (160 s, where chat lets go of a keying anyway), and clears over a while
// once the radio is let go or turned down. Its level runs from 0 (none) to 1.
class SmokeGauge
{
public:
    static constexpr double POWER_OVER = 0.8;   // RFPOWER, as Hamlib scales it 0 to 1
    static constexpr double START = 2.0 / 3.0;  // of the time-out
    static constexpr double FULL = 8.0 / 9.0;
    static constexpr double CLEAR_SECONDS = 20.0;

    // now: any steady clock, in seconds. keyedSeconds: how long the radio
    // has been keyed without a break (0 while it isn't). rfPower: the
    // radio's power setting, NaN when it can't be told; such a radio never
    // smokes.
    double update(double now, double keyedSeconds, double rfPower, int timeOutSeconds)
    {
        double dt = std::isnan(lastNow_) ? 0.0 : std::max(0.0, now - lastNow_);
        lastNow_ = now;

        double target = 0.0;
        if (keyedSeconds > 0.0 && rfPower > POWER_OVER && timeOutSeconds > 0)
        {
            double start = START * timeOutSeconds;
            double full = FULL * timeOutSeconds;
            target = std::clamp((keyedSeconds - start) / (full - start), 0.0, 1.0);
        }

        // Thickens as fast as the keying goes on; clears slowly.
        if (target >= level_)
            level_ = target;
        else
            level_ = std::max(target, level_ - dt / CLEAR_SECONDS);
        return level_;
    }

    double level() const { return level_; }

private:
    double level_ = 0.0;
    double lastNow_ = NAN;
};

#endif // SMOKE_GAUGE_H
