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
#include <chrono>
#include <cmath>
#include <cstdlib>

#include <memory>

#include <wx/datetime.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/filesys.h>
#include <wx/fs_mem.h>
#include <wx/graphics.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>

#include "main.h"
#include "gui/glissando/ChaoticaControls.h"
#include "gui/glissando/ChaoticaTheme.h"
#include "gui/glissando/GlissandoConsole.h"
#include "text_messaging/Data2GLink.h"
#include "text_messaging/DeliveryChip.h"
#include "text_messaging/FrameCodec.h"
#include "text_messaging/HamText.h"
#include "text_messaging/HeardStationList.h"
#include "text_messaging/MessageStore.h"
#include "text_messaging/TextMessagingSession.h"

using namespace TextMessaging;

namespace
{

// How often the station list ages out and the "last heard" column is redrawn.
constexpr int REFRESH_INTERVAL_MS = 1000;

// Often enough to follow the shared blink, which changes every half second.
// A queued message's countdown bar moves on the same timer.
constexpr int BLINK_INTERVAL_MS = 125;

// The countdown bar on a queued message's chip, in pixels before scaling.
constexpr int QUEUE_BAR_WIDTH = 112;

// Past this a file is sent only once the operator has agreed to the time
// it will take on the air.
constexpr uint64_t LARGE_FILE_BYTES = 100 * 1000;

// File lines kept in the chat; past this the oldest finished ones go.
constexpr size_t FILE_LINES_KEPT = 100;

// A finished transfer stays in the status area this long, saying how it
// ended.
constexpr double FINISHED_SHOWN_SECONDS = 30.0;

// The offer box's red, dark and lit, and its border's.
const wxColour OFFER_BACKGROUND(58, 21, 19);
const wxColour OFFER_BORDER_DARK(94, 37, 34);

double steadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
    ID_MENU_WOAH,
    ID_MENU_RESEND,
    ID_MENU_TEMPO,     // back to the tempo the console picks
    ID_MENU_TEMPO_LAST = ID_MENU_TEMPO + Glissando::MAX_GEAR, // ID_MENU_TEMPO + each gear
    ID_MENU_SEND_FILE,
    ID_MENU_SEND_FILE_WHY,
    ID_MENU_CANCEL_TRANSFER,
    ID_MENU_SEND_GROUP_FILE,
    ID_MENU_SEND_GROUP_FILE_WHY,
    ID_MENU_STOP_SERVING,
    ID_MENU_CANCEL_GROUP_FILE,
    ID_MENU_RECEIVE_GROUP_FILE,
    ID_MENU_IGNORE_GROUP_FILE,
    ID_OFFER_SAVE,
    ID_OFFER_DECLINE,
    ID_STATUS_CANCEL,
    ID_STATUS_RECEIVE,
    ID_DISCONNECT,
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

// The same for text already decoded, such as a file's name in UTF-8.
wxString escapeHtml(const wxString& text)
{
    wxString result;
    result.reserve(text.length() + 16);
    for (wxUniChar c : text)
    {
        if (c == '&') result += "&amp;";
        else if (c == '<') result += "&lt;";
        else if (c == '>') result += "&gt;";
        else if (c == '"') result += "&quot;";
        else result += c;
    }
    return result;
}

// 7,000: the way the chat counts the bytes of a file.
wxString groupDigits(uint64_t value)
{
    std::string digits = std::to_string(value);
    std::string grouped;
    for (size_t i = 0; i < digits.size(); i++)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0) grouped.push_back(',');
        grouped.push_back(digits[i]);
    }
    return wxString(grouped);
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
        case DeliveryChipKind::Held:
            // Handed to data2g-host, which holds the group while a session
            // is up: it goes when the session idles or ends.
            chip.label = _("HELD") + wxString::FromUTF8(" \u00B7 ") +
                         wxString::Format(_("session with %s"), wxString::FromUTF8(state.heldBy));
            chip.background = smoke;
            chip.foreground = "#E0A060";
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
    Connect(ID_MENU_WOAH, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TextMessagingDialog::OnMenuWoah));
    Connect(ID_MENU_RESEND, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TextMessagingDialog::OnMenuResend));
    Connect(ID_MENU_TEMPO, ID_MENU_TEMPO_LAST, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuTempo));
    Connect(ID_MENU_SEND_FILE, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TextMessagingDialog::OnMenuSendFile));
    Connect(ID_MENU_SEND_GROUP_FILE, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuSendGroupFile));
    Connect(ID_MENU_STOP_SERVING, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuStopServing));
    Connect(ID_MENU_CANCEL_GROUP_FILE, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuCancelGroupFile));
    Connect(ID_MENU_RECEIVE_GROUP_FILE, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuReceiveGroupFile));
    Connect(ID_MENU_IGNORE_GROUP_FILE, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuIgnoreGroupFile));
    Connect(ID_MENU_CANCEL_TRANSFER, wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(TextMessagingDialog::OnMenuCancelTransfer));
    Connect(ID_OFFER_SAVE, wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TextMessagingDialog::OnOfferSave));
    Connect(ID_OFFER_DECLINE, wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TextMessagingDialog::OnOfferDecline));
    Connect(ID_STATUS_CANCEL, wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TextMessagingDialog::OnStatusCancel));
    Connect(ID_STATUS_RECEIVE, wxEVT_COMMAND_BUTTON_CLICKED,
            wxCommandEventHandler(TextMessagingDialog::OnStatusReceive));
    Connect(ID_DISCONNECT, wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TextMessagingDialog::OnDisconnect));
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
    m_chatWindow->Bind(wxEVT_LEFT_DCLICK, &TextMessagingDialog::OnChatDoubleClick, this);
    m_chatWindow->Bind(wxEVT_CONTEXT_MENU, &TextMessagingDialog::OnChatContextMenu, this);
    // The links on a group file's line: Receive... and Ignore.
    m_chatWindow->Bind(wxEVT_HTML_LINK_CLICKED, &TextMessagingDialog::OnChatLink, this);

    // The countdown bars are pictures the chat page loads from memory.
    static bool memoryFiles = false;
    if (!memoryFiles)
    {
        wxFileSystem::AddHandler(new wxMemoryFSHandler);
        memoryFiles = true;
    }

    TextMessagingSession::instance().protocol().setObserver(this);
    m_refreshTimer.Start(REFRESH_INTERVAL_MS);
    m_blinkTimer.Start(BLINK_INTERVAL_MS);

    if (uiLogEnabled()) log_info("UI: chat window created, observer registered");
}

TextMessagingDialog::~TextMessagingDialog()
{
    m_refreshTimer.Stop();
    m_blinkTimer.Stop();
    for (const wxString& name : m_queueBarImages) wxMemoryFSHandler::RemoveFile(name);
    TextMessagingSession::instance().protocol().setObserver(nullptr);

    m_txtEntry->Disconnect(wxEVT_KEY_DOWN, wxKeyEventHandler(TextMessagingDialog::OnEntryKeyDown),
                           nullptr, this);
    connectStationMouse(false);
    m_chatWindow->Unbind(wxEVT_LEFT_DCLICK, &TextMessagingDialog::OnChatDoubleClick, this);
    m_chatWindow->Unbind(wxEVT_CONTEXT_MENU, &TextMessagingDialog::OnChatContextMenu, this);
    m_chatWindow->Unbind(wxEVT_HTML_LINK_CLICKED, &TextMessagingDialog::OnChatLink, this);
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

// The transfer under way, at the top of the COMMS plate: who and what on
// the first line with what it is doing beside it, a bar drawn the
// console's way, and the figures under it, with Cancel (and, for a group
// file not yet answered, Receive...) beside the bar.
class TransferStatusArea : public wxPanel
{
public:
    struct View
    {
        wxString who;
        wxString stage;
        wxString meta;
        wxString more;                  // "+2 more"
        bool group = false;
        double handed = 0.0;            // a session's: handed to the modem, 0 to 1
        double acked = 0.0;             // and acknowledged (or, received, written)
        std::vector<uint8_t> pieces;    // a group file's: 0 not yet, 1 held, 2 missed
        bool failed = false;

        bool operator==(const View&) const = default;
    };

    static constexpr int HEIGHT = 74;

    TransferStatusArea(wxWindow* parent, wxWindowID cancelId, wxWindowID receiveId)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, HEIGHT))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, HEIGHT));
        cancel_ = new Chaotica::Button(this, cancelId, _("Cancel"), false, wxSize(84, 28));
        cancel_->SetBackdrop(BACKGROUND);
        receive_ = new Chaotica::Button(this, receiveId, _("Receive..."), false, wxSize(100, 28));
        receive_->SetBackdrop(BACKGROUND);
        receive_->Hide();
        Bind(wxEVT_PAINT, &TransferStatusArea::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            place();
            Refresh();
            event.Skip();
        });
    }

    void setView(const View& view, const wxString& cancelLabel, bool canCancel, bool canReceive)
    {
        bool buttons = cancel_->IsShown() != canCancel || receive_->IsShown() != canReceive ||
                       cancel_->GetLabel() != cancelLabel;
        if (!buttons && view == view_) return;
        view_ = view;
        cancel_->SetLabel(cancelLabel);
        cancel_->Show(canCancel);
        receive_->Show(canReceive);
        place();
        Refresh();
    }

