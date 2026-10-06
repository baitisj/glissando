//=========================================================================
// Name:            Globe.h
// Purpose:         The sums behind the console's map ball: where a
//                  Maidenhead locator is, how far and which way, which way
//                  up the ball shows a path best, how it rolls there, and
//                  its land and sea pixel by pixel.
//
// Nothing here depends on wxWidgets: MapBall paints what this works out.
//
// Earth coordinates are unit vectors with x through 0 N 0 E, y through
// 0 N 90 E and z through the north pole. A view looks down on the ball with
// x to the right, y up and z out of the screen at the viewer, so a point of
// the earth is on the near side when its view z is positive.
//=========================================================================

#ifndef GUI_GLISSANDO__GLOBE_H
#define GUI_GLISSANDO__GLOBE_H

#include <cstdint>
#include <string>
#include <vector>

namespace Globe
{

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct LatLon
{
    double lat = 0.0;   // degrees north
    double lon = 0.0;   // degrees east
};

Vec3 toVector(const LatLon& position);
LatLon toLatLon(const Vec3& v);

// The centre of a Maidenhead locator of four or six characters, in any
// case. False for anything else.
bool locatorCentre(const std::string& locator, LatLon& centreOut);

// Great circle distance in kilometres, on a sphere of the earth's mean
// radius, and the initial bearing from one point to the other in degrees
// clockwise from north, 0 to 360.
double distanceKm(const LatLon& from, const LatLon& to);
double bearingDegrees(const LatLon& from, const LatLon& to);

// What the caption under the ball says: the station's grid square, and
// from home its distance and bearing, as in "DN06 · 1,240 km · 104°".
// Without a home locator, just the square; without a station, our own.
std::string caption(const std::string& homeLocator, const std::string& stationLocator);

// Which way the ball is turned: the earth directions that point right, up
// and out of the screen.
struct Attitude
{
    Vec3 right{0.0, 1.0, 0.0};
    Vec3 up{0.0, 0.0, 1.0};
    Vec3 out{1.0, 0.0, 0.0};
};

Vec3 toView(const Attitude& attitude, const Vec3& earth);
Vec3 toEarth(const Attitude& attitude, const Vec3& view);

// Looking straight down on centre with north up, then the picture turned
// clockwise by roll radians.
Attitude lookingAt(const Vec3& centre, double roll = 0.0);

// The angle between two attitudes, in radians: how far the ball has to roll.
double angleBetween(const Attitude& a, const Attitude& b);

// The shorter great circle from a to b in segments steps: segments + 1
// points, a first.
std::vector<Vec3> greatCircle(const Vec3& a, const Vec3& b, int segments);

// The window the ball is seen through and the ball under it, in pixels: the
// ball is centred in the window.
struct Window
{
    double width = 416.0;
    double height = 104.0;
    double radius = 245.0;
};

// The attitude that best shows the path from home to a station. The ball
// centres a little to the equator side of the path's midpoint, so the path
// curls over the ball rather than running straight through the middle,
// and turns from north up only as far as it has to show as much of the
// path as can be shown in the window.
Attitude framePath(const LatLon& home, const LatLon& station, const Window& window);

// How many of the path's points the window shows with the ball at attitude.
int pointsInView(const Attitude& attitude, const std::vector<Vec3>& path, const Window& window);

// A ball floating in something thick: it rolls towards the attitude it is
// sent to and eases to a stop there over about two seconds, without
// overshooting.
class Roller
{
public:
    Roller();

    // Straight there, no rolling.
    void jump(const Attitude& attitude);
    void rollTo(const Attitude& attitude);

    // Advances the roll by seconds. Returns whether the ball is still
    // moving afterwards.
    bool step(double seconds);

    Attitude attitude() const;
    bool moving() const { return moving_; }

private:
    struct Quaternion
    {
        double w = 1.0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };
    static Quaternion fromAttitude(const Attitude& attitude);
    static Attitude toAttitude(const Quaternion& q);

    Quaternion current_;
    Quaternion target_;
    Vec3 spin_;         // radians a second, about view axes
    bool moving_;
};

// Paints the window's picture into rgb, width * height pixels of three
// bytes, top row first: the ball at attitude, lit from the upper left,
// with its land, its sea and the Maidenhead fields ruled on it, floating in
// the dark around it.
void paintBall(const Attitude& attitude, const Window& window, int width, int height,
               std::vector<uint8_t>& rgb);

// Whether the land mask has land at a position, for tests.
bool isLand(const LatLon& position);

} // namespace Globe

#endif // GUI_GLISSANDO__GLOBE_H
