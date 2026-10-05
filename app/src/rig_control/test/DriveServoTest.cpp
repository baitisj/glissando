//=========================================================================
// Name:            DriveServoTest.cpp
// Purpose:         Tests for DriveServo.
//=========================================================================

#include <cstdio>

#include "../DriveServo.h"

namespace
{

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

} // namespace

int main()
{
    const int FLOOR = -300;
    DriveServo servo(FLOOR, 0.5);

    // At the target or under it: left alone, however long it goes on.
    int level = -120;
    for (int i = 0; i < 10; i++) level = servo.reading(i % 2 ? 0.5 : 0.2, level);
    check(level == -120, "readings at or under the target leave the level alone");

    // One stray reading over the target doesn't cut; two in a row do.
    level = servo.reading(0.55, level);
    check(level == -120, "a single reading over the target doesn't cut");
    level = servo.reading(0.4, level);
    level = servo.reading(0.55, level);
    check(level == -120, "readings over the target with one under between don't cut");
    level = servo.reading(0.55, level);
    check(level == -125, "two readings in a row over the target cut 0.5 dB");
    level = servo.reading(0.55, level);
    check(level == -125, "the count starts again after a cut");
    level = servo.reading(0.55, level);
    check(level == -130, "and two more cut again");

    // Far over: the big cut, at once.
    level = servo.reading(0.9, level);
    check(level == -150, "a reading over 0.8 cuts 2 dB at once");

    // Never up, whatever it reads.
    for (int i = 0; i < 20; i++) level = servo.reading(0.0, level);
    check(level == -150, "never turns the level back up");

    // A new keying forgets a reading over the target on the last one.
    level = servo.reading(0.6, level);
    servo.restart();
    level = servo.reading(0.6, level);
    check(level == -150, "a reading over the target on the last keying doesn't count");

    // Never below the bottom of the knob.
    level = -290;
    level = servo.reading(1.0, level);
    check(level == FLOOR, "stops at the bottom of the knob");
    level = servo.reading(1.0, level);
    check(level == FLOOR, "and stays there");

    // Jeff's IC-7100: about 0.5 at -14 dB, a little over at -12.
    DriveServo rig(FLOOR, 0.5);
    level = -120;
    for (int second = 0; second < 30; second++)
    {
        double alc = level > -140 ? 0.5 + (level + 140) * 0.0015 : 0.5;
        level = rig.reading(alc, level);
    }
    check(level == -140, "a rig a little over at -12 dB settles at -14 dB");

    if (failures == 0) std::printf("All drive servo tests passed\n");
    return failures == 0 ? 0 : 1;
}
