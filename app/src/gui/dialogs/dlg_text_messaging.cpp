//=========================================================================
// Name:            dlg_text_messaging.cpp
// Purpose:         The Glissando chat window, in the console's Chaotica dress.
//
// Authors:         FreeDV text messaging contributors
// License:
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//=========================================================================

#include "dlg_text_messaging.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <memory>

#include <wx/datetime.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>

#include "main.h"
#include "gui/glissando/ChaoticaControls.h"
#include "gui/glissando/ChaoticaTheme.h"
#include "text_messaging/DeliveryChip.h"
#include "text_messaging/FrameCodec.h"
#include "text_messaging/HeardStationList.h"
#include "text_messaging/MessageStore.h"
#include "text_messaging/TextMessagingSession.h"

using namespace TextMessaging;

namespace
{

// How often the station list ages out and the "last heard" column is redrawn.
constexpr int REFRESH_INTERVAL_MS = 1000;

// Often enough to follow the shared blink, which changes every half second.
constexpr int BLINK_INTERVAL_MS = 125;

// A press and release on the chat log further apart than this is a drag to
// select text, not a click on a message.
constexpr int CLICK_SLOP_PIXELS = 4;

enum
{
    ID_STATION_LIST = wxID_HIGHEST + 700,
    ID_ADD_STATION_ENTRY,
    ID_ADD_STATION,
    ID_MENU_SELECT_STATION,
    ID_MENU_REMOVE_STATION,
    ID_MENU_LAST_HEARD,
    ID_MENU_REMOVE_MESSAGE,
    ID_MENU_ABORT_MESSAGE,
    ID_MENU_CLEAR_MESSAGES,
    ID_PING,
    ID_SEND,
    ID_AUTO_REPLY,
    ID_ENTRY,
    ID_REFRESH_TIMER,
    ID_BLINK_TIMER,
};

wxString escapeHtml(const std::string& text)
{
    wxString result;
    result.reserve(text.size() + 16);

    for (char c : text)
    {
        switch (c)
        {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\n': result += "<br>"; break;
            case '\r': break;
            default: result += (wxChar)(unsigned char)c; break;
        }
    }

    return result;
}

wxString formatTime(std::time_t when)
{
    return wxDateTime((time_t)when).Format("%H:%M:%S");
}

wxString formatSnr(float snr)
{
    // Data2G does not report one.
    if (!std::isfinite(snr)) return wxString::FromUTF8("\u2014");
    return wxString::Format("%.1f dB", (double)snr);
}

// Relative time reads better than a clock for "is this station still here".
wxString formatAge(std::time_t lastHeard, std::time_t now)
{
    std::time_t age = now - lastHeard;
    if (age < 60) return _("just now");
    if (age < 3600) return wxString::Format(_("%d min ago"), (int)(age / 60));

    return wxString::Format(_("%d hr ago"), (int)(age / 3600));
}

// A station added by hand has no decode to show until it is heard.
wxString formatStationSnr(const HeardStation& station)
{
    return station.lastHeard == 0 ? wxString() : formatSnr(station.snr);
}

wxString formatStationAge(const HeardStation& station, std::time_t now)
{
    return station.lastHeard == 0 ? wxString(_("never")) : formatAge(station.lastHeard, now);
}

// Set FREEDV_TEXT_CHAT_UI_LOG to have the window report what it is showing.
// Watching a chat window over someone's shoulder is a poor way to find a
// refresh bug; this puts the same information in the log.
bool uiLogEnabled()
{
    static const bool enabled = std::getenv("FREEDV_TEXT_CHAT_UI_LOG") != nullptr;
    return enabled;
}

// The delivery chip on the right of a sent message: its text and its colours.
struct DeliveryChip
{
    wxString label;
    wxString background;
    wxString foreground = "#EEE8D8";
};

// Which chip applies is decided in DeliveryChip.h; this is its wording and
// colours.
DeliveryChip deliveryChip(const TextMessage& message, bool waitingForEngage = false, bool lit = false)
{
    DeliveryChipState state = deliveryChipState(message, waitingForEngage);

    // Black and white, like the rest of the console: dark while waiting,
    // lit while going out, chrome once confirmed. Only a failure is red.
    const wxString smoke = "#4A4845";
    const wxString silver = "#B8B2A2";
    const wxString black = "#0A0A0B";

    DeliveryChip chip;
    switch (state.kind)
    {
        case DeliveryChipKind::None:
            return chip;
        case DeliveryChipKind::Queued:
            chip.label = _("QUEUED");
            chip.background = smoke;
            break;
        case DeliveryChipKind::EngageToSend:
            // Flashes with the Engage button: red with dark lettering, then
            // smoke with red lettering, so it reads in both halves.
            chip.label = _("ENGAGE TO SEND");
            chip.background = lit ? "#EC4034" : smoke;
            chip.foreground = lit ? black : "#EC4034";
            break;
        case DeliveryChipKind::Sending:
            chip.label = _("SENDING");
            chip.background = "#FFFCF0";
            chip.foreground = black;
            break;
        case DeliveryChipKind::Sent:
            chip.label = _("SENT");
            chip.background = smoke;
            break;
        case DeliveryChipKind::Retry:
            chip.label = wxString::Format(_("RETRY #%d"), state.retry);
            chip.background = silver;
            chip.foreground = black;
            break;
        case DeliveryChipKind::Resend:
            chip.label = _("RESEND");
            chip.background = silver;
            chip.foreground = black;
            break;
        case DeliveryChipKind::Acknowledged:
            chip.label = _("OK");
            chip.background = "#C8C6C0";
            chip.foreground = black;
            break;
        case DeliveryChipKind::NotAcknowledged:
            chip.label = _("NO ACK");
            chip.background = "#8E2A20";
            break;
        case DeliveryChipKind::NotSent:
            chip.label = _("NOT SENT");
            chip.background = smoke;
            break;
        case DeliveryChipKind::Aborted:
            chip.label = _("ABORTED");
            chip.background = "#8E2A20";
            break;
    }

    // How many fragments the far end has, out of how many there are.
    if (state.showsProgress())
    {
        chip.label += wxString::Format(" %d/%d", state.fragmentsConfirmed, state.fragmentCount);
    }

    return chip;
}

// Where a ping of ours has got to, for its line in the chat, while that is
// not plain from the lines around it: a pong or "no response" follows the
// rest.
wxString pingState(const TextMessage& message)
{
    if (message.direction != MessageDirection::Sent) return "";

    switch (message.status)
    {
        case MessageStatus::Queued: return _("queued");
        case MessageStatus::Transmitting: return _("on the air");
        case MessageStatus::AwaitingAck: return _("awaiting PONG");
        case MessageStatus::NotSent: return _("not sent");
        case MessageStatus::Aborted: return _("aborted");
        default: return "";
    }
}

wxString statusChip(const TextMessage& message, bool waitingForEngage, bool lit)
{
    DeliveryChip chip = deliveryChip(message, waitingForEngage, lit);
    if (chip.label.empty()) return "";

    return "<table cellpadding=\"2\" cellspacing=\"0\" bgcolor=\"" + chip.background +
           "\"><tr><td><font size=\"-2\" color=\"" + chip.foreground + "\">" + chip.label +
           "</font></td></tr></table>";
}

} // namespace

