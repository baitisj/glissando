//=========================================================================
// Name:            MapBall.h
// Purpose:         The console's map ball: a map of the world on a big ball
//                  floating in something thick, seen through a window four
//                  times as wide as it is tall, beside the Engaged, Receiving
//                  and Transmitting lamps.
//
// Given our grid square and a station's, it rolls to show the path between
// them as close as it can and eases to a stop, the path curling out from
// our dot to the station's red square, with the square, the distance and
// the bearing under the window. A station whose square is not known leaves
// the ball where it is. The mouse wheel over it zooms in and out; a double
// click fits the path again. It is all drawn as outlines, so it is sharp at
// any size; the borders between countries, and the land as brushed metal,
// can each be turned off in Preferences. The sums are in Globe.h.
//
// The ball is heavy and the fluid thick. Dragged, it spins, and with only
// our own square to show it coasts on until the fluid stops it. Held, it
// stops almost at once. With a path to show, a magnet in it, from the
// path's southern end to its northern end, is drawn into line by a field
// that brakes it as it goes, so a spinning ball soon tumbles round into
// the path's view and settles there.
//=========================================================================

#ifndef GUI_GLISSANDO__MAP_BALL_H
#define GUI_GLISSANDO__MAP_BALL_H

#include <string>

#include <wx/graphics.h>
#include <wx/timer.h>

#include "ChaoticaControls.h"
#include "Globe.h"

class MapBall : public Chaotica::Control
{
public:
    explicit MapBall(wxWindow* parent);

    // Our locator and the station's, either of them possibly empty, and
    // whether the station's came from it this contact. The ball rolls when
    // they change; the first ones it is given, it starts at. Until then it
    // shows the North Atlantic, and asks for our square. A station's square
    // kept from an earlier contact is shown dim and marked as such.
    void setLocators(const std::string& home, const std::string& station, bool stationCurrent = true);

    // Whether the land shows the borders between countries, and whether it
    // looks like brushed metal. Both cost a little more to draw.
    void setLook(bool borders, bool brushedMetal);

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override;

private:
    void OnTimer(wxTimerEvent& event);
    void OnMouseWheel(wxMouseEvent& event);
    void OnDoubleClick(wxMouseEvent& event);
    void OnMouseDown(wxMouseEvent& event);
    void OnMouseUp(wxMouseEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void OnCaptureLost(wxMouseCaptureLostEvent& event);

    bool showsPath() const { return haveHome_ && haveStation_; }

    // The view that shows what we have best: the path, or one end of it;
    // and the magnet that draws the ball there.
    Globe::View fitted() const;
    Globe::Vec3 magnet() const;
    void rollTo(const Globe::View& view);
    void wake();

    // Where the mouse is, in pixels right and up from the window's middle.
    void pointer(const wxMouseEvent& event, double& x, double& y) const;

    void drawBall(wxGraphicsContext* gc, const Globe::View& view, double left, double top);
    void drawBrushing(wxGraphicsContext* gc, const Globe::View& view, const wxGraphicsPath& land, double left,
                      double top);
    void drawPath(wxGraphicsContext* gc, const Globe::View& view, double left, double top);

    Globe::Window window_;
    Globe::Zoom zoom_;
    Globe::Roller roller_;
    bool placed_;                   // the ball has been put at a grid square

    std::string home_;
    std::string station_;
    bool haveHome_;
    bool haveStation_;
    bool stationCurrent_;
    bool borders_;
    bool brushedMetal_;
    Globe::LatLon homeAt_;
    Globe::LatLon stationAt_;
    double pathDrawn_;              // how much of the path is drawn so far, 0 to 1
    wxString caption_;

    wxTimer timer_;
    long long lastTickMs_;
};

#endif // GUI_GLISSANDO__MAP_BALL_H
