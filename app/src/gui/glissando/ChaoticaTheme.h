//=========================================================================
// Name:            ChaoticaTheme.h
// Purpose:         Look and feel of the Glissando console: a 1930s movie
//                  serial control room as the Captain Proton holonovel on
//                  Voyager imagined it, shot in black and white. Silver on
//                  black, riveted gunmetal plates, lamps that glow white.
//
// Every control on the console is painted by hand rather than left to the
// platform, because native GTK, Cocoa and Win32 widgets ignore most colour
// requests and would look like a spreadsheet dropped into a rocket ship.
//=========================================================================

#ifndef GUI_GLISSANDO__CHAOTICA_THEME_H
#define GUI_GLISSANDO__CHAOTICA_THEME_H

#include <wx/colour.h>
#include <wx/font.h>
#include <wx/brush.h>
#include <wx/graphics.h>
#include <wx/pen.h>
#include <wx/string.h>

namespace Chaotica
{

// Silver screen greys, with the faintest warm cast of a nitrate print.
namespace Colour
{
    const wxColour Void(10, 10, 11);            // behind everything
    const wxColour Plate(34, 33, 33);           // gunmetal panel face
    const wxColour PlateEdge(66, 64, 62);       // light catching a bevel
    const wxColour PlateShadow(8, 8, 8);
    const wxColour Rivet(150, 148, 142);
    const wxColour Chrome(200, 198, 192);
    const wxColour Bone(238, 232, 216);         // lettering
    const wxColour Dim(128, 124, 114);          // captions, unlit lettering
    const wxColour Glow(255, 252, 240);         // a lit lamp
    const wxColour Bakelite(20, 19, 19);        // unlit button face
    const wxColour Phosphor(232, 236, 230);     // the visi-scope's trace
}

enum class FontRole
{
    Marquee,    // the console's name across the top
    Plate,      // engraved panel titles
    Button,
    Readout,    // numbers on meters and dials
    Caption,    // small print under things
};

wxFont font(FontRole role);

// Letter spaced, upper case text, the way a title card or an engraved plate
// sets it. x and y are the top left corner; returns the width drawn.
double drawSpacedText(wxGraphicsContext* gc, const wxString& text, double x, double y,
                      double spacing);
double spacedTextWidth(wxGraphicsContext* gc, const wxString& text, double spacing);

// Draws text letter spaced and centred on (cx, y top).
void drawSpacedTextCentred(wxGraphicsContext* gc, const wxString& text, double cx, double y,
                           double spacing);

// A domed rivet head.
void drawRivet(wxGraphicsContext* gc, double cx, double cy, double radius);

// A bevelled, riveted plate filling the rectangle.
void drawPlate(wxGraphicsContext* gc, double x, double y, double w, double h, bool rivets);

// A soft white halo, for lit lamps and buttons.
void drawGlow(wxGraphicsContext* gc, double cx, double cy, double radius, double strength);

} // namespace Chaotica

#endif // GUI_GLISSANDO__CHAOTICA_THEME_H