TextMessagingDialog::TextMessagingDialog(wxWindow* parent, wxWindowID id, const wxString& title,
                                         const wxPoint& pos, const wxSize& size, long style)
    : wxDialog(parent, id, title, pos, size, style)
    , m_stationList(nullptr)
    , m_txtAddStation(nullptr)
    , m_btnAddStation(nullptr)
    , m_btnPing(nullptr)
    , m_chatWindow(nullptr)
    , m_txtEntry(nullptr)
    , m_btnSend(nullptr)
    , m_chkAutoReply(nullptr)
    , m_txtStatus(nullptr)
    , m_txtInhibited(nullptr)
    , m_txtModem(nullptr)
    , m_refreshTimer(this, ID_REFRESH_TIMER)
    , m_blinkTimer(this, ID_BLINK_TIMER)
    , m_transmitting(false)
    , m_transmitControlsDisabled(false)
    , m_statusKind(StatusKind::Sticky)
    , m_lastAckWait(AckWait::Nothing)
{
    buildControls();

    Connect(ID_SEND, wxEVT_COMMAND_BUTTON_CLICKED,
            wxCommandEventHandler(TextMessagingDialog::OnSend));
    Connect(ID_PING, wxEVT_COMMAND_BUTTON_CLICKED,
            wxCommandEventHandler(TextMessagingDialog::OnPing));
    Connect(ID_ADD_STATION, wxEVT_COMMAND_BUTTON_CLICKED,
            wxCommandEventHandler(TextMessagingDialog::OnAddStation));
    Connect(ID_ADD_STATION_ENTRY, wxEVT_COMMAND_TEXT_ENTER,
            wxCommandEventHandler(TextMessagingDialog::OnAddStation));
    Connect(ID_ADD_STATION_ENTRY, wxEVT_COMMAND_TEXT_UPDATED,
            wxCommandEventHandler(TextMessagingDialog::OnAddStationText));
    Connect(ID_MENU_SELECT_STATION, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuSelectStation));
    Connect(ID_MENU_REMOVE_STATION, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuRemoveStation));
    Connect(ID_MENU_REMOVE_MESSAGE, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuCancelMessage));
    Connect(ID_MENU_ABORT_MESSAGE, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuCancelMessage));
    Connect(ID_MENU_CLEAR_MESSAGES, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuClearMessages));
    Connect(ID_AUTO_REPLY, wxEVT_TOGGLEBUTTON,
            wxCommandEventHandler(TextMessagingDialog::OnAutoReplyToggled));
    Connect(ID_STATION_LIST, wxEVT_COMMAND_LIST_ITEM_SELECTED,
            wxListEventHandler(TextMessagingDialog::OnStationSelected));
    Connect(ID_STATION_LIST, wxEVT_COMMAND_LIST_ITEM_DESELECTED,
            wxListEventHandler(TextMessagingDialog::OnStationDeselected));
    Connect(ID_REFRESH_TIMER, wxEVT_TIMER, wxTimerEventHandler(TextMessagingDialog::OnTimer));
    Connect(ID_BLINK_TIMER, wxEVT_TIMER, wxTimerEventHandler(TextMessagingDialog::OnBlinkTimer));
    Connect(wxEVT_CLOSE_WINDOW, wxCloseEventHandler(TextMessagingDialog::OnClose));

    m_txtEntry->Connect(wxEVT_KEY_DOWN, wxKeyEventHandler(TextMessagingDialog::OnEntryKeyDown),
                        nullptr, this);
    Connect(ID_ENTRY, wxEVT_COMMAND_TEXT_UPDATED, wxCommandEventHandler(TextMessagingDialog::OnEntryText));
    connectStationMouse(true);

    // The chat log keeps its own handling of the mouse, for selecting text;
    // these only watch it, and pass every event on.
    m_chatWindow->Bind(wxEVT_LEFT_DOWN, &TextMessagingDialog::OnChatLeftDown, this);
    m_chatWindow->Bind(wxEVT_LEFT_UP, &TextMessagingDialog::OnChatLeftUp, this);
    m_chatWindow->Bind(wxEVT_CONTEXT_MENU, &TextMessagingDialog::OnChatContextMenu, this);

    TextMessagingSession::instance().protocol().setObserver(this);
    m_refreshTimer.Start(REFRESH_INTERVAL_MS);
    m_blinkTimer.Start(BLINK_INTERVAL_MS);

    if (uiLogEnabled()) log_info("UI: chat window created, observer registered");
}

TextMessagingDialog::~TextMessagingDialog()
{
    m_refreshTimer.Stop();
    m_blinkTimer.Stop();
    TextMessagingSession::instance().protocol().setObserver(nullptr);

    m_txtEntry->Disconnect(wxEVT_KEY_DOWN, wxKeyEventHandler(TextMessagingDialog::OnEntryKeyDown),
                           nullptr, this);
    connectStationMouse(false);
    m_chatWindow->Unbind(wxEVT_LEFT_DOWN, &TextMessagingDialog::OnChatLeftDown, this);
    m_chatWindow->Unbind(wxEVT_LEFT_UP, &TextMessagingDialog::OnChatLeftUp, this);
    m_chatWindow->Unbind(wxEVT_CONTEXT_MENU, &TextMessagingDialog::OnChatContextMenu, this);
}

// Selection is decided here rather than by the list. A click on the selected
// station clears it, and a right click leaves the selection alone so the menu
// can offer either choice; the toolkit does the opposite on both counts, so
// the press is taken before it sees it. Where the control is native the press
// arrives on the control; the generic implementation delivers it to a child
// window instead, so every window the list is made of is listened to.
void TextMessagingDialog::connectStationMouse(bool connect)
{
    std::vector<wxWindow*> windows;
    windows.push_back(m_stationList);
    for (wxWindow* child : m_stationList->GetChildren()) windows.push_back(child);

    for (wxWindow* window : windows)
    {
        if (connect)
        {
            window->Connect(wxEVT_LEFT_DOWN,
                            wxMouseEventHandler(TextMessagingDialog::OnStationLeftDown),
                            nullptr, this);
            window->Connect(wxEVT_RIGHT_DOWN,
                            wxMouseEventHandler(TextMessagingDialog::OnStationRightDown),
                            nullptr, this);
        }
        else
        {
            window->Disconnect(wxEVT_LEFT_DOWN,
                               wxMouseEventHandler(TextMessagingDialog::OnStationLeftDown),
                               nullptr, this);
            window->Disconnect(wxEVT_RIGHT_DOWN,
                               wxMouseEventHandler(TextMessagingDialog::OnStationRightDown),
                               nullptr, this);
        }
    }
}

