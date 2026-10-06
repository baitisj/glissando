//=========================================================================
// Name:            Globe.cpp
// Purpose:         The sums behind the console's map ball.
//=========================================================================

#include "Globe.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "LandOutlines.h"

namespace Globe
{

namespace
{

constexpr double PI = 3.14159265358979323846;
constexpr double DEGREES = 180.0 / PI;
constexpr double EARTH_RADIUS_KM = 6371.0088;

// How the ball moves through its fluid. Taking its weight as one, a pull is
// the spin it adds a second for each radian it has to turn, and a brake the
// share of the spin it takes away a second.
//
// Coasting, the fluid takes a tenth of its spin a third of a second, and
// takes 0.08 radians a second off it every second besides, so that at a
// crawl it is too thick to turn in at all.
constexpr double FLUID_DRAG = 0.3;
constexpr double FLUID_STICK = 0.08;
// The field: the magnet's pull towards lining up, the view's middle floating
// up to the window, a little pull straight towards the view so nothing can
// balance it the wrong way round, and the braking, all coming on over a
// third of a second. From still it settles in about two seconds, barely
// swinging past; spinning, in about three.
constexpr double MAGNET_PULL = 12.0;
constexpr double FLOAT_PULL = 10.0;
constexpr double STRAIGHT_PULL = 3.0;
constexpr double FIELD_BRAKE = 7.0;
constexpr double FIELD_RISE_SECONDS = 0.35;
// Held, a stiff spring from the point held to the pointer, braked hard.
constexpr double HOLD_PULL = 600.0;
constexpr double HOLD_BRAKE = 49.0;
// The zoom's spring, a little past critical.
constexpr double ZOOM_RATE = 5.0;   // radians a second
constexpr double ZOOM_DAMPING_RATIO = 1.15;
constexpr double STEP_SECONDS = 1.0 / 240.0;
constexpr double SETTLED_RADIANS = 0.002;    // a fifth of a pixel at the edge of the ball
constexpr double SETTLED_SPIN = 0.005;

// Points along a path when framing it, and how far inside the window's
// edge they have to be to count as shown.
constexpr int FRAMING_SEGMENTS = 64;
constexpr double FRAMING_MARGIN = 12.0;
// The most the middle of the path is lifted towards the pole, as a share of
// the window's half height, so the bow stays in a short window.
constexpr double CURL_SHARE = 0.5;
// Looking for how close to come, from nearest out, a step at a time; and
// how much closer a turned picture has to be to win over north up.
constexpr double ZOOM_STEP = 0.93;
constexpr double NORTH_UP_SHARE = 0.75;

// Steps along a line of the grid, and round the rim where land goes behind.
constexpr double GRID_STEP_DEGREES = 2.0;
constexpr double RIM_STEP_RADIANS = 3.0 / DEGREES;

double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 scaled(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 plus(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
double length(const Vec3& a) { return std::sqrt(dot(a, a)); }

Vec3 normalized(const Vec3& a)
{
    double l = length(a);
    return l > 0.0 ? scaled(a, 1.0 / l) : a;
}

double angleOf(const Vec3& a, const Vec3& b)
{
    return std::atan2(length(cross(a, b)), dot(a, b));
}

const Vec3 NORTH{0.0, 0.0, 1.0};

// Any direction at right angles to a.
Vec3 perpendicular(const Vec3& a)
{
    Vec3 p = cross(a, NORTH);
    if (length(p) < 1e-6) p = cross(a, Vec3{1.0, 0.0, 0.0});
    return normalized(p);
}

// The outlines, unpacked once as unit vectors, each with the cap that holds
// it, so outlines nowhere near the window can be passed over, and which way
// round it goes.
struct Ring
{
    std::vector<Vec3> points;
    Vec3 centre;
    double reach = 0.0;     // radians from centre to the farthest point
    bool clockwise = true;  // land; a hole goes anticlockwise
};

const std::vector<Ring>& rings()
{
    static const std::vector<Ring> all = []() {
        std::vector<Ring> unpacked;
        for (int r = 0; r < LandOutlines::RING_COUNT; r++)
        {
            Ring ring;
            double area = 0.0;
            uint32_t first = LandOutlines::RING_START[r];
            uint32_t last = LandOutlines::RING_START[r + 1];
            Vec3 sum;
            for (uint32_t i = first; i < last; i++)
            {
                double lon = LandOutlines::POINTS[2 * i] / LandOutlines::UNITS_PER_DEGREE;
                double lat = LandOutlines::POINTS[2 * i + 1] / LandOutlines::UNITS_PER_DEGREE;
                uint32_t j = i + 1 < last ? i + 1 : first;
                double lon2 = LandOutlines::POINTS[2 * j] / LandOutlines::UNITS_PER_DEGREE;
                double lat2 = LandOutlines::POINTS[2 * j + 1] / LandOutlines::UNITS_PER_DEGREE;
                area += lon * lat2 - lon2 * lat;
                ring.points.push_back(toVector(LatLon{lat, lon}));
                sum = plus(sum, ring.points.back());
            }
            ring.clockwise = area < 0.0;
            ring.centre = length(sum) > 1e-9 ? normalized(sum) : ring.points.front();
            for (const Vec3& p : ring.points) ring.reach = std::max(ring.reach, angleOf(ring.centre, p));
            unpacked.push_back(std::move(ring));
        }
        return unpacked;
    }();
    return all;
}

// How far from the middle of the view, in radians over the ball, the window
// can show anything.
double visibleReach(const View& view, const Window& window)
{
    double corner = std::hypot(window.width / 2.0, window.height / 2.0) / view.radius;
    return corner >= 1.0 ? PI / 2.0 : std::asin(corner);
}

// Where the line from a (near side) to b (far side) goes over the rim: the
// angle round the rim, anticlockwise from the right, in the view.
double rimAngle(const Vec3& a, const Vec3& b)
{
    double t = a.z / (a.z - b.z);
    return std::atan2(a.y + (b.y - a.y) * t, a.x + (b.x - a.x) * t);
}

Point screenPoint(const View& view, const Window& window, double x, double y)
{
    return Point{window.width / 2.0 + x * view.radius, window.height / 2.0 - y * view.radius};
}

// Round the rim from an angle by sweep radians, anticlockwise if positive,
// leaving out the first point and putting in the last.
void followRim(Outline& outline, const View& view, const Window& window, double from, double sweep)
{
    if (std::fabs(sweep) < 1e-9) return;
    int steps = std::max(1, (int)std::ceil(std::fabs(sweep) / RIM_STEP_RADIANS));
    for (int k = 1; k <= steps; k++)
    {
        double angle = from + sweep * k / steps;
        outline.push_back(screenPoint(view, window, std::cos(angle), std::sin(angle)));
    }
}

// Splits a line of points into the runs on the near side, ending each run
// on the rim where it goes behind.
void addNearRuns(std::vector<Outline>& lines, const std::vector<Vec3>& points, const View& view, const Window& window)
{
    Outline run;
    Vec3 previous;
    bool previousNear = false;
    for (size_t i = 0; i < points.size(); i++)
    {
        Vec3 v = toView(view.attitude, points[i]);
        bool near = v.z > 0.0;
        if (i > 0 && near != previousNear)
        {
            double angle = near ? rimAngle(v, previous) : rimAngle(previous, v);
            run.push_back(screenPoint(view, window, std::cos(angle), std::sin(angle)));
            if (!near)
            {
                if (run.size() >= 2) lines.push_back(run);
                run.clear();
            }
        }
        if (near) run.push_back(screenPoint(view, window, v.x, v.y));
        previous = v;
        previousNear = near;
    }
    if (run.size() >= 2) lines.push_back(run);
}

std::string withThousands(long value)
{
    std::string digits = std::to_string(value);
    std::string out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
    {
        if (count > 0 && count % 3 == 0) out.insert(out.begin(), ',');
        out.insert(out.begin(), *it);
        count++;
    }
    return out;
}

// A locator tidied the way FrameCodec writes one: fields upper case,
// subsquare lower case. Empty unless it is one.
std::string tidyLocator(const std::string& locator)
{
    std::string text;
    for (char c : locator)
    {
        if (!std::isspace((unsigned char)c)) text += c;
    }
    if (text.size() != 4 && text.size() != 6) return "";
    for (size_t i = 0; i < text.size(); i++)
    {
        char c = text[i];
        if (i < 2) c = (char)std::toupper((unsigned char)c);
        else if (i >= 4) c = (char)std::tolower((unsigned char)c);
        bool ok = i < 2 ? (c >= 'A' && c <= 'R') : i < 4 ? (c >= '0' && c <= '9') : (c >= 'a' && c <= 'x');
        if (!ok) return "";
        text[i] = c;
    }
    return text;
}

} // namespace

Vec3 toVector(const LatLon& position)
{
    double lat = position.lat / DEGREES;
    double lon = position.lon / DEGREES;
    return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
}

LatLon toLatLon(const Vec3& v)
{
    Vec3 n = normalized(v);
    return {std::asin(std::clamp(n.z, -1.0, 1.0)) * DEGREES, std::atan2(n.y, n.x) * DEGREES};
}

bool locatorCentre(const std::string& locator, LatLon& centreOut)
{
    std::string grid = tidyLocator(locator);
    if (grid.empty()) return false;

    double lon = (grid[0] - 'A') * 20.0 - 180.0 + (grid[2] - '0') * 2.0;
    double lat = (grid[1] - 'A') * 10.0 - 90.0 + (grid[3] - '0') * 1.0;
    if (grid.size() == 6)
    {
        lon += (grid[4] - 'a') * (2.0 / 24.0) + 1.0 / 24.0;
        lat += (grid[5] - 'a') * (1.0 / 24.0) + 1.0 / 48.0;
    }
    else
    {
        lon += 1.0;
        lat += 0.5;
    }
    centreOut = {lat, lon};
    return true;
}

double distanceKm(const LatLon& from, const LatLon& to)
{
    return angleOf(toVector(from), toVector(to)) * EARTH_RADIUS_KM;
}

double bearingDegrees(const LatLon& from, const LatLon& to)
{
    double lat1 = from.lat / DEGREES;
    double lat2 = to.lat / DEGREES;
    double dlon = (to.lon - from.lon) / DEGREES;
    double y = std::sin(dlon) * std::cos(lat2);
    double x = std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(dlon);
    double bearing = std::atan2(y, x) * DEGREES;
    return bearing < 0.0 ? bearing + 360.0 : bearing;
}

std::string caption(const std::string& homeLocator, const std::string& stationLocator, bool current)
{
    static const std::string DOT = " \xC2\xB7 ";       // a middle dot, in UTF-8
    static const std::string DEGREE = "\xC2\xB0";

    std::string home = tidyLocator(homeLocator);
    std::string station = tidyLocator(stationLocator);
    if (station.empty()) return home;
    std::string label = current ? station : station + " (last contact)";

    LatLon from;
    LatLon to;
    if (!locatorCentre(home, from) || !locatorCentre(station, to)) return label;
    long km = std::lround(distanceKm(from, to));
    if (km < 1) return label;

    long bearing = std::lround(bearingDegrees(from, to)) % 360;
    return label + DOT + withThousands(km) + " km" + DOT + std::to_string(bearing) + DEGREE;
}

Vec3 toView(const Attitude& attitude, const Vec3& earth)
{
    return {dot(attitude.right, earth), dot(attitude.up, earth), dot(attitude.out, earth)};
}

Vec3 toEarth(const Attitude& attitude, const Vec3& view)
{
    return plus(plus(scaled(attitude.right, view.x), scaled(attitude.up, view.y)), scaled(attitude.out, view.z));
}

Attitude lookingAt(const Vec3& centre, double roll)
{
    Vec3 out = normalized(centre);
    Vec3 up = plus(NORTH, scaled(out, -dot(NORTH, out)));
    // Straight down on a pole, north has no direction: put 0 E at the bottom.
    if (length(up) < 1e-9) up = out.z > 0.0 ? Vec3{-1.0, 0.0, 0.0} : Vec3{1.0, 0.0, 0.0};
    up = normalized(up);
    Vec3 right = cross(up, out);

    double c = std::cos(roll);
    double s = std::sin(roll);
    Attitude attitude;
    attitude.right = plus(scaled(right, c), scaled(up, s));
    attitude.up = plus(scaled(right, -s), scaled(up, c));
    attitude.out = out;
    return attitude;
}

double angleBetween(const Attitude& a, const Attitude& b)
{
    double trace = dot(a.right, b.right) + dot(a.up, b.up) + dot(a.out, b.out);
    return std::acos(std::clamp((trace - 1.0) / 2.0, -1.0, 1.0));
}

std::vector<Vec3> greatCircle(const Vec3& a, const Vec3& b, int segments)
{
    segments = std::max(segments, 1);
    Vec3 from = normalized(a);
    Vec3 to = normalized(b);
    std::vector<Vec3> points;
    points.reserve((size_t)segments + 1);

    double total = angleOf(from, to);
    // Opposite points have no one shorter path: go by way of a point at
    // right angles to both, which is one of them.
    Vec3 axis = cross(from, to);
    if (length(axis) < 1e-9) axis = total > 1.0 ? perpendicular(from) : Vec3{0.0, 0.0, 0.0};
    if (length(axis) < 1e-12)
    {
        points.assign((size_t)segments + 1, from);
        return points;
    }
    axis = normalized(axis);
    Vec3 side = cross(axis, from);  // at right angles to from, towards to

    for (int i = 0; i <= segments; i++)
    {
        double angle = total * i / segments;
        points.push_back(plus(scaled(from, std::cos(angle)), scaled(side, std::sin(angle))));
    }
    return points;
}

Point onScreen(const View& view, const Window& window, const Vec3& earth)
{
    Vec3 v = toView(view.attitude, earth);
    return screenPoint(view, window, v.x, v.y);
}

int pointsInView(const View& view, const std::vector<Vec3>& path, const Window& window)
{
    double halfWidth = window.width / 2.0 - FRAMING_MARGIN;
    double halfHeight = window.height / 2.0 - FRAMING_MARGIN;
    int count = 0;
    for (const Vec3& point : path)
    {
        Vec3 v = toView(view.attitude, point);
        if (v.z <= 0.0) continue;
        if (std::fabs(v.x * view.radius) <= halfWidth && std::fabs(v.y * view.radius) <= halfHeight) count++;
    }
    return count;
}

namespace
{

// The ball's middle for showing a path at a radius: the path's midpoint,
// moved towards the equator.
Vec3 pathCentre(const Vec3& h, const Vec3& s, const std::vector<Vec3>& path, const Window& window, double radius)
{
    double span = angleOf(h, s);
    Vec3 middle = path[path.size() / 2];

    // The side of the path away from the nearer pole. Looking from there,
    // the path bows towards the pole the way a great circle route does on
    // a wall map.
    Vec3 normal = cross(h, s);
    if (length(normal) < 1e-9) normal = cross(h, middle);
    normal = normalized(normal);
    if (dot(normal, NORTH) < 0.0) normal = scaled(normal, -1.0);
    double lift = std::min(1.0, CURL_SHARE * (window.height / 2.0 - FRAMING_MARGIN) / radius);
    double curl = std::min({0.25 * span, 15.0 / DEGREES, std::asin(lift)});

    return normalized(plus(scaled(middle, std::cos(curl)), scaled(normal, -std::sin(curl))));
}

} // namespace

Attitude framePathAt(const LatLon& home, const LatLon& station, const Window& window, double radius)
{
    Vec3 h = toVector(home);
    Vec3 s = toVector(station);
    if (angleOf(h, s) < 1e-6) return lookingAt(s);

    std::vector<Vec3> path = greatCircle(h, s, FRAMING_SEGMENTS);
    Vec3 centre = pathCentre(h, s, path, window, radius);

    std::vector<std::pair<Attitude, int>> candidates;
    for (int step = 0; step <= 90; step++)
    {
        for (int sign : {1, -1})
        {
            if ((step == 0 || step == 90) && sign < 0) continue;
            View view{lookingAt(centre, sign * step * 2.0 / DEGREES), radius};
            candidates.push_back({view.attitude, pointsInView(view, path, window)});
        }
    }

    int most = 0;
    for (const auto& candidate : candidates) most = std::max(most, candidate.second);
    int enough = most == (int)path.size() ? most : most - 1;
    for (const auto& candidate : candidates)
    {
        if (candidate.second >= enough) return candidate.first;
    }
    return candidates.front().first;
}

View framePath(const LatLon& home, const LatLon& station, const Window& window, const Zoom& zoom)
{
    Vec3 h = toVector(home);
    Vec3 s = toVector(station);
    if (angleOf(h, s) < 1e-6) return View{lookingAt(s), zoom.standard};

    // From closest out: the first radius any turn shows the whole path at,
    // and the first north up does.
    std::vector<Vec3> path = greatCircle(h, s, FRAMING_SEGMENTS);
    const int all = (int)path.size();
    View turned;
    bool haveTurned = false;
    for (double radius = zoom.nearest; radius >= zoom.farthest; radius *= ZOOM_STEP)
    {
        View view{framePathAt(home, station, window, radius), radius};
        if (pointsInView(view, path, window) < all) continue;
        if (!haveTurned)
        {
            turned = view;
            haveTurned = true;
        }
        View upright{lookingAt(pathCentre(h, s, path, window, radius)), radius};
        if (pointsInView(upright, path, window) == all) return upright;
        if (radius < NORTH_UP_SHARE * turned.radius) return turned;
    }
    if (haveTurned) return turned;

    // Nothing shows it all: as much as can be shown, from as far as allowed.
    return View{framePathAt(home, station, window, zoom.farthest), zoom.farthest};
}

Vec3 magnetFor(const LatLon& home, const LatLon& station)
{
    bool homeNorth = home.lat > station.lat;
    Vec3 south = toVector(homeNorth ? station : home);
    Vec3 north = toVector(homeNorth ? home : station);
    return normalized(plus(north, scaled(south, -1.0)));
}

Roller::Roller()
    : logRadius_(std::log(View().radius))
    , targetLogRadius_(logRadius_)
    , zoomSpeed_(0.0)
    , backOff_(0.0)
    , fieldOn_(false)
    , field_(0.0)
    , held_(false)
    , pointerX_(0.0)
    , pointerY_(0.0)
    , moving_(false)
{
    // empty
}

Roller::Quaternion Roller::fromAttitude(const Attitude& a)
{
    // The rotation taking earth to view: its rows are right, up and out.
    double m00 = a.right.x, m01 = a.right.y, m02 = a.right.z;
    double m10 = a.up.x, m11 = a.up.y, m12 = a.up.z;
    double m20 = a.out.x, m21 = a.out.y, m22 = a.out.z;

    Quaternion q;
    double trace = m00 + m11 + m22;
    if (trace > 0.0)
    {
        double s = std::sqrt(trace + 1.0) * 2.0;
        q.w = 0.25 * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q.w = (m21 - m12) / s;
        q.x = 0.25 * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25 * s;
        q.z = (m12 + m21) / s;
    }
    else
    {
        double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25 * s;
    }
    return q;
}

Attitude Roller::toAttitude(const Quaternion& q)
{
    Attitude a;
    a.right = {1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y - q.w * q.z), 2.0 * (q.x * q.z + q.w * q.y)};
    a.up = {2.0 * (q.x * q.y + q.w * q.z), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z - q.w * q.x)};
    a.out = {2.0 * (q.x * q.z - q.w * q.y), 2.0 * (q.y * q.z + q.w * q.x), 1.0 - 2.0 * (q.x * q.x + q.y * q.y)};
    return a;
}

void Roller::jump(const View& view)
{
    current_ = fromAttitude(view.attitude);
    target_ = current_;
    spin_ = Vec3{};
    logRadius_ = std::log(view.radius);
    targetLogRadius_ = logRadius_;
    zoomSpeed_ = 0.0;
    fieldOn_ = true;
    field_ = 1.0;
    magnet_ = view.attitude.up;
    floats_ = view.attitude.out;
    moving_ = held_;
}

void Roller::rollTo(const View& view)
{
    rollTo(view, view.attitude.up);
}

void Roller::rollTo(const View& view, const Vec3& magnet)
{
    target_ = fromAttitude(view.attitude);
    targetLogRadius_ = std::log(view.radius);
    if (!fieldOn_) field_ = 0.0;
    fieldOn_ = true;
    magnet_ = length(magnet) > 1e-9 ? normalized(magnet) : view.attitude.up;
    floats_ = view.attitude.out;
    moving_ = true;
}

void Roller::coast()
{
    fieldOn_ = false;
    field_ = 0.0;
    moving_ = true;
}

void Roller::zoomTo(double radius)
{
    targetLogRadius_ = std::log(radius);
    moving_ = true;
}

Vec3 Roller::underPointer(double radius) const
{
    double x = pointerX_ / radius;
    double y = pointerY_ / radius;
    double off = x * x + y * y;
    if (off >= 1.0) return Vec3{x / std::sqrt(off), y / std::sqrt(off), 0.0};
    return Vec3{x, y, std::sqrt(1.0 - off)};
}

bool Roller::grab(double x, double y)
{
    double radius = std::exp(logRadius_);
    if (x * x + y * y >= radius * radius) return false;
    pointerX_ = x;
    pointerY_ = y;
    grabbed_ = toEarth(toAttitude(current_), underPointer(radius));
    held_ = true;
    moving_ = true;
    return true;
}

void Roller::dragTo(double x, double y)
{
    pointerX_ = x;
    pointerY_ = y;
}

void Roller::letGo()
{
    held_ = false;
}

bool Roller::step(double seconds)
{
    if (!moving_) return false;

    auto multiply = [](const Quaternion& a, const Quaternion& b) {
        Quaternion r;
        r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
        r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
        r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
        r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
        return r;
    };

    const Vec3 TOWARDS_WINDOW{0.0, 0.0, 1.0};
    const double zoomStiffness = ZOOM_RATE * ZOOM_RATE;
    const double zoomDamping = 2.0 * ZOOM_DAMPING_RATIO * ZOOM_RATE;
    Vec3 error;
    while (seconds > 0.0)
    {
        double dt = std::min(seconds, STEP_SECONDS);
        seconds -= dt;
        Attitude now = toAttitude(current_);

        // What is left to turn, as a turn about the view's axes: the
        // target is that turn applied after where the ball is now.
        Quaternion inverse{current_.w, -current_.x, -current_.y, -current_.z};
        Quaternion e = multiply(target_, inverse);
        if (e.w < 0.0) e = Quaternion{-e.w, -e.x, -e.y, -e.z};
        double half = std::acos(std::clamp(e.w, -1.0, 1.0));
        double sinHalf = std::sin(half);
        error = sinHalf > 1e-12 ? scaled(Vec3{e.x, e.y, e.z}, 2.0 * half / sinHalf) : Vec3{};

        // Each pull turns the ball about the axis that brings a point of it
        // round towards where it is pulled, harder the more nearly at right
        // angles they are.
        Vec3 torque;
        double brake = FLUID_DRAG;
        if (fieldOn_)
        {
            field_ = std::min(1.0, field_ + dt / FIELD_RISE_SECONDS);
            Attitude goal = toAttitude(target_);
            Vec3 pulls = scaled(cross(toView(now, magnet_), toView(goal, magnet_)), MAGNET_PULL);
            pulls = plus(pulls, scaled(cross(toView(now, floats_), TOWARDS_WINDOW), FLOAT_PULL));
            pulls = plus(pulls, scaled(error, STRAIGHT_PULL));
            torque = plus(torque, scaled(pulls, field_));
            brake += FIELD_BRAKE * field_;
        }
        if (held_)
        {
            Vec3 pointer = underPointer(std::exp(logRadius_));
            torque = plus(torque, scaled(cross(toView(now, grabbed_), pointer), HOLD_PULL));
            brake += HOLD_BRAKE;
        }

        // The braking taken as it will be at the end of the step, so a hard
        // brake cannot turn the ball back the other way.
        spin_ = scaled(plus(spin_, scaled(torque, dt)), 1.0 / (1.0 + brake * dt));
        if (!fieldOn_ && !held_)
        {
            // Too slow to stir the fluid: it holds still.
            double speed = length(spin_);
            spin_ = speed > FLUID_STICK * dt ? scaled(spin_, 1.0 - FLUID_STICK * dt / speed) : Vec3{};
        }

        Vec3 turn = scaled(spin_, dt);
        double angle = length(turn);
        if (angle > 1e-15)
        {
            Vec3 axis = scaled(turn, 1.0 / angle);
            Quaternion r{std::cos(angle / 2.0), axis.x * std::sin(angle / 2.0), axis.y * std::sin(angle / 2.0),
                         axis.z * std::sin(angle / 2.0)};
            current_ = multiply(r, current_);
            double norm = std::sqrt(current_.w * current_.w + current_.x * current_.x +
                                    current_.y * current_.y + current_.z * current_.z);
            current_ = Quaternion{current_.w / norm, current_.x / norm, current_.y / norm, current_.z / norm};
        }

        // The zoom goes by a spring of its own, held back while the field
        // has far to roll the ball: no closer than shows the rest of the
        // roll across backOff_.
        double goal = targetLogRadius_;
        double left = std::min(length(error), PI / 2.0);
        if (fieldOn_ && !held_ && backOff_ > 0.0 && left > 1e-6)
        {
            goal = std::min(goal, std::log(backOff_ / std::sin(left)));
        }
        zoomSpeed_ += (zoomStiffness * (goal - logRadius_) - zoomDamping * zoomSpeed_) * dt;
        logRadius_ += zoomSpeed_ * dt;
    }

    bool zoomed = std::fabs(targetLogRadius_ - logRadius_) < SETTLED_RADIANS && std::fabs(zoomSpeed_) < SETTLED_SPIN;
    if (held_ || !zoomed) return moving_;
    if (fieldOn_ && field_ >= 1.0 && length(error) < SETTLED_RADIANS && length(spin_) < SETTLED_SPIN)
    {
        current_ = target_;
        spin_ = Vec3{};
        logRadius_ = targetLogRadius_;
        zoomSpeed_ = 0.0;
        moving_ = false;
    }
    else if (!fieldOn_ && length(spin_) == 0.0)
    {
        logRadius_ = targetLogRadius_;
        zoomSpeed_ = 0.0;
        moving_ = false;
    }
    return moving_;
}

View Roller::view() const
{
    return View{toAttitude(current_), std::exp(logRadius_)};
}

View Roller::target() const
{
    return View{toAttitude(fieldOn_ ? target_ : current_), std::exp(targetLogRadius_)};
}

std::vector<Outline> landOutlines(const View& view, const Window& window)
{
    std::vector<Outline> outlines;
    const double reach = visibleReach(view, window);
    std::vector<Vec3> projected;
    for (const Ring& ring : rings())
    {
        // Wholly behind, or nowhere near the window: nothing to draw.
        if (angleOf(ring.centre, view.attitude.out) - ring.reach > reach) continue;

        projected.clear();
        bool anyNear = false;
        bool allNear = true;
        for (const Vec3& p : ring.points)
        {
            projected.push_back(toView(view.attitude, p));
            anyNear = anyNear || projected.back().z > 0.0;
            allNear = allNear && projected.back().z > 0.0;
        }
        if (!anyNear) continue;
        if (allNear)
        {
            Outline outline;
            for (const Vec3& v : projected) outline.push_back(screenPoint(view, window, v.x, v.y));
            outlines.push_back(std::move(outline));
            continue;
        }

        // The runs on the near side, each from where the outline comes over
        // the rim to where it goes behind again, starting from a point
        // behind so no run is cut in two.
        struct Run
        {
            Outline points;
            double inAt = 0.0;      // angles round the rim
            double outAt = 0.0;
            bool used = false;
        };
        std::vector<Run> runs;
        const size_t n = projected.size();
        size_t behind = 0;
        while (projected[behind].z > 0.0) behind++;
        for (size_t k = 1; k <= n; k++)
        {
            const Vec3& previous = projected[(behind + k - 1) % n];
            const Vec3& v = projected[(behind + k) % n];
            bool wasNear = previous.z > 0.0;
            bool near = v.z > 0.0;
            if (!wasNear && near)
            {
                runs.emplace_back();
                runs.back().inAt = rimAngle(v, previous);
                runs.back().points.push_back(
                    screenPoint(view, window, std::cos(runs.back().inAt), std::sin(runs.back().inAt)));
            }
            if (near) runs.back().points.push_back(screenPoint(view, window, v.x, v.y));
            if (wasNear && !near)
            {
                runs.back().outAt = rimAngle(previous, v);
                runs.back().points.push_back(
                    screenPoint(view, window, std::cos(runs.back().outAt), std::sin(runs.back().outAt)));
            }
        }

        // Joined up round the rim: from where a run goes behind, the way
        // the outline goes round, to the first place any run comes back.
        // Land is on the right of a clockwise outline, so following the
        // rim clockwise keeps the land on the right too.
        auto aroundRim = [&](double from, double to) {
            double sweep = ring.clockwise ? from - to : to - from;
            sweep = std::fmod(sweep, 2.0 * PI);
            return sweep < 0.0 ? sweep + 2.0 * PI : sweep;
        };
        for (size_t first = 0; first < runs.size(); first++)
        {
            if (runs[first].used) continue;
            Outline outline;
            size_t at = first;
            while (!runs[at].used)
            {
                runs[at].used = true;
                outline.insert(outline.end(), runs[at].points.begin(), runs[at].points.end());
                size_t next = at;
                double nearest = 4.0 * PI;
                for (size_t r = 0; r < runs.size(); r++)
                {
                    double sweep = aroundRim(runs[at].outAt, runs[r].inAt);
                    if (sweep < nearest && (r == first || !runs[r].used))
                    {
                        nearest = sweep;
                        next = r;
                    }
                }
                followRim(outline, view, window, runs[at].outAt, ring.clockwise ? -nearest : nearest);
                at = next;
            }
            if (outline.size() >= 3) outlines.push_back(std::move(outline));
        }
    }
    return outlines;
}

std::vector<Outline> fieldLines(const View& view, const Window& window)
{
    std::vector<Outline> lines;
    std::vector<Vec3> points;
    for (int lat = -80; lat <= 80; lat += 10)
    {
        points.clear();
        for (double lon = -180.0; lon <= 180.0 + 1e-9; lon += GRID_STEP_DEGREES) points.push_back(toVector(LatLon{(double)lat, lon}));
        addNearRuns(lines, points, view, window);
    }
    for (int lon = -180; lon < 180; lon += 20)
    {
        points.clear();
        for (double lat = -90.0; lat <= 90.0 + 1e-9; lat += GRID_STEP_DEGREES) points.push_back(toVector(LatLon{lat, (double)lon}));
        addNearRuns(lines, points, view, window);
    }
    return lines;
}

bool isLand(const LatLon& position)
{
    // Winding round the point in longitude and latitude, as the outlines
    // were drawn on the map they come from.
    int winding = 0;
    for (int r = 0; r < LandOutlines::RING_COUNT; r++)
    {
        uint32_t first = LandOutlines::RING_START[r];
        uint32_t last = LandOutlines::RING_START[r + 1];
        for (uint32_t i = first; i < last; i++)
        {
            uint32_t j = i + 1 < last ? i + 1 : first;
            double x0 = LandOutlines::POINTS[2 * i] / LandOutlines::UNITS_PER_DEGREE;
            double y0 = LandOutlines::POINTS[2 * i + 1] / LandOutlines::UNITS_PER_DEGREE;
            double x1 = LandOutlines::POINTS[2 * j] / LandOutlines::UNITS_PER_DEGREE;
            double y1 = LandOutlines::POINTS[2 * j + 1] / LandOutlines::UNITS_PER_DEGREE;
            double side = (x1 - x0) * (position.lat - y0) - (position.lon - x0) * (y1 - y0);
            if (y0 <= position.lat && y1 > position.lat && side > 0.0) winding++;
            else if (y0 > position.lat && y1 <= position.lat && side < 0.0) winding--;
        }
    }
    return winding != 0;
}

} // namespace Globe
