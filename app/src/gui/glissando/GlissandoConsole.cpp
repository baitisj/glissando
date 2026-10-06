//=========================================================================
// Name:            GlissandoConsole.cpp
// Purpose:         The Glissando console window.
//=========================================================================

#include "GlissandoConsole.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/sizer.h>
#include <wx/textdlg.h>
#include <wx/tglbtn.h>

#include "ChaoticaControls.h"
#include "ChaoticaTheme.h"
#include "ChaoticaTheme.h"
#include "GlissandoScope.h"
#include "MapBall.h"

using namespace Chaotica;

namespace
{

constexpr double PI = 3.14159265358979323846;

// Tuning range: enough to walk the whole scale well away from a carrier or
// a neighbour, and no more, since the melody has to stay inside the SSB
// passband. The lowest pentatonic note is 330 Hz, so -250 still leaves it
// above the 80 Hz or so an SSB filter passes.
constexpr double MAX_TUNING_HZ = 250.0;

// Mirrors the receiver's default frequency search, drawn on the scope.
constexpr double SEARCH_HALF_WIDTH_HZ = 25.0;

constexpr int REFRESH_MILLISECONDS = 250;

// The title card across the top, the gap above it, and the smallest the
// console can be with it.
constexpr int MARQUEE_HEIGHT = 92;
constexpr int MARQUEE_MARGIN = 6;
const wxSize MINIMUM_SIZE(1060, 770);

// The telemetry meter's two faces. Signal reads red where a frame is only just
// copyable; SWR reads red above 2.5:1, short of the 3:1 Preferences can abort at.
Chaotica::Meter::Scale signalScale()
{
    Chaotica::Meter::Scale scale;
    scale.caption = _("Signal");
    scale.minimum = -30.0;
    scale.maximum = 10.0;
    scale.labelFormat = "%+.0f";
    scale.figureFormat = "%+.1f dB";
    scale.redFrom = -30.0;
    scale.redTo = -20.0;
    return scale;
}

Chaotica::Meter::Scale swrScale()
{
    Chaotica::Meter::Scale scale;
    scale.caption = _("SWR");
    scale.minimum = 1.0;
    scale.maximum = 5.0;
    scale.labelFormat = "%.0f";
    scale.figureFormat = "%.1f:1";
    scale.redFrom = 2.5;
    scale.redTo = 5.0;
    return scale;
}

// The console's name across the top: a title card with searchlight rays
// behind it, and a line under it that changes with the scale.
class Marquee : public wxPanel
{
public:
    explicit Marquee(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, MARQUEE_HEIGHT))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, MARQUEE_HEIGHT));
        Bind(wxEVT_PAINT, &Marquee::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { Refresh(); event.Skip(); });
    }

    void setTagline(const wxString& tagline)
    {
        if (tagline_ == tagline) return;
        tagline_ = tagline;
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(Colour::Void));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
        if (!gc) return;

        wxSize size = GetClientSize();
        double cx = size.x / 2.0;
        double cy = size.y * 1.4;

        // Searchlight rays fanning up from below the title.
        gc->SetPen(*wxTRANSPARENT_PEN);
        for (int i = -9; i <= 9; i++)
        {
            double a = -PI / 2.0 + i * 0.085;
            double spread = 0.018;
            double reach = size.y * 3.0;
            wxGraphicsPath ray = gc->CreatePath();
            ray.MoveToPoint(cx, cy);
            ray.AddLineToPoint(cx + reach * std::cos(a - spread), cy + reach * std::sin(a - spread));
            ray.AddLineToPoint(cx + reach * std::cos(a + spread), cy + reach * std::sin(a + spread));
            ray.CloseSubpath();
            gc->SetBrush(gc->CreateRadialGradientBrush(cx, cy, cx, cy, reach,
                                                       wxColour(255, 250, 235, i % 2 ? 26 : 40),
                                                       wxColour(255, 250, 235, 0)));
            gc->FillPath(ray);
        }

        // Chrome rules above and below, deco style: one heavy, two light.
        gc->SetPen(wxPen(Colour::Chrome, 2));
        gc->StrokeLine(16, 6, size.x - 16, 6);
        gc->SetPen(wxPen(Colour::Dim, 1));
        gc->StrokeLine(16, 10, size.x - 16, 10);
        gc->StrokeLine(16, size.y - 8, size.x - 16, size.y - 8);
        gc->SetPen(wxPen(Colour::Chrome, 2));
        gc->StrokeLine(16, size.y - 4, size.x - 16, size.y - 4);

        // The name, with a drop shadow as if embossed in chrome.
        gc->SetFont(font(FontRole::Marquee), Colour::PlateShadow);
        drawSpacedTextCentred(gc.get(), "Glissando", cx + 3, 17, 14.0);
        gc->SetFont(font(FontRole::Marquee), Colour::Chrome);
        drawSpacedTextCentred(gc.get(), "Glissando", cx, 14, 14.0);

        gc->SetFont(font(FontRole::Caption), Colour::Bone);
        drawSpacedTextCentred(gc.get(), tagline_, cx, size.y - 27, 3.0);
    }

    wxString tagline_;
};

wxString taglineFor(Glissando::Scale scale)
{
    switch (scale)
    {
        case Glissando::Scale::Pentatonic:
            return wxString::FromUTF8("Pentatonic melody transmitter · harmonious interference guaranteed");
        case Glissando::Scale::WholeTone:
            return wxString::FromUTF8("Whole tone · the dream sequence dissolve is engaged");
        case Glissando::Scale::Diminished:
            return wxString::FromUTF8("Diminished · suspense mounts on the planet of the spider people");
        case Glissando::Scale::Diabolus:
            return wxString::FromUTF8("Diabolus in musica · Chaotica's death ray is armed");
    }
    return wxEmptyString;
}

