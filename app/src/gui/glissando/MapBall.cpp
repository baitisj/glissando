//=========================================================================
// Name:            MapBall.cpp
// Purpose:         The console's map ball.
//=========================================================================

#include "MapBall.h"

#include <algorithm>
#include <cmath>

#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/time.h>

#include "ChaoticaTheme.h"

using namespace Chaotica;

namespace
{

constexpr int WINDOW_WIDTH = 416;
constexpr int WINDOW_HEIGHT = 104;
constexpr double BALL_RADIUS = 245.0;
constexpr int BEZEL = 2;
constexpr int CAPTION_GAP = 4;
constexpr int CAPTION_HEIGHT = 14;

// Frames while the ball rolls, and how long the path takes to curl out
// from our dot to the station.
constexpr int FRAME_MILLISECONDS = 30;
constexpr double PATH_SECONDS = 1.6;
constexpr int PATH_SEGMENTS = 96;

const wxColour PATH_CASING(0, 0, 0, 150);

// Eases the path out quickly and in slowly, like the ball.
double eased(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1.0 - std::pow(1.0 - t, 3.0);
}

} // namespace

MapBall::MapBall(wxWindow* parent)
    : Control(parent, wxID_ANY,
              wxSize(WINDOW_WIDTH + 2 * BEZEL, WINDOW_HEIGHT + 2 * BEZEL + CAPTION_GAP + CAPTION_HEIGHT))
    , placed_(false)
    , haveHome_(false)
    , haveStation_(false)
    , pathDrawn_(1.0)
    , timer_(this)
    , lastTickMs_(0)
    , ballStale_(true)
{
    window_.width = WINDOW_WIDTH;
    window_.height = WINDOW_HEIGHT;
    window_.radius = BALL_RADIUS;
    SetToolTip(_("Where the last station heard or sent to is, if it has sent its grid square: "
                 "the square, how far it is from yours and which way. Set yours in "
                 "Preferences, Station."));
    Bind(wxEVT_TIMER, &MapBall::OnTimer, this);
}

void MapBall::setLocators(const std::string& home, const std::string& station)
{
    if (placed_ && home == home_ && station == station_) return;
    bool stationChanged = station != station_;
    home_ = home;
    station_ = station;
    haveHome_ = Globe::locatorCentre(home, homeAt_);
    haveStation_ = Globe::locatorCentre(station, stationAt_);
    caption_ = wxString::FromUTF8(Globe::caption(home, station).c_str());

    Globe::Attitude target;
    if (haveHome_ && haveStation_) target = Globe::framePath(homeAt_, stationAt_, window_);
    else if (haveStation_) target = Globe::lookingAt(Globe::toVector(stationAt_));
    else if (haveHome_) target = Globe::lookingAt(Globe::toVector(homeAt_));
    else
    {
        Refresh();
        return;
    }

    if (!placed_)
    {
        // Where it starts: no rolling to get there.
        roller_.jump(target);
        placed_ = true;
        pathDrawn_ = 1.0;
    }
    else
    {
        roller_.rollTo(target);
        if (stationChanged) pathDrawn_ = 0.0;
        lastTickMs_ = wxGetLocalTimeMillis().GetValue();
        if (!timer_.IsRunning()) timer_.Start(FRAME_MILLISECONDS);
    }
    ballStale_ = true;
    Refresh();
}

void MapBall::OnTimer(wxTimerEvent&)
{
    long long now = wxGetLocalTimeMillis().GetValue();
    double seconds = std::clamp((now - lastTickMs_) / 1000.0, 0.0, 0.1);
    lastTickMs_ = now;

    if (roller_.moving())
    {
        roller_.step(seconds);
        ballStale_ = true;
    }
    pathDrawn_ = std::min(1.0, pathDrawn_ + seconds / PATH_SECONDS);
    if (!roller_.moving() && pathDrawn_ >= 1.0) timer_.Stop();
    Refresh();
}

void MapBall::drawPath(wxGraphicsContext* gc, const Globe::Attitude& attitude, double left, double top)
{
    double cx = left + WINDOW_WIDTH / 2.0;
    double cy = top + WINDOW_HEIGHT / 2.0;
    auto onScreen = [&](const Globe::Vec3& v) {
        return wxPoint2DDouble(cx + v.x * BALL_RADIUS, cy - v.y * BALL_RADIUS);
    };

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
            Globe::Vec3 v;
            if (i <= reach)
            {
                v = Globe::toView(attitude, path[(size_t)i]);
            }
            else
            {
                // The last piece, part of a segment.
                double f = reach - std::floor(reach);
                const Globe::Vec3& a = path[(size_t)i - 1];
                const Globe::Vec3& b = path[(size_t)i];
                Globe::Vec3 p{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f};
                v = Globe::toView(attitude, p);
            }
            if (v.z > 0.0) runs.back().push_back(onScreen(v));
            else if (!runs.back().empty()) runs.emplace_back();
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
    if (haveHome_)
    {
        Globe::Vec3 v = Globe::toView(attitude, home);
        if (v.z > 0.0)
        {
            wxPoint2DDouble p = onScreen(v);
            gc->SetPen(wxPen(PATH_CASING, 1));
            gc->SetBrush(wxBrush(Colour::Glow));
            gc->DrawEllipse(p.m_x - 2.5, p.m_y - 2.5, 5, 5);
        }
    }
    if (haveStation_)
    {
        Globe::Vec3 v = Globe::toView(attitude, station);
        if (v.z > 0.0)
        {
            wxPoint2DDouble p = onScreen(v);
            gc->SetPen(wxPen(PATH_CASING, 1));
            gc->SetBrush(wxBrush(Colour::Alarm));
            gc->DrawRectangle(p.m_x - 2.5, p.m_y - 2.5, 5, 5);
        }
    }
}

void MapBall::paint(wxGraphicsContext* gc, const wxSize&)
{
    const double left = BEZEL;
    const double top = BEZEL;
    Globe::Attitude attitude = roller_.attitude();

    if (ballStale_ || !ball_.IsOk())
    {
        Globe::paintBall(attitude, window_, WINDOW_WIDTH, WINDOW_HEIGHT, rgb_);
        wxImage image(WINDOW_WIDTH, WINDOW_HEIGHT, rgb_.data(), true);
        ball_ = wxBitmap(image);
        ballStale_ = false;
    }

    // A recessed window in the plate, the ball under it.
    gc->SetPen(wxPen(Colour::PlateEdge, 1));
    gc->SetBrush(wxBrush(Colour::PlateShadow));
    gc->DrawRoundedRectangle(left - 1.5, top - 1.5, WINDOW_WIDTH + 3, WINDOW_HEIGHT + 3, 4);

    gc->PushState();
    gc->Clip(left, top, WINDOW_WIDTH, WINDOW_HEIGHT);
    gc->DrawBitmap(ball_, left, top, WINDOW_WIDTH, WINDOW_HEIGHT);
    if (placed_) drawPath(gc, attitude, left, top);

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
        gc->SetFont(font(FontRole::Caption), haveStation_ ? Colour::Bone : Colour::Dim);
        gc->GetTextExtent(caption_, &tw, &th);
        gc->DrawText(caption_, left + (WINDOW_WIDTH - tw) / 2.0,
                     top + WINDOW_HEIGHT + BEZEL + CAPTION_GAP + (CAPTION_HEIGHT - th) / 2.0);
    }
}
