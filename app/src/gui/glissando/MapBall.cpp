//=========================================================================
// Name:            MapBall.cpp
// Purpose:         The console's map ball.
//=========================================================================

#include "MapBall.h"

#include <algorithm>
#include <cmath>

#include <wx/graphics.h>
#include <wx/time.h>

#include "ChaoticaTheme.h"

using namespace Chaotica;

namespace
{

constexpr int WINDOW_WIDTH = 416;
constexpr int WINDOW_HEIGHT = 104;
constexpr int BEZEL = 2;
constexpr int CAPTION_GAP = 4;
constexpr int CAPTION_HEIGHT = 14;

// Frames while the ball rolls, and how long the path takes to curl out
// from our dot to the station.
constexpr int FRAME_MILLISECONDS = 30;
constexpr double PATH_SECONDS = 1.6;
constexpr int PATH_SEGMENTS = 96;

// Each notch of the mouse wheel comes this much closer or backs this much
// off.
constexpr double WHEEL_STEP = 1.25;

// Where the ball looks before it knows any grid square: the North Atlantic,
// the Americas to one side and Europe and Africa to the other.
const Globe::LatLon NOWHERE_YET{30.0, -30.0};

// Silver land on a dark sea in darker fluid, as the rest of the console is
// silver on black, with the Maidenhead fields ruled faintly over both.
const wxColour FLUID(14, 13, 12);
const wxColour SEA(50, 52, 56);
const wxColour LAND(186, 181, 168);
const wxColour RULING(232, 236, 230, 44);
const wxColour PATH_CASING(0, 0, 0, 150);

// Borders between countries, if shown: faint dark lines on the land.
const wxColour BORDER(64, 60, 54, 120);
constexpr double BORDER_WIDTH = 0.9;

// Brushed metal, if shown: the marks at their lightest and darkest, and the
// band where the light catches them at its brightest.
constexpr double MARK_LIGHT = 30.0;
constexpr double MARK_DARK = 24.0;
constexpr double SHEEN_BRIGHTEST = 120.0;

// Where the light comes from, over the viewer's left shoulder, as a point
// on the ball's face: the highlight sits there and the ball darkens away
// from it.
constexpr double LIGHT_X = -0.35;
constexpr double LIGHT_Y = 0.5;

// Eases the path out quickly and in slowly, like the ball.
double eased(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1.0 - std::pow(1.0 - t, 3.0);
}

wxPoint2DDouble at(const Globe::Point& p, double left, double top)
{
    return wxPoint2DDouble(left + p.x, top + p.y);
}

} // namespace

MapBall::MapBall(wxWindow* parent)
    : Control(parent, wxID_ANY,
              wxSize(WINDOW_WIDTH + 2 * BEZEL, WINDOW_HEIGHT + 2 * BEZEL + CAPTION_GAP + CAPTION_HEIGHT))
    , placed_(false)
    , haveHome_(false)
    , haveStation_(false)
    , stationCurrent_(true)
    , borders_(true)
    , brushedMetal_(true)
    , pathDrawn_(1.0)
    , timer_(this)
    , lastTickMs_(0)
{
    window_.width = WINDOW_WIDTH;
    window_.height = WINDOW_HEIGHT;
    roller_.setBackOff(WINDOW_WIDTH / 2.0);
    roller_.jump(fitted());
    caption_ = _("Set your grid square in Preferences, Station");
    SetToolTip(_("Where the last station heard or sent to is, if it has sent its grid square: "
                 "the square, how far it is from yours and which way. Drag the ball to spin it "
                 "and hold it to stop it; turn the mouse wheel over it to zoom; double-click to "
                 "fit the path again. Set your own square in Preferences, Station."));
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_TIMER, &MapBall::OnTimer, this);
    Bind(wxEVT_MOUSEWHEEL, &MapBall::OnMouseWheel, this);
    Bind(wxEVT_LEFT_DCLICK, &MapBall::OnDoubleClick, this);
    Bind(wxEVT_LEFT_DOWN, &MapBall::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &MapBall::OnMouseUp, this);
    Bind(wxEVT_MOTION, &MapBall::OnMouseMove, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &MapBall::OnCaptureLost, this);
}