wxBoxSizer* row() { return new wxBoxSizer(wxHORIZONTAL); }

// The DRIVE knob's range in dB, with -12 dB straight up: full scale from the
// sound card overdrives most radios' ALC, so the upper end gets less sweep.
constexpr double DRIVE_TOP_DB = 0.0;
constexpr double DRIVE_UPRIGHT_DB = -12.0;
constexpr double DRIVE_BOTTOM_DB = -30.0;

// Little engraved animals over the slowest and fastest tempos, the tortoise
// and the hare, the way a tractor's throttle lever is marked, with a dashed
// track between them. Each sits centred over its end button, columnWidth
// wide, and lights with its tempo.
class Racetrack : public Control
{
public:
    static constexpr int HEIGHT = 22;

    Racetrack(wxWindow* parent, int width, int columnWidth)
        : Control(parent, wxID_ANY, wxSize(width, HEIGHT))
        , columnWidth_(columnWidth)
    {
        // empty
    }

    void SetLit(bool tortoise, bool hare)
    {
        if (tortoiseLit_ == tortoise && hareLit_ == hare) return;
        tortoiseLit_ = tortoise;
        hareLit_ = hare;
        Refresh();
    }

protected:
    virtual void paint(wxGraphicsContext* gc, const wxSize& size) override
    {
        // Each animal is drawn on a 48 by 22 grid, facing right: the way the
        // music runs.
        const double ANIMAL = 48.0;
        double tortoiseX = columnWidth_ / 2.0 - ANIMAL / 2.0;
        double hareX = size.x - columnWidth_ / 2.0 - ANIMAL / 2.0;

        // The track: faint dashes from the tortoise's nose to the hare's tail.
        gc->SetPen(wxPen(wxColour(76, 74, 70), 1));
        const double DASH = 5.0, GAP = 5.0, TRACK_Y = 15.5;
        for (double x = tortoiseX + ANIMAL + 2; x + DASH <= hareX + 2; x += DASH + GAP)
            gc->StrokeLine(x, TRACK_Y, x + DASH, TRACK_Y);

        gc->PushState();
        gc->Translate(tortoiseX, (size.y - HEIGHT) / 2.0);
        wxColour ink = tortoiseLit_ ? Colour::Bone : Colour::Dim;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(ink));
        paintTortoise(gc, ink);
        gc->PopState();

        gc->PushState();
        gc->Translate(hareX, (size.y - HEIGHT) / 2.0);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(hareLit_ ? Colour::Bone : Colour::Dim));
        paintHare(gc);
        gc->PopState();
    }

private:
    static void paintTortoise(wxGraphicsContext* gc, const wxColour& ink)
    {
        // Feet, tail and head under and around a domed shell.
        gc->DrawRoundedRectangle(13, 14, 5, 6, 2);
        gc->DrawRoundedRectangle(28, 14, 5, 6, 2);
        wxGraphicsPath tail = gc->CreatePath();
        tail.MoveToPoint(10, 14);
        tail.AddLineToPoint(5, 16);
        tail.AddLineToPoint(10, 16.5);
        tail.CloseSubpath();
        gc->FillPath(tail);
        gc->DrawRoundedRectangle(33, 10, 7, 4, 2);     // neck
        gc->DrawEllipse(37, 7, 8, 7);                   // head

        wxGraphicsPath shell = gc->CreatePath();
        shell.MoveToPoint(9, 16);
        shell.AddCurveToPoint(9, 1, 37, 1, 37, 16);
        shell.CloseSubpath();
        gc->FillPath(shell);

        // The shell's plates, cut in.
        gc->SetPen(wxPen(Colour::Plate, 1));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->StrokeLine(9.5, 13, 36.5, 13);
        gc->StrokeLine(18, 13, 16, 6.5);
        gc->StrokeLine(28, 13, 30, 6.5);
        gc->StrokeLine(17, 8, 29, 8);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(ink));
        gc->SetBrush(wxBrush(Colour::Plate));
        gc->DrawEllipse(41, 9, 1.8, 1.8);               // eye
    }

    static void paintHare(wxGraphicsContext* gc)
    {
        // In full stride: body stretched out, ears laid back.
        wxGraphicsPath body = gc->CreatePath();
        body.MoveToPoint(8, 12);
        body.AddCurveToPoint(10, 5, 28, 4, 34, 8);
        body.AddCurveToPoint(36, 10, 34, 14, 30, 14);
        body.AddCurveToPoint(22, 15, 14, 16, 8, 12);
        body.CloseSubpath();
        gc->FillPath(body);

        gc->DrawEllipse(32, 4, 9, 7);                   // head
        wxGraphicsPath ears = gc->CreatePath();
        ears.MoveToPoint(34, 6);
        ears.AddCurveToPoint(30, 1, 24, 0, 20, 1);
        ears.AddCurveToPoint(25, 2, 30, 4, 32, 7);
        ears.CloseSubpath();
        gc->FillPath(ears);

        gc->DrawEllipse(5, 8, 5, 5);                    // tail

        // Hind legs flung back, forelegs reaching.
        wxGraphicsPath legs = gc->CreatePath();
        legs.MoveToPoint(12, 13);
        legs.AddLineToPoint(3, 19);
        legs.AddLineToPoint(6, 20);
        legs.AddLineToPoint(16, 14);
        legs.CloseSubpath();
        legs.MoveToPoint(29, 13);
        legs.AddLineToPoint(44, 17);
        legs.AddLineToPoint(44, 19);
        legs.AddLineToPoint(27, 15);
        legs.CloseSubpath();
        gc->FillPath(legs);

        gc->SetBrush(wxBrush(Colour::Plate));
        gc->DrawEllipse(37, 6, 1.8, 1.8);               // eye
    }

    int columnWidth_;
    bool tortoiseLit_ = false;
    bool hareLit_ = false;
};

} // namespace

