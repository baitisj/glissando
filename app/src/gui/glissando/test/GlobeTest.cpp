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

void testLand()
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
    CHECK(isLand(LatLon{66.0, 178.0}));     // Chukotka, both sides of it
    CHECK(isLand(LatLon{66.0, -178.0}));
    CHECK(isLand(LatLon{-28.0, 25.0}));     // South Africa, and Lesotho in the
    CHECK(isLand(LatLon{-29.5, 28.2}));     // hole in it
}

// Whether a point of the window is inside the outlines, by the non-zero rule.
bool filled(const std::vector<Outline>& outlines, double x, double y)
{
    int winding = 0;
    for (const Outline& outline : outlines)
    {
        for (size_t i = 0; i < outline.size(); i++)
        {
            const Point& a = outline[i];
            const Point& b = outline[(i + 1) % outline.size()];
            double side = (b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y);
            if (a.y <= y && b.y > y && side > 0.0) winding++;
            else if (a.y > y && b.y <= y && side < 0.0) winding--;
        }
    }
    return winding != 0;
}

void testOutlines()
{
    // Drawn outlines agree with the land, point for point, however the ball
    // is turned and however close: whole continents, land going round the
    // back of the ball at its rim, the poles, the date line.
    struct Case
    {
        double lat, lon, roll, radius;
    } cases[] = {
        {0.0, -150.0, 0.0, 180.0}, {39.0, -98.0, 0.0, 245.0}, {-89.0, 0.0, 0.0, 180.0},
        {65.0, 180.0, 0.4, 180.0}, {0.0, 20.0, 0.0, 180.0},   {52.0, 13.0, 0.0, 2000.0},
        {20.0, 80.0, 1.0, 120.0},  {10.0, -30.0, 2.5, 60.0},
    };
    Window window{400.0, 400.0};
    for (const Case& c : cases)
    {
        View view{lookingAt(toVector(LatLon{c.lat, c.lon}), c.roll), c.radius};
        std::vector<Outline> outlines = landOutlines(view, window);
        int shown = 0;
        int wrong = 0;
        for (int py = 0; py < 400; py += 4)
        {
            for (int px = 0; px < 400; px += 4)
            {
                double x = px + 0.5;
                double y = py + 0.5;
                double sx = (x - 200.0) / c.radius;
                double sy = (200.0 - y) / c.radius;
                double r2 = sx * sx + sy * sy;
                if (r2 > 0.99) continue;
                Vec3 face{sx, sy, std::sqrt(1.0 - r2)};
                shown++;
                if (filled(outlines, x, y) != isLand(toLatLon(toEarth(view.attitude, face)))) wrong++;
            }
        }
        CHECK(shown > 0);
        CHECK(wrong <= shown / 500);
    }

    // Seen from far over the Pacific, nothing at all is drawn for the land
    // round the back.
    View pacific{lookingAt(toVector(LatLon{0.0, -150.0})), 50.0};
    Window small{120.0, 120.0};
    CHECK(!filled(landOutlines(pacific, small), 60.0, 60.0));

    // The grid's lines stay on the near side of the ball.
    View kansas{lookingAt(toVector(LatLon{39.0, -98.0})), 50.0};
    std::vector<Outline> lines = fieldLines(kansas, small);
    CHECK(lines.size() > 10);
    for (const Outline& line : lines)
    {
        for (const Point& p : line) CHECK(std::hypot(p.x - 60.0, p.y - 60.0) <= 50.0 + 1e-6);
    }
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
    Zoom zoom;

    // A short path is shown whole, north up, as close as it can be, curling
    // over the ball's middle: not drawn straight through it.
    LatLon home = centreOf("CN87");
    LatLon near_ = centreOf("DN06");
    std::vector<Vec3> path = greatCircle(toVector(home), toVector(near_), 64);
    View v = framePath(home, near_, window, zoom);
    CHECK(pointsInView(v, path, window) == (int)path.size());
    CHECK(near(v.attitude.right.z, 0.0, 1e-9));      // right is level: north is up
    CHECK(v.radius > 4.0 * View().radius);

    // So is Europe from Seattle, over the pole, farther out, and the path
    // bows towards the pole rather than running through the centre.
    LatLon berlin = centreOf("JO62");
    path = greatCircle(toVector(home), toVector(berlin), 64);
    View europe = framePath(home, berlin, window, zoom);
    CHECK(pointsInView(europe, path, window) == (int)path.size());
    CHECK(europe.radius < v.radius);
    Vec3 middle = toView(europe.attitude, path[32]);
    CHECK(middle.y * europe.radius > 4.0);             // above the centre, towards the pole

    // North to south, 8,600 km, does not fit the wide window north up: the
    // ball turns to show all of it closer than north up could.
    LatLon connecticut = centreOf("FN31");
    LatLon uruguay = centreOf("GF15");
    path = greatCircle(toVector(connecticut), toVector(uruguay), 64);
    View south = framePath(connecticut, uruguay, window, zoom);
    CHECK(std::fabs(south.attitude.right.z) > 0.2);
    CHECK(pointsInView(south, path, window) == (int)path.size());
    View upright{lookingAt(south.attitude.out), south.radius};
    CHECK(pointsInView(upright, path, window) < (int)path.size());

    // Never closer than nearest nor farther than farthest.
    for (const View& each : {v, europe, south}) CHECK(each.radius <= zoom.nearest && each.radius >= zoom.farthest);

    // The same square at both ends: just look at it.
    View same = framePath(home, home, window, zoom);
    Vec3 there = toView(same.attitude, toVector(home));
    CHECK(near(there.z, 1.0, 1e-9));
    CHECK(near(same.radius, zoom.standard, 1e-9));

    // Opposite sides of the earth: something sensible, not a crash.
    View opposite = framePath(LatLon{10.0, 20.0}, LatLon{-10.0, -160.0}, window, zoom);
    CHECK(std::isfinite(opposite.attitude.out.x) && std::isfinite(opposite.attitude.right.y));
    CHECK(std::isfinite(opposite.radius));
}