Globe::View MapBall::fitted() const
{
    if (showsPath()) return Globe::framePath(homeAt_, stationAt_, window_, zoom_);
    Globe::View view;
    view.radius = zoom_.standard;
    view.attitude = Globe::lookingAt(Globe::toVector(haveStation_ ? stationAt_ : haveHome_ ? homeAt_ : NOWHERE_YET));
    return view;
}

Globe::Vec3 MapBall::magnet() const
{
    if (showsPath()) return Globe::magnetFor(homeAt_, stationAt_);
    return fitted().attitude.up;
}

void MapBall::rollTo(const Globe::View& view)
{
    roller_.rollTo(view, magnet());
    wake();
}

void MapBall::wake()
{
    if (timer_.IsRunning()) return;
    lastTickMs_ = wxGetLocalTimeMillis().GetValue();
    timer_.Start(FRAME_MILLISECONDS);
}

void MapBall::pointer(const wxMouseEvent& event, double& x, double& y) const
{
    x = event.GetX() - (BEZEL + WINDOW_WIDTH / 2.0);
    y = (BEZEL + WINDOW_HEIGHT / 2.0) - event.GetY();
}

void MapBall::setLocators(const std::string& home, const std::string& station, bool stationCurrent)
{
    if (home == home_ && station == station_ && stationCurrent == stationCurrent_) return;
    bool moved = home != home_ || station != station_;
    bool stationChanged = station != station_;
    home_ = home;
    station_ = station;
    stationCurrent_ = stationCurrent;
    haveHome_ = Globe::locatorCentre(home, homeAt_);
    haveStation_ = Globe::locatorCentre(station, stationAt_);
    caption_ = haveHome_ || haveStation_
                   ? wxString::FromUTF8(Globe::caption(home, station, stationCurrent).c_str())
                   : _("Set your grid square in Preferences, Station");
    if (!moved)
    {
        // Only the square's standing changed: the caption says so.
        Refresh();
        return;
    }

    if (!placed_ && (haveHome_ || haveStation_))
    {
        // The first square it is given, it starts at: no rolling to get
        // there.
        roller_.jump(fitted());
        placed_ = true;
        pathDrawn_ = 1.0;
    }
    else
    {
        if (stationChanged) pathDrawn_ = 0.0;
        rollTo(fitted());
    }
    Refresh();
}

void MapBall::setLook(bool borders, bool brushedMetal)
{
    if (borders == borders_ && brushedMetal == brushedMetal_) return;
    borders_ = borders;
    brushedMetal_ = brushedMetal;
    Refresh();
}

void MapBall::OnTimer(wxTimerEvent&)
{
    long long now = wxGetLocalTimeMillis().GetValue();
    double seconds = std::clamp((now - lastTickMs_) / 1000.0, 0.0, 0.1);
    lastTickMs_ = now;

    roller_.step(seconds);
    pathDrawn_ = std::min(1.0, pathDrawn_ + seconds / PATH_SECONDS);
    if (!roller_.moving() && pathDrawn_ >= 1.0) timer_.Stop();
    Refresh();
}

void MapBall::OnMouseWheel(wxMouseEvent& event)
{
    if (event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL || event.GetWheelDelta() == 0) return;

    // From where it is going, so quick turns of the wheel add up.
    double notches = (double)event.GetWheelRotation() / event.GetWheelDelta();
    roller_.zoomTo(std::clamp(roller_.target().radius * std::pow(WHEEL_STEP, notches), zoom_.farthest, zoom_.nearest));
    wake();
}

void MapBall::OnDoubleClick(wxMouseEvent&)
{
    rollTo(fitted());
}