wxString GlissandoConsole::gearLabel(int gear)
{
    switch (gear)
    {
        case 1: return _("Adagio");
        case 2: return _("Andante");
        case 3: return _("Allegro");
        case 4: return _("Presto");
        case 5: return _("Duet");
    }
    return "---";
}

wxString GlissandoConsole::scaleLabel(Glissando::Scale scale)
{
    switch (scale)
    {
        case Glissando::Scale::Pentatonic: return _("Pentatonic");
        case Glissando::Scale::WholeTone: return _("Whole tone");
        case Glissando::Scale::Diminished: return _("Diminished");
        case Glissando::Scale::Diabolus: return _("Diabolus");
    }
    return "---";
}

wxString GlissandoConsole::noteName(double hz)
{
    static const char* names[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    if (hz <= 0.0) return "?";
    int midi = (int)std::lround(69.0 + 12.0 * std::log2(hz / 440.0));
    return wxString::Format("%s%d", names[((midi % 12) + 12) % 12], midi / 12 - 1);
}

GlissandoConsole::GlissandoConsole(wxWindow* parent, IGlissandoHost* host,
                                   const GlissandoConsoleSettings& settings, const wxRect& position)
    : wxFrame(parent, wxID_ANY, _("Glissando"), position.GetPosition(), position.GetSize(),
              wxDEFAULT_FRAME_STYLE)
    , host_(host)
    , settings_(settings)
    , sendingGear_(settings.gear)
    , timer_(this)
{
    SetBackgroundColour(Colour::Void);
    buildControls();
    applySettings(false);

    if (position.GetWidth() <= 0 || position.GetHeight() <= 0)
    {
        SetSize(wxSize(std::max(1100, minimumWidth_), 790));
        Centre();
    }
    else if (GetSize().x < minimumWidth_)
    {
        // Saved narrower than the console now lays out.
        SetSize(minimumWidth_, GetSize().y);
    }

    Bind(wxEVT_TIMER, &GlissandoConsole::OnTimer, this);
    Bind(wxEVT_CLOSE_WINDOW, &GlissandoConsole::OnClose, this);
    timer_.Start(REFRESH_MILLISECONDS);
    refreshTelemetry();
}

GlissandoConsole::~GlissandoConsole()
{
    timer_.Stop();
}

void GlissandoConsole::showMarquee(bool show, bool resize)
{
    if (marquee_->IsShown() == show) return;
    marquee_->Show(show);

    // The console grows or shrinks by the title's height, so the scope
    // keeps its size; a maximised console just lays itself out again.
    int height = MARQUEE_HEIGHT + MARQUEE_MARGIN;
    SetMinSize(wxSize(minimumWidth_, MINIMUM_SIZE.y - (show ? 0 : height)));
    if (resize && !IsMaximized() && !IsFullScreen())
    {
        wxSize size = GetSize();
        SetSize(size.x, size.y + (show ? height : -height));
    }
    Layout();
}

void GlissandoConsole::buildControls()
{
    auto* page = new wxPanel(this);
    page->SetBackgroundColour(Colour::Void);
    auto* pageSizer = new wxBoxSizer(wxVERTICAL);

    auto* marquee = new Marquee(page);
    marquee_ = marquee;
    pageSizer->Add(marquee_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, MARQUEE_MARGIN);

    // --- Middle: the visi-scope, and the tuning and telemetry column.
    auto* middle = row();

    auto* scopePlate = new Panel(page, _("Visi-scope"));
    scope_ = new GlissandoScope(scopePlate);
    scope_->setSpectrumSource([this](std::vector<float>& db, double& nyquist) {
        return host_->glissandoSpectrum(db, nyquist);
    });
    scopePlate->GetContentSizer()->Add(scope_, 1, wxEXPAND);

    auto* scopeControls = row();
    scanRateDial_ = new Dial(scopePlate, wxID_ANY, _("Scan rate"), 0.5, 20.0, 0.5,
                             settings_.scanRate, wxSize(110, 120));
    scanRateDial_->SetFormatter([](double v) { return wxString::Format("%.1f/s", v); });
    scopeControls->Add(scanRateDial_, 0, wxRIGHT, 8);

    auto* scopeButtons = new wxBoxSizer(wxVERTICAL);
    lensButton_ = new Button(scopePlate, wxID_ANY, _("Time lens"), true, wxSize(120, 32));
    lensButton_->SetToolTip(_("Magnify the most recent band activity at the top of the scope and squeeze "
                              "older history below it on a logarithmic time scale, so eight times "
                              "as much stays in view and nothing faint is averaged away."));
    scopeButtons->Add(lensButton_, 0, wxBOTTOM, 6);
    wideButton_ = new Button(scopePlate, wxID_ANY, _("Duet voice"), true, wxSize(120, 32));
    wideButton_->SetToolTip(_("Widen the scope to show the duet's high voice, C6 to E7."));
    scopeButtons->Add(wideButton_, 0, wxBOTTOM, 6);
    listenAllButton_ = new Button(scopePlate, wxID_ANY, _("All tempos"), true, wxSize(120, 32));
    listenAllButton_->SetToolTip(_("Listen for every tempo at once, so a station that shifts "
                                   "gear is still heard. Costs processor time."));
    scopeButtons->Add(listenAllButton_, 0);
    scopeControls->Add(scopeButtons, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    auto* lamps = new wxBoxSizer(wxVERTICAL);
    engagedLamp_ = new Lamp(scopePlate, _("Engaged"), wxSize(160, 22));
    receivingLamp_ = new Lamp(scopePlate, _("Receiving"), wxSize(160, 22));
    transmittingLamp_ = new Lamp(scopePlate, _("Transmitting"), wxSize(160, 22));
    lamps->Add(engagedLamp_, 0, wxBOTTOM, 4);
    lamps->Add(receivingLamp_, 0, wxBOTTOM, 4);
    lamps->Add(transmittingLamp_, 0);
    scopeControls->Add(lamps, 0, wxALIGN_CENTER_VERTICAL);
    mapBall_ = new MapBall(scopePlate);
    scopeControls->Add(mapBall_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    scopeControls->AddStretchSpacer();

    scopePlate->GetContentSizer()->Add(scopeControls, 0, wxEXPAND | wxTOP, 8);
    middle->Add(scopePlate, 1, wxEXPAND | wxRIGHT, 6);

    auto* column = new wxBoxSizer(wxVERTICAL);

    auto* tuningPlate = new Panel(page, _("Tuning"));
    tuningDial_ = new Dial(tuningPlate, wxID_ANY, _("Melody offset"), -MAX_TUNING_HZ, MAX_TUNING_HZ,
                           0.1, settings_.tuningOffsetHz, wxSize(170, 150));
    tuningDial_->SetFormatter([](double v) { return wxString::Format("%+.1f Hz", v); });
    tuningDial_->SetDefault(0.0);
    tuningDial_->SetWheelSteps(0.1, 10.0, 1.0);
    tuningDial_->SetToolTip(_("Mouse wheel: 0.1 Hz a click, 10 Hz with shift, 1 Hz with control. "
                              "Drag up or down (shift for fine); dragging catches at 0 Hz, "
                              "and a double click returns to it."));
    tuningPlate->GetContentSizer()->Add(tuningDial_, 0, wxALIGN_CENTER_HORIZONTAL);
    column->Add(tuningPlate, 0, wxEXPAND | wxBOTTOM, 6);

    auto* telemetryPlate = new Panel(page, _("Telemetry"));
    snrMeter_ = new Meter(telemetryPlate, signalScale(), wxSize(200, 104));
    telemetryPlate->GetContentSizer()->Add(snrMeter_, 0, wxEXPAND | wxBOTTOM, 6);
    auto* grid = new wxGridSizer(2, 4, 6);
    dopplerReadout_ = new Readout(telemetryPlate, _("Doppler"), wxSize(96, 44));
    heardReadout_ = new Readout(telemetryPlate, _("Last heard"), wxSize(96, 44));
    tempoReadout_ = new Readout(telemetryPlate, _("Sending at"), wxSize(96, 44));
    frameReadout_ = new Readout(telemetryPlate, _("Frame length"), wxSize(96, 44));
    grid->Add(dopplerReadout_, 0, wxEXPAND);
    grid->Add(heardReadout_, 0, wxEXPAND);
    grid->Add(tempoReadout_, 0, wxEXPAND);
    grid->Add(frameReadout_, 0, wxEXPAND);
    telemetryPlate->GetContentSizer()->Add(grid, 0, wxEXPAND | wxBOTTOM, 4);
    // The receiver hears every scale, whichever one we sing in.
    heardScaleReadout_ = new Readout(telemetryPlate, _("Heard singing in"), wxSize(196, 44));
    heardScaleReadout_->SetToolTip(_("The scale of the last frame heard. Every scale is heard, "
                                     "whichever one this station sends in."));
    telemetryPlate->GetContentSizer()->Add(heardScaleReadout_, 0, wxEXPAND);
    column->Add(telemetryPlate, 1, wxEXPAND);

    middle->Add(column, 0, wxEXPAND);
    pageSizer->Add(middle, 1, wxEXPAND | wxALL, 6);

    // --- Bottom: modulation, and the master switches.
    auto* bottom = row();

    // Modulation: DRIVE on the left; to its right the tempos, slowest to
    // fastest with the tortoise and the hare over the ends and AUTO after
    // them, and the scales under them across the same width.
    auto* modulationPlate = new Panel(page, _("Modulation"));
    auto* modulationRow = row();

    const int TEMPO_WIDTH = 94;
    const int AUTO_WIDTH = 76;
    const int BUTTON_HEIGHT = 34;
    const int BUTTON_GAP = 4;
    const int AUTO_GAP = 8;
    const int GEARS = Glissando::MAX_GEAR - Glissando::MIN_GEAR + 1;
    const int ROW_WIDTH = GEARS * (TEMPO_WIDTH + BUTTON_GAP) + AUTO_GAP + AUTO_WIDTH;
    const int SCALES = 4;
    const int SCALE_WIDTH = (ROW_WIDTH - (SCALES - 1) * BUTTON_GAP) / SCALES;

    driveDial_ = new Dial(modulationPlate, wxID_ANY, _("Drive"), DRIVE_BOTTOM_DB, DRIVE_TOP_DB, 0.1,
                          DRIVE_UPRIGHT_DB, wxSize(110, 124));
    driveDial_->SetCentre(DRIVE_UPRIGHT_DB);
    driveDial_->SetFormatter([](double v) { return wxString::Format("%.1f dB", v); });
    driveDial_->SetWheelSteps(0.5, 0.1, 0.1);
    driveDial_->SetPushable(true);
    modulationRow->Add(driveDial_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    auto* buttons = new wxBoxSizer(wxVERTICAL);
    auto* racetrack = new Racetrack(modulationPlate, GEARS * (TEMPO_WIDTH + BUTTON_GAP) - BUTTON_GAP, TEMPO_WIDTH);
    racetrack_ = racetrack;
    buttons->Add(racetrack_, 0);
    auto* tempoRow = row();
    for (int gear = Glissando::MIN_GEAR; gear <= Glissando::MAX_GEAR; gear++)
    {
        const Glissando::GearInfo& info = Glissando::gearInfo(gear);
        auto* button = new Button(modulationPlate, wxID_ANY, gearLabel(gear), true,
                                  wxSize(TEMPO_WIDTH, BUTTON_HEIGHT));
        button->SetToolTip(wxString::Format(_("%s: %.0f ms notes, %.0f s per frame%s"),
                                            info.tempo, info.symbolSeconds * 1000.0,
                                            info.frameSeconds(),
                                            info.voices > 1 ? _(", two voices") : wxString()));
        button->Bind(wxEVT_TOGGLEBUTTON, [this, gear](wxCommandEvent&) { selectGear(gear); });
        gearButtons_.push_back(button);
        tempoRow->Add(button, 0, wxRIGHT, BUTTON_GAP);
    }
    tempoRow->AddSpacer(AUTO_GAP - BUTTON_GAP);
    autoButton_ = new Button(modulationPlate, wxID_ANY, _("Auto"), true, wxSize(AUTO_WIDTH, BUTTON_HEIGHT));
    autoButton_->SetToolTip(_("Shift tempo automatically, from the signal and fading measured on the last "
                              "frame heard. The lit tempo is the one being sent; the one chosen "
                              "by hand glows faintly and is used until something has been heard."));
    tempoRow->Add(autoButton_, 0);
    buttons->Add(tempoRow, 0, wxBOTTOM, 8);

    auto* scaleRow = row();
    const Glissando::Scale scales[] = {Glissando::Scale::Pentatonic, Glissando::Scale::WholeTone,
                                       Glissando::Scale::Diminished, Glissando::Scale::Diabolus};
    for (Glissando::Scale scale : scales)
    {
        auto* button = new Button(modulationPlate, wxID_ANY, scaleLabel(scale), true,
                                  wxSize(SCALE_WIDTH, BUTTON_HEIGHT));
        button->SetToolTip(taglineFor(scale));
        button->Bind(wxEVT_TOGGLEBUTTON, [this, scale](wxCommandEvent&) { selectScale(scale); });
        scaleButtons_.push_back(button);
        scaleRow->Add(button, 0, scale == Glissando::Scale::Diabolus ? 0 : wxRIGHT, BUTTON_GAP);
    }
    buttons->Add(scaleRow, 0);
    modulationRow->Add(buttons, 0, wxALIGN_CENTER_VERTICAL);
    modulationPlate->GetContentSizer()->Add(modulationRow, 0);
    modulationPlate->CentreTitleOver(buttons);
    bottom->Add(modulationPlate, 1, wxEXPAND | wxRIGHT, 6);

    // Command: the master switches beside the radio's dial.
    auto* commandPlate = new Panel(page, _("Command"));
    auto* commandRow = row();

    // Two columns on one grid: each switch on the left sits level with the
    // row beside it, Engage with the dial's window, then a row of buttons each.
    const int COLUMN = 190;
    const int READOUT = 46;
    const int ROW = 32;
    const int GAP = 6;
    const int RIM = 3;                              // Button paints this far in
    int windowTop = Readout::WindowTop();
    int windowHeight = READOUT - windowTop - 2;     // as Readout paints it
    int engageTop = windowTop - RIM;
    int engageHeight = windowHeight + 2 * RIM;

    auto* switches = new wxBoxSizer(wxVERTICAL);
    engageButton_ = new Button(commandPlate, wxID_ANY, _("Engage"), true, wxSize(COLUMN, engageHeight));
    engageButton_->SetToolTip(_("Start or stop the audio."));
    chatButton_ = new Button(commandPlate, wxID_ANY, _("Comms"), true, wxSize(COLUMN, ROW));
    chatButton_->SetToolTip(_("Open or close the COMMS window, where chat is sent and read. "
                              "Flashes red while it is closed and a message has come in."));
    preferencesButton_ = new Button(commandPlate, wxID_ANY, _("Preferences"), false, wxSize(COLUMN, ROW));
    preferencesButton_->SetToolTip(_("Options, sound cards, rig control and audio filters."));
    switches->AddSpacer(engageTop);
    switches->Add(engageButton_, 0, wxBOTTOM, READOUT - engageTop - engageHeight + GAP);
    switches->Add(chatButton_, 0, wxBOTTOM, GAP);
    switches->Add(preferencesButton_, 0);
    commandRow->Add(switches, 0, wxRIGHT, 10);

    auto* dial = new wxBoxSizer(wxVERTICAL);
    rigReadout_ = new Readout(commandPlate, _("Radio dial"), wxSize(COLUMN, READOUT));
    dial->Add(rigReadout_, 0, wxEXPAND | wxBOTTOM, GAP);
    auto* dialButtons = row();
    presetsButton_ = new Button(commandPlate, wxID_ANY, _("Presets"), false, wxSize(92, ROW));
    presetsButton_->SetToolTip(_("Pick a frequency from your list (edited in Preferences, Options)."));
    dialButtons->Add(presetsButton_, 0, wxRIGHT, 6);
    rigButton_ = new Button(commandPlate, wxID_ANY, _("Set"), false, wxSize(92, ROW));
    rigButton_->SetToolTip(_("Type in a dial frequency. With rig control set up, the radio tunes to it."));
    dialButtons->Add(rigButton_, 0);
    dial->Add(dialButtons, 0, wxBOTTOM, GAP);

    // Lit while the snooping window is up, the way Engage is lit while the
    // audio runs.
    snoopButton_ = new Button(commandPlate, wxID_ANY, _("Snooper"), true, wxSize(COLUMN, ROW));
    snoopButton_->SetToolTip(_("Open or close the snooping window: every message heard, whoever it was sent to."));
    dial->Add(snoopButton_, 0);
    commandRow->Add(dial, 0);

    commandPlate->GetContentSizer()->Add(commandRow, 0);
    bottom->Add(commandPlate, 0, wxEXPAND);

    pageSizer->Add(bottom, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    page->SetSizer(pageSizer);

    auto* frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(page, 1, wxEXPAND);
    SetSizer(frameSizer);
    // Never so narrow that the row along the bottom is cut off.
    minimumWidth_ = std::max(MINIMUM_SIZE.x, bottom->GetMinSize().x + 12);
    SetMinSize(wxSize(minimumWidth_, MINIMUM_SIZE.y));
    Layout();

    // --- Events.
    tuningDial_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { setTuning(tuningDial_->GetValue()); });
    scanRateDial_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        settings_.scanRate = scanRateDial_->GetValue();
        applySettings(true);
    });
    scope_->Bind(EVT_GLISSANDO_SCOPE_TUNE, [this](wxCommandEvent& event) {
        if (event.GetExtraLong() != 0)
        {
            setTuning(settings_.tuningOffsetHz + event.GetExtraLong() / 10.0);
        }
        else
        {
            // Put the scale's lowest note where the click was.
            double low = Glissando::scaleNotes(settings_.scale, 0)[0];
            setTuning(event.GetInt() / 10.0 - low);
        }
    });
    driveDial_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        // Turning it by hand takes over from the ALC, as grabbing a knob
        // takes it from a servo: it pops out.
        if (driveDial_->IsPushed())
        {
            driveDial_->SetPushed(false);
            host_->glissandoSetDriveAuto(false);
        }
        host_->glissandoSetDrive(driveDial_->GetValue());
    });
    driveDial_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& event) {
        host_->glissandoSetDriveAuto(event.GetInt() != 0);
        refreshTelemetry();
    });
    autoButton_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& event) {
        settings_.autoGear = event.GetInt() != 0;
        applySettings(true);
    });
    listenAllButton_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& event) {
        settings_.listenAllGears = event.GetInt() != 0;
        applySettings(true);
    });
    lensButton_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& event) {
        settings_.lens = event.GetInt() != 0;
        applySettings(true);
    });
    wideButton_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& event) {
        settings_.wideScope = event.GetInt() != 0;
        applySettings(true);
    });
    engageButton_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& event) {
        // While anything is on the air the button is Abort. Decided afresh
        // on the click as well: a transmission that started since the label
        // last changed is stopped, not the audio under it, and one that ended
        // leaves the audio running.
        if (engageAborts_ || host_->glissandoTelemetry().transmitBusy)
        {
            host_->glissandoAbortTransmit();
            scope_->clearSent();
        }
        else
        {
            host_->glissandoSetAudioRunning(event.GetInt() != 0);
        }
        refreshTelemetry();
    });
    chatButton_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) {
        host_->glissandoShowChat(chatButton_->IsChecked());
        chatButton_->SetChecked(host_->glissandoChatShown());
    });
    snoopButton_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) {
        host_->glissandoShowSnoop(snoopButton_->IsChecked());
        snoopButton_->SetChecked(host_->glissandoSnoopShown());
    });
    preferencesButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { showPreferences(); });
    presetsButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { showFrequencyPresets(); });
    rigButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { enterRigFrequency(); });
}

