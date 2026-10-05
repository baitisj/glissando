//=========================================================================
// Name:            SmokeGaugeTest.cpp
// Purpose:         Tests for SmokeGauge.
//=========================================================================

#include <cmath>
#include <cstdio>

#include "../SmokeGauge.h"

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

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

} // namespace

int main()
{
    {
        // Starting at 120 s, at full power: nothing until 120 s, half by
        // 140 s, all of it at 160 s.
        SmokeGauge gauge;
        check(near(gauge.update(0.0, 0.0, 1.0, 120), 0.0), "no smoke unkeyed");
        check(near(gauge.update(119.0, 119.0, 1.0, 120), 0.0), "no smoke before 120 s");
        check(near(gauge.update(140.0, 140.0, 1.0, 120), 0.5), "half way at 140 s");
        check(near(gauge.update(160.0, 160.0, 1.0, 120), 1.0), "full at 160 s");
        check(near(gauge.update(170.0, 170.0, 1.0, 120), 1.0), "never over full");

        // Let go: clears over 20 s, not at once.
        check(near(gauge.update(175.0, 0.0, 1.0, 120), 0.75), "a quarter gone 5 s after unkeying");
        check(near(gauge.update(190.0, 0.0, 1.0, 120), 0.0), "gone 20 s after unkeying");

        // Keyed again straight away: the keying starts over, so no smoke.
        check(near(gauge.update(250.0, 60.0, 1.0, 120), 0.0), "a fresh keying starts clear");
    }

    {
        // At or under 0.8 power, or with no reading, never.
        SmokeGauge gauge;
        check(near(gauge.update(150.0, 150.0, 0.8, 120), 0.0), "no smoke at 0.8 power");
        check(near(gauge.update(151.0, 151.0, NAN, 120), 0.0), "no smoke without a power reading");
        check(gauge.update(152.0, 152.0, 0.81, 120) > 0.0, "smoke over 0.8 power");

        // Turned down mid-keying: clears as after unkeying.
        double before = gauge.level();
        double after = gauge.update(162.0, 162.0, 0.5, 120);
        check(after < before && near(after, before - 0.5), "turning the power down clears it slowly");
    }

    {
        // Started early, for trying it out: 10 s, full by 50 s.
        SmokeGauge gauge;
        check(near(gauge.update(9.0, 9.0, 1.0, 10), 0.0), "early start: none before 10 s");
        check(near(gauge.update(30.0, 30.0, 1.0, 10), 0.5), "early start: half by 30 s");
        check(near(gauge.update(50.0, 50.0, 1.0, 10), 1.0), "early start: full by 50 s");
    }

    if (failures == 0) std::printf("All smoke gauge tests passed.\n");
    return failures == 0 ? 0 : 1;
}