void MapBall::OnMouseDown(wxMouseEvent& event)
{
    double x = 0.0, y = 0.0;
    pointer(event, x, y);
    if (std::fabs(y) > WINDOW_HEIGHT / 2.0 || !roller_.grab(x, y)) return;

    // With nothing but our own square to show, nothing draws the ball
    // back: let go, it is free to spin.
    if (!showsPath()) roller_.coast();
    if (!HasCapture()) CaptureMouse();
    wake();
}

void MapBall::OnMouseUp(wxMouseEvent&)
{
    if (HasCapture()) ReleaseMouse();
    roller_.letGo();
}

void MapBall::OnMouseMove(wxMouseEvent& event)
{
    if (!roller_.held()) return;
    double x = 0.0, y = 0.0;
    pointer(event, x, y);
    roller_.dragTo(x, y);
}

void MapBall::OnCaptureLost(wxMouseCaptureLostEvent&)
{
    roller_.letGo();
}

void MapBall::drawBall(wxGraphicsContext* gc, const Globe::View& view, double left, double top)
{
    const double radius = view.radius;
    const double cx = left + WINDOW_WIDTH / 2.0;
    const double cy = top + WINDOW_HEIGHT / 2.0;

    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(FLUID));
    gc->DrawRectangle(left, top, WINDOW_WIDTH, WINDOW_HEIGHT);
    gc->SetBrush(wxBrush(SEA));
    gc->DrawEllipse(cx - radius, cy - radius, 2.0 * radius, 2.0 * radius);

    // The land in one go, so where two countries meet there is no seam.
    wxGraphicsPath land = gc->CreatePath();
    for (const Globe::Outline& outline : Globe::landOutlines(view, window_))
    {
        land.MoveToPoint(at(outline.front(), left, top));
        for (size_t i = 1; i < outline.size(); i++) land.AddLineToPoint(at(outline[i], left, top));
        land.CloseSubpath();
    }
    gc->SetBrush(wxBrush(LAND));
    gc->FillPath(land, wxWINDING_RULE);
    if (brushedMetal_) drawBrushing(gc, view, land, left, top);

    if (borders_)
    {
        wxGraphicsPath lines = gc->CreatePath();
        for (const Globe::Outline& line : Globe::borderLines(view, window_))
        {
            lines.MoveToPoint(at(line.front(), left, top));
            for (size_t i = 1; i < line.size(); i++) lines.AddLineToPoint(at(line[i], left, top));
        }
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(BORDER, BORDER_WIDTH).Join(wxJOIN_ROUND)));
        gc->StrokePath(lines);
    }

    wxGraphicsPath fields = gc->CreatePath();
    for (const Globe::Outline& line : Globe::fieldLines(view, window_))
    {
        fields.MoveToPoint(at(line.front(), left, top));
        for (size_t i = 1; i < line.size(); i++) fields.AddLineToPoint(at(line[i], left, top));
    }
    gc->SetPen(wxPen(RULING, 1));
    gc->StrokePath(fields);

    // Light over the left shoulder: darker the farther round from it, and
    // a soft shine where it catches.
    gc->SetPen(*wxTRANSPARENT_PEN);
    wxGraphicsGradientStops shade(wxColour(0, 0, 0, 0), wxColour(0, 0, 0, 175));
    shade.Add(wxColour(0, 0, 0, 40), 0.55f);
    shade.Add(wxColour(0, 0, 0, 105), 0.85f);
    gc->SetBrush(gc->CreateRadialGradientBrush(cx + LIGHT_X * radius, cy - LIGHT_Y * radius, cx, cy, radius, shade));
    gc->DrawEllipse(cx - radius, cy - radius, 2.0 * radius, 2.0 * radius);

    wxGraphicsGradientStops shine(wxColour(255, 255, 255, 46), wxColour(255, 255, 255, 0));
    double glow = 0.45 * radius;
    double gx = cx + LIGHT_X * radius;
    double gy = cy - LIGHT_Y * radius;
    gc->SetBrush(gc->CreateRadialGradientBrush(gx, gy, gx, gy, glow, shine));
    gc->DrawEllipse(gx - glow, gy - glow, 2.0 * glow, 2.0 * glow);
}