void GlissandoConsole::enterRigFrequency()
{
    GlissandoTelemetry telemetry = host_->glissandoTelemetry();
    wxString current = telemetry.rigFrequencyKnown
                           ? wxString::Format("%.3f", telemetry.rigFrequencyHz / 1000.0)
                           : wxString();
    wxTextEntryDialog entry(this, _("Radio dial frequency in kHz:"), _("Tune the radio"), current);
    if (entry.ShowModal() != wxID_OK) return;
    double khz = 0.0;
    if (entry.GetValue().ToDouble(&khz) && khz > 0.0) host_->glissandoSetRigFrequency(khz * 1000.0);
}

void GlissandoConsole::showFrequencyPresets()
{
    std::vector<double> presets = host_->glissandoFrequencyPresets();
    std::vector<Choice> choices;
    for (double hz : presets) choices.push_back({wxString::Format("%.1f kHz", hz / 1000.0), true, wxString()});
    choices.push_back({_("Other..."), true, _("Type in a dial frequency.")});

    ShowChoices(presetsButton_, choices, [this, presets](int index) {
        if (index < (int)presets.size())
            host_->glissandoSetRigFrequency(presets[index]);
        else
            enterRigFrequency();
    });
}

void GlissandoConsole::showPreferences()
{
    struct Entry
    {
        GlissandoSetup setup;
        wxString label;
        wxString tooltip;
    };
    const std::vector<Entry> entries = {
        {GlissandoSetup::Options, _("Options"), _("Callsign, frequency list, chat and other options.")},
        {GlissandoSetup::AudioDevices, _("Sound cards"), _("Which sound cards talk to the radio.")},
        {GlissandoSetup::RigControl, _("Rig control"), _("CAT and PTT: how the radio is keyed and tuned.")},
        {GlissandoSetup::EasySetup, _("Easy setup"), _("Sound cards, rig control and callsign on one page.")},
    };

    std::vector<Choice> choices;
    for (const Entry& entry : entries)
    {
        bool available = host_->glissandoSetupAvailable(entry.setup);
        choices.push_back({entry.label, available,
                           available ? entry.tooltip : _("Disengage first: this can't change while audio runs.")});
    }

    // Lit while its column is down and while the setup dialog it opens is
    // up; those dialogs are modal, so closed comes once they have gone.
    preferencesButton_->SetChecked(true);
    ShowChoices(
        preferencesButton_, choices,
        [this, entries](int index) { host_->glissandoOpenSetup(entries[index].setup); },
        [this]() { preferencesButton_->SetChecked(false); });
}

