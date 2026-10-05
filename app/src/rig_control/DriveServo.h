//=========================================================================
// Name:            DriveServo.h
// Purpose:         Turns the console's DRIVE knob down from the radio's ALC.
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

#ifndef DRIVE_SERVO_H
#define DRIVE_SERVO_H

// With DRIVE pushed in, the radio's ALC reading steers the transmit audio
// level. It only ever turns the level down: never louder than where the knob
// was when it was pushed in (the ceiling), and never back up on its own, so
// the operator sees the knob move one way only. Levels are in tenths of a dB
// of attenuation, as the transmit level is kept (0 is full scale).
class DriveServo
{
public:
    // Two readings in a row over the target cut this much; one reading far
    // over it cuts the bigger step at once.
    static constexpr int SMALL_CUT = 5;     // 0.5 dB
    static constexpr int BIG_CUT = 20;      // 2 dB
    static constexpr double FAR_OVER = 0.8;
    static constexpr int READINGS_TO_CUT = 2;

    DriveServo(int floor, double target) : floor_(floor), target_(target) {}

    void setTarget(double target) { target_ = target; }
    double target() const { return target_; }

    // A new keying: a reading over the target on the last one doesn't count
    // toward a cut on this one.
    void restart() { over_ = 0; }

    // One ALC reading, as Hamlib gives it (0 to 1), taken while transmitting
    // at level. Returns the level to transmit at from now on: level or lower.
    int reading(double alc, int level)
    {
        if (alc > FAR_OVER)
        {
            over_ = 0;
            return cut(level, BIG_CUT);
        }
        if (!(alc > target_))
        {
            // At the target or under it: where it should sit. A single
            // jittery reading over it doesn't move the knob.
            over_ = 0;
            return level;
        }
        if (++over_ < READINGS_TO_CUT) return level;
        over_ = 0;
        return cut(level, SMALL_CUT);
    }

private:
    int cut(int level, int by) const
    {
        int lower = level - by;
        return lower < floor_ ? floor_ : lower;
    }

    int floor_;
    double target_;
    int over_ = 0;
};

#endif // DRIVE_SERVO_H