TextMessagingDialog::Palette TextMessagingDialog::palette() const
{
    auto html = [](const wxColour& colour) { return colour.GetAsString(wxC2S_HTML_SYNTAX); };

    Palette palette;
    palette.page = html(Chaotica::Colour::Void);
    palette.text = html(Chaotica::Colour::Bone);
    palette.sentBubble = "#3A3936";
    palette.receivedBubble = html(Chaotica::Colour::Plate);
    palette.subdued = html(Chaotica::Colour::Dim);
    return palette;
}

// No autoresize: a label that sized itself to its text would feed that width
// back to the layout, which would wrap it to that, and so on. The minimum width
// is nil so that a long line does not hold the window open; the height follows
// the wrapped text.
WrappingText::WrappingText(wxWindow* parent)
    : wxStaticText(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                   wxST_NO_AUTORESIZE)
    , m_wrapWidth(-1)
{
    SetMinSize(wxSize(0, -1));
    Bind(wxEVT_SIZE, &WrappingText::OnSize, this);
}

void WrappingText::setText(const wxString& text)
{
    if (text == m_text) return;

    m_text = text;
    m_wrapWidth = -1;
    rewrap(GetSize().GetWidth());
}

void WrappingText::rewrap(int width)
{
    if (width == m_wrapWidth) return;
    m_wrapWidth = width;

    int oldHeight = GetBestSize().GetHeight();

    // wxWidgets 3.3 skips a Wrap() to the width it last wrapped at, even when
    // the label has changed since, which left a new label on one line cut off
    // at the window edge. Wrap(-1), no wrapping, resets that.
    SetLabelText(m_text);
    Wrap(-1);
    if (width > 0) Wrap(width);
    InvalidateBestSize();

    // More lines or fewer: the window has to make room. Not from inside the
    // size event that got us here, though.
    if (GetBestSize().GetHeight() != oldHeight)
    {
        CallAfter([this]() {
            wxWindow* top = wxGetTopLevelParent(this);
            if (top != nullptr) top->Layout();
        });
    }
}

void WrappingText::OnSize(wxSizeEvent& event)
{
    rewrap(event.GetSize().GetWidth());
    event.Skip();
}

namespace
{

// Column captions over the station list, engraved like the console's, in
// place of the platform's light header bar.
class StationHeader : public wxPanel
{
public:
    StationHeader(wxWindow* parent, wxListCtrl* list)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 18))
        , list_(list)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &StationHeader::OnPaint, this);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(Chaotica::Colour::Plate));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
        if (!gc) return;

        gc->SetFont(Chaotica::font(Chaotica::FontRole::Caption), Chaotica::Colour::Dim);
        double x = 6;
        for (int column = 0; column < list_->GetColumnCount(); column++)
        {
            wxListItem item;
            item.SetMask(wxLIST_MASK_TEXT);
            list_->GetColumn(column, item);
            Chaotica::drawSpacedText(gc.get(), item.GetText(), x, 3, 2.0);
            x += list_->GetColumnWidth(column);
        }
    }

    wxListCtrl* list_;
};

void darken(wxWindow* window)
{
    window->SetBackgroundColour(Chaotica::Colour::Bakelite);
    window->SetForegroundColour(Chaotica::Colour::Bone);
}

} // namespace