void GlissandoConsole::selectGear(int gear)
{
    settings_.gear = std::min(Glissando::MAX_GEAR, std::max(Glissando::MIN_GEAR, gear));
    applySettings(true);
}

void GlissandoConsole::selectScale(Glissando::Scale scale)
{
    settings_.scale = scale;
    applySettings(true);
}

void GlissandoConsole::setTuning(double hz)
{
    hz = std::round(std::min(MAX_TUNING_HZ, std::max(-MAX_TUNING_HZ, hz)) * 10.0) / 10.0;
    if (hz == 0.0) hz = 0.0; // never "-0.0"
    settings_.tuningOffsetHz = hz;
    tuningDial_->SetValue(hz);
    applySettings(true);
}

void GlissandoConsole::applySettings(bool notifyHost)
{
    // Radio button behaviour for the tempo and scale rows.
    updateGearButtons();
    const Glissando::Scale scales[] = {Glissando::Scale::Pentatonic, Glissando::Scale::WholeTone,
                                       Glissando::Scale::Diminished, Glissando::Scale::Diabolus};
    for (size_t i = 0; i < scaleButtons_.size(); i++)
    {
        scaleButtons_[i]->SetChecked(scales[i] == settings_.scale);
    }
    autoButton_->SetChecked(settings_.autoGear);
    listenAllButton_->SetChecked(settings_.listenAllGears);
    wideButton_->SetChecked(settings_.wideScope);
    lensButton_->SetChecked(settings_.lens);
    scope_->setLens(settings_.lens);
    tuningDial_->SetValue(settings_.tuningOffsetHz);
    scanRateDial_->SetValue(settings_.scanRate);
    scope_->setScanRate(settings_.scanRate);
    static_cast<Marquee*>(marquee_)->setTagline(taglineFor(settings_.scale));

    updateStaff();

    if (notifyHost) host_->glissandoSettingsChanged(settings_);
}

