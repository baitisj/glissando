//=========================================================================
// Name:            dlg_snoop.cpp
// Purpose:         The snooping window: every message the station hears,
//                  whoever it was sent to.
//
// Written for Glissando; see dlg_snoop.h.
//=========================================================================

#include "dlg_snoop.h"

#include <algorithm>
#include <cmath>

#include <wx/datetime.h>
#include <wx/sizer.h>
#include <wx/tglbtn.h>

#include "gui/glissando/ChaoticaControls.h"
#include "gui/glissando/ChaoticaTheme.h"
#include "text_messaging/TextMessagingSession.h"

using namespace TextMessaging;

namespace
{

enum
{
    ID_SHOW_FRAMES = wxID_HIGHEST + 1,
    ID_CLEAR,
};

wxString html(const wxColour& colour)
{
    return colour.GetAsString(wxC2S_HTML_SYNTAX);
}

wxString escapeHtml(const std::string& text)
{
    wxString in = wxString::FromUTF8(text.c_str());
    if (in.empty() && !text.empty()) in = wxString(text.c_str(), wxConvISO8859_1);

    wxString out;
    out.reserve(in.size() + 16);
    for (wxUniChar c : in)
    {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '\n') out += "<br>";
        else out += c;
    }
    return out;
}

wxString formatTime(std::time_t when)
{
    return wxDateTime((time_t)when).Format("%H:%M:%S");
}

wxString formatSnr(float snr)
{
    // Data2G does not report one.
    if (!std::isfinite(snr)) return wxEmptyString;
    return wxString::Format("%.1f dB", (double)snr);
}

// "W1ABC → K2XYZ", "W1ABC → ALL".
wxString route(const SnoopEvent& event)
{
    return escapeHtml(event.origin) + " &#8594; " + escapeHtml(event.broadcast ? "ALL" : event.destination);
}

} // namespace

SnoopDialog::SnoopDialog(wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos,
                         const wxSize& size, long style)
    : wxDialog(parent, id, title, pos, size, style)
    , m_log(nullptr)
    , m_btnFrames(nullptr)
    , m_btnClear(nullptr)
    , m_txtTally(nullptr)
    , m_listenerId(0)
{
    buildControls();

    Bind(wxEVT_CLOSE_WINDOW, &SnoopDialog::OnClose, this);
    m_btnFrames->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { render(); });
    m_btnClear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_events.clear();
        render();
    });

    SnoopFeed& feed = TextMessagingSession::instance().snoop();

    // Whatever was heard before the window opened.
    m_events = feed.recent();

    // Frames arrive on the modem's thread; the window is the GUI thread's.
    m_listenerId = feed.addListener([this](const SnoopEvent& event) {
        SnoopEvent copy = event;
        CallAfter([this, copy]() { add(copy); });
    });

    render();
}

SnoopDialog::~SnoopDialog()
{
    TextMessagingSession::instance().snoop().removeListener(m_listenerId);
}