void MapBall::drawBrushing(wxGraphicsContext* gc, const Globe::View& view, const wxGraphicsPath& land, double left,
                           double top)
{
    Globe::Brushing brushing = Globe::brushing(view, window_, Globe::Point{LIGHT_X, -LIGHT_Y});
    gc->SetPen(*wxTRANSPARENT_PEN);

    // The band of light, laid over the land: a bright streak down its
    // middle in a softer glow, fading out to either side.
    auto light = [](double share) { return wxColour(255, 255, 250, (unsigned char)std::lround(SHEEN_BRIGHTEST * share)); };
    wxGraphicsGradientStops sheen(light(0.0), light(0.0));
    sheen.Add(light(0.15), 0.3f);
    sheen.Add(light(0.45), 0.42f);
    sheen.Add(light(1.0), 0.5f);
    sheen.Add(light(0.45), 0.58f);
    sheen.Add(light(0.15), 0.7f);
    const Globe::Point& middle = brushing.sheenMiddle;
    const Globe::Point& across = brushing.across;
    const double half = brushing.sheenHalfWidth;
    gc->SetBrush(gc->CreateLinearGradientBrush(left + middle.x - across.x * half, top + middle.y - across.y * half,
                                               left + middle.x + across.x * half, top + middle.y + across.y * half,
                                               sheen));
    gc->FillPath(land, wxWINDING_RULE);

    // The marks: a gradient with a stop for every few pixels, round the
    // centre or straight across north.
    if (brushing.marks.size() < 2) return;
    auto shade = [](double value) {
        return value >= 0.0 ? wxColour(255, 255, 250, (unsigned char)std::lround(MARK_LIGHT * value))
                            : wxColour(0, 0, 0, (unsigned char)std::lround(-MARK_DARK * value));
    };
    const double from = brushing.marks.front().first;
    const double to = brushing.marks.back().first;
    wxGraphicsGradientStops marks(shade(brushing.marks.front().second), shade(brushing.marks.back().second));
    for (const auto& mark : brushing.marks)
    {
        double position = brushing.straight ? (mark.first - from) / (to - from) : mark.first / to;
        marks.Add(shade(mark.second), (float)position);
    }
    const Globe::Point& centre = brushing.centre;
    if (brushing.straight)
    {
        const Globe::Point& north = brushing.north;
        gc->SetBrush(gc->CreateLinearGradientBrush(left + centre.x + north.x * from, top + centre.y + north.y * from,
                                                   left + centre.x + north.x * to, top + centre.y + north.y * to,
                                                   marks));
    }
    else
    {
        gc->SetBrush(gc->CreateRadialGradientBrush(left + centre.x, top + centre.y, left + centre.x, top + centre.y,
                                                   to, marks));
    }
    gc->FillPath(land, wxWINDING_RULE);
}