void GlissandoConsole::updateGearButtons()
{
    // The lit tempo is the one going out. In automatic that is the one the
    // measurements chose, and the tempo picked by hand, which automatic
    // falls back to when nothing has been heard, glows faintly beside it.
    int lit = settings_.autoGear ? sendingGear_ : settings_.gear;
    for (size_t i = 0; i < gearButtons_.size(); i++)
    {
        int gear = (int)i + Glissando::MIN_GEAR;
        gearButtons_[i]->SetChecked(gear == lit);
        gearButtons_[i]->SetHinted(settings_.autoGear && gear == settings_.gear && gear != lit);
    }
    // The tortoise and the hare light with their tempos.
    static_cast<Racetrack*>(racetrack_)->SetLit(lit == Glissando::MIN_GEAR, lit == Glissando::MAX_GEAR);
}

void GlissandoConsole::updateStaff()
{
    std::vector<double> notes;
    std::vector<wxString> names;
    int sending = settings_.autoGear ? sendingGear_ : settings_.gear;
    bool duet = settings_.wideScope || Glissando::gearInfo(sending).voices > 1;
    for (int voice = 0; voice < (duet ? 2 : 1); voice++)
    {
        auto scaleNotes = Glissando::scaleNotes(settings_.scale, voice);
        for (double note : scaleNotes)
        {
            notes.push_back(note + settings_.tuningOffsetHz);
            names.push_back(noteName(note));
        }
    }
    scope_->setStaff(notes, names, SEARCH_HALF_WIDTH_HZ);

    auto [low, high] = std::minmax_element(notes.begin(), notes.end());
    double margin = duet ? 120.0 : 80.0;
    scope_->setSpan(std::max(0.0, *low - margin), *high + margin);
}

