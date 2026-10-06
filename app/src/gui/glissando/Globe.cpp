//=========================================================================
// Name:            Globe.cpp
// Purpose:         The sums behind the console's map ball.
//=========================================================================

#include "Globe.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "LandMask.h"

namespace Globe
{

namespace
{

constexpr double PI = 3.14159265358979323846;
constexpr double DEGREES = 180.0 / PI;
constexpr double EARTH_RADIUS_KM = 6371.0088;

// How the ball moves through its fluid: a spring towards where it is sent,
// damped a little past critical so it never overshoots, and stiff enough to
// settle in about two seconds.
constexpr double SPRING_RATE = 5.0;     // radians a second
constexpr double DAMPING_RATIO = 1.15;
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

// The ball's colours: silver land on a dark sea in darker fluid, as the
// rest of the console is silver on black.
constexpr double SEA[3] = {50, 52, 56};
constexpr double LAND[3] = {186, 181, 168};
constexpr double FLUID[3] = {14, 13, 12};
constexpr double RULING[3] = {232, 236, 230};
constexpr double RULING_STRENGTH = 0.16;

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

// The land mask, unpacked once: one byte a cell, 1 for land.
const std::vector<uint8_t>& landCells()
{
    static const std::vector<uint8_t> cells = []() {
        std::vector<uint8_t> unpacked((size_t)LandMask::WIDTH * LandMask::HEIGHT, 0);
        for (int row = 0; row < LandMask::HEIGHT; row++)
        {
            uint8_t value = 0;
            size_t column = 0;
            for (uint32_t i = LandMask::LAND_ROW_START[row]; i < LandMask::LAND_ROW_START[row + 1]; i++)
            {
                size_t run = LandMask::LAND_RUNS[i];
                if (value != 0)
                {
                    std::fill_n(unpacked.begin() + (size_t)row * LandMask::WIDTH + column, run, (uint8_t)1);
                }
                column += run;
                value ^= 1;
            }
        }
        return unpacked;
    }();
    return cells;
}

// How much of a position is land, 0 to 1, blending the four cells around
// it so coasts come out smooth.
double landAt(const std::vector<uint8_t>& cells, double lat, double lon)
{
    double u = (lon + 180.0) * LandMask::CELLS_PER_DEGREE - 0.5;
    double v = (90.0 - lat) * LandMask::CELLS_PER_DEGREE - 0.5;
    double u0 = std::floor(u);
    double v0 = std::floor(v);
    double fu = u - u0;
    double fv = v - v0;

    // Round the date line without dividing: this is done a lot.
    int left = (int)u0;
    while (left < 0) left += LandMask::WIDTH;
    while (left >= LandMask::WIDTH) left -= LandMask::WIDTH;
    int right = left + 1 == LandMask::WIDTH ? 0 : left + 1;
    const uint8_t* above = &cells[(size_t)std::clamp((int)v0, 0, LandMask::HEIGHT - 1) * LandMask::WIDTH];
    const uint8_t* below = &cells[(size_t)std::clamp((int)v0 + 1, 0, LandMask::HEIGHT - 1) * LandMask::WIDTH];
    double top = above[left] * (1.0 - fu) + above[right] * fu;
    double bottom = below[left] * (1.0 - fu) + below[right] * fu;
    return top * (1.0 - fv) + bottom * fv;
}

// How much of a pixel is land, the pixel being about the given number of
// degrees across. Where the pixel is wider than a cell or so, one look
// makes the coasts ragged, so it takes four spread over the pixel.
double landOver(const std::vector<uint8_t>& cells, double lat, double lon, double cosLat, double pixelDegrees)
{
    if (pixelDegrees * LandMask::CELLS_PER_DEGREE < 1.5) return landAt(cells, lat, lon);

    double dLat = 0.25 * pixelDegrees;
    double dLon = dLat / std::max(cosLat, 0.2);
    return 0.25 * (landAt(cells, lat - dLat, lon - dLon) + landAt(cells, lat - dLat, lon + dLon) +
                   landAt(cells, lat + dLat, lon - dLon) + landAt(cells, lat + dLat, lon + dLon));
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

std::string caption(const std::string& homeLocator, const std::string& stationLocator)
{
    static const std::string DOT = " \xC2\xB7 ";       // a middle dot, in UTF-8
    static const std::string DEGREE = "\xC2\xB0";

    std::string home = tidyLocator(homeLocator);
    std::string station = tidyLocator(stationLocator);
    if (station.empty()) return home;

    LatLon from;
    LatLon to;
    if (!locatorCentre(home, from) || !locatorCentre(station, to)) return station;
    long km = std::lround(distanceKm(from, to));
    if (km < 1) return station;

    long bearing = std::lround(bearingDegrees(from, to)) % 360;
    return station + DOT + withThousands(km) + " km" + DOT + std::to_string(bearing) + DEGREE;
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

int pointsInView(const Attitude& attitude, const std::vector<Vec3>& path, const Window& window)
{
    double halfWidth = window.width / 2.0 - FRAMING_MARGIN;
    double halfHeight = window.height / 2.0 - FRAMING_MARGIN;
    int count = 0;
    for (const Vec3& point : path)
    {
        Vec3 v = toView(attitude, point);
        if (v.z <= 0.0) continue;
        if (std::fabs(v.x * window.radius) <= halfWidth && std::fabs(v.y * window.radius) <= halfHeight) count++;
    }
    return count;
}

Attitude framePath(const LatLon& home, const LatLon& station, const Window& window)
{
    Vec3 h = toVector(home);
    Vec3 s = toVector(station);
    double span = angleOf(h, s);
    if (span < 1e-6) return lookingAt(s);

    std::vector<Vec3> path = greatCircle(h, s, FRAMING_SEGMENTS);
    Vec3 middle = path[(size_t)FRAMING_SEGMENTS / 2];

    // The side of the path away from the nearer pole. Looking from there,
    // the path bows towards the pole the way a great circle route does on
    // a wall map.
    Vec3 normal = cross(h, s);
    if (length(normal) < 1e-9) normal = cross(h, middle);
    normal = normalized(normal);
    if (dot(normal, NORTH) < 0.0) normal = scaled(normal, -1.0);
    double lift = std::min(1.0, CURL_SHARE * (window.height / 2.0 - FRAMING_MARGIN) / window.radius);
    double curl = std::min({0.25 * span, 15.0 / DEGREES, std::asin(lift)});

    Vec3 centre = normalized(plus(scaled(middle, std::cos(curl)), scaled(normal, -std::sin(curl))));

    // North up if that shows the whole path, or as much of it as any turn
    // does; otherwise the smallest turn, either way, that shows the most.
    std::vector<std::pair<Attitude, int>> candidates;
    for (int step = 0; step <= 90; step++)
    {
        for (int sign : {1, -1})
        {
            if ((step == 0 || step == 90) && sign < 0) continue;
            Attitude attitude = lookingAt(centre, sign * step * 2.0 / DEGREES);
            candidates.push_back({attitude, pointsInView(attitude, path, window)});
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

Roller::Roller()
    : moving_(false)
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

void Roller::jump(const Attitude& attitude)
{
    current_ = fromAttitude(attitude);
    target_ = current_;
    spin_ = Vec3{};
    moving_ = false;
}

void Roller::rollTo(const Attitude& attitude)
{
    target_ = fromAttitude(attitude);
    moving_ = true;
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

    const double stiffness = SPRING_RATE * SPRING_RATE;
    const double damping = 2.0 * DAMPING_RATIO * SPRING_RATE;
    Vec3 error;
    while (seconds > 0.0)
    {
        double dt = std::min(seconds, STEP_SECONDS);
        seconds -= dt;

        // What is left to turn, as a turn about the view's axes: the
        // target is that turn applied after where the ball is now.
        Quaternion inverse{current_.w, -current_.x, -current_.y, -current_.z};
        Quaternion e = multiply(target_, inverse);
        if (e.w < 0.0) e = Quaternion{-e.w, -e.x, -e.y, -e.z};
        double half = std::acos(std::clamp(e.w, -1.0, 1.0));
        double sinHalf = std::sin(half);
        error = sinHalf > 1e-12 ? scaled(Vec3{e.x, e.y, e.z}, 2.0 * half / sinHalf) : Vec3{};

        spin_ = plus(spin_, scaled(plus(scaled(error, stiffness), scaled(spin_, -damping)), dt));

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
    }

    if (length(error) < SETTLED_RADIANS && length(spin_) < SETTLED_SPIN)
    {
        current_ = target_;
        spin_ = Vec3{};
        moving_ = false;
    }
    return moving_;
}

Attitude Roller::attitude() const
{
    return toAttitude(current_);
}

void paintBall(const Attitude& attitude, const Window& window, int width, int height, std::vector<uint8_t>& rgb)
{
    rgb.assign((size_t)width * height * 3, 0);
    const std::vector<uint8_t>& cells = landCells();

    const double radius = window.radius;
    const Vec3 light = normalized(Vec3{-0.35, 0.5, 0.79});
    const Vec3 halfway = normalized(plus(light, Vec3{0.0, 0.0, 1.0}));
    const double degreesPerPixel = DEGREES / radius;

    for (int py = 0; py < height; py++)
    {
        for (int px = 0; px < width; px++)
        {
            double sx = (px + 0.5 - width / 2.0) / radius;
            double sy = (height / 2.0 - (py + 0.5)) / radius;
            double r = std::sqrt(sx * sx + sy * sy);

            double colour[3] = {FLUID[0], FLUID[1], FLUID[2]};

            double coverage = std::clamp((1.0 - r) * radius + 0.5, 0.0, 1.0);
            if (coverage > 0.0)
            {
                double sz = std::sqrt(std::max(0.0, 1.0 - std::min(r, 1.0) * std::min(r, 1.0)));
                Vec3 normal{sx, sy, sz};
                if (r > 1.0) normal = normalized(normal);
                LatLon position = toLatLon(toEarth(attitude, normal));

                // About how many degrees the pixel covers, foreshortened.
                double pixel = degreesPerPixel / std::max(sz, 0.15);

                double cosLat = std::cos(position.lat / DEGREES);
                double land = landOver(cells, position.lat, position.lon, cosLat, pixel);
                double surface[3];
                for (int k = 0; k < 3; k++) surface[k] = SEA[k] + (LAND[k] - SEA[k]) * land;

                // The Maidenhead fields, 20 degrees of longitude by 10 of
                // latitude, ruled about a pixel wide however foreshortened.
                double offLat = std::fabs(position.lat - 10.0 * std::round(position.lat / 10.0));
                double offLon = std::fabs(position.lon - 20.0 * std::round(position.lon / 20.0)) * cosLat;
                double ruling = std::clamp(1.0 - std::min(offLat, offLon) / (0.7 * pixel), 0.0, 1.0);
                for (int k = 0; k < 3; k++) surface[k] += (RULING[k] - surface[k]) * ruling * RULING_STRENGTH;

                double lambert = std::max(0.0, dot(normal, light));
                double shade = 0.28 + 0.82 * lambert;
                double glint = std::max(0.0, dot(normal, halfway));
                double glint8 = glint * glint * glint * glint;
                glint8 *= glint8;
                double glint32 = glint8 * glint8;
                glint32 *= glint32;
                double shine = glint32 * glint8 * 50.0;     // to the 40th
                for (int k = 0; k < 3; k++)
                {
                    double lit = surface[k] * shade + shine;
                    colour[k] += (lit - colour[k]) * coverage;
                }
            }

            uint8_t* out = &rgb[((size_t)py * width + px) * 3];
            for (int k = 0; k < 3; k++) out[k] = (uint8_t)std::clamp(colour[k] + 0.5, 0.0, 255.0);
        }
    }
}

bool isLand(const LatLon& position)
{
    return landAt(landCells(), position.lat, position.lon) >= 0.5;
}

} // namespace Globe