void TextMessagingDialog::buildControls()
{
    using Chaotica::Button;
    using Chaotica::Panel;
    namespace Colour = Chaotica::Colour;

    SetBackgroundColour(Colour::Void);
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);

    // Heard stations on the left, the log of transmissions on the right.
    wxBoxSizer* topSizer = new wxBoxSizer(wxHORIZONTAL);

    Panel* stationPlate = new Panel(this, _("Heard stations"));
    wxSizer* stationSizer = stationPlate->GetContentSizer();

    m_stationList = new wxListCtrl(stationPlate, ID_STATION_LIST, wxDefaultPosition, wxSize(260, -1),
                                   wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_NO_HEADER | wxBORDER_NONE);
    darken(m_stationList);
    m_stationList->SetTextColour(Colour::Bone);
    m_stationList->InsertColumn(0, _("Callsign"), wxLIST_FORMAT_LEFT, 100);
    m_stationList->InsertColumn(1, _("SNR"), wxLIST_FORMAT_RIGHT, 70);
    m_stationList->InsertColumn(2, _("Heard"), wxLIST_FORMAT_LEFT, 90);
    stationSizer->Add(new StationHeader(stationPlate, m_stationList), 0, wxEXPAND);
    stationSizer->Add(m_stationList, 1, wxEXPAND | wxBOTTOM, 6);

    // Typing a callsign puts a station on the list before it has been heard,
    // so a directed message can be the first thing sent.
    wxBoxSizer* addSizer = new wxBoxSizer(wxHORIZONTAL);
    m_txtAddStation = new wxTextCtrl(stationPlate, ID_ADD_STATION_ENTRY, wxEmptyString,
                                     wxDefaultPosition, wxSize(-1, 32), wxTE_PROCESS_ENTER | wxBORDER_SIMPLE);
    darken(m_txtAddStation);
    m_txtAddStation->SetHint(_("Callsign"));
    m_txtAddStation->SetToolTip(_("Add a station to message before it has been heard."));
    addSizer->Add(m_txtAddStation, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

    m_btnAddStation = new Button(stationPlate, ID_ADD_STATION, _("Add"), false, wxSize(76, 32));
    m_btnAddStation->Enable(false);
    addSizer->Add(m_btnAddStation, 0);
    stationSizer->Add(addSizer, 0, wxEXPAND | wxBOTTOM, 6);

    m_btnPing = new Button(stationPlate, ID_PING, _("Ping"), false, wxSize(-1, 32));
    m_btnPing->SetToolTip(_("Ask the selected station to answer, to see whether you are being heard."));
    m_btnPing->Enable(false);
    stationSizer->Add(m_btnPing, 0, wxEXPAND);

    topSizer->Add(stationPlate, 0, wxEXPAND | wxRIGHT, 6);

    Panel* logPlate = new Panel(this, _("Transmission log"));
    m_chatWindow = new wxHtmlWindow(logPlate, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxHW_SCROLLBAR_AUTO | wxBORDER_NONE);
    m_chatWindow->SetBackgroundColour(Colour::Void);
    m_chatWindow->SetStandardFonts(10, "DejaVu Sans Condensed", "DejaVu Sans Mono");
    logPlate->GetContentSizer()->Add(m_chatWindow, 1, wxEXPAND);
    topSizer->Add(logPlate, 1, wxEXPAND);

    mainSizer->Add(topSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

    // The transmitter: entry box with a send button beside it.
    Panel* transmitPlate = new Panel(this, _("Transmitter"));
    wxBoxSizer* entrySizer = new wxBoxSizer(wxHORIZONTAL);
    m_txtEntry = new wxTextCtrl(transmitPlate, ID_ENTRY, wxEmptyString, wxDefaultPosition, wxSize(-1, 70),
                                wxTE_MULTILINE | wxBORDER_SIMPLE);
    darken(m_txtEntry);
    m_txtEntry->SetToolTip(_("Enter sends the message; Shift+Enter starts a new line."));
    entrySizer->Add(m_txtEntry, 1, wxEXPAND | wxRIGHT, 6);

    // The button names where the message goes; updateSelectionControls keeps
    // it right as the selection changes.
    m_btnSend = new Button(transmitPlate, ID_SEND, wxEmptyString, false, wxSize(170, 70));
    entrySizer->Add(m_btnSend, 0, wxEXPAND);
    transmitPlate->GetContentSizer()->Add(entrySizer, 0, wxEXPAND | wxBOTTOM, 6);

    // Why the station may not transmit, while it may not. A line of its own,
    // so that the ordinary status line cannot write over it.
    m_txtInhibited = new WrappingText(transmitPlate);
    m_txtInhibited->SetForegroundColour(wxColour("#E0A060"));
    m_txtInhibited->Hide();
    transmitPlate->GetContentSizer()->Add(m_txtInhibited, 0, wxEXPAND | wxBOTTOM, 4);

    // Which external modem chat goes through, and whether it is there.
    m_txtModem = new WrappingText(transmitPlate);
    m_txtModem->SetForegroundColour(Colour::Bone);
    m_txtModem->Hide();
    transmitPlate->GetContentSizer()->Add(m_txtModem, 0, wxEXPAND | wxBOTTOM, 4);

    wxBoxSizer* bottomSizer = new wxBoxSizer(wxHORIZONTAL);
    m_chkAutoReply = new Button(transmitPlate, ID_AUTO_REPLY, _("Auto acknowledge"), true, wxSize(190, 30));
    m_chkAutoReply->SetChecked(TextMessagingSession::instance().protocol().autoReplyEnabled());
    m_chkAutoReply->SetToolTip(
        _("When lit, this station transmits on its own to confirm messages and answer pings."));
    bottomSizer->Add(m_chkAutoReply, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    m_txtStatus = new WrappingText(transmitPlate);
    m_txtStatus->SetForegroundColour(Colour::Bone);
    bottomSizer->Add(m_txtStatus, 1, wxALIGN_CENTER_VERTICAL);
    transmitPlate->GetContentSizer()->Add(bottomSizer, 0, wxEXPAND);

    mainSizer->Add(transmitPlate, 0, wxEXPAND | wxALL, 6);

    updateSelectionControls();

    SetSizer(mainSizer);

    // As narrow as the plates allow and no narrower: below that their
    // nameplates and buttons are cut off. The status lines wrap to fit.
    SetMinClientSize(mainSizer->GetMinSize());
    Layout();
}

void TextMessagingDialog::refreshFromSession()
{
    auto& session = TextMessagingSession::instance();

    // The chat callsign follows the reporting callsign, which is the one the
    // operator has already told FreeDV about.
    session.protocol().setMyCallsign(
        wxGetApp().appConfiguration.reportingConfiguration.reportingCallsign->ToStdString());
    session.snoop().setMyCallsign(session.protocol().myCallsign());

    m_messages = session.store().recentMessages(TextMessagingSession::MESSAGES_TO_RESTORE);
    m_chkAutoReply->SetChecked(session.protocol().autoReplyEnabled());

    if (session.protocol().myCallsign().empty())
    {
        setStatus(_("Set your callsign in Preferences, Options before sending anything."));
    }
    else
    {
        setStatus(wxEmptyString);
    }

    renderChat();
    refreshStations();
    updateTransmitControls();
}

void TextMessagingDialog::setStatus(const wxString& status, StatusKind kind)
{
    m_statusKind = status.empty() ? StatusKind::Sticky : kind;
    m_txtStatus->setText(status);

    if (uiLogEnabled()) log_info("UI: status \"%s\"", (const char*)status.ToUTF8());
}

// The status line follows the acknowledgement cycle while one is running. It
// takes precedence over whatever was there: an error from a minute ago is less
// use than saying what the station is doing now.
void TextMessagingDialog::updateAckWaitStatus()
{
    // The transmitter being keyed is the more immediate news; the wait is
    // reported once the burst ends.
    if (m_statusKind == StatusKind::Activity) return;

    AckWait wait = TextMessagingSession::instance().protocol().ackWait();
    if (wait == m_lastAckWait) return;

    m_lastAckWait = wait;

    switch (wait)
    {
        case AckWait::Message:
            setStatus(_("Awaiting message ACK."), StatusKind::AckWait);
            break;
        case AckWait::Ping:
            setStatus(_("Awaiting ping ACK."), StatusKind::AckWait);
            break;
        case AckWait::Nothing:
            // Only our own notice is cleared; anything newer stands.
            if (m_statusKind == StatusKind::AckWait) setStatus(wxEmptyString);
            break;
    }
}

void TextMessagingDialog::appendMessage(const TextMessage& message)
{
    m_messages.push_back(message);

    int excess = (int)m_messages.size() - TextMessagingSession::MESSAGES_TO_RESTORE;
    if (excess > 0) m_messages.erase(m_messages.begin(), m_messages.begin() + excess);
}

void TextMessagingDialog::renderChat(bool keepPlace)
{
    Palette colors = palette();

    wxString html;
    html.reserve(4096);
    html += "<html><body bgcolor=\"" + colors.page + "\" text=\"" + colors.text + "\">";

    for (size_t index = 0; index < m_messages.size(); index++)
    {
        const TextMessage& message = m_messages[index];

        // Marks where each message starts, so a click can be traced back to
        // it: see messageAt(). Ahead of the message's table rather than in
        // it, where the page would place it at the message's foot.
        html += wxString::Format("<a name=\"m%d\"></a>", (int)index);

        if (message.kind == MessageKind::System)
        {
            wxString state = pingState(message);
            if (!state.empty()) state = " &middot; " + state;
            html += "<table width=\"100%\"><tr><td align=\"center\"><font size=\"-2\" color=\"" +
                    colors.subdued + "\">" + escapeHtml(message.text) + state + " &middot; " +
                    formatTime(message.timestamp) + "</font></td></tr></table>";
            continue;
        }

        bool sent = message.direction == MessageDirection::Sent;
        wxString align = sent ? "right" : "left";
        wxString bubble = sent ? colors.sentBubble : colors.receivedBubble;

        wxString body;
        if (!sent) body += "<b>" + escapeHtml(message.originCallsign) + ":</b> ";
        body += escapeHtml(message.text);

        wxString tag;
        if (message.broadcast)
        {
            tag = " <font size=\"-2\" color=\"" + colors.subdued + "\">[BCAST]</font>";
        }

        // Timestamp on the left, delivery status on the right.
        wxString right;
        if (sent)
        {
            // Only a message still in the queue waits for Engage; one left
            // queued by an earlier run never goes, engaged or not.
            bool waits = m_waitingForEngage && message.status == MessageStatus::Queued &&
                         TextMessagingSession::instance().protocol().isMessageQueued(message.id);
            right = statusChip(message, waits, m_engageChipLit);
        }
        else if (message.snr != 0.0f && std::isfinite(message.snr))
        {
            right = "<font size=\"-2\" color=\"" + colors.subdued + "\">" +
                    formatSnr(message.snr) + "</font>";
        }

        // One coloured block per message. The text and the line describing it
        // share a single padded cell, so they sit tight against each other and
        // the background encloses both rather than the status floating below.
        html += "<table width=\"100%\" cellpadding=\"0\" cellspacing=\"0\"><tr><td align=\"" +
                align + "\">";
        html += "<table cellpadding=\"6\" cellspacing=\"0\" bgcolor=\"" + bubble +
                "\"><tr><td>";
        html += "<font color=\"" + colors.text + "\">" + body + tag + "</font>";
        html += "<table width=\"100%\" cellpadding=\"0\" cellspacing=\"0\"><tr>"
                "<td align=\"left\"><font size=\"-2\" color=\"" + colors.subdued + "\">" +
                formatTime(message.timestamp) + "</font></td>"
                "<td align=\"right\">" + right + "</td></tr></table>";
        html += "</td></tr></table>";
        html += "</td></tr>";

        // Air between messages, none inside one.
        html += "<tr><td height=\"10\"></td></tr>";
        html += "</table>";
    }

    html += "</body></html>";

    // Setting the page and then scrolling it are two repaints; frozen, the
    // window shows the result rather than the intermediate state.
    int viewX = 0, viewY = 0;
    m_chatWindow->GetViewStart(&viewX, &viewY);

    m_chatWindow->Freeze();
    m_chatWindow->SetPage(html);

    // Keep the newest message in view, the way a chat window should; but a
    // chip flashing must not pull the view off whatever is being read.
    if (keepPlace)
    {
        m_chatWindow->Scroll(viewX, viewY);
    }
    else
    {
        m_chatWindow->Scroll(0, m_chatWindow->GetScrollRange(wxVERTICAL));
    }
    m_chatWindow->Thaw();
}

void TextMessagingDialog::refreshStations()
{
    auto& session = TextMessagingSession::instance();
    std::time_t now = std::time(nullptr);
    session.stations().prune(now);

    std::vector<HeardStation> stations = session.stations().stations();

    // This runs on a one second timer. Rebuilding the list every time made the
    // window flicker and, worse, dropped and restored the selection underneath
    // whoever was trying to click it. Rows are updated in place, and the list
    // is only rebuilt when the stations themselves change.
    bool sameRows = (long)stations.size() == m_stationList->GetItemCount();
    for (size_t index = 0; sameRows && index < stations.size(); index++)
    {
        sameRows = m_stationList->GetItemText((long)index, 0).ToStdString() ==
                   stations[index].callsign;
    }

    if (sameRows)
    {
        for (size_t index = 0; index < stations.size(); index++)
        {
            setColumnIfChanged((long)index, 1, formatStationSnr(stations[index]));
            setColumnIfChanged((long)index, 2, formatStationAge(stations[index], now));
        }
        return;
    }

    if (uiLogEnabled())
    {
        log_info("UI: heard station list rebuilt (%d rows)", (int)stations.size());
    }

    std::string previousSelection = selectedCallsign();
    long selectedIndex = -1;

    m_stationList->Freeze();
    m_stationList->DeleteAllItems();

    for (size_t index = 0; index < stations.size(); index++)
    {
        const HeardStation& station = stations[index];
        long item = m_stationList->InsertItem((long)index, wxString::FromUTF8(station.callsign));
        m_stationList->SetItem(item, 1, formatStationSnr(station));
        m_stationList->SetItem(item, 2, formatStationAge(station, now));

        if (station.callsign == previousSelection) selectedIndex = item;
    }

    m_stationList->Thaw();

    if (selectedIndex >= 0)
    {
        m_stationList->SetItemState(selectedIndex, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }

    // A rebuild can drop the selection without the list saying so.
    updateSelectionControls();
}

// A wxListCtrl repaints a cell whenever it is set, so the text is compared
// first: on a quiet minute nothing changes and nothing is redrawn.
void TextMessagingDialog::setColumnIfChanged(long item, int column, const wxString& text)
{
    if (m_stationList->GetItemText(item, column) == text) return;
    m_stationList->SetItem(item, column, text);
}

std::string TextMessagingDialog::selectedCallsign() const
{
    long item = m_stationList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (item < 0) return "";

    return m_stationList->GetItemText(item).ToStdString();
}

long TextMessagingDialog::stationItem(const std::string& callsign) const
{
    for (long item = 0; item < m_stationList->GetItemCount(); item++)
    {
        if (m_stationList->GetItemText(item).ToStdString() == callsign) return item;
    }

    return -1;
}

// The row under the mouse, or -1 for the header, the space below the last
// row, or anywhere else a click means nothing. Row rectangles come back in
// the control's own coordinates on every port, so the press is mapped into
// those whichever of the list's windows delivered it.
long TextMessagingDialog::stationAt(const wxMouseEvent& event) const
{
    wxWindow* source = dynamic_cast<wxWindow*>(event.GetEventObject());
    if (source == nullptr) return -1;

    wxPoint point = m_stationList->ScreenToClient(source->ClientToScreen(event.GetPosition()));
    for (long item = 0; item < m_stationList->GetItemCount(); item++)
    {
        wxRect rect;
        if (m_stationList->GetItemRect(item, rect) && rect.Contains(point)) return item;
    }

    return -1;
}

void TextMessagingDialog::setStationSelected(long item, bool selected)
{
    m_stationList->SetItemState(item, selected ? wxLIST_STATE_SELECTED : 0,
                                wxLIST_STATE_SELECTED);

    // The list reports the change as an event, but not on every platform for
    // every path, so the controls are brought up to date here regardless.
    updateSelectionControls();
}

// Ping and the send button follow the selection: the send button names where
// the message goes, so there is no second button for broadcasting. Clearing
// the selection is how the operator reaches everybody.
void TextMessagingDialog::updateSelectionControls()
{
    std::string callsign = selectedCallsign();
    bool selected = !callsign.empty();

    bool pingable = selected && m_inhibitReason.empty();
    if (m_btnPing->IsEnabled() != pingable) m_btnPing->Enable(pingable);

    wxString label = selected ? wxString::Format(">> %s", wxString::FromUTF8(callsign))
                              : wxString(_(">> Broadcast"));
    if (m_btnSend->GetLabel() == label) return;

    m_btnSend->SetLabel(label);
    m_sendToolTip =
        selected ? wxString::Format(_("Send to %s and ask for confirmation."),
                                    wxString::FromUTF8(callsign))
                 : wxString(_("Send to everybody listening. Nothing is expected back, "
                              "so there is no delivery check."));
    updateSendToolTip();

    if (uiLogEnabled()) log_info("UI: send button \"%s\"", (const char*)label.ToUTF8());
}

void TextMessagingDialog::addStation()
{
    auto& session = TextMessagingSession::instance();

    wxString typed = m_txtAddStation->GetValue();
    if (typed.empty()) return;

    std::string callsign = FrameCodec::normalizeCallsign(typed.ToStdString());
    if (callsign.empty())
    {
        setStatus(wxString::Format(_("\"%s\" has nothing in it a callsign can carry."), typed));
        return;
    }

    if ((int)callsign.size() > MAX_PACKED_CALLSIGN_CHARS)
    {
        setStatus(wxString::Format(_("%s is longer than the %d characters a frame can carry."),
                                   wxString::FromUTF8(callsign), MAX_PACKED_CALLSIGN_CHARS));
        return;
    }

    if (callsign == session.protocol().myCallsign())
    {
        setStatus(_("That is your own callsign."));
        return;
    }

    session.stations().pin(callsign);
    session.snoop().addKnownCallsign(callsign);
    m_txtAddStation->Clear();
    refreshStations();

    // Adding a station is the first step of messaging it, so it is selected.
    long item = stationItem(callsign);
    if (item >= 0) setStationSelected(item, true);

    setStatus(wxString::Format(_("%s added to the station list."), wxString::FromUTF8(callsign)));
    if (uiLogEnabled()) log_info("UI: station %s added by hand", callsign.c_str());
}

void TextMessagingDialog::send(const std::string& destination)
{
    std::string text = m_txtEntry->GetValue().ToStdString();
    std::string error;

    if (TextMessagingSession::instance().protocol().sendMessage(text, destination, error))
    {
        m_txtEntry->Clear();
        setStatus(destination.empty()
                      ? _("Broadcast queued.")
                      : wxString::Format(_("Message to %s queued."),
                                         wxString::FromUTF8(destination)),
                  StatusKind::Queued);
    }
    else
    {
        setStatus(wxString::FromUTF8(error));
    }
}

void TextMessagingDialog::OnSend(wxCommandEvent&)
{
    send(selectedCallsign());
    updateTransmitControls();
}

void TextMessagingDialog::OnPing(wxCommandEvent&)
{
    std::string destination = selectedCallsign();
    if (destination.empty()) return;

    std::string error;
    if (!TextMessagingSession::instance().protocol().sendPing(destination, error))
    {
        setStatus(wxString::FromUTF8(error));
    }
    else
    {
        setStatus(wxString::Format(_("Ping to %s queued."), wxString::FromUTF8(destination)),
                  StatusKind::Queued);
    }
}

void TextMessagingDialog::OnStationSelected(wxListEvent& event)
{
    updateSelectionControls();
    event.Skip();
}

void TextMessagingDialog::OnStationDeselected(wxListEvent& event)
{
    updateSelectionControls();
    event.Skip();
}

void TextMessagingDialog::OnStationLeftDown(wxMouseEvent& event)
{
    long item = stationAt(event);
    if (item >= 0 && m_stationList->GetItemState(item, wxLIST_STATE_SELECTED) != 0)
    {
        // Clicking the selected station again clears it. The press is not
        // passed on, or the list would select it straight back.
        setStationSelected(item, false);
        return;
    }

    event.Skip();
}

void TextMessagingDialog::OnStationRightDown(wxMouseEvent& event)
{
    long item = stationAt(event);
    if (item < 0)
    {
        event.Skip();
        return;
    }

    m_menuCallsign = m_stationList->GetItemText(item).ToStdString();
    bool selected = m_stationList->GetItemState(item, wxLIST_STATE_SELECTED) != 0;

    wxString lastHeard = _("Never heard");
    HeardStation station;
    if (TextMessagingSession::instance().stations().find(m_menuCallsign, station) &&
        station.lastHeard != 0)
    {
        lastHeard = wxString::Format(_("Last heard %s (%s) at %s"), formatTime(station.lastHeard),
                                     formatAge(station.lastHeard, std::time(nullptr)),
                                     formatSnr(station.snr));
    }

    wxMenu menu;
    menu.Append(ID_MENU_SELECT_STATION, selected ? _("Deselect Station") : _("Select Station"));
    menu.Append(ID_MENU_REMOVE_STATION, _("Remove"));
    menu.AppendSeparator();
    menu.Append(ID_MENU_LAST_HEARD, lastHeard)->Enable(false);

    // The press is not passed on: the list would move the selection to the
    // row under the menu, which is the choice the menu is there to offer.
    PopupMenu(&menu);
}

void TextMessagingDialog::OnMenuSelectStation(wxCommandEvent&)
{
    long item = stationItem(m_menuCallsign);
    if (item < 0) return; // aged out while the menu was open

    bool selected = m_stationList->GetItemState(item, wxLIST_STATE_SELECTED) != 0;
    setStationSelected(item, !selected);
}

void TextMessagingDialog::OnMenuRemoveStation(wxCommandEvent&)
{
    if (!TextMessagingSession::instance().stations().remove(m_menuCallsign)) return;

    // The rebuild cannot restore a selection the list no longer holds, so a
    // removed selected station leaves the send button on Broadcast.
    refreshStations();
    setStatus(wxString::Format(_("%s removed from the station list."),
                               wxString::FromUTF8(m_menuCallsign)));
    if (uiLogEnabled()) log_info("UI: station %s removed", m_menuCallsign.c_str());
}

// The message under a point in the chat log's window, as an index into
// m_messages. Each message starts with an anchor named for its index; they
// run down the page in order, so the last one at or above the point is the
// message it falls in. The first message runs from the top of the page:
// the toolkit places an anchor that opens the page at the foot of what
// follows it.
int TextMessagingDialog::messageAt(const wxPoint& point) const
{
    wxHtmlContainerCell* root = m_chatWindow->GetInternalRepresentation();
    if (root == nullptr || m_messages.empty()) return -1;

    int y = m_chatWindow->CalcUnscrolledPosition(point).y;
    auto anchorTop = [root](int index) -> int
    {
        wxString name = wxString::Format("m%d", index);
        const wxHtmlCell* cell = root->Find(wxHTML_COND_ISANCHOR, &name);
        return cell != nullptr ? cell->GetAbsPos().y : -1;
    };

    // A binary search, so a long log costs a handful of lookups.
    int low = 1;
    int high = (int)m_messages.size() - 1;
    int found = 0;
    while (low <= high)
    {
        int middle = (low + high) / 2;
        int top = anchorTop(middle);
        if (top < 0) return -1; // the page is not the one m_messages describes

        if (top <= y)
        {
            found = middle;
            low = middle + 1;
        }
        else
        {
            high = middle - 1;
        }
    }

    return found;
}

// The station a message is with: who sent it to us, or who we sent it to.
// Empty for a broadcast of our own.
std::string TextMessagingDialog::stationOf(const TextMessage& message)
{
    // A ping or pong line names the other station as its destination.
    if (message.kind == MessageKind::System) return message.destCallsign;
    if (message.direction == MessageDirection::Received) return message.originCallsign;

    return message.broadcast ? std::string() : message.destCallsign;
}

// Makes this station the one the send button goes to. A station that has
// aged out of the list is put back in it when the operator asked for it;
// selecting on the operator's behalf never adds one.
void TextMessagingDialog::selectStation(const std::string& callsign, bool addIfMissing)
{
    if (callsign.empty()) return;

    // The station may have been heard a moment ago, ahead of the list being
    // redrawn for it.
    long item = stationItem(callsign);
    if (item < 0) refreshStations();
    item = stationItem(callsign);

    if (item < 0 && addIfMissing)
    {
        TextMessagingSession::instance().stations().pin(callsign);
        refreshStations();
        item = stationItem(callsign);
    }

    if (item < 0) return;

    setStationSelected(item, true);
    m_stationList->EnsureVisible(item);

    if (uiLogEnabled()) log_info("UI: station %s selected from the chat log", callsign.c_str());
}

void TextMessagingDialog::OnChatLeftDown(wxMouseEvent& event)
{
    m_chatPressAt = event.GetPosition();
    event.Skip();
}

// A click on a message chooses the station it is with, to answer it. A
// click on a broadcast of our own changes nothing, and a drag is somebody
// selecting text and is left to the log.
void TextMessagingDialog::OnChatLeftUp(wxMouseEvent& event)
{
    event.Skip();

    wxPoint moved = event.GetPosition() - m_chatPressAt;
    if (std::abs(moved.x) > CLICK_SLOP_PIXELS || std::abs(moved.y) > CLICK_SLOP_PIXELS) return;

    int index = messageAt(event.GetPosition());
    if (index < 0) return;

    selectStation(stationOf(m_messages[(size_t)index]), true);
}

void TextMessagingDialog::OnChatContextMenu(wxContextMenuEvent& event)
{
    // From the keyboard there is no position; the newest message is meant.
    wxPoint screen = event.GetPosition();
    int index = screen == wxDefaultPosition
                    ? (int)m_messages.size() - 1
                    : messageAt(m_chatWindow->ScreenToClient(screen));

    m_menuMessageId = 0;
    TextMessagingProtocol::Cancel cancel = TextMessagingProtocol::Cancel::None;
    if (index >= 0)
    {
        const TextMessage& message = m_messages[(size_t)index];
        // Our own messages and pings; the protocol says which can be stopped.
        if (message.direction == MessageDirection::Sent)
        {
            cancel = TextMessagingSession::instance().protocol().cancelFor(message.id);
            if (cancel != TextMessagingProtocol::Cancel::None) m_menuMessageId = message.id;
        }
    }

    wxMenu menu;
    if (cancel == TextMessagingProtocol::Cancel::Remove)
    {
        menu.Append(ID_MENU_REMOVE_MESSAGE, _("Remove from Queue"));
        menu.AppendSeparator();
    }
    else if (cancel == TextMessagingProtocol::Cancel::Abort)
    {
        menu.Append(ID_MENU_ABORT_MESSAGE, _("Abort"));
        menu.AppendSeparator();
    }
    menu.Append(ID_MENU_CLEAR_MESSAGES, _("Clear Messages"))->Enable(!m_messages.empty());

    PopupMenu(&menu);
}

void TextMessagingDialog::OnMenuCancelMessage(wxCommandEvent&)
{
    if (m_menuMessageId == 0) return;

    bool ping = std::any_of(m_messages.begin(), m_messages.end(), [this](const TextMessage& message)
                            { return message.id == m_menuMessageId && message.kind == MessageKind::System; });

    bool onAir = false;
    auto done = TextMessagingSession::instance().protocol().cancelMessage(m_menuMessageId, &onAir);
    if (done == TextMessagingProtocol::Cancel::None)
    {
        // Delivered, or given up on, while the menu was open.
        setStatus(ping ? _("That ping is no longer waiting to be answered.")
                       : _("That message is no longer waiting to be sent."));
        return;
    }

    // The protocol has forgotten it, so stopping the keying cannot make it
    // take the cut-off burst for a finished one. Anything that rode in the
    // same keying goes with it.
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (onAir && frame != nullptr) frame->chatStopKeying();

    if (done == TextMessagingProtocol::Cancel::Remove)
    {
        setStatus(ping ? _("Ping removed from the queue.") : _("Message removed from the queue."));
    }
    else
    {
        setStatus(ping ? _("Ping aborted.") : _("Message aborted."));
    }
    if (uiLogEnabled())
    {
        log_info("UI: message id=%d %s%s", (int)m_menuMessageId,
                 done == TextMessagingProtocol::Cancel::Remove ? "removed from queue" : "aborted",
                 onAir ? " on the air" : "");
    }
}

// Clears the log for good, here and in the message store. Messages still
// being sent stay, so their chips can still say how they got on.
void TextMessagingDialog::OnMenuClearMessages(wxCommandEvent&)
{
    if (m_messages.empty()) return;

    wxMessageDialog confirm(this, _("Clear every message from the transmission log? "
                                    "Messages still being sent are kept."),
                            _("Clear Messages"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES) return;

    auto& session = TextMessagingSession::instance();
    std::vector<int64_t> keep = session.protocol().outstandingMessageIds();
    if (!session.store().deleteMessagesExcept(keep))
    {
        setStatus(wxString::Format(_("The messages could not be cleared: %s"),
                                   wxString::FromUTF8(session.store().lastError())));
        return;
    }

    m_messages.erase(std::remove_if(m_messages.begin(), m_messages.end(),
                                    [&keep](const TextMessage& message)
                                    { return std::find(keep.begin(), keep.end(), message.id) == keep.end(); }),
                     m_messages.end());
    renderChat();
    setStatus(_("Messages cleared."));

    if (uiLogEnabled()) log_info("UI: transmission log cleared, %d kept", (int)m_messages.size());
}

void TextMessagingDialog::OnAddStationText(wxCommandEvent& event)
{
    bool hasText = !m_txtAddStation->GetValue().empty();
    if (m_btnAddStation->IsEnabled() != hasText) m_btnAddStation->Enable(hasText);
    event.Skip();
}

void TextMessagingDialog::OnAddStation(wxCommandEvent&)
{
    addStation();
}

void TextMessagingDialog::OnAutoReplyToggled(wxCommandEvent& event)
{
    TextMessagingSession::instance().protocol().setAutoReplyEnabled(m_chkAutoReply->IsChecked());

    if (!m_chkAutoReply->IsChecked())
    {
        setStatus(_("This station will no longer transmit on its own."));
    }
    else
    {
        setStatus(wxEmptyString);
    }

    event.Skip();
}

void TextMessagingDialog::OnEntryKeyDown(wxKeyEvent& event)
{
    bool isEnter = event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER;
    if (isEnter && !event.ShiftDown())
    {
        // Enter is the send button by another route, so it is held off where
        // the transmitter may not be used, just as the button is. The text
        // stays put; the window already says why.
        if (!m_transmitControlsDisabled) send(selectedCallsign());
        return;
    }

    event.Skip();
}

void TextMessagingDialog::OnEntryText(wxCommandEvent& event)
{
    updateAirTime();
    event.Skip();
}

void TextMessagingDialog::OnTimer(wxTimerEvent&)
{
    refreshStations();
    updateTransmitControls();
    updateAckWaitStatus();
    updateModemStatus();

    // Auto shift can change the tempo while the text sits there.
    updateAirTime();
}

void TextMessagingDialog::OnBlinkTimer(wxTimerEvent&)
{
    updateEngageChips();
}

// Redraws the chat only when a queued message's chip changes: when the
// console is engaged or disengaged, and each half second while one flashes.
void TextMessagingDialog::updateEngageChips()
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    bool waiting = frame != nullptr && frame->chatWaitsForEngage();
    auto& protocol = TextMessagingSession::instance().protocol();
    bool anyQueued = std::any_of(m_messages.begin(), m_messages.end(), [&](const TextMessage& message)
                                 { return message.direction == MessageDirection::Sent &&
                                          message.status == MessageStatus::Queued &&
                                          protocol.isMessageQueued(message.id); });
    bool lit = waiting && anyQueued && Chaotica::blinkLit();

    bool changed = waiting != m_waitingForEngage || (anyQueued && lit != m_engageChipLit);
    m_waitingForEngage = waiting;
    m_engageChipLit = lit;
    if (changed && anyQueued) renderChat(true);
}

// Counting frames is a handful of integer sums, so this runs on every change
// to the text rather than waiting for typing to stop.
void TextMessagingDialog::updateAirTime()
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());

    // The protocol sends the text trimmed, counted in UTF-8 bytes.
    wxString text = m_txtEntry->GetValue();
    text.Trim(true).Trim(false);
    size_t bytes = text.ToUTF8().length();

    double seconds = frame != nullptr && bytes > 0 ? frame->chatMessageAirSeconds(bytes) : 0.0;
    wxString note;
    wxString tip;
    bool over = false;
    if (seconds > 0.0)
    {
        int whole = (int)std::lround(seconds);
        note = wxString::Format(_("%d:%02d on air"), whole / 60, whole % 60);

        int limit = frame->chatTimeOutSeconds();
        over = seconds > limit;
        if (over)
        {
            tip = wxString::Format(
                _("This message takes %d:%02d to send, longer than the %d s transmit time-out. "
                  "The radio is let up for a moment every few frames to restart the timer; "
                  "a shorter message or a faster tempo avoids that."),
                whole / 60, whole % 60, limit);
        }
    }

    m_btnSend->SetNote(note, over ? Chaotica::Colour::Alarm : Chaotica::Colour::Bone);
    if (tip != m_airTimeToolTip)
    {
        m_airTimeToolTip = tip;
        updateSendToolTip();
    }
}

void TextMessagingDialog::updateSendToolTip()
{
    m_btnSend->SetToolTip(m_airTimeToolTip.empty() ? m_sendToolTip
                                                   : m_sendToolTip + "\n\n" + m_airTimeToolTip);
}

void TextMessagingDialog::updateModemStatus()
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    wxString line = frame != nullptr ? frame->chatModemStatus() : wxString();
    if (line == m_txtModem->text() && m_txtModem->IsShown() == !line.IsEmpty()) return;

    m_txtModem->setText(line);
    m_txtModem->Show(!line.IsEmpty());
    Layout();
}