void GlissandoConsole::refreshTelemetry()
{
    GlissandoTelemetry t = host_->glissandoTelemetry();

    bool aborts = t.audioRunning && t.transmitBusy;
    if (aborts != engageAborts_)
    {
        engageAborts_ = aborts;
        engageButton_->SetToolTip(aborts ? _("Stop transmitting now. The message is dropped, not retried.")
                                         : _("Start or stop the audio."));
    }
    engageButton_->SetChecked(t.audioRunning);
    engageButton_->SetLabel(aborts ? _("Abort") : t.audioRunning ? _("Disengage") : _("Engage"));

    // Red while it reads Abort. And a chat message is waiting for a
    // transmitter that only Engage brings up: the button flashes red with the
    // message's chip until it is pressed.
    engageButton_->SetAlarm(aborts || (t.engageToSend && Chaotica::blinkLit()));
    engagedLamp_->SetLit(t.audioRunning);
    receivingLamp_->SetLit(t.receiving);
    transmittingLamp_->SetLit(t.transmitting);
    scope_->setActivity(t.receiving, t.transmitting);
    scope_->setSmoke(t.smoke);
    scope_->setCarrierSense(t.channelHeld, t.carrierHz);
    mapBall_->setLocators(t.homeLocator, t.stationLocator, t.stationLocatorCurrent);
    for (const GlissandoScopeFrame& frame : host_->glissandoHeardFrames()) scope_->addHeard(frame);
    for (const GlissandoScopeSent& frame : host_->glissandoSentFrames()) scope_->addSent(frame);

    // On the air, a radio that reports SWR has the meter instead of the
    // signal last heard.
    if (t.showSwr != meterShowsSwr_)
    {
        meterShowsSwr_ = t.showSwr;
        snrMeter_->SetScale(meterShowsSwr_ ? swrScale() : signalScale());
    }
    if (meterShowsSwr_) snrMeter_->SetValue(t.swrKnown ? t.swr : std::nan(""));
    else snrMeter_->SetValue(t.haveReport ? t.snrDb : std::nan(""));
    dopplerReadout_->SetText(t.haveReport ? wxString::Format("%.2f Hz", t.dopplerHz) : wxString("---"));

    if (t.haveReport)
    {
        double s = t.secondsSinceHeard;
        wxString ago = s < 90 ? wxString::Format("%.0fs", s)
                              : s < 5400 ? wxString::Format("%.0fm", s / 60) : wxString::Format("%.0fh", s / 3600);
        heardReadout_->SetText(wxString::Format("%s %s", gearLabel(t.heardGear).Left(4), ago));
        heardScaleReadout_->SetText(scaleLabel(t.heardScale));
    }
    else
    {
        heardReadout_->SetText("---");
        heardScaleReadout_->SetText("---");
    }

    tempoReadout_->SetText(gearLabel(t.transmitGear));
    frameReadout_->SetText(wxString::Format("%.1f s", Glissando::gearInfo(t.transmitGear).frameSeconds()));

    if (sendingGear_ != t.transmitGear)
    {
        sendingGear_ = t.transmitGear;
        updateGearButtons();
        updateStaff();
    }

    updateDrive(t);

    rigReadout_->SetText(t.rigFrequencyKnown ? wxString::Format("%.3f kHz", t.rigFrequencyHz / 1000.0)
                                             : wxString("---"));
    updateBandWidthWarning(t);

    // Both windows can be opened or closed from elsewhere too.
    chatButton_->SetChecked(host_->glissandoChatShown());
    snoopButton_->SetChecked(host_->glissandoSnoopShown());

    // A message waiting in a closed COMMS window: the button flashes red,
    // in step with ENGAGE TO SEND, until the window is opened.
    chatButton_->SetAlarm(t.chatUnread && Chaotica::blinkLit());
}