void SnoopDialog::buildControls()
{
    using Chaotica::Button;
    using Chaotica::Panel;
    namespace Colour = Chaotica::Colour;

    SetBackgroundColour(Colour::Void);
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);

    Panel* logPlate = new Panel(this, _("Intercepted traffic"));
    m_log = new wxHtmlWindow(logPlate, wxID_ANY, wxDefaultPosition, wxSize(-1, 300),
                             wxHW_SCROLLBAR_AUTO | wxBORDER_NONE);
    m_log->SetBackgroundColour(Colour::Void);
    m_log->SetStandardFonts(10, "DejaVu Sans Condensed", "DejaVu Sans Mono");
    logPlate->GetContentSizer()->Add(m_log, 1, wxEXPAND);
    mainSizer->Add(logPlate, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

    Panel* controlPlate = new Panel(this, _("Listening post"));
    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);

    m_btnFrames = new Button(controlPlate, ID_SHOW_FRAMES, _("Every frame"), true, wxSize(150, 32));
    m_btnFrames->SetToolTip(
        _("When lit, each fragment is listed as it is decoded, not only the finished message."));
    row->Add(m_btnFrames, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

    m_btnClear = new Button(controlPlate, ID_CLEAR, _("Clear"), false, wxSize(96, 32));
    m_btnClear->SetToolTip(_("Empty the window. Nothing is sent or deleted from chat."));
    row->Add(m_btnClear, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    m_txtTally = new wxStaticText(controlPlate, wxID_ANY, wxEmptyString);
    m_txtTally->SetForegroundColour(Colour::Dim);
    m_txtTally->SetFont(Chaotica::font(Chaotica::FontRole::Caption));
    row->Add(m_txtTally, 1, wxALIGN_CENTER_VERTICAL);

    controlPlate->GetContentSizer()->Add(row, 0, wxEXPAND);
    mainSizer->Add(controlPlate, 0, wxEXPAND | wxALL, 6);

    SetSizer(mainSizer);
    SetMinClientSize(mainSizer->GetMinSize());
    Layout();
}

void SnoopDialog::add(const SnoopEvent& event)
{
    m_events.push_back(event);
    int excess = (int)m_events.size() - (int)SnoopFeed::HISTORY;
    if (excess > 0) m_events.erase(m_events.begin(), m_events.begin() + excess);

    if (shown(event)) render();
    else updateTally();
}

// Finished messages and the signalling around them (pings, pongs and
// acknowledgements) always; the fragments a message is built from only when
// asked for, since the finished message repeats them.
bool SnoopDialog::shown(const SnoopEvent& event) const
{
    if (event.kind == SnoopEvent::Kind::Message) return true;
    if (!FrameCodec::isSignallingFrameType(event.frameType)) return m_btnFrames->IsChecked();
    return true;
}

void SnoopDialog::render()
{
    namespace Colour = Chaotica::Colour;
    wxString page = html(Colour::Void);
    wxString bone = html(Colour::Bone);
    wxString dim = html(Colour::Dim);
    wxString glow = html(Colour::Glow);
    wxString plate = html(Colour::Plate);
    wxString litPlate = "#45433F";      // a plate with a lamp on it

    wxString out;
    out.reserve(8192);
    out += "<html><body bgcolor=\"" + page + "\" text=\"" + bone + "\">";

    if (m_events.empty())
    {
        out += "<p align=\"center\"><font color=\"" + dim +
               "\">Nothing heard yet. Every station's traffic will appear here, "
               "whoever it is addressed to.</font></p>";
    }

    for (const SnoopEvent& event : m_events)
    {
        if (!shown(event)) continue;

        wxString source = event.source == SnoopSource::Data2G ? " &middot; DATA2G" : "";
        wxString snr = formatSnr(event.snr);

        if (event.kind == SnoopEvent::Kind::Message)
        {
            // A message for this station is lit, the way the console lights
            // what concerns the operator; everyone else's is plain silver.
            wxString heading = event.toMe ? glow : bone;
            wxString tag = event.toMe ? " <font size=\"-2\" color=\"" + glow + "\">&nbsp;FOR YOU</font>" : "";

            // An outer table only to leave air under the block, as the chat
            // window does.
            out += "<table width=\"100%\" cellpadding=\"0\" cellspacing=\"0\"><tr><td>";
            out += "<table width=\"100%\" cellpadding=\"6\" cellspacing=\"0\" bgcolor=\"" +
                   (event.toMe ? litPlate : plate) + "\"><tr><td>";
            out += "<table width=\"100%\" cellpadding=\"0\" cellspacing=\"0\"><tr>"
                   "<td align=\"left\"><font color=\"" + heading + "\"><b>" + route(event) + "</b></font>" +
                   tag + "</td>"
                   "<td align=\"right\"><font size=\"-2\" color=\"" + dim + "\">" + formatTime(event.timestamp) +
                   (snr.empty() ? "" : " &middot; " + snr) + source + "</font></td></tr></table>";
            out += "<font color=\"" + (event.toMe ? glow : bone) + "\">" + escapeHtml(event.text) + "</font>";
            out += "</td></tr></table>";
            out += "</td></tr><tr><td height=\"6\"><font size=\"1\">&nbsp;</font></td></tr></table>";
            continue;
        }

        // A frame: one dim line in the teleprinter's hand.
        wxString what = escapeHtml(describeSnoopFrame(event));
        if (!event.text.empty()) what += " &nbsp;&laquo;" + escapeHtml(event.text) + "&raquo;";

        out += "<table width=\"100%\" cellpadding=\"1\" cellspacing=\"0\"><tr>"
               "<td nowrap><tt><font size=\"-1\" color=\"" + dim + "\">" + formatTime(event.timestamp) +
               "&nbsp; " + route(event) + "&nbsp; " + what + "</font></tt></td>"
               "<td align=\"right\" nowrap><font size=\"-2\" color=\"" + dim + "\">" + snr + source +
               "</font></td></tr></table>";
    }

    out += "</body></html>";

    // Frozen, so the page and the scroll to its end are one repaint.
    m_log->Freeze();
    m_log->SetPage(out);
    m_log->Scroll(0, m_log->GetScrollRange(wxVERTICAL));
    m_log->Thaw();

    updateTally();
}

void SnoopDialog::updateTally()
{
    int messages = 0;
    std::vector<std::string> stations;
    for (const SnoopEvent& event : m_events)
    {
        if (event.kind == SnoopEvent::Kind::Message) messages++;
        if (std::find(stations.begin(), stations.end(), event.origin) == stations.end())
        {
            stations.push_back(event.origin);
        }
    }

    m_txtTally->SetLabel(wxString::Format(_("%d messages from %d stations"), messages, (int)stations.size()));
}

void SnoopDialog::OnClose(wxCloseEvent&)
{
    // The feed keeps listening; the window reopens from the console's
    // Command plate with everything heard meanwhile.
    Hide();
}