// More can be queued while a burst is on the air; it goes when the
// transmitter is free. Only a place the station may not send from stops it.
void TextMessagingDialog::updateTransmitControls()
{
    auto& protocol = TextMessagingSession::instance().protocol();

    // Somewhere the station may not send data: say so, on its own line.
    std::string reason = protocol.transmitInhibitedReason();
    if (reason != m_inhibitReason)
    {
        m_inhibitReason = reason;
        m_txtInhibited->setText(
            reason.empty() ? wxString()
                           : wxString::Format(_("Receive only. %s"), wxString::FromUTF8(reason)));
        m_txtInhibited->Show(!reason.empty());
        updateSelectionControls();
        Layout();

        if (uiLogEnabled())
        {
            log_info("UI: %s", reason.empty() ? "transmitting permitted again"
                                              : ("receive only: " + reason).c_str());
        }
    }

    // Whatever was queued is on the air now, so a notice saying it is waiting
    // has become a lie. The chat pane's delivery chip carries on from here.
    bool transmitting = protocol.isTransmitting();
    if (transmitting != m_transmitting)
    {
        m_transmitting = transmitting;
        if (transmitting)
        {
            setStatus(_("Transmitting..."), StatusKind::Activity);
        }
        else if (m_statusKind == StatusKind::Activity)
        {
            setStatus(wxEmptyString);
        }
    }

    bool disabled = !m_inhibitReason.empty();
    if (disabled == m_transmitControlsDisabled) return;

    m_transmitControlsDisabled = disabled;
    m_btnSend->Enable(!disabled);

    if (uiLogEnabled())
    {
        log_info("UI: send button %s", disabled ? "disabled, receive only" : "enabled");
    }
}