// Much of the world holds 30 m to 500 Hz (the IARU Region 1 band plan, which
// some countries make law); US rules set no width there. So it is a warning
// on the dial, never a block: the operator knows which rules they are under.
void GlissandoConsole::updateBandWidthWarning(const GlissandoTelemetry& t)
{
    const double LIMIT_HZ = 500.0;
    double widthHz = Glissando::signalWidthHz(settings_.scale, t.transmitGear);
    bool warn = t.rigFrequencyKnown && t.rigFrequencyHz >= 10100000.0 && t.rigFrequencyHz <= 10150000.0 &&
                widthHz > LIMIT_HZ;
    wxString tip = warn ? wxString::Format(_("%s at %s is about %.0f Hz wide. Much of the world holds 30 m to "
                                             "%.0f Hz (IARU Region 1); there, send in the diminished or whole "
                                             "tone scale and leave the duet off. US rules set no width limit on "
                                             "30 m."),
                                           scaleLabel(settings_.scale), gearLabel(t.transmitGear), widthHz, LIMIT_HZ)
                        : wxString();
    if (tip == bandWidthTip_) return;
    bandWidthTip_ = tip;

    rigReadout_->SetCaption(warn ? _("Over 500 Hz for 30 m") : _("Radio dial"), warn);
    if (warn)
        rigReadout_->SetToolTip(tip);
    else
        rigReadout_->UnsetToolTip();
}

void GlissandoConsole::updateDrive(const GlissandoTelemetry& t)
{
    // The knob shows the level the host has, so it turns itself while the
    // ALC turns it down; not while the operator has hold of it.
    if (!driveDial_->IsDragging())
    {
        driveDial_->SetValue(t.driveDb);
        driveDial_->SetPushed(t.driveAuto);
    }
    driveDial_->SetRing(t.driveAutoDeaf, t.driveAuto && t.alcOver);

    wxString tip;
    if (t.driveAuto && t.driveAutoDeaf)
        tip = _("Pushed in, but this radio doesn't report its ALC, so nothing turns it down. "
                "Click to pop it out.");
    else if (t.driveAuto)
        tip = _("Pushed in: while transmitting, the radio's ALC is read once a second and the level is turned "
                "down whenever it reads over the target in Preferences, Rig control. It never turns up on its "
                "own. Click to pop it out, or turn it to take over.");
    else
        tip = _("Transmit audio level. Drag up or down, or turn the mouse wheel (0.5 dB a click, 0.1 dB with "
                "shift). Click to push it in, and the radio's ALC turns it down from there as needed.");
    if (tip != driveTip_)
    {
        driveTip_ = tip;
        driveDial_->SetToolTip(tip);
    }
}

void GlissandoConsole::OnTimer(wxTimerEvent&)
{
    refreshTelemetry();
}

void GlissandoConsole::OnClose(wxCloseEvent&)
{
    timer_.Stop();
    host_->glissandoConsoleClosed(GetRect());
    Destroy();
}