void testRolling()
{
    View from{lookingAt(toVector(centreOf("CN87"))), 600.0};
    View to{lookingAt(toVector(centreOf("JO62")), 0.3), 330.0};

    Roller roller;
    roller.setBackOff(208.0);
    roller.jump(from);
    CHECK(!roller.moving());
    CHECK(near(angleBetween(roller.view().attitude, from.attitude), 0.0, 1e-6));
    CHECK(near(roller.view().radius, 600.0, 1e-9));

    // It eases in, never overshoots, backs off on the way, and is still in
    // a few seconds, where it was sent.
    roller.rollTo(to);
    double last = angleBetween(roller.view().attitude, to.attitude);
    double atHalfSecond = 0.0;
    double farthest = roller.view().radius;
    bool steady = true;
    double t = 0.0;
    while (roller.moving() && t < 6.0)
    {
        roller.step(1.0 / 60.0);
        t += 1.0 / 60.0;
        double left = angleBetween(roller.view().attitude, to.attitude);
        if (left > last + 1e-9) steady = false;
        last = left;
        farthest = std::min(farthest, roller.view().radius);
        if (near(t, 0.5, 0.009)) atHalfSecond = left;
    }
    CHECK(steady);
    CHECK(!roller.moving());
    CHECK(t > 1.2 && t < 4.0);
    CHECK(atHalfSecond > 0.2 * angleBetween(from.attitude, to.attitude));     // not a jump
    CHECK(farthest < 0.5 * from.radius && farthest < to.radius);               // backed off
    CHECK(near(angleBetween(roller.view().attitude, to.attitude), 0.0, 1e-6));
    CHECK(near(roller.view().radius, 330.0, 1e-6));

    // Zooming in place is just as smooth, and does not back off.
    View closer = to;
    closer.radius = 600.0;
    roller.rollTo(closer);
    double smallest = 1e9;
    for (int i = 0; i < 600 && roller.moving(); i++)
    {
        roller.step(1.0 / 60.0);
        smallest = std::min(smallest, roller.view().radius);
    }
    CHECK(!roller.moving());
    CHECK(smallest >= 330.0 - 1e-6);
    CHECK(near(roller.view().radius, 600.0, 1e-6));

    // Sent somewhere new mid roll, it carries on smoothly to there.
    roller.rollTo(from);
    roller.step(0.4);
    roller.rollTo(to);
    for (int i = 0; i < 600 && roller.moving(); i++) roller.step(1.0 / 60.0);
    CHECK(!roller.moving());
    CHECK(near(angleBetween(roller.view().attitude, to.attitude), 0.0, 1e-6));

    // The attitude stays a rotation all the way.
    Attitude now = roller.view().attitude;
    double lengthRight = std::sqrt(now.right.x * now.right.x + now.right.y * now.right.y + now.right.z * now.right.z);
    CHECK(near(lengthRight, 1.0, 1e-9));
}

} // namespace

int main()
{
    testLocators();
    testDistanceAndBearing();
    testCaption();
    testLand();
    testAttitude();
    testGreatCircle();
    testFraming();
    testRolling();
    testOutlines();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all globe checks passed\n");
    return 0;
}