private:
    inline static const wxColour BACKGROUND{11, 12, 13};

    // The buttons at the right of the bar's row.
    int buttonsWidth() const
    {
        int width = 0;
        if (cancel_->IsShown()) width += cancel_->GetSize().x + 6;
        if (receive_->IsShown()) width += receive_->GetSize().x + 6;
        return width;
    }

    void place()
    {
        wxSize size = GetClientSize();
        int x = size.x - 8;
        int y = BAR_TOP + BAR_HEIGHT / 2;
        if (cancel_->IsShown())
        {
            x -= cancel_->GetSize().x;
            cancel_->Move(x, y - cancel_->GetSize().y / 2);
            x -= 6;
        }
        if (receive_->IsShown())
        {
            x -= receive_->GetSize().x;
            receive_->Move(x, y - receive_->GetSize().y / 2);
        }
    }

    static constexpr int BAR_TOP = 28;
    static constexpr int BAR_HEIGHT = 14;

    void OnPaint(wxPaintEvent&)
    {
        namespace Colour = Chaotica::Colour;
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(Colour::Plate));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
        if (!gc) return;

        wxSize size = GetClientSize();
        gc->SetPen(wxPen(wxColour(60, 64, 67), 1));
        gc->SetBrush(wxBrush(BACKGROUND));
        gc->DrawRoundedRectangle(0.5, 0.5, size.x - 1, size.y - 1, 4);

        // Who and what, and beside it what it is doing, and how many more.
        wxFont bold = Chaotica::font(Chaotica::FontRole::Button);
        bold.SetPointSize(10);
        wxFont plain = bold;
        plain.SetWeight(wxFONTWEIGHT_NORMAL);
        wxString stage = view_.stage;
        if (!view_.more.empty()) stage += wxString::FromUTF8("  \xC2\xB7  ") + view_.more;
        gc->SetFont(plain, view_.failed ? Colour::Alarm : Colour::Dim);
        double sw = 0, sh = 0;
        gc->GetTextExtent(stage, &sw, &sh);
        double right = size.x - 10;
        double stageX = std::max(size.x * 0.45, right - sw);
        gc->Clip(stageX, 0, right - stageX, BAR_TOP);
        gc->DrawText(stage, std::max(stageX, right - sw), 6);
        gc->ResetClip();
        gc->SetFont(bold, Colour::Bone);
        gc->Clip(10, 0, std::max(0.0, stageX - 22), BAR_TOP);
        gc->DrawText(view_.who, 10, 6);
        gc->ResetClip();

        // The bar: a session's handed over in grey under what is
        // acknowledged in bone; a group file's one segment per piece.
        double bx = 10, bw = std::max(20, size.x - 20 - buttonsWidth());
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(22, 24, 26)));
        gc->DrawRectangle(bx, BAR_TOP, bw, BAR_HEIGHT);
        if (!view_.group)
        {
            gc->SetBrush(wxBrush(wxColour(107, 111, 109)));
            gc->DrawRectangle(bx, BAR_TOP + 2, bw * std::clamp(view_.handed, 0.0, 1.0), BAR_HEIGHT - 4);
            gc->SetBrush(wxBrush(view_.failed ? wxColour(150, 52, 46) : Colour::Bone));
            gc->DrawRectangle(bx, BAR_TOP + 2, bw * std::clamp(view_.acked, 0.0, 1.0), BAR_HEIGHT - 4);
        }
        else
        {
            size_t n = view_.pieces.size();
            for (size_t i = 0; i < n; i++)
            {
                double a = std::floor(bx + i * bw / n), b = std::floor(bx + (i + 1) * bw / n);
                double gap = b - a >= 4 ? 2 : b - a >= 3 ? 1 : 0;
                uint8_t mark = view_.pieces[i];
                gc->SetBrush(wxBrush(mark == 1   ? Colour::Bone
                                     : mark == 2 ? wxColour(94, 37, 34)
                                                 : wxColour(42, 45, 48)));
                gc->DrawRectangle(a + gap / 2, BAR_TOP + 2, std::max(1.0, b - a - gap), BAR_HEIGHT - 4);
            }
        }

        // The figures.
        gc->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)), Colour::Dim);
        gc->Clip(10, BAR_TOP + BAR_HEIGHT, size.x - 20, size.y - BAR_TOP - BAR_HEIGHT);
        gc->DrawText(view_.meta, 10, BAR_TOP + BAR_HEIGHT + 9);
        gc->ResetClip();
    }

    View view_;
    Chaotica::Button* cancel_;
    Chaotica::Button* receive_;
};

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

    // A Data2G session: lit while one is connected, with the far end named
    // under it, and the one way to end it without being asked.
    m_stationSizer = stationSizer;
    m_sessionSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* sessionRow = new wxBoxSizer(wxHORIZONTAL);
    m_sessionLamp = new Chaotica::Lamp(stationPlate, _("CONNECTED"), wxSize(140, 30));
    m_sessionLamp->SetLit(true);
    sessionRow->Add(m_sessionLamp, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    Button* disconnect = new Button(stationPlate, ID_DISCONNECT, _("Disconnect"), false, wxSize(110, 30));
    disconnect->SetToolTip(_("End the Data2G session now. Anything not yet acknowledged in it is dropped."));
    sessionRow->Add(disconnect, 0, wxALIGN_CENTER_VERTICAL);
    m_sessionSizer->Add(sessionRow, 0, wxEXPAND);
    m_sessionText = new WrappingText(stationPlate);
    m_sessionText->SetForegroundColour(Colour::Bone);
    m_sessionSizer->Add(m_sessionText, 0, wxEXPAND | wxTOP, 2);
    stationSizer->Add(m_sessionSizer, 0, wxEXPAND | wxBOTTOM, 6);
    stationSizer->Show(m_sessionSizer, false, true);

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

    Panel* logPlate = new Panel(this, _("Comms"));

    // A file offered to us: who offers what, how long the offer has left,
    // and the two answers. Shown over the chat only while an offer waits,
    // red, with its border flashing on the console's blink.
    m_offerBox = new wxPanel(logPlate);
    m_offerBox->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_offerBox->SetBackgroundColour(OFFER_BACKGROUND);
    m_offerBox->Bind(wxEVT_PAINT, &TextMessagingDialog::paintOfferBox, this);
    wxBoxSizer* offerSizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* offerWords = new wxBoxSizer(wxVERTICAL);
    m_offerText = new WrappingText(m_offerBox);
    m_offerText->SetForegroundColour(Colour::Bone);
    m_offerText->SetBackgroundColour(OFFER_BACKGROUND);
    offerWords->Add(m_offerText, 0, wxEXPAND);
    m_offerCountdown = new wxStaticText(m_offerBox, wxID_ANY, wxEmptyString);
    m_offerCountdown->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)));
    m_offerCountdown->SetForegroundColour(wxColour(255, 179, 173));
    m_offerCountdown->SetBackgroundColour(OFFER_BACKGROUND);
    offerWords->Add(m_offerCountdown, 0, wxTOP, 2);
    offerSizer->Add(offerWords, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 10);
    Button* offerSave = new Button(m_offerBox, ID_OFFER_SAVE, _("Save as..."), false, wxSize(110, 30));
    offerSave->SetToolTip(_("Choose where to save the file; it is then sent. Nothing received is opened or run."));
    offerSave->SetBackdrop(OFFER_BACKGROUND);
    offerSizer->Add(offerSave, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, 8);
    Button* offerDecline = new Button(m_offerBox, ID_OFFER_DECLINE, _("Decline"), false, wxSize(90, 30));
    offerDecline->SetBackdrop(OFFER_BACKGROUND);
    offerSizer->Add(offerDecline, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, 8);
    m_offerBox->SetSizer(offerSizer);
    m_offerBox->Hide();
    logPlate->GetContentSizer()->Add(m_offerBox, 0, wxEXPAND | wxBOTTOM, 6);

    // The transfer under way, while one is, and a while after it ends.
    m_transferStatus = new TransferStatusArea(logPlate, ID_STATUS_CANCEL, ID_STATUS_RECEIVE);
    m_transferStatus->Hide();
    logPlate->GetContentSizer()->Add(m_transferStatus, 0, wxEXPAND | wxBOTTOM, 6);

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
                                wxTE_MULTILINE | wxTE_RICH2 | wxBORDER_SIMPLE);
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
        _("When lit, this station transmits on its own to confirm messages and answer pings. "
          "When dark, its messages and pings say so, and other stations send to it once without retrying. "
          "In a Data2G session the modem confirms everything itself, so this makes no difference there."));
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

    m_queueBarGeneration++;
    m_newQueueBarImages.clear();

    wxString html;
    html.reserve(4096);
    html += "<html><body bgcolor=\"" + colors.page + "\" text=\"" + colors.text + "\">";

    // Each file has a line when it starts and another once it has ended.
    struct FileRow
    {
        std::time_t at;
        size_t index;
        bool result;
    };
    std::vector<FileRow> fileRows;
    for (size_t i = 0; i < m_fileLines.size(); i++)
    {
        fileRows.push_back({m_fileLines[i].at, i, false});
        if (m_fileLines[i].endedAt != 0) fileRows.push_back({m_fileLines[i].endedAt, i, true});
    }
    std::stable_sort(fileRows.begin(), fileRows.end(),
                     [](const FileRow& a, const FileRow& b) { return a.at < b.at; });

    // Messages and file lines, each in the order it came, a file line ahead
    // of the messages that came after it.
    m_rows.clear();
    for (size_t index = 0, file = 0; index < m_messages.size() || file < fileRows.size();)
    {
        bool fileFirst = file < fileRows.size() &&
                         (index == m_messages.size() || fileRows[file].at < m_messages[index].timestamp);
        m_rows.push_back({fileFirst, fileFirst ? fileRows[file].index : index, fileFirst && fileRows[file].result});

        // Marks where each line starts, so a click can be traced back to
        // it: see rowAt(). Ahead of the line's table rather than in it,
        // where the page would place it at the line's foot.
        html += wxString::Format("<a name=\"m%d\"></a>", (int)m_rows.size() - 1);

        if (fileFirst)
        {
            const FileRow& row = fileRows[file++];
            const FileLine& line = m_fileLines[row.index];
            html += line.group ? groupLineHtml(line, colors, row.result) : fileLineHtml(line, colors, row.result);
            continue;
        }

        const TextMessage& message = m_messages[index++];

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
            if (!waits && m_queueBars.count(message.id) != 0)
            {
                right = queueBarChip(message);
            }
            else if (message.status == MessageStatus::Transmitting && message.kind == MessageKind::Chat &&
                     message.heldBy.empty() && m_sendFillPixels >= 0)
            {
                // On the air: SENDING, or RETRY or RESEND, over a bar that
                // fills as it goes out.
                right = barChip(message.id, deliveryChip(message).label, m_sendFillPixels,
                                wxColour(0xFF, 0xFC, 0xF0));
            }
            else
            {
                right = statusChip(message, waits, m_engageChipLit);
            }
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

    // The page has loaded its pictures; the last page's are done with.
    for (const wxString& name : m_queueBarImages) wxMemoryFSHandler::RemoveFile(name);
    m_queueBarImages.swap(m_newQueueBarImages);
    m_newQueueBarImages.clear();

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
    // The far end of a Data2G session stays listed while the session lasts,
    // however quiet it is.
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    std::string peer = frame != nullptr ? frame->chatSessionPeer() : std::string();
    std::string keep;
    if (!peer.empty())
    {
        for (const HeardStation& station : session.stations().stations())
        {
            if (Data2G::commandCallsign(station.callsign) != peer) continue;
            keep = station.callsign;
            break;
        }
    }
    session.stations().prune(now, keep);

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
// the selection is how the operator reaches everybody. The console's map
// follows it too: the path to the station picked, and none once the pick
// is cleared.
void TextMessagingDialog::updateSelectionControls()
{
    if (m_restoringSelection) return;

    std::string callsign = selectedCallsign();
    bool selected = !callsign.empty();

    // A change nothing here asked about (the keyboard, say) that would let
    // go of a station in a session, or with files going: the selection is
    // put back, and the operator asked.
    // Put back only once the list has finished: put back from in here,
    // wxGTK's list goes on to light the row it moved to as well, and two
    // rows show selected.
    if (callsign != m_mapPick && !m_lettingGo && !m_mapPick.empty() && needsAskingToLetGo(m_mapPick) &&
        stationItem(m_mapPick) >= 0)
    {
        askToLetGoLater(m_mapPick, callsign, true);
        return;
    }

    if (callsign != m_mapPick)
    {
        std::string released = m_mapPick;
        m_mapPick = callsign;
        TextMessagingSession::instance().protocol().setMapSelection(callsign);
        if (uiLogEnabled()) log_info("UI: map follows \"%s\"", callsign.c_str());

        // Letting go of a station ends a Data2G session with it, so the
        // group is free again, and drops what is still outstanding for it.
        // Over any other modem this changes nothing.
        int dropped = released.empty() ? 0 : TextMessagingSession::instance().protocol().releaseStation(released);
        if (dropped > 0)
        {
            setStatus(wxString::Format(wxPLURAL("%d message to %s aborted.", "%d messages to %s aborted.", dropped),
                                       dropped, wxString::FromUTF8(released)));
            if (uiLogEnabled()) log_info("UI: %s deselected, %d outstanding dropped", released.c_str(), dropped);
        }
    }

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
    if (queueText(m_txtEntry->GetValue().ToStdString(), destination)) m_txtEntry->Clear();
}

// Queues a message and says so, or why not.
bool TextMessagingDialog::queueText(const std::string& text, const std::string& destination)
{
    std::string error;
    if (!TextMessagingSession::instance().protocol().sendMessage(text, destination, error))
    {
        setStatus(wxString::FromUTF8(error));
        return false;
    }

    setStatus(destination.empty()
                  ? _("Broadcast queued.")
                  : wxString::Format(_("Message to %s queued."), wxString::FromUTF8(destination)),
              StatusKind::Queued);
    return true;
}

// The same text to the same station, or to everybody, as a new message at
// the back of the queue.
void TextMessagingDialog::OnMenuResend(wxCommandEvent&)
{
    if (m_menuResend.id == 0) return;

    std::string destination = m_menuResend.broadcast ? std::string() : m_menuResend.destCallsign;
    bool queued = queueText(m_menuResend.text, destination);
    if (uiLogEnabled())
    {
        log_info("UI: message id=%d re-sent%s", (int)m_menuResend.id, queued ? "" : ", refused");
    }
    updateTransmitControls();
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
    if (item < 0)
    {
        event.Skip();
        return;
    }

    // Clicking the selected station again clears it; clicking another
    // chooses that one. Either lets go of the station selected, which is
    // asked about first while that ends a session or cancels files, and
    // then the press is not passed on: the list would change the selection
    // under the question.
    bool selected = m_stationList->GetItemState(item, wxLIST_STATE_SELECTED) != 0;
    std::string wanted = selected ? std::string() : m_stationList->GetItemText(item).ToStdString();
    std::string before = selectedCallsign();
    if (!before.empty() && before != wanted && needsAskingToLetGo(before))
    {
        askToLetGoLater(before, wanted);
        return;
    }

    if (selected)
    {
        // Not passed on, or the list would select it straight back.
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

    // Files go only through a Data2G session; while one can't be had, the
    // item stays dark and the line under it says why.
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    wxString why = _("Files go through a Data2G session.");
    bool files = frame != nullptr && frame->chatCanSendFiles(why);
    wxMenuItem* sendFile = menu.Append(ID_MENU_SEND_FILE, _("Send File..."));
    sendFile->Enable(files && m_inhibitReason.empty());
    if (!files)
    {
        sendFile->SetHelp(why);
        menu.Append(ID_MENU_SEND_FILE_WHY, why)->Enable(false);
    }
    else if (!m_inhibitReason.empty())
    {
        menu.Append(ID_MENU_SEND_FILE_WHY, _("Receive only here."))->Enable(false);
    }
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
    std::string before = selectedCallsign();
    std::string wanted = selected ? std::string() : m_menuCallsign;
    if (!before.empty() && before != wanted && !askToLetGo(before, wanted, _("Choose Station"))) return;

    item = stationItem(m_menuCallsign);
    if (item < 0) return;
    m_lettingGo = true;
    setStationSelected(item, !selected);
    m_lettingGo = false;
}

void TextMessagingDialog::OnMenuRemoveStation(wxCommandEvent&)
{
    // Removing the selected station lets go of it.
    bool selected = m_menuCallsign == selectedCallsign();
    if (selected && !askToLetGo(m_menuCallsign, std::string(), _("Remove Station"))) return;
    if (!TextMessagingSession::instance().stations().remove(m_menuCallsign)) return;

    // The rebuild cannot restore a selection the list no longer holds, so a
    // removed selected station leaves the send button on Broadcast.
    m_lettingGo = true;
    refreshStations();
    m_lettingGo = false;
    setStatus(wxString::Format(_("%s removed from the station list."),
                               wxString::FromUTF8(m_menuCallsign)));
    if (uiLogEnabled()) log_info("UI: station %s removed", m_menuCallsign.c_str());
}

// The line under a point in the chat log's window, as an index into
// m_rows. Each line starts with an anchor named for its index; they run
// down the page in order, so the last one at or above the point is the
// line it falls in. The first line runs from the top of the page: the
// toolkit places an anchor that opens the page at the foot of what
// follows it.
int TextMessagingDialog::rowAt(const wxPoint& point) const
{
    wxHtmlContainerCell* root = m_chatWindow->GetInternalRepresentation();
    if (root == nullptr || m_rows.empty()) return -1;

    int y = m_chatWindow->CalcUnscrolledPosition(point).y;
    auto anchorTop = [root](int index) -> int
    {
        wxString name = wxString::Format("m%d", index);
        const wxHtmlCell* cell = root->Find(wxHTML_COND_ISANCHOR, &name);
        return cell != nullptr ? cell->GetAbsPos().y : -1;
    };

    // A binary search, so a long log costs a handful of lookups.
    int low = 1;
    int high = (int)m_rows.size() - 1;
    int found = 0;
    while (low <= high)
    {
        int middle = (low + high) / 2;
        int top = anchorTop(middle);
        if (top < 0) return -1; // the page is not the one m_rows describes

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

// A double click on a message chooses the station it is with, to answer it.
// One on a broadcast of our own changes nothing. A single click only places
// the caret or starts selecting text: choosing a station lets go of the one
// before, which can end a session, so it takes more than a stray click.
void TextMessagingDialog::OnChatDoubleClick(wxMouseEvent& event)
{
    event.Skip();

    int row = rowAt(event.GetPosition());
    if (row < 0) return;

    // A file line is with the station it goes to or comes from; one for
    // the group is with nobody, and its links are clicked instead.
    const ChatRow& line = m_rows[(size_t)row];
    if (line.file && m_fileLines[line.index].group) return;
    std::string callsign = line.file ? m_fileLines[line.index].transfer.peer : stationOf(m_messages[line.index]);
    if (callsign.empty()) return;

    std::string before = selectedCallsign();
    if (!before.empty() && before != callsign && needsAskingToLetGo(before))
    {
        askToLetGoLater(before, callsign);
        return;
    }
    selectStation(callsign, true);
}

// A session with the station, or files going to or from it.
bool TextMessagingDialog::needsAskingToLetGo(const std::string& before) const
{
    return !before.empty() && (sessionWith(before) || liveFilesWith(before) > 0);
}

// One question, whichever of the two it is about, with No the default.
bool TextMessagingDialog::askToLetGo(const std::string& before, const std::string& wanted, const wxString& title)
{
    bool session = sessionWith(before);
    int live = liveFilesWith(before);
    if (!session && live == 0) return true;

    wxString was = wxString::FromUTF8(before);
    wxString will = wxString::FromUTF8(wanted);
    wxString text;
    if (session)
    {
        text = wxString::Format(_("End the session with %s?"), was);
        text += "\n\n";
        text += wanted.empty() ? wxString::Format(_("Letting go of %s ends it."), was)
                               : wxString::Format(_("Choosing %s ends it."), will);
        if (live > 0)
        {
            text += " ";
            text += wxString::Format(wxPLURAL("%d file going to or from %s is cancelled.",
                                              "%d files going to or from %s are cancelled.", live),
                                     live, was);
        }
    }
    else if (wanted.empty())
    {
        text = wxString::Format(wxPLURAL("%d file is going to or from %s. Letting go of %s cancels it. Go ahead?",
                                         "%d files are going to or from %s. Letting go of %s cancels them. Go ahead?",
                                         live),
                                live, was, was);
    }
    else
    {
        text = wxString::Format(wxPLURAL("%d file is going to or from %s. Choosing %s lets go of %s, which "
                                         "cancels it. Go ahead?",
                                         "%d files are going to or from %s. Choosing %s lets go of %s, which "
                                         "cancels them. Go ahead?",
                                         live),
                                live, was, will, was);
    }

    wxMessageDialog confirm(this, text, title, wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
    bool yes = confirm.ShowModal() == wxID_YES;
    if (uiLogEnabled()) log_info("UI: let go of %s for \"%s\"? %s", before.c_str(), wanted.c_str(), yes ? "yes" : "no");
    return yes;
}

void TextMessagingDialog::askToLetGoLater(const std::string& before, const std::string& wanted, bool putBack)
{
    // One question at a time; a click while it is on its way says what
    // the operator wants.
    if (!putBack)
    {
        m_letGoClicked = true;
        m_letGoWanted = wanted;
    }
    if (m_letGoAsked) return;
    m_letGoAsked = true;
    CallAfter([this, before]() {
        m_letGoAsked = false;
        bool clicked = m_letGoClicked;
        std::string chosen = m_letGoWanted;
        m_letGoClicked = false;
        m_letGoWanted.clear();

        long keep = stationItem(before);
        if (keep < 0 || m_mapPick != before)
        {
            // Gone from the list, or let go of meanwhile.
            updateSelectionControls();
            return;
        }

        // Whatever the list moved the selection to is put back, and that is
        // the station the question is about.
        bool moved = false;
        m_restoringSelection = true;
        for (long item = 0; item < m_stationList->GetItemCount(); item++)
        {
            if (item == keep || m_stationList->GetItemState(item, wxLIST_STATE_SELECTED) == 0) continue;
            if (!moved) chosen = m_stationList->GetItemText(item).ToStdString();
            moved = true;
            m_stationList->SetItemState(item, 0, wxLIST_STATE_SELECTED);
        }
        if (m_stationList->GetItemState(keep, wxLIST_STATE_SELECTED) == 0)
        {
            if (!moved) chosen.clear();
            moved = true;
            m_stationList->SetItemState(keep, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
        m_restoringSelection = false;
        if (!moved && !clicked) return; // the list came back by itself

        if (askToLetGo(before, chosen, _("Choose Station"))) chooseStation(before, chosen);
    });
}

// The operator has agreed: the selection moves to wanted, or is cleared.
void TextMessagingDialog::chooseStation(const std::string& before, const std::string& wanted)
{
    m_lettingGo = true;
    if (wanted.empty())
    {
        long item = stationItem(before);
        if (item >= 0) setStationSelected(item, false);
    }
    else
    {
        selectStation(wanted, true);
    }
    m_lettingGo = false;
}

std::string TextMessagingDialog::sessionStation() const
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    std::string peer = frame != nullptr ? frame->chatSessionPeer() : std::string();
    if (peer.empty()) return peer;
    for (long item = 0; item < m_stationList->GetItemCount(); item++)
    {
        std::string callsign = m_stationList->GetItemText(item).ToStdString();
        if (Data2G::commandCallsign(callsign) == peer) return callsign;
    }
    return peer;
}

bool TextMessagingDialog::sessionWith(const std::string& callsign) const
{
    if (callsign.empty()) return false;
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    std::string peer = frame != nullptr ? frame->chatSessionPeer() : std::string();
    return !peer.empty() && Data2G::commandCallsign(callsign) == peer;
}

// The lamp and Disconnect show only while a session is connected, which
// never happens with Data2G off.
void TextMessagingDialog::updateSessionPlate()
{
    std::string station = sessionStation();
    if (station == m_sessionShown) return;
    m_sessionShown = station;

    bool up = !station.empty();
    if (up)
    {
        m_sessionText->setText(wxString::Format(_("Session with %s"), wxString::FromUTF8(station)));
        m_sessionLamp->SetToolTip(wxString::Format(_("A Data2G session with %s is connected. Choosing another "
                                                     "station, or none, ends it; you are asked first."),
                                                   wxString::FromUTF8(station)));
    }
    m_stationSizer->Show(m_sessionSizer, up, true);
    Layout();
    if (uiLogEnabled()) log_info("UI: session plate %s", up ? station.c_str() : "hidden");
}

// Ends the session without asking: the button says what it does.
void TextMessagingDialog::OnDisconnect(wxCommandEvent&)
{
    std::string station = sessionStation();
    if (station.empty()) return;

    long item = stationItem(station);
    int dropped = 0;
    if (item >= 0 && m_stationList->GetItemState(item, wxLIST_STATE_SELECTED) != 0)
    {
        // Letting go of the selected station is what ends its session.
        m_lettingGo = true;
        setStationSelected(item, false);
        m_lettingGo = false;
    }
    else
    {
        dropped = TextMessagingSession::instance().protocol().releaseStation(station);
    }
    setStatus(dropped > 0 ? wxString::Format(wxPLURAL("Ending the session with %s; %d message aborted.",
                                                      "Ending the session with %s; %d messages aborted.", dropped),
                                             wxString::FromUTF8(station), dropped)
                          : wxString::Format(_("Ending the session with %s."), wxString::FromUTF8(station)));
    if (uiLogEnabled()) log_info("UI: Disconnect %s", station.c_str());
}

int TextMessagingDialog::liveFilesWith(const std::string& callsign) const
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (frame == nullptr) return 0;
    int live = 0;
    for (const Data2G::FileTransfer& t : frame->chatFileTransfers())
    {
        if (t.peer == callsign && t.live()) live++;
    }
    return live;
}

void TextMessagingDialog::OnChatContextMenu(wxContextMenuEvent& event)
{
    // From the keyboard there is no position; the newest line is meant.
    wxPoint screen = event.GetPosition();
    int row = screen == wxDefaultPosition
                  ? (int)m_rows.size() - 1
                  : rowAt(m_chatWindow->ScreenToClient(screen));

    m_menuMessageId = 0;
    m_menuResend = TextMessage();
    m_menuTransferId = 0;
    m_menuGroupFileId = 0;
    Data2G::GroupFile groupFile;
    TextMessagingProtocol::Cancel cancel = TextMessagingProtocol::Cancel::None;
    if (row >= 0 && m_rows[(size_t)row].file && m_fileLines[m_rows[(size_t)row].index].group)
    {
        groupFile = m_fileLines[m_rows[(size_t)row].index].groupFile;
        m_menuGroupFileId = groupFile.id;
    }
    else if (row >= 0 && m_rows[(size_t)row].file)
    {
        // A file still going can be stopped, either way.
        const FileLine& line = m_fileLines[m_rows[(size_t)row].index];
        if (line.transfer.live()) m_menuTransferId = line.transfer.id;
    }
    else if (row >= 0)
    {
        const TextMessage& message = m_messages[m_rows[(size_t)row].index];
        // Our own messages and pings; the protocol says which can be stopped.
        if (message.direction == MessageDirection::Sent)
        {
            cancel = TextMessagingSession::instance().protocol().cancelFor(message.id);
            if (cancel != TextMessagingProtocol::Cancel::None) m_menuMessageId = message.id;

            // Any chat message of ours that has left the queue can go again,
            // however it got on: delivered, given up on, or still going.
            if (message.kind == MessageKind::Chat && cancel != TextMessagingProtocol::Cancel::Remove)
            {
                m_menuResend = message;
            }
        }
    }

    wxMenu menu;
    menu.Append(ID_MENU_WOAH, _("Woah!"));
    menu.AppendSeparator();
    if (m_menuTransferId != 0)
    {
        menu.Append(ID_MENU_CANCEL_TRANSFER, _("Cancel Transfer"));
        menu.AppendSeparator();
    }

    // A group file of ours still going can stop taking requests, or stop
    // altogether; one coming in can be received or ignored.
    using GroupState = Data2G::GroupFile::State;
    if (m_menuGroupFileId != 0 && groupFile.outgoing && groupFile.live())
    {
        menu.Append(ID_MENU_STOP_SERVING, _("Stop Serving Repairs"));
        menu.Append(ID_MENU_CANCEL_GROUP_FILE, _("Cancel Transfer"));
        menu.AppendSeparator();
    }
    else if (m_menuGroupFileId != 0 && !groupFile.outgoing)
    {
        bool open = groupFile.state == GroupState::Heard || groupFile.state == GroupState::Incomplete;
        if (open) menu.Append(ID_MENU_RECEIVE_GROUP_FILE, _("Receive..."));
        if (open || groupFile.state == GroupState::Receiving) menu.Append(ID_MENU_IGNORE_GROUP_FILE, _("Ignore"));
        if (open || groupFile.state == GroupState::Receiving) menu.AppendSeparator();
    }

    // A file for everybody on the GLISS group; dark, with the reason, when
    // one can't go.
    {
        MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
        wxString why = _("Files for the group go through Data2G.");
        bool files = frame != nullptr && frame->chatCanSendGroupFiles(why);
        wxMenuItem* item = menu.Append(ID_MENU_SEND_GROUP_FILE, _("Send File to Group..."));
        item->Enable(files && m_inhibitReason.empty());
        if (!files) menu.Append(ID_MENU_SEND_GROUP_FILE_WHY, why)->Enable(false);
        else if (!m_inhibitReason.empty()) menu.Append(ID_MENU_SEND_GROUP_FILE_WHY, _("Receive only here."))->Enable(false);
        menu.AppendSeparator();
    }
    if (cancel == TextMessagingProtocol::Cancel::Remove)
    {
        menu.Append(ID_MENU_REMOVE_MESSAGE, _("Remove from Queue"));

        // A message with a countdown chip can go out at a tempo of its own,
        // unless it goes through a Data2G session, which picks its own speed.
        MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
        int current = frame != nullptr ? frame->chatTransmitGear() : 0;
        auto bar = m_queueBars.find(m_menuMessageId);
        if (current != 0 && bar != m_queueBars.end() && bar->second.gear != 0)
        {
            wxMenu* tempos = new wxMenu;
            for (int gear = Glissando::MIN_GEAR; gear <= Glissando::MAX_GEAR; gear++)
            {
                tempos->AppendCheckItem(ID_MENU_TEMPO + gear, GlissandoConsole::gearLabel(gear))
                    ->Check(bar->second.tempoChosen && bar->second.gear == gear);
            }
            tempos->AppendSeparator();
            tempos->AppendCheckItem(ID_MENU_TEMPO, wxString::Format(_("Console's Tempo (%s)"),
                                                                    GlissandoConsole::gearLabel(current)))
                ->Check(!bar->second.tempoChosen);
            menu.AppendSubMenu(tempos, _("Change Tempo to..."));
        }
        menu.AppendSeparator();
    }
    else if (cancel == TextMessagingProtocol::Cancel::Abort)
    {
        menu.Append(ID_MENU_ABORT_MESSAGE, _("Abort"));
    }
    if (m_menuResend.id != 0) menu.Append(ID_MENU_RESEND, _("Re-send"));
    if (cancel == TextMessagingProtocol::Cancel::Abort || m_menuResend.id != 0) menu.AppendSeparator();
    menu.Append(ID_MENU_CLEAR_MESSAGES, _("Clear Messages"))->Enable(!m_rows.empty());

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

// The operator hears somebody the receiver has missed: hold the
// transmitter a frame longer, and the queue's countdowns with it.
void TextMessagingDialog::OnMenuWoah(wxCommandEvent&)
{
    uint64_t heldMs = TextMessagingSession::instance().protocol().holdTransmissions();
    // File keyings on the GLISS group too.
    if (MainFrame* frame = dynamic_cast<MainFrame*>(GetParent())) frame->chatHoldGroupFiles(heldMs);
    int seconds = (int)((heldMs + 999) / 1000);
    setStatus(wxString::Format(_("Woah! Holding the transmitter for %d s."), seconds), StatusKind::Queued);
    updateQueueBars();
    if (uiLogEnabled()) log_info("UI: woah, transmitter held for %d s", seconds);
}

// Moves a queued message to the tempo picked, or back to the console's.
void TextMessagingDialog::OnMenuTempo(wxCommandEvent& event)
{
    if (m_menuMessageId == 0) return;

    int gear = event.GetId() - ID_MENU_TEMPO;
    if (!TextMessagingSession::instance().protocol().setMessageTempo(m_menuMessageId, gear))
    {
        setStatus(_("That message is no longer waiting to be sent."));
        return;
    }

    setStatus(gear != 0 ? wxString::Format(_("Message will go out at %s."), GlissandoConsole::gearLabel(gear))
                        : wxString(_("Message will go out at the console's tempo.")),
              StatusKind::Queued);
    updateQueueBars();
    if (uiLogEnabled()) log_info("UI: message id=%d tempo %d", (int)m_menuMessageId, gear);
}

// Clears the log for good, here and in the message store. Messages still
// being sent stay, so their chips can still say how they got on.
void TextMessagingDialog::OnMenuClearMessages(wxCommandEvent&)
{
    if (m_messages.empty() && m_fileLines.empty()) return;

    wxMessageDialog confirm(this, _("Clear every message from COMMS? "
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
    // Files still going stay too. The transport still lists the finished
    // ones, so they are remembered as cleared.
    for (const FileLine& line : m_fileLines)
    {
        if (line.live()) continue;
        if (line.group) m_clearedGroupFiles.insert(line.groupFile.id);
        else m_clearedFiles.insert(line.transfer.id);
    }
    m_fileLines.erase(std::remove_if(m_fileLines.begin(), m_fileLines.end(),
                                     [](const FileLine& line) { return !line.live(); }),
                      m_fileLines.end());
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
    // Group files are received without asking only while it is lit.
    if (MainFrame* frame = dynamic_cast<MainFrame*>(GetParent())) frame->chatApplyGroupFileAutoReceive();

    if (!m_chkAutoReply->IsChecked())
    {
        setStatus(_("This station will no longer transmit on its own. Other stations will send to it "
                    "once, without retries. Glissando 0.3 and older cannot read its messages until "
                    "this is lit again."));
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
    updatePhraseHighlight();
    event.Skip();
}

// The phrases the ham text table codes as one symbol (" the", "CQ CQ") get
// a faint red background as they are typed, found the way the encoder will
// find them in the text the protocol sends (trimmed).
void TextMessagingDialog::updatePhraseHighlight()
{
    wxString text = m_txtEntry->GetValue();

    // Phrases are plain ASCII, so a character outside it can only stand in
    // the way of one: replace each with a byte no phrase holds, and byte i
    // is character i.
    std::string ascii;
    ascii.reserve(text.length());
    for (wxUniChar c : text) ascii.push_back(c.IsAscii() ? (char)c.GetValue() : '\x01');
    size_t first = ascii.find_first_not_of(" \t\r\n");
    size_t last = ascii.find_last_not_of(" \t\r\n");

    std::vector<TextMessaging::HamText::PhraseSpan> spans;
    if (first != std::string::npos)
        spans = TextMessaging::HamText::phraseSpans(ascii.substr(first, last + 1 - first));
    if (spans.empty() && !m_phraseHighlighted) return;

    wxTextAttr plain;
    plain.SetBackgroundColour(Chaotica::Colour::Bakelite);
    m_txtEntry->SetStyle(0, m_txtEntry->GetLastPosition(), plain);
    wxTextAttr accelerated;
    accelerated.SetBackgroundColour(Chaotica::Colour::Accelerated);
    for (const auto& span : spans)
    {
        long from = (long)(first + span.start);
        m_txtEntry->SetStyle(from, from + (long)span.length, accelerated);
    }
    m_phraseHighlighted = !spans.empty();
}

void TextMessagingDialog::OnTimer(wxTimerEvent&)
{
    updateSessionPlate();
    refreshStations();
    updateTransmitControls();
    updateAckWaitStatus();
    updateModemStatus();
    updateSmokeWarning();

    // Auto can change the tempo while the text sits there.
    updateAirTime();
}

// Once the visi-scope smokes a fair bit (see SmokeGauge), the status line
// says a word to the wise after Transmitting.
wxString TextMessagingDialog::transmittingStatus() const
{
    return m_smoking ? _("Transmitting. Your rig might be on fire. Please check your finals.")
                     : _("Transmitting...");
}

void TextMessagingDialog::updateSmokeWarning()
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    // Only once there is a fair bit of it: half as thick as it gets, 20 s
    // after it starts.
    bool smoking = frame != nullptr && frame->smokeLevel() >= 0.5;
    if (smoking == m_smoking) return;
    m_smoking = smoking;
    if (m_transmitting && m_statusKind == StatusKind::Activity) setStatus(transmittingStatus(), StatusKind::Activity);
}

void TextMessagingDialog::OnBlinkTimer(wxTimerEvent&)
{
    updateEngageChips();
    updateQueueBars();
    updateFileTransfers();
}

// Each message waiting for its first turn on the air counts down to it. The
// bar starts full at the wait it was first given and runs down to nothing;
// a wait that grows, such as a hold that starts while it waits, fills it
// again. While somebody else has the channel nothing can be said about when
// it ends, so the bar stops where it is and dims until the channel clears.
// The chat is redrawn only when a bar moves by a pixel.
void TextMessagingDialog::updateQueueBars()
{
    std::vector<QueuedWait> waits = TextMessagingSession::instance().protocol().queuedWaits();
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    int current = frame != nullptr ? frame->chatTransmitGear() : 0;

    std::map<int64_t, QueueBar> bars;
    bool changed = waits.size() != m_queueBars.size();
    for (const QueuedWait& wait : waits)
    {
        auto old = m_queueBars.find(wait.messageId);
        bool fresh = old == m_queueBars.end();
        QueueBar bar = fresh ? QueueBar() : old->second;

        if (fresh || !wait.channelBusy) bar.remainingMs = std::max<int64_t>(0, wait.waitMs);
        if (bar.remainingMs > bar.totalMs) bar.totalMs = bar.remainingMs;
        bar.channelBusy = wait.channelBusy;

        int fill = bar.totalMs > 0
                       ? (int)std::lround((double)QUEUE_BAR_WIDTH * (double)bar.remainingMs / (double)bar.totalMs)
                       : 0;
        // The tempo it keys at: its own, or whatever the console or Auto
        // shift picks by then, shown as it stands now. None through a Data2G
        // session, which picks its own speed.
        bool tempo = current != 0 && !wait.reliableLink;
        int gear = !tempo ? 0 : wait.gear != 0 ? wait.gear : current;
        bool chosen = tempo && wait.gear != 0;
        if (fresh || fill != bar.fillPixels || bar.channelBusy != old->second.channelBusy || gear != bar.gear ||
            chosen != bar.tempoChosen)
        {
            changed = true;
        }
        bar.fillPixels = fill;
        bar.gear = gear;
        bar.tempoChosen = chosen;
        bars[wait.messageId] = bar;
    }

    m_queueBars.swap(bars);

    // And the bar behind the chip of the message on the air.
    double progress = frame != nullptr ? frame->chatSendProgress() : -1.0;
    int sendFill = progress < 0.0 ? -1 : (int)std::lround(QUEUE_BAR_WIDTH * progress);
    if (sendFill != m_sendFillPixels) changed = true;
    m_sendFillPixels = sendFill;

    if (changed && IsShown()) renderChat(true);
}

// A queued message's chip: the countdown bar with QUEUED across it, in
// lettering that changes colour where the bar ends, light on the empty
// part and dark on the filled part, so it reads wherever the bar has got to.
wxString TextMessagingDialog::queueBarChip(const TextMessage& message)
{
    const QueueBar& bar = m_queueBars.at(message.id);

    // With the tempo it will key at, in the console's words.
    const wxString label = bar.gear != 0
                               ? wxString::Format("%s %s %s", _("QUEUED"), wxString(wxUniChar(0x00B7)),
                                                  GlissandoConsole::gearLabel(bar.gear).Upper())
                               : wxString(_("QUEUED"));
    return barChip(message.id, label, bar.fillPixels,
                   bar.channelBusy ? Chaotica::Colour::Dim : Chaotica::Colour::Chrome);
}

// A chip with a bar behind its label, fillPixels of QUEUE_BAR_WIDTH filled
// from the left, as an image the chat page shows from memory.
wxString TextMessagingDialog::barChip(int64_t messageId, const wxString& label, int fillPixels,
                                      const wxColour& filled)
{
    const wxColour empty(0x4A, 0x48, 0x45);   // the smoke of the other waiting chips
    const wxColour lightText = Chaotica::Colour::Bone;
    const wxColour darkText = Chaotica::Colour::Void;

    wxFont font = Chaotica::font(Chaotica::FontRole::Caption);
    double scale = GetDPIScaleFactor();
    int width = (int)std::lround(QUEUE_BAR_WIDTH * scale);
    int fill = (int)std::lround(std::max(fillPixels, 0) * scale);

    wxBitmap probe(1, 1);
    wxMemoryDC measure(probe);
    measure.SetFont(font);
    wxSize text = measure.GetTextExtent(label);
    measure.SelectObject(wxNullBitmap);
    int height = text.GetHeight() + (int)std::lround(4 * scale);

    wxBitmap bitmap(width, height);
    {
        wxMemoryDC dc(bitmap);
        dc.SetFont(font);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(empty));
        dc.DrawRectangle(0, 0, width, height);
        dc.SetBrush(wxBrush(filled));
        dc.DrawRectangle(0, 0, fill, height);

        int x = (width - text.GetWidth()) / 2;
        int y = (height - text.GetHeight()) / 2;
        if (fill > 0)
        {
            dc.SetClippingRegion(0, 0, fill, height);
            dc.SetTextForeground(darkText);
            dc.DrawText(label, x, y);
            dc.DestroyClippingRegion();
        }
        if (fill < width)
        {
            dc.SetClippingRegion(fill, 0, width - fill, height);
            dc.SetTextForeground(lightText);
            dc.DrawText(label, x, y);
            dc.DestroyClippingRegion();
        }
        dc.SelectObject(wxNullBitmap);
    }

    wxString name = wxString::Format("glissando-queue-%u-%lld.bmp", m_queueBarGeneration,
                                     (long long)messageId);
    wxMemoryFSHandler::AddFile(name, bitmap, wxBITMAP_TYPE_BMP);
    m_newQueueBarImages.push_back(name);

    return wxString::Format("<img src=\"memory:%s\" width=\"%d\" height=\"%d\">", name,
                            (int)std::lround(width / scale), (int)std::lround(height / scale));
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

// Coding a message of a few hundred characters takes microseconds, so this runs on every change
// to the text rather than waiting for typing to stop.
void TextMessagingDialog::updateAirTime()
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());

    // The protocol sends the text trimmed, as UTF-8.
    wxString text = m_txtEntry->GetValue();
    text.Trim(true).Trim(false);
    std::string utf8 = text.ToStdString(wxConvUTF8);

    double seconds = frame != nullptr && !utf8.empty() ? frame->chatMessageAirSeconds(utf8) : 0.0;
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
            setStatus(transmittingStatus(), StatusKind::Activity);
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

namespace
{

// "45 s" or "12 min", for how long something takes.
wxString duration(double seconds)
{
    if (seconds < 90) return wxString::Format(_("%d s"), (int)std::ceil(seconds));
    return wxString::Format(_("%d min"), (int)std::ceil(seconds / 60.0));
}

// The time of day so many seconds from now.
wxString clockIn(double seconds)
{
    return (wxDateTime::Now() + wxTimeSpan::Seconds((wxLongLong)std::llround(seconds))).Format("%H:%M");
}

wxString groupFileName(const Data2G::GroupFile& f)
{
    if (!f.name.empty()) return wxString::FromUTF8(f.name);
    return wxString::Format(_("file %06X"), (unsigned)f.fileId);
}

// How a group file is getting on, or how it ended, in words; and, for
// one of ours, who has asked for pieces, and for one heard, whether
// Receive... and Ignore still apply.
wxString groupStateText(const Data2G::GroupFile& f, wxString& note, bool& receive, bool& ignore)
{
    using State = Data2G::GroupFile::State;
    using Phase = Data2G::GroupFile::Phase;
    wxString text;
    note.clear();
    receive = ignore = false;
    if (f.outgoing)
    {
        wxString state;
        switch (f.state)
        {
            case State::Sending:
                state = f.have == 0 ? wxString(_("announced"))
                                    : wxString::Format(_("sending %d of %d"), f.have, f.pieces);
                break;
            case State::Repairing:
                switch (f.phase)
                {
                    case Phase::WindowOpen:
                        state = wxString::Format(_("repairs open, round %d (%d s left)"), f.round + 1,
                                                 f.secondsLeftInPhase);
                        break;
                    case Phase::Resending:
                        state = wxString::Format(_("round %d: resending %d pieces, %s first in line"), f.round + 1,
                                                 f.resending, wxString::FromUTF8(f.firstInLine));
                        if (f.othersAsking > 0) state += wxString::Format(_(" (+%d others)"), f.othersAsking);
                        break;
                    case Phase::Waiting:
                        state = wxString::Format(_("idle; the next window in %d s"), f.secondsLeftInPhase);
                        break;
                    case Phase::Ending: state = _("ending"); break;
                    default: state = _("repairs open"); break;
                }
                if (f.serviceSecondsLeft >= 0 && f.phase != Phase::Ending)
                {
                    state += wxString::Format(_("; repairs until %s at the latest"), clockIn(f.serviceSecondsLeft));
                }
                break;
            default:
                switch (f.endReason)
                {
                    case Data2G::GroupFileEnd::Quiet: state = _("ended: no more requests"); break;
                    case Data2G::GroupFileEnd::Deadline: state = _("ended: repair time over"); break;
                    case Data2G::GroupFileEnd::Stopped: state = _("ended: stopped serving repairs"); break;
                    case Data2G::GroupFileEnd::Cancelled: state = _("cancelled"); break;
                    case Data2G::GroupFileEnd::Failed: state = _("failed"); break;
                }
                break;
        }
        text = state;

        // Who asked, and when; and that nobody says they have it.
        for (const Data2G::GroupFile::Asker& asker : f.askers)
        {
            note += note.empty() ? wxString(_("Asked: ")) : wxString(", ");
            note += wxString::Format(_("%s (%d missing, %s ago)"), wxString::FromUTF8(asker.call), asker.missing,
                                     duration((double)asker.secondsAgo));
        }
        if (!note.empty()) note += ". ";
        note += _("Stations don't confirm receipt, so silence says nothing.");
    }
    else
    {
        wxString from = wxString::FromUTF8(f.sender);
        wxString have = f.pieces > 0 ? wxString::Format(_("have %d of %d"), f.have, f.pieces)
                                     : wxString::Format(_("have %d"), f.have);
        wxString state;
        switch (f.state)
        {
            case State::Heard:
                if (f.verified) state = _("complete, verified");
                else if (f.serviceSecondsLeft > 0)
                {
                    state = have + wxString::Format(_("; Receive to ask for the rest before %s"),
                                                    clockIn(f.serviceSecondsLeft));
                }
                else state = have;
                receive = ignore = true;
                break;
            case State::Receiving:
                state = have;
                if (f.verified) state = _("complete, verified; waiting for its name");
                else if (f.slot >= 0)
                {
                    state += wxString::Format(_("; asking in slot %d (in %d s)"), f.slot + 1, f.slotInSeconds);
                }
                else if (!f.firstInLine.empty())
                {
                    state += "; " + wxString::Format(_("%s first in line; %d of your pieces coming"),
                                                     wxString::FromUTF8(f.firstInLine), f.comingForUs);
                }
                ignore = true;
                break;
            case State::Incomplete:
                state = wxString::Format(_("incomplete: %d missing (pieces kept 24 h)"), std::max(0, f.pieces - f.have));
                if (!f.path.empty())
                {
                    state += "; " + wxString::Format(_("saved to %s if it is sent again"), wxString::FromUTF8(f.path));
                }
                receive = ignore = true;
                break;
            case State::Saved:
                state = wxString::Format(f.autoReceived ? _("saved to %s without asking") : _("saved to %s"),
                                         wxString::FromUTF8(f.path));
                break;
            case State::Ignored: state = _("ignored"); break;
            case State::FailedVerification: state = _("failed verification: nothing saved"); break;
            case State::CancelledThere: state = wxString::Format(_("cancelled by %s"), from); break;
            default: state = _("failed"); break;
        }
        text = state;
    }
    if (!f.live() && !f.error.empty()) text += " (" + wxString::FromUTF8(f.error) + ")";
    return text;
}

} // namespace

namespace
{

// How a session's file is getting on, or how it ended, in a few words.
wxString fileStateText(const Data2G::FileTransfer& t)
{
    using State = Data2G::FileTransfer::State;
    wxString peer = wxString::FromUTF8(t.peer);
    wxString state;
    switch (t.state)
    {
        case State::Waiting: state = wxString::Format(_("waiting for a session with %s"), peer); break;
        case State::Offered: state = _("offered; waiting for an answer"); break;
        case State::Sending: state = _("sending"); break;
        case State::Delivered: state = _("delivered"); break;
        case State::Declined: state = _("declined"); break;
        case State::Asking: state = _("offered"); break;
        case State::Receiving: state = _("receiving"); break;
        case State::Saved:
            state = wxString::Format(t.autoAccepted ? _("saved to %s without asking") : _("saved to %s"),
                                     wxString::FromUTF8(t.path));
            break;
        case State::Cancelled: state = _("cancelled"); break;
        case State::CancelledThere: state = wxString::Format(_("cancelled by %s"), peer); break;
        case State::Expired: state = _("expired"); break;
        case State::Failed: state = _("failed"); break;
        case State::FailedThere: state = wxString::Format(_("failed on %s's side"), peer); break;
        case State::NotSupported: state = _("failed: their Glissando can't take files"); break;
    }
    if (t.state == State::Failed && !t.error.empty()) state += ": " + wxString::FromUTF8(t.error);
    return state;
}

// One bubble of the chat for a file: ours on the right, theirs on the
// left, like messages.
wxString fileBubble(bool outgoing, const wxString& body, const wxString& tag, const wxString& extra,
                    std::time_t at, const wxString& text, const wxString& bubble, const wxString& subdued)
{
    wxString align = outgoing ? "right" : "left";
    wxString html = "<table width=\"100%\" cellpadding=\"0\" cellspacing=\"0\"><tr><td align=\"" + align + "\">";
    html += "<table cellpadding=\"6\" cellspacing=\"0\" bgcolor=\"" + bubble + "\"><tr><td>";
    html += "<font color=\"" + text + "\">" + body + " <font size=\"-2\" color=\"" + subdued + "\">[" + tag +
            "]</font></font>";
    html += extra;
    html += "<table width=\"100%\" cellpadding=\"0\" cellspacing=\"0\"><tr><td align=\"left\">"
            "<font size=\"-2\" color=\"" + subdued + "\">" + formatTime(at) + "</font></td></tr></table>";
    html += "</td></tr></table></td></tr><tr><td height=\"10\"></td></tr></table>";
    return html;
}

} // namespace

// A file's lines in the chat: what it is when it starts, and how it ended.
wxString TextMessagingDialog::fileLineHtml(const FileLine& line, const Palette& colors, bool result) const
{
    const Data2G::FileTransfer& t = line.transfer;
    wxString peer = wxString::FromUTF8(t.peer);
    wxString name = wxString::FromUTF8(t.name);
    wxString body;
    if (result)
    {
        body = escapeHtml(name + ": " + fileStateText(t));
    }
    else if (t.outgoing)
    {
        body = escapeHtml(wxString::Format(_("%s to %s, %s bytes"), name, peer, groupDigits(t.size)));
    }
    else
    {
        body = "<b>" + escapeHtml(t.peer) + "</b> " +
               escapeHtml(wxString::Format(_("offers %s, %s bytes"), name, groupDigits(t.size)));
    }
    wxString tag = t.outgoing ? wxString::Format(_("FILE TO %s"), peer) : wxString(_("FILE"));
    return fileBubble(t.outgoing, body, tag, wxEmptyString, result ? line.endedAt : line.at, colors.text,
                      t.outgoing ? colors.sentBubble : colors.receivedBubble, colors.subdued);
}

wxString TextMessagingDialog::fileLinesShown(const FileLine& line) const
{
    Palette colors = palette();
    wxString shown = line.group ? groupLineHtml(line, colors, false) : fileLineHtml(line, colors, false);
    if (line.endedAt != 0) shown += line.group ? groupLineHtml(line, colors, true) : fileLineHtml(line, colors, true);
    return shown;
}

void TextMessagingDialog::noteEnded(FileLine& line)
{
    if (line.live() || line.endedAt != 0) return;
    line.endedAt = std::max(line.at, std::time(nullptr));
    line.endedSteady = steadySeconds();
}

// Follows the transfers the transport has, on the blink timer: a new one
// gets its line, and one that has ended a line saying how; an offer to us
// the box over the chat, and COMMS comes forward for it; the one under way
// the status area. A file coming in while the window is closed flashes the
// console's COMMS button, as a message does.
void TextMessagingDialog::updateFileTransfers()
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (frame == nullptr) return;

    bool added = false;
    bool changed = false;
    bool incoming = false;
    updateGroupFiles(added, changed, incoming);

    uint64_t changes = frame->chatFileTransferChanges();
    bool fresh = !m_fileLinesRead || changes != m_fileChanges;
    m_fileLinesRead = true;
    m_fileChanges = changes;

    bool offered = false;
    double now = steadySeconds();
    for (const Data2G::FileTransfer& t : fresh ? frame->chatFileTransfers() : std::vector<Data2G::FileTransfer>())
    {
        if (m_clearedFiles.count(t.id) != 0) continue;
        auto line = std::find_if(m_fileLines.begin(), m_fileLines.end(),
                                 [&](const FileLine& l) { return !l.group && l.transfer.id == t.id; });
        if (line == m_fileLines.end())
        {
            FileLine newLine;
            newLine.transfer = t;
            newLine.at = std::time(nullptr);
            newLine.readAt = now;
            noteEnded(newLine);
            newLine.shown = fileLinesShown(newLine);
            m_fileLines.push_back(newLine);
            added = true;
            incoming = incoming || !t.outgoing;
            if (uiLogEnabled()) log_info("UI: file line id=%d %s", (int)t.id, t.name.c_str());
            continue;
        }

        // The status bar says why one failed, as well as its line.
        if (t.state == Data2G::FileTransfer::State::Failed && line->transfer.state != t.state && !t.error.empty())
        {
            setStatus(wxString::Format(_("%s failed: %s"), wxString::FromUTF8(t.name), wxString::FromUTF8(t.error)));
        }
        bool moving = t.state == Data2G::FileTransfer::State::Sending ||
                      t.state == Data2G::FileTransfer::State::Receiving;
        if (moving && line->rateSince < 0.0)
        {
            line->rateSince = now;
            line->rateBytes = t.done;
        }
        line->transfer = t;
        line->readAt = now;
        noteEnded(*line);
        wxString shown = fileLinesShown(*line);
        if (shown != line->shown)
        {
            line->shown = shown;
            changed = true;
        }
    }

    // An offer nobody has answered brings COMMS forward, once each.
    for (const FileLine& line : m_fileLines)
    {
        if (line.group || line.transfer.outgoing || line.transfer.state != Data2G::FileTransfer::State::Asking) continue;
        if (!m_offersRaised.insert(line.transfer.id).second) continue;
        offered = true;
    }

    // The oldest finished lines go once there are many.
    while (m_fileLines.size() > FILE_LINES_KEPT)
    {
        auto finished = std::find_if(m_fileLines.begin(), m_fileLines.end(),
                                     [](const FileLine& l) { return !l.live(); });
        if (finished == m_fileLines.end()) break;
        // Not to come back from the engines, which keep more than this.
        if (finished->group) m_clearedGroupFiles.insert(finished->groupFile.id);
        else m_clearedFiles.insert(finished->transfer.id);
        m_fileLines.erase(finished);
        added = true;
    }

    if (incoming && !IsShown() && !offered) frame->noteChatUnread();
    updateOfferBox();
    updateTransferStatus();

    // A new line scrolls to it; a line that ended leaves the view where it is.
    if (added) renderChat();
    else if (changed && IsShown()) renderChat(true);

    if (offered)
    {
        if (uiLogEnabled()) log_info("UI: a file is offered; COMMS comes forward");
        if (IsShown())
        {
            Iconize(false);
            Raise();
        }
        else
        {
            frame->glissandoShowChat(true);
        }
    }
}

// The oldest offer still waiting for the operator, in the box over the
// chat, with how long it has left; the box goes once none is.
void TextMessagingDialog::updateOfferBox()
{
    const FileLine* offer = nullptr;
    for (const FileLine& line : m_fileLines)
    {
        if (!line.group && !line.transfer.outgoing && line.transfer.state == Data2G::FileTransfer::State::Asking)
        {
            offer = &line;
            break;
        }
    }

    if (offer != nullptr)
    {
        double left = offer->transfer.expiresInMs / 1000.0 - (steadySeconds() - offer->readAt);
        int seconds = std::max(0, (int)std::ceil(left));
        wxString countdown = offer->transfer.expiresInMs == 0
                                 ? wxString()
                                 : wxString::Format(_("expires in %d:%02d"), seconds / 60, seconds % 60);
        if (m_offerCountdown->GetLabel() != countdown) m_offerCountdown->SetLabel(countdown);
        bool lit = Chaotica::blinkLit();
        if (lit != m_offerLit)
        {
            m_offerLit = lit;
            m_offerBox->Refresh();
        }
    }

    uint64_t id = offer != nullptr ? offer->transfer.id : 0;
    if (id == m_offerId && m_offerBox->IsShown() == (offer != nullptr)) return;
    m_offerId = id;
    if (offer != nullptr)
    {
        m_offerText->setText(wxString::Format(_("%s offers %s (%s bytes). Save it?"),
                                              wxString::FromUTF8(offer->transfer.peer),
                                              wxString::FromUTF8(offer->transfer.name),
                                              groupDigits(offer->transfer.size)));
    }
    m_offerBox->Show(offer != nullptr);
    Layout();
    // The plate stretches, so showing the box doesn't resize it, and its
    // own sizer has to be run to make room for the box.
    m_offerBox->GetParent()->Layout();
    m_offerBox->Layout();
}

void TextMessagingDialog::paintOfferBox(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_offerBox);
    dc.SetBackground(wxBrush(Chaotica::Colour::Plate));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
    if (!gc) return;
    wxSize size = m_offerBox->GetClientSize();
    gc->SetPen(wxPen(m_offerLit ? Chaotica::Colour::Alarm : OFFER_BORDER_DARK, 2));
    gc->SetBrush(wxBrush(OFFER_BACKGROUND));
    gc->DrawRoundedRectangle(1, 1, size.x - 2, size.y - 2, 4);
}

namespace
{

// "34 B/s", "1.2 kB/s".
wxString rateText(double bytesPerSecond)
{
    if (bytesPerSecond < 1000.0) return wxString::Format(_("%.0f B/s"), bytesPerSecond);
    return wxString::Format(_("%.1f kB/s"), bytesPerSecond / 1000.0);
}

// "45 s left", "12 min left".
wxString timeLeftText(double seconds)
{
    if (seconds < 90) return wxString::Format(_("%d s left"), (int)std::ceil(seconds));
    return wxString::Format(_("%d min left"), (int)std::ceil(seconds / 60.0));
}

wxString joinMeta(const std::vector<wxString>& parts)
{
    wxString out;
    for (const wxString& part : parts)
    {
        if (part.empty()) continue;
        if (!out.empty()) out += wxString::FromUTF8("   \xC2\xB7   ");
        out += part;
    }
    return out;
}

} // namespace

// The status area over the chat: the newest transfer still going, unless
// it is an offer to us (the box above has that), or else the one it last
// showed, for a while after that one ended.
void TextMessagingDialog::updateTransferStatus()
{
    double now = steadySeconds();
    const FileLine* shown = nullptr;
    int others = 0;
    for (const FileLine& line : m_fileLines)
    {
        if (!line.live()) continue;
        if (!line.group && line.transfer.state == Data2G::FileTransfer::State::Asking) continue;
        if (shown != nullptr) others++;
        if (shown == nullptr || line.at >= shown->at) shown = &line;
    }
    if (shown == nullptr && m_statusId != 0)
    {
        auto last = std::find_if(m_fileLines.begin(), m_fileLines.end(), [this](const FileLine& l) {
            return l.group == m_statusGroup && (l.group ? l.groupFile.id : l.transfer.id) == m_statusId;
        });
        if (last != m_fileLines.end() && !last->live() && now - last->endedSteady < FINISHED_SHOWN_SECONDS)
        {
            shown = &*last;
        }
    }

    if (shown == nullptr)
    {
        m_statusId = 0;
        if (m_transferStatus->IsShown())
        {
            m_transferStatus->Hide();
            m_transferStatus->GetParent()->Layout();
        }
        return;
    }
    m_statusGroup = shown->group;
    m_statusId = shown->group ? shown->groupFile.id : shown->transfer.id;

    TransferStatusArea::View view;
    wxString cancel = _("Cancel");
    bool canCancel = shown->live();
    bool canReceive = false;
    if (others > 0) view.more = wxString::Format(_("+%d more"), others);
    wxString arrowOut = wxString::FromUTF8("\xE2\x86\x92  "), arrowIn = wxString::FromUTF8("\xE2\x86\x90  ");

    if (!shown->group)
    {
        using State = Data2G::FileTransfer::State;
        const Data2G::FileTransfer& t = shown->transfer;
        view.who = (t.outgoing ? arrowOut : arrowIn) + wxString::FromUTF8(t.peer) + "     " + wxString::FromUTF8(t.name);
        view.stage = fileStateText(t);
        double size = (double)std::max<uint64_t>(1, t.size);
        bool whole = t.state == State::Delivered || t.state == State::Saved;
        view.acked = whole ? 1.0 : t.done / size;
        view.handed = whole ? 1.0 : t.handed / size;
        view.failed = !t.live() && !whole;

        std::vector<wxString> meta;
        if (t.state == State::Sending || t.state == State::Receiving)
        {
            meta.push_back(wxString::Format(t.outgoing ? _("%s of %s bytes acknowledged") : _("%s of %s bytes"),
                                            groupDigits(t.done), groupDigits(t.size)));
            double elapsed = now - shown->rateSince;
            if (shown->rateSince >= 0.0 && elapsed >= 3.0 && t.done > shown->rateBytes)
            {
                double rate = (t.done - shown->rateBytes) / elapsed;
                meta.push_back(rateText(rate));
                meta.push_back(timeLeftText((t.size - t.done) / rate));
            }
        }
        else
        {
            meta.push_back(wxString::Format(_("%s bytes"), groupDigits(t.size)));
            if (t.state == State::Offered && t.expiresInMs != 0)
            {
                int left = std::max(0, (int)std::ceil(t.expiresInMs / 1000.0 - (now - shown->readAt)));
                meta.push_back(wxString::Format(_("the offer expires in %d:%02d"), left / 60, left % 60));
            }
        }
        view.meta = joinMeta(meta);
    }
    else
    {
        using State = Data2G::GroupFile::State;
        using Phase = Data2G::GroupFile::Phase;
        const Data2G::GroupFile& f = shown->groupFile;
        wxString name = groupFileName(f);
        view.group = true;
        view.pieces = f.pieceMap;
        if (view.pieces.empty()) view.pieces.assign(20, Data2G::GroupFile::PieceNotYet);
        view.who = f.outgoing ? arrowOut + "GLISS     " + name
                              : arrowIn + wxString::Format(_("%s to GLISS"), wxString::FromUTF8(f.sender)) + "     " +
                                    name;
        wxString tempo = GlissandoConsole::gearLabel(f.tempo + 1);
        std::vector<wxString> meta;
        if (f.outgoing)
        {
            switch (f.state)
            {
                case State::Sending: view.stage = f.have == 0 ? _("announced") : _("streaming"); break;
                case State::Repairing:
                    switch (f.phase)
                    {
                        case Phase::WindowOpen:
                            view.stage = wxString::Format(_("repairs open, round %d (%d s left)"), f.round + 1,
                                                          f.secondsLeftInPhase);
                            break;
                        case Phase::Resending:
                            view.stage = wxString::Format(_("repairs, round %d, %s first in line"), f.round + 1,
                                                          wxString::FromUTF8(f.firstInLine));
                            break;
                        case Phase::Waiting:
                            view.stage = wxString::Format(_("idle; the next window in %d s"), f.secondsLeftInPhase);
                            break;
                        case Phase::Ending: view.stage = _("ending"); break;
                        default: view.stage = _("repairs open"); break;
                    }
                    break;
                default:
                {
                    wxString note;
                    bool receive = false, ignore = false;
                    view.stage = groupStateText(f, note, receive, ignore);
                    view.failed = f.endReason == Data2G::GroupFileEnd::Failed;
                    break;
                }
            }
            meta.push_back(f.state == State::Sending ? wxString::Format(_("sent %d of %d pieces"), f.have, f.pieces)
                                                     : wxString::Format(_("%d pieces"), f.pieces));
            meta.push_back(tempo);
            if (f.live() && f.serviceSecondsLeft >= 0)
            {
                meta.push_back(wxString::Format(_("serving until %s"), clockIn(f.serviceSecondsLeft)));
            }
            if (!f.askers.empty()) meta.push_back(wxString::Format(_("%d asked"), (int)f.askers.size()));
        }
        else
        {
            switch (f.state)
            {
                case State::Heard:
                    view.stage = f.verified ? _("complete: Receive to save it") : _("heard: Receive or Ignore");
                    canReceive = true;
                    break;
                case State::Receiving:
                    if (f.verified) view.stage = _("complete; waiting for its name");
                    else if (f.slot >= 0)
                    {
                        view.stage = wxString::Format(_("asking in slot %d (in %d s)"), f.slot + 1, f.slotInSeconds);
                    }
                    else if (!f.firstInLine.empty())
                    {
                        view.stage = wxString::Format(_("repairs, round %d, %s first in line"), f.round + 1,
                                                      wxString::FromUTF8(f.firstInLine));
                    }
                    else view.stage = _("receiving");
                    break;
                default:
                {
                    wxString note;
                    bool receive = false, ignore = false;
                    view.stage = groupStateText(f, note, receive, ignore);
                    view.failed = f.state != State::Saved && f.state != State::Ignored;
                    break;
                }
            }
            meta.push_back(f.pieces > 0 ? wxString::Format(_("have %d of %d pieces"), f.have, f.pieces)
                                        : wxString::Format(_("have %d pieces"), f.have));
            meta.push_back(tempo);
            if (f.live() && f.serviceSecondsLeft > 0)
            {
                meta.push_back(wxString::Format(_("served until %s"), clockIn(f.serviceSecondsLeft)));
            }
            cancel = _("Ignore");
            canCancel = f.state == State::Heard || f.state == State::Receiving;
        }
        view.meta = joinMeta(meta);
    }

    m_transferStatus->setView(view, cancel, canCancel, canReceive);
    if (!m_transferStatus->IsShown())
    {
        m_transferStatus->Show();
        m_transferStatus->GetParent()->Layout();
    }
}

// The status area's buttons act on the transfer it shows.
void TextMessagingDialog::OnStatusCancel(wxCommandEvent& event)
{
    if (m_statusId == 0) return;
    if (!m_statusGroup)
    {
        m_menuTransferId = m_statusId;
        OnMenuCancelTransfer(event);
        return;
    }
    auto line = std::find_if(m_fileLines.begin(), m_fileLines.end(),
                             [this](const FileLine& l) { return l.group && l.groupFile.id == m_statusId; });
    if (line == m_fileLines.end()) return;
    if (line->groupFile.outgoing)
    {
        m_menuGroupFileId = m_statusId;
        OnMenuCancelGroupFile(event);
    }
    else
    {
        ignoreGroupFile(m_statusId);
    }
}

void TextMessagingDialog::OnStatusReceive(wxCommandEvent&)
{
    if (m_statusId != 0 && m_statusGroup) receiveGroupFile(m_statusId);
}

// Save as...: the system's save dialog, in the received files folder with
// the offered name filled in. Choosing a place accepts the file; backing
// out of the dialog declines it.
void TextMessagingDialog::OnOfferSave(wxCommandEvent&)
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    uint64_t id = m_offerId;
    auto line = std::find_if(m_fileLines.begin(), m_fileLines.end(),
                             [id](const FileLine& l) { return !l.group && l.transfer.id == id; });
    if (frame == nullptr || id == 0 || line == m_fileLines.end()) return;
    Data2G::FileTransfer offer = line->transfer;

    wxString folder = frame->chatReceivedFilesFolder();
    if (!wxDirExists(folder)) wxFileName::Mkdir(folder, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    wxFileDialog dialog(this, wxString::Format(_("Save %s from %s"), wxString::FromUTF8(offer.name),
                                               wxString::FromUTF8(offer.peer)),
                        folder, wxString::FromUTF8(offer.name), wxFileSelectorDefaultWildcardStr,
                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK)
    {
        frame->chatDeclineFile(id);
        setStatus(wxString::Format(_("%s from %s declined."), wxString::FromUTF8(offer.name),
                                   wxString::FromUTF8(offer.peer)));
        updateFileTransfers();
        return;
    }

    wxString path = dialog.GetPath();
    frame->chatSetReceivedFilesFolder(wxFileName(path).GetPath());
    wxString error;
    if (frame->chatAcceptFile(id, path, error))
    {
        setStatus(wxString::Format(_("Receiving %s from %s."), wxString::FromUTF8(offer.name),
                                   wxString::FromUTF8(offer.peer)));
    }
    else
    {
        setStatus(error);
    }
    if (uiLogEnabled()) log_info("UI: file id=%d to be saved as %s", (int)id, (const char*)path.utf8_str());
    updateFileTransfers();
}

void TextMessagingDialog::OnOfferDecline(wxCommandEvent&)
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (frame == nullptr || m_offerId == 0) return;
    frame->chatDeclineFile(m_offerId);
    setStatus(_("File declined."));
    updateFileTransfers();
}

void TextMessagingDialog::OnMenuSendFile(wxCommandEvent&)
{
    sendFileTo(m_menuCallsign);
}

// The file picked is queued for the station, which is then selected, as
// for a message: it is offered once the session with the station is open.
// Selecting it lets go of the station selected before, which cancels any
// file going to or from that one, so the operator is asked first.
void TextMessagingDialog::sendFileTo(const std::string& callsign)
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (frame == nullptr || callsign.empty()) return;

    wxString why;
    if (!frame->chatCanSendFiles(why))
    {
        setStatus(why);
        return;
    }
    if (!m_inhibitReason.empty())
    {
        setStatus(wxString::Format(_("Receive only. %s"), wxString::FromUTF8(m_inhibitReason)));
        return;
    }

    if (stationItem(callsign) < 0) return; // aged out while the menu was open

    wxString call = wxString::FromUTF8(callsign);
    wxFileDialog dialog(this, wxString::Format(_("Send a file to %s"), call), wxEmptyString, wxEmptyString,
                        wxFileSelectorDefaultWildcardStr, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return;
    wxString path = dialog.GetPath();

    wxULongLong size = wxFileName::GetSize(path);
    if (size != wxInvalidSize && size.GetValue() > LARGE_FILE_BYTES)
    {
        wxMessageDialog confirm(
            this,
            wxString::Format(_("%s is %s bytes. Over the air a file this large can take a long time, and "
                               "holds the channel while it goes. Send it to %s anyway?"),
                             wxFileName(path).GetFullName(), groupDigits(size.GetValue()), call),
            _("Send File"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES) return;
    }

    std::string before = selectedCallsign();
    if (!before.empty() && before != callsign && !askToLetGo(before, callsign, _("Send File"))) return;
    long item = stationItem(callsign);
    if (item < 0) return; // aged out while the dialogs were open
    if (m_stationList->GetItemState(item, wxLIST_STATE_SELECTED) == 0)
    {
        m_lettingGo = true;
        setStationSelected(item, true);
        m_lettingGo = false;
    }

    wxString error;
    if (frame->chatSendFile(callsign, path, error) == 0)
    {
        setStatus(error);
        return;
    }
    setStatus(wxString::Format(_("%s queued for %s."), wxFileName(path).GetFullName(), call), StatusKind::Queued);
    if (uiLogEnabled()) log_info("UI: file %s queued for %s", (const char*)path.utf8_str(), callsign.c_str());
    updateFileTransfers();
}

void TextMessagingDialog::OnMenuCancelTransfer(wxCommandEvent&)
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (frame == nullptr || m_menuTransferId == 0) return;
    setStatus(frame->chatCancelFile(m_menuTransferId) ? _("Transfer cancelled.")
                                                       : _("That file is no longer going."));
    updateFileTransfers();
}

//-------------------------------------------------------------------------
// Files for everybody on the GLISS group
//-------------------------------------------------------------------------


// A group file's lines: ours on the right, theirs on the left; the first
// saying what it is, with Receive... and Ignore while that is still for
// the operator to say, the second how it ended.
wxString TextMessagingDialog::groupLineHtml(const FileLine& line, const Palette& colors, bool result) const
{
    using State = Data2G::GroupFile::State;
    const Data2G::GroupFile& f = line.groupFile;
    wxString size = f.size != 0 ? ", " + groupDigits(f.size) + " " + _("bytes") : wxString();
    wxString from = wxString::FromUTF8(f.sender);

    wxString note;
    bool receive = false;
    bool ignore = false;
    wxString state = groupStateText(f, note, receive, ignore);

    wxString text;
    if (result) text = groupFileName(f) + ": " + state;
    else if (f.outgoing) text = groupFileName(f) + " " + _("to the group") + size;
    else if (f.name.empty() && f.state != State::Saved)
    {
        text = wxString::Format(_("incoming file %06X from %s, waiting for details"), (unsigned)f.fileId, from);
    }
    else text = wxString::Format(_("%s is sending %s to the group"), from, groupFileName(f)) + size;

    // The links go on the line that is last: the first while it is going,
    // the second once it has ended incomplete.
    bool last = result || line.endedAt == 0;
    wxString links;
    if (last && receive)
    {
        links += wxString::Format("<a href=\"gfile:receive:%llu\">%s</a>", (unsigned long long)f.id, _("Receive..."));
    }
    if (last && ignore)
    {
        if (!links.empty()) links += " &nbsp; ";
        links += wxString::Format("<a href=\"gfile:ignore:%llu\">%s</a>", (unsigned long long)f.id, _("Ignore"));
    }
    wxString extra;
    if (!links.empty()) extra += "<br>" + links;
    if (result && !note.empty())
    {
        extra += "<br><font size=\"-2\" color=\"" + colors.subdued + "\">" + escapeHtml(note) + "</font>";
    }
    wxString tag = f.outgoing ? wxString(_("FILE TO GROUP")) : wxString(_("GROUP FILE"));
    return fileBubble(f.outgoing, escapeHtml(text), tag, extra, result ? line.endedAt : line.at, colors.text,
                      f.outgoing ? colors.sentBubble : colors.receivedBubble, colors.subdued);
}

// Follows the group files the transport has: a new one gets its line, a
// changed one its line redrawn. Lines still going are looked at every
// time, since their countdowns move by themselves.
void TextMessagingDialog::updateGroupFiles(bool& added, bool& changed, bool& incoming)
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (frame == nullptr) return;

    uint64_t changes = frame->chatGroupFileChanges();
    bool anyLive = std::any_of(m_fileLines.begin(), m_fileLines.end(),
                               [](const FileLine& l) { return l.group && l.live(); });
    if (m_groupLinesRead && changes == m_groupFileChanges && !anyLive) return;
    m_groupLinesRead = true;
    m_groupFileChanges = changes;

    for (const Data2G::GroupFile& f : frame->chatGroupFiles())
    {
        // A cleared line stays gone, unless the engine brings the file back
        // to life (sent again, it carries on where it was left): then it
        // has a line again.
        if (m_clearedGroupFiles.count(f.id) != 0)
        {
            if (!f.live()) continue;
            m_clearedGroupFiles.erase(f.id);
        }
        auto line = std::find_if(m_fileLines.begin(), m_fileLines.end(),
                                 [&](const FileLine& l) { return l.group && l.groupFile.id == f.id; });
        if (line == m_fileLines.end())
        {
            FileLine fresh;
            fresh.at = std::time(nullptr);
            fresh.group = true;
            fresh.groupFile = f;
            fresh.readAt = steadySeconds();
            noteEnded(fresh);
            fresh.shown = fileLinesShown(fresh);
            m_fileLines.push_back(fresh);
            added = true;
            incoming = incoming || !f.outgoing;
            if (uiLogEnabled()) log_info("UI: group file line id=%d %s", (int)f.id, f.name.c_str());
            continue;
        }

        const Data2G::GroupFile& was = line->groupFile;
        if (f.state == Data2G::GroupFile::State::Failed && was.state != f.state && !f.error.empty())
        {
            setStatus(wxString::Format(_("%s failed: %s"), groupFileName(f), wxString::FromUTF8(f.error)));
        }
        else if (f.outgoing && f.state == Data2G::GroupFile::State::Ended && was.state != f.state && !f.error.empty())
        {
            setStatus(wxString::Format(_("%s: %s"), groupFileName(f), wxString::FromUTF8(f.error)));
        }
        // Sent again, it is going once more: it gets a line saying how
        // this time ends.
        if (f.live() && line->endedAt != 0)
        {
            line->endedAt = 0;
            line->endedSteady = 0.0;
        }
        line->groupFile = f;
        line->readAt = steadySeconds();
        noteEnded(*line);
        wxString shown = fileLinesShown(*line);
        if (shown != line->shown)
        {
            line->shown = shown;
            changed = true;
        }
    }
}

void TextMessagingDialog::OnMenuSendGroupFile(wxCommandEvent&)
{
    sendFileToGroup();
}

// The file picked goes to everybody on the GLISS group once the operator
// has agreed to how long it will hold the channel: above 15 minutes of air
// the box says so and names the faster tempos; above an hour, or 64 KiB,
// it does not go.
void TextMessagingDialog::sendFileToGroup()
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (frame == nullptr) return;

    wxString why;
    if (!frame->chatCanSendGroupFiles(why))
    {
        setStatus(why);
        return;
    }
    if (!m_inhibitReason.empty())
    {
        setStatus(wxString::Format(_("Receive only. %s"), wxString::FromUTF8(m_inhibitReason)));
        return;
    }

    wxFileDialog dialog(this, _("Send a file to the GLISS group"), wxEmptyString, wxEmptyString,
                        wxFileSelectorDefaultWildcardStr, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return;
    wxString path = dialog.GetPath();
    wxString name = wxFileName(path).GetFullName();

    wxULongLong size = wxFileName::GetSize(path);
    if (size == wxInvalidSize || size.GetValue() == 0)
    {
        setStatus(wxString::Format(_("%s can't be sent: it is empty or can't be read."), name));
        return;
    }
    if (size.GetValue() > Data2G::GROUP_FILE_MAX_BYTES)
    {
        wxMessageBox(wxString::Format(_("%s is %s bytes. A file for the whole group can be 64 KiB at most."), name,
                                      groupDigits(size.GetValue())),
                     _("Send File to Group"), wxOK | wxICON_INFORMATION, this);
        return;
    }

    int gear = frame->chatTransmitGear();
    if (gear < Glissando::MIN_GEAR) gear = Glissando::MIN_GEAR;
    Data2G::GroupFileEstimate estimate = frame->chatGroupFileEstimate(size.GetValue(), gear);

    // The faster tempos, for a file that would take long.
    wxString faster;
    for (int g = gear + 1; g <= Glissando::MAX_GEAR; g++)
    {
        Data2G::GroupFileEstimate at = frame->chatGroupFileEstimate(size.GetValue(), g);
        if (!faster.empty()) faster += ", ";
        faster += GlissandoConsole::gearLabel(g) + ": " + duration(at.airSeconds);
    }

    if (estimate.airSeconds > Data2G::GroupFileEngine::REFUSE_AIR_SECONDS)
    {
        wxString text = wxString::Format(_("At %s, %s would take about %s on the air. A file for the group may take "
                                           "an hour at most."),
                                         GlissandoConsole::gearLabel(gear), name, duration(estimate.airSeconds));
        if (!faster.empty()) text += "\n\n" + wxString::Format(_("Faster tempos: %s."), faster);
        wxMessageBox(text, _("Send File to Group"), wxOK | wxICON_INFORMATION, this);
        return;
    }

    wxString text = wxString::Format(
        _("%s, %s bytes, %d pieces at %s: about %s on the air (%s with the pauses between keyings), then "
          "repairs until %s at the latest. Stations will not confirm receipt."),
        name, groupDigits(size.GetValue()), estimate.pieces, GlissandoConsole::gearLabel(gear),
        duration(estimate.airSeconds), duration(estimate.wallSeconds),
        clockIn(estimate.wallSeconds + estimate.serviceSeconds));
    bool long_ = estimate.airSeconds > Data2G::GroupFileEngine::WARN_AIR_SECONDS;
    if (long_)
    {
        text += "\n\n" + _("That holds the GLISS group for a long time.");
        if (!faster.empty()) text += " " + wxString::Format(_("Faster tempos: %s."), faster);
    }
    text += "\n\n" + _("Send it to the group?");
    wxMessageDialog confirm(this, text, _("Send File to Group"),
                            wxYES_NO | (long_ ? wxNO_DEFAULT | wxICON_WARNING : wxICON_QUESTION));
    if (confirm.ShowModal() != wxID_YES) return;

    wxString error;
    if (frame->chatSendGroupFile(path, error) == 0)
    {
        setStatus(error);
        return;
    }
    setStatus(wxString::Format(_("%s is going to the group."), name), StatusKind::Queued);
    if (uiLogEnabled()) log_info("UI: file %s going to the group", (const char*)path.utf8_str());
    updateFileTransfers();
}

// Receive...: the system's save dialog, in the received files folder with
// the name the sender gave. The file is saved there once all of it is here,
// and the pieces missing are asked for until then.
void TextMessagingDialog::receiveGroupFile(uint64_t id)
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    auto line = std::find_if(m_fileLines.begin(), m_fileLines.end(),
                             [id](const FileLine& l) { return l.group && l.groupFile.id == id; });
    if (frame == nullptr || id == 0 || line == m_fileLines.end()) return;
    Data2G::GroupFile file = line->groupFile;

    wxString folder = frame->chatReceivedFilesFolder();
    if (!wxDirExists(folder)) wxFileName::Mkdir(folder, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxString name = file.name.empty() ? wxString("received-file") : wxString::FromUTF8(file.name);
    wxFileDialog dialog(this, wxString::Format(_("Save %s from %s"), name, wxString::FromUTF8(file.sender)), folder,
                        name, wxFileSelectorDefaultWildcardStr, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK) return;

    wxString path = dialog.GetPath();
    frame->chatSetReceivedFilesFolder(wxFileName(path).GetPath());
    wxString error;
    if (frame->chatReceiveGroupFile(id, path, error))
    {
        setStatus(wxString::Format(_("Receiving %s from %s."), name, wxString::FromUTF8(file.sender)));
    }
    else
    {
        setStatus(error);
    }
    if (uiLogEnabled()) log_info("UI: group file id=%d to be saved as %s", (int)id, (const char*)path.utf8_str());
    updateFileTransfers();
}

void TextMessagingDialog::ignoreGroupFile(uint64_t id)
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (frame == nullptr || id == 0) return;
    setStatus(frame->chatIgnoreGroupFile(id) ? _("File ignored.") : _("That file is no longer coming."));
    updateFileTransfers();
}

void TextMessagingDialog::OnMenuStopServing(wxCommandEvent&)
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (frame == nullptr || m_menuGroupFileId == 0) return;
    setStatus(frame->chatStopServingGroupFile(m_menuGroupFileId) ? _("No more repairs for that file.")
                                                                 : _("That file is no longer going."));
    updateFileTransfers();
}

void TextMessagingDialog::OnMenuCancelGroupFile(wxCommandEvent&)
{
    MainFrame* frame = dynamic_cast<MainFrame*>(GetParent());
    if (frame == nullptr || m_menuGroupFileId == 0) return;
    setStatus(frame->chatCancelGroupFile(m_menuGroupFileId) ? _("Transfer cancelled.")
                                                            : _("That file is no longer going."));
    updateFileTransfers();
}

void TextMessagingDialog::OnMenuReceiveGroupFile(wxCommandEvent&)
{
    receiveGroupFile(m_menuGroupFileId);
}

void TextMessagingDialog::OnMenuIgnoreGroupFile(wxCommandEvent&)
{
    ignoreGroupFile(m_menuGroupFileId);
}

// The links on a group file's line. Nothing else in the chat is a link,
// and nothing is ever loaded from one.
void TextMessagingDialog::OnChatLink(wxHtmlLinkEvent& event)
{
    wxString href = event.GetLinkInfo().GetHref();
    unsigned long long id = 0;
    if (href.StartsWith("gfile:receive:") && href.Mid(14).ToULongLong(&id)) receiveGroupFile(id);
    else if (href.StartsWith("gfile:ignore:") && href.Mid(13).ToULongLong(&id)) ignoreGroupFile(id);
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

        // Somebody's message, to us or to all, landing in a closed window.
        if (copy.kind == MessageKind::Chat && copy.direction == MessageDirection::Received && !IsShown())
        {
            if (MainFrame* frame = dynamic_cast<MainFrame*>(GetParent())) frame->noteChatUnread();
        }

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