void TextMessagingDialog::OnClose(wxCloseEvent&)
{
    // Chat keeps running with the window closed, so this only hides it.
    if (uiLogEnabled()) log_info("UI: chat window hidden, observer still registered");
    Hide();
}

void TextMessagingDialog::onMessageAdded(const TextMessage& message)
{
    TextMessage copy = message;
    CallAfter([this, copy]()
    {
        appendMessage(copy);
        renderChat();

        // Somebody calling us is somebody to answer, so with nobody else
        // chosen they become who the send button goes to. A station already
        // chosen is left alone: the operator may be in the middle of a QSO.
        bool callingUs = copy.kind == MessageKind::Chat &&
                         copy.direction == MessageDirection::Received && !copy.broadcast &&
                         !copy.destCallsign.empty() &&
                         copy.destCallsign == TextMessagingSession::instance().protocol().myCallsign();
        if (callingUs && selectedCallsign().empty()) selectStation(copy.originCallsign, false);

        if (uiLogEnabled())
        {
            log_info("UI: added id=%d %s %s", (int)copy.id,
                     copy.direction == MessageDirection::Sent ? "TX" : "RX",
                     (const char*)wxString(copy.text).Left(40).ToUTF8());
        }
    });
}

void TextMessagingDialog::onMessageUpdated(const TextMessage& message)
{
    TextMessage copy = message;
    CallAfter([this, copy]()
    {
        for (TextMessage& existing : m_messages)
        {
            if (existing.id != copy.id) continue;

            existing = copy;
            renderChat();

            // A broadcast has no acknowledgement coming, so its own completion
            // is the last thing the status line can usefully report.
            if (copy.broadcast && copy.status == MessageStatus::Sent)
            {
                setStatus(_("Broadcast sent."));
            }

            if (uiLogEnabled())
            {
                log_info("UI: chip id=%d now \"%s\"", (int)copy.id,
                         (const char*)deliveryChip(copy).label.ToUTF8());
            }
            return;
        }

        // The message is not on screen, so the chip the operator sees is now
        // stale. This is the silent failure to look for when a status change
        // never appears in the window.
        if (uiLogEnabled())
        {
            log_warn("UI: update for id=%d dropped, message is not in the view",
                     (int)copy.id);
        }
    });
}

void TextMessagingDialog::onStationsChanged()
{
    CallAfter([this]() { refreshStations(); });
}
