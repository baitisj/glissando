//=========================================================================
// Name:            GlobeTest.cpp
// Purpose:         Checks the sums behind the console's map ball.
//=========================================================================

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../Globe.h"

using namespace Globe;

namespace
{

int failures = 0;

void check(bool condition, const char* what, int line)
{
    if (!condition)
    {
        failures++;
        fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

LatLon centreOf(const char* locator)
{
    LatLon position;
    CHECK(locatorCentre(locator, position));
    return position;
}

void testLocators()
{
    LatLon square = centreOf("CN87");
    CHECK(near(square.lat, 47.5, 1e-9));
    CHECK(near(square.lon, -123.0, 1e-9));

    LatLon subsquare = centreOf("cn87UX");
    CHECK(near(subsquare.lat, 47.0 + 23.5 / 24.0, 1e-9));
    CHECK(near(subsquare.lon, -124.0 + 20.5 / 12.0, 1e-9));

    LatLon corner = centreOf("AA00");
    CHECK(near(corner.lat, -89.5, 1e-9) && near(corner.lon, -179.0, 1e-9));
    LatLon other = centreOf("RR99");
    CHECK(near(other.lat, 89.5, 1e-9) && near(other.lon, 179.0, 1e-9));

    LatLon unused;
    CHECK(!locatorCentre("", unused));
    CHECK(!locatorCentre("CN8", unused));
    CHECK(!locatorCentre("SN87", unused));   // fields stop at R
    CHECK(!locatorCentre("CN87zz", unused)); // subsquares stop at x
    CHECK(!locatorCentre("CN87u", unused));
}

void testDistanceAndBearing()
{
    // Seattle's square to Berlin's: about 8,100 km, leaving a little east
    // of north over Greenland.
    LatLon seattle = centreOf("CN87");
    LatLon berlin = centreOf("JO62");
    CHECK(near(distanceKm(seattle, berlin), 8139.0, 5.0));
    CHECK(near(bearingDegrees(seattle, berlin), 26.0, 1.0));
    CHECK(near(distanceKm(berlin, seattle), distanceKm(seattle, berlin), 1e-6));

    // A degree of latitude is about 111 km, due north or south.
    LatLon a{10.0, 20.0};
    LatLon b{11.0, 20.0};
    CHECK(near(distanceKm(a, b), 111.19, 0.05));
    CHECK(near(bearingDegrees(a, b), 0.0, 1e-6));
    CHECK(near(bearingDegrees(b, a), 180.0, 1e-6));

    // Due west comes out as 270, not -90.
    CHECK(near(bearingDegrees(LatLon{0.0, 10.0}, LatLon{0.0, 0.0}), 270.0, 1e-6));
}

void testCaption()
{
    LatLon home = centreOf("CN87");
    LatLon there = centreOf("DN06");
    std::string expected = "DN06 \xC2\xB7 " + std::to_string(std::lround(distanceKm(home, there))) + " km \xC2\xB7 " +
                           std::to_string(std::lround(bearingDegrees(home, there))) + "\xC2\xB0";
    CHECK(caption("CN87", "DN06") == expected);
    CHECK(caption("cn87", "dn06") == expected);

    // Thousands get a separator.
    std::string far = caption("CN87", "JO62");
    CHECK(far.find("8,139 km") != std::string::npos);

    CHECK(caption("", "DN06") == "DN06");          // no home: just where they are
    CHECK(caption("CN87", "") == "CN87");          // nobody yet: where we are
    CHECK(caption("CN87ux", "") == "CN87ux");
    CHECK(caption("CN87", "CN87") == "CN87");      // same square
    CHECK(caption("", "").empty());
    CHECK(caption("bogus", "DN06") == "DN06");
}

void testLandMask()
{
    CHECK(isLand(LatLon{39.0, -98.0}));     // Kansas
    CHECK(isLand(LatLon{23.0, 10.0}));      // the Sahara
    CHECK(isLand(LatLon{-25.0, 134.0}));    // Australia
    CHECK(isLand(LatLon{-85.0, 0.0}));      // Antarctica
    CHECK(isLand(LatLon{72.0, -40.0}));     // Greenland
    CHECK(!isLand(LatLon{0.0, -150.0}));    // the Pacific
    CHECK(!isLand(LatLon{30.0, -40.0}));    // the Atlantic
    CHECK(!isLand(LatLon{-30.0, 80.0}));    // the Indian Ocean
    CHECK(!isLand(LatLon{0.0, 180.0}));     // the date line wraps
}

void testAttitude()
{
    LatLon home = centreOf("CN87");
    Attitude a = lookingAt(toVector(home));

    // Home is in the middle, north is up and east is to the right.
    Vec3 middle = toView(a, toVector(home));
    CHECK(near(middle.x, 0.0, 1e-9) && near(middle.y, 0.0, 1e-9) && near(middle.z, 1.0, 1e-9));
    Vec3 north = toView(a, toVector(LatLon{home.lat + 5.0, home.lon}));
    CHECK(north.y > 0.05 && near(north.x, 0.0, 1e-9));
    Vec3 east = toView(a, toVector(LatLon{home.lat, home.lon + 5.0}));
    CHECK(east.x > 0.05);

    // Turned clockwise a quarter: north is to the right.
    Attitude turned = lookingAt(toVector(home), std::acos(-1.0) / 2.0);
    Vec3 northTurned = toView(turned, toVector(LatLon{home.lat + 5.0, home.lon}));
    CHECK(northTurned.x > 0.05 && near(northTurned.y, 0.0, 1e-9));
    CHECK(near(angleBetween(a, turned), std::acos(-1.0) / 2.0, 1e-9));
    CHECK(near(angleBetween(a, a), 0.0, 1e-6));

    // And back again.
    Vec3 v = toVector(LatLon{12.0, 34.0});
    Vec3 back = toEarth(turned, toView(turned, v));
    CHECK(near(back.x, v.x, 1e-12) && near(back.y, v.y, 1e-12) && near(back.z, v.z, 1e-12));

    // Straight down on a pole still gives a usable attitude.
    Attitude pole = lookingAt(Vec3{0.0, 0.0, 1.0});
    CHECK(near(pole.out.z, 1.0, 1e-12) && near(pole.up.z, 0.0, 1e-12));
}

void testGreatCircle()
{
    Vec3 a = toVector(LatLon{0.0, 0.0});
    Vec3 b = toVector(LatLon{0.0, 90.0});
    std::vector<Vec3> path = greatCircle(a, b, 4);
    CHECK(path.size() == 5);
    LatLon middle = toLatLon(path[2]);
    CHECK(near(middle.lat, 0.0, 1e-9) && near(middle.lon, 45.0, 1e-9));

    // Opposite points still make a path half way round.
    std::vector<Vec3> round = greatCircle(a, toVector(LatLon{0.0, 180.0}), 8);
    LatLon end = toLatLon(round.back());
    CHECK(near(std::fabs(end.lon), 180.0, 1e-6));
    for (const Vec3& p : round) CHECK(near(std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z), 1.0, 1e-9));
}

void testFraming()
{
    Window window;

    // A short path is shown whole, north up, curling over the ball's
    // middle: not drawn straight through it.
    LatLon home = centreOf("CN87");
    LatLon near_ = centreOf("DN06");
    std::vector<Vec3> path = greatCircle(toVector(home), toVector(near_), 64);
    Attitude a = framePath(home, near_, window);
    CHECK(pointsInView(a, path, window) == (int)path.size());
    CHECK(near(a.right.z, 0.0, 1e-9));      // right is level: north is up

    // So is Europe from Seattle, over the pole, and the path bows towards
    // it rather than running through the centre.
    LatLon berlin = centreOf("JO62");
    path = greatCircle(toVector(home), toVector(berlin), 64);
    a = framePath(home, berlin, window);
    CHECK(pointsInView(a, path, window) == (int)path.size());
    CHECK(near(a.right.z, 0.0, 1e-9));
    Vec3 middle = toView(a, path[32]);
    CHECK(middle.y * window.radius > 4.0);  // above the centre, towards the pole

    // North to south, 8,600 km, does not fit a window twice as wide as it
    // is tall north up: the ball turns, and shows more of it.
    LatLon connecticut = centreOf("FN31");
    LatLon uruguay = centreOf("GF15");
    path = greatCircle(toVector(connecticut), toVector(uruguay), 64);
    a = framePath(connecticut, uruguay, window);
    Vec3 centre = a.out;
    Attitude upright = lookingAt(centre);
    CHECK(std::fabs(a.right.z) > 0.2);
    CHECK(pointsInView(a, path, window) > pointsInView(upright, path, window));

    // The same square at both ends: just look at it.
    a = framePath(home, home, window);
    Vec3 there = toView(a, toVector(home));
    CHECK(near(there.z, 1.0, 1e-9));

    // Opposite sides of the earth: something sensible, not a crash.
    a = framePath(LatLon{10.0, 20.0}, LatLon{-10.0, -160.0}, window);
    CHECK(std::isfinite(a.out.x) && std::isfinite(a.right.y));
}

void testRolling()
{
    Attitude from = lookingAt(toVector(centreOf("CN87")));
    Attitude to = lookingAt(toVector(centreOf("JO62")), 0.3);

    Roller roller;
    roller.jump(from);
    CHECK(!roller.moving());
    CHECK(near(angleBetween(roller.attitude(), from), 0.0, 1e-6));

    // It eases in, never overshoots, and is still in about two seconds.
    roller.rollTo(to);
    double last = angleBetween(roller.attitude(), to);
    double atHalfSecond = 0.0;
    bool steady = true;
    double t = 0.0;
    while (roller.moving() && t < 5.0)
    {
        roller.step(1.0 / 60.0);
        t += 1.0 / 60.0;
        double left = angleBetween(roller.attitude(), to);
        if (left > last + 1e-9) steady = false;
        last = left;
        if (near(t, 0.5, 0.009)) atHalfSecond = left;
    }
    CHECK(steady);
    CHECK(!roller.moving());
    CHECK(t > 1.2 && t < 3.0);
    CHECK(atHalfSecond > 0.2 * angleBetween(from, to));     // not a jump
    CHECK(near(angleBetween(roller.attitude(), to), 0.0, 1e-6));

    // Sent somewhere new mid roll, it carries on smoothly to there.
    roller.rollTo(from);
    roller.step(0.4);
    roller.rollTo(to);
    for (int i = 0; i < 300 && roller.moving(); i++) roller.step(1.0 / 60.0);
    CHECK(!roller.moving());
    CHECK(near(angleBetween(roller.attitude(), to), 0.0, 1e-6));

    // The attitude stays a rotation all the way.
    Attitude now = roller.attitude();
    double lengthRight = std::sqrt(now.right.x * now.right.x + now.right.y * now.right.y + now.right.z * now.right.z);
    CHECK(near(lengthRight, 1.0, 1e-9));
}

void testPainting()
{
    Window window;
    const int width = 208;
    const int height = 104;
    std::vector<uint8_t> rgb;

    auto brightness = [&](int x, int y) {
        const uint8_t* p = &rgb[((size_t)y * width + x) * 3];
        return p[0] + p[1] + p[2];
    };

    paintBall(lookingAt(toVector(LatLon{39.0, -98.0})), window, width, height, rgb);
    CHECK(rgb.size() == (size_t)width * height * 3);
    int land = brightness(width / 2, height / 2);
    int fluid = brightness(0, height / 2);              // outside the ball

    paintBall(lookingAt(toVector(LatLon{0.0, -150.0})), window, width, height, rgb);
    int sea = brightness(width / 2, height / 2);

    CHECK(land > sea + 150);
    CHECK(sea > fluid);
    CHECK(fluid < 60);
}

} // namespace

int main()
{
    testLocators();
    testDistanceAndBearing();
    testCaption();
    testLandMask();
    testAttitude();
    testGreatCircle();
    testFraming();
    testRolling();
    testPainting();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all globe checks passed\n");
    return 0;
}