void MapBall::drawPath(wxGraphicsContext* gc, const Globe::View& view, double left, double top)
{
    Globe::Vec3 home = Globe::toVector(homeAt_);
    Globe::Vec3 station = Globe::toVector(stationAt_);

    if (haveHome_ && haveStation_ && pathDrawn_ > 0.0)
    {
        // The near side of the path, as far as it has curled out so far.
        std::vector<Globe::Vec3> path = Globe::greatCircle(home, station, PATH_SEGMENTS);
        double reach = eased(pathDrawn_) * PATH_SEGMENTS;
        std::vector<std::vector<wxPoint2DDouble>> runs(1);
        for (int i = 0; i <= PATH_SEGMENTS; i++)
        {
            Globe::Vec3 p = path[(size_t)i];
            if (i > reach)
            {
                // The last piece, part of a segment.
                double f = reach - std::floor(reach);
                const Globe::Vec3& a = path[(size_t)i - 1];
                const Globe::Vec3& b = path[(size_t)i];
                p = Globe::Vec3{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f};
            }
            if (Globe::toView(view.attitude, p).z > 0.0)
            {
                runs.back().push_back(at(Globe::onScreen(view, window_, p), left, top));
            }
            else if (!runs.back().empty())
            {
                runs.emplace_back();
            }
            if (i > reach) break;
        }

        for (int pass = 0; pass < 2; pass++)
        {
            gc->SetPen(pass == 0 ? wxPen(PATH_CASING, 3) : wxPen(Colour::Glow, 1));
            for (const auto& run : runs)
            {
                if (run.size() >= 2) gc->StrokeLines(run.size(), run.data());
            }
        }
    }

    // Our dot and the station's red square, where they are on the near side.
    if (haveHome_ && Globe::toView(view.attitude, home).z > 0.0)
    {
        wxPoint2DDouble p = at(Globe::onScreen(view, window_, home), left, top);
        gc->SetPen(wxPen(PATH_CASING, 1));
        gc->SetBrush(wxBrush(Colour::Glow));
        gc->DrawEllipse(p.m_x - 2.5, p.m_y - 2.5, 5, 5);
    }
    if (haveStation_ && Globe::toView(view.attitude, station).z > 0.0)
    {
        wxPoint2DDouble p = at(Globe::onScreen(view, window_, station), left, top);
        gc->SetPen(wxPen(PATH_CASING, 1));
        gc->SetBrush(wxBrush(Colour::Alarm));
        gc->DrawRectangle(p.m_x - 2.5, p.m_y - 2.5, 5, 5);
    }
}

void MapBall::paint(wxGraphicsContext* gc, const wxSize&)
{
    const double left = BEZEL;
    const double top = BEZEL;
    Globe::View view = roller_.view();

    // A recessed window in the plate, the ball under it.
    gc->SetPen(wxPen(Colour::PlateEdge, 1));
    gc->SetBrush(wxBrush(Colour::PlateShadow));
    gc->DrawRoundedRectangle(left - 1.5, top - 1.5, WINDOW_WIDTH + 3, WINDOW_HEIGHT + 3, 4);

    gc->PushState();
    gc->Clip(left, top, WINDOW_WIDTH, WINDOW_HEIGHT);
    drawBall(gc, view, left, top);
    drawPath(gc, view, left, top);

    // The glass over it: a sheen across the top, and the fluid's edge
    // darkening into the frame top and bottom.
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateLinearGradientBrush(0, top, 0, top + WINDOW_HEIGHT * 0.45, wxColour(255, 255, 255, 30),
                                               wxColour(255, 255, 255, 0)));
    gc->DrawRectangle(left, top, WINDOW_WIDTH, WINDOW_HEIGHT * 0.45);
    gc->SetBrush(gc->CreateLinearGradientBrush(0, top, 0, top + 8, wxColour(0, 0, 0, 120), wxColour(0, 0, 0, 0)));
    gc->DrawRectangle(left, top, WINDOW_WIDTH, 8);
    gc->SetBrush(gc->CreateLinearGradientBrush(0, top + WINDOW_HEIGHT - 8, 0, top + WINDOW_HEIGHT,
                                               wxColour(0, 0, 0, 0), wxColour(0, 0, 0, 120)));
    gc->DrawRectangle(left, top + WINDOW_HEIGHT - 8, WINDOW_WIDTH, 8);
    gc->PopState();

    if (!caption_.empty())
    {
        double tw = 0, th = 0;
        gc->SetFont(font(FontRole::Caption), haveStation_ && stationCurrent_ ? Colour::Bone : Colour::Dim);
        gc->GetTextExtent(caption_, &tw, &th);
        gc->DrawText(caption_, left + (WINDOW_WIDTH - tw) / 2.0,
                     top + WINDOW_HEIGHT + BEZEL + CAPTION_GAP + (CAPTION_HEIGHT - th) / 2.0);
    }
}
