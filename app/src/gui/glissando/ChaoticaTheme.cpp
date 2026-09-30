//=========================================================================
// Name:            ChaoticaTheme.cpp
// Purpose:         Look and feel of the Glissando console.
//=========================================================================

#include "ChaoticaTheme.h"

#include <chrono>
#include <cmath>

#include <wx/fontenum.h>

namespace Chaotica
{

bool blinkLit()
{
    // Once a second, lit for half of it: quick enough to catch the eye from
    // across the shack, slow enough to read the lettering in either half.
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    return (ms / 500) % 2 == 0;
}

namespace
{

// DejaVu Sans Condensed where the desktop has it. Otherwise the family
// alone: a face that isn't installed can draw no text at all on Windows.
wxFontInfo condensed(int points)
{
    static const bool haveDejaVu = wxFontEnumerator::IsValidFacename("DejaVu Sans Condensed");
    wxFontInfo info(points);
    info.Family(wxFONTFAMILY_SWISS);
    if (haveDejaVu) info.FaceName("DejaVu Sans Condensed");
    return info;
}

} // namespace

wxFont font(FontRole role)
{
    // Condensed grotesques and slab serifs are what the title cards of the
    // serials used; which of them a desktop has varies, so these are asked
    // for by family and fall back to the platform's bold sans.
    switch (role)
    {
        case FontRole::Marquee:
            return wxFont(condensed(30).Bold());
        case FontRole::Plate:
            return wxFont(condensed(9).Bold());
        case FontRole::Button:
            return wxFont(condensed(9).Bold());
        case FontRole::Readout:
            return wxFont(wxFontInfo(13).Family(wxFONTFAMILY_TELETYPE).Bold());
        case FontRole::Caption:
            return wxFont(condensed(7));
    }
    return *wxNORMAL_FONT;
}

double spacedTextWidth(wxGraphicsContext* gc, const wxString& text, double spacing)
{
    double total = 0.0;
    wxString upper = text.Upper();
    for (size_t i = 0; i < upper.length(); i++)
    {
        double w = 0, h = 0;
        gc->GetTextExtent(wxString(upper[i]), &w, &h);
        total += w;
        if (i + 1 < upper.length()) total += spacing;
    }
    return total;
}

double drawSpacedText(wxGraphicsContext* gc, const wxString& text, double x, double y,
                      double spacing)
{
    double start = x;
    wxString upper = text.Upper();
    for (size_t i = 0; i < upper.length(); i++)
    {
        wxString letter(upper[i]);
        double w = 0, h = 0;
        gc->GetTextExtent(letter, &w, &h);
        gc->DrawText(letter, x, y);
        x += w + spacing;
    }
    return x - start - spacing;
}

void drawSpacedTextCentred(wxGraphicsContext* gc, const wxString& text, double cx, double y,
                           double spacing)
{
    drawSpacedText(gc, text, cx - spacedTextWidth(gc, text, spacing) / 2.0, y, spacing);
}

void drawRivet(wxGraphicsContext* gc, double cx, double cy, double radius)
{
    wxGraphicsBrush brush = gc->CreateRadialGradientBrush(
        cx - radius * 0.35, cy - radius * 0.35, cx, cy, radius, Colour::Chrome, Colour::PlateShadow);
    gc->SetBrush(brush);
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
}

void drawPlate(wxGraphicsContext* gc, double x, double y, double w, double h, bool rivets)
{
    // Shadow, then a face lit from the top left with a bright top/left bevel
    // and a dark bottom/right one.
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(Colour::PlateShadow));
    gc->DrawRoundedRectangle(x + 2, y + 2, w - 2, h - 2, 7);

    gc->SetBrush(gc->CreateLinearGradientBrush(x, y, x, y + h, Colour::PlateEdge, Colour::Plate));
    gc->DrawRoundedRectangle(x, y, w - 2, h - 2, 7);

    gc->SetBrush(wxBrush(Colour::Plate));
    gc->DrawRoundedRectangle(x + 3, y + 3, w - 8, h - 8, 5);

    if (rivets)
    {
        double r = 2.6;
        double inset = 8.0;
        drawRivet(gc, x + inset, y + inset, r);
        drawRivet(gc, x + w - inset - 2, y + inset, r);
        drawRivet(gc, x + inset, y + h - inset - 2, r);
        drawRivet(gc, x + w - inset - 2, y + h - inset - 2, r);
    }
}

void drawGlow(wxGraphicsContext* gc, double cx, double cy, double radius, double strength)
{
    wxColour inner(Colour::Glow.Red(), Colour::Glow.Green(), Colour::Glow.Blue(),
                   (unsigned char)std::lround(std::min(1.0, std::max(0.0, strength)) * 150));
    wxColour outer(Colour::Glow.Red(), Colour::Glow.Green(), Colour::Glow.Blue(), 0);
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateRadialGradientBrush(cx, cy, cx, cy, radius, inner, outer));
    gc->DrawEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
}

} // namespace Chaotica
