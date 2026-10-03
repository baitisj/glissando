//=========================================================================
// Name:            dlg_text_messaging.h
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

#ifndef __FDV_TEXT_MESSAGING_DIALOG__
#define __FDV_TEXT_MESSAGING_DIALOG__

#include <map>
#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/html/htmlwin.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include "text_messaging/TextMessagingTypes.h"
#include "text_messaging/TextMessagingProtocol.h"

namespace Chaotica
{
class Button;
}

// A label that wraps to whatever width the layout gives it, and wraps again
// when the window is resized, rather than running off the edge. Set its text
// with setText(), which keeps the unwrapped text to wrap from.
class WrappingText : public wxStaticText
{
public:
    explicit WrappingText(wxWindow* parent);

    void setText(const wxString& text);
    const wxString& text() const { return m_text; }

private:
    void rewrap(int width);
    void OnSize(wxSizeEvent& event);

    wxString m_text;
    int m_wrapWidth;
};

// The chat window: who has been heard, what has been said, and a place to say
// something back. All protocol work happens in the session, which keeps
// running when this window is closed; the dialog only observes it.
class TextMessagingDialog : public wxDialog, public TextMessaging::ITextMessagingObserver
{
public:
    TextMessagingDialog(wxWindow* parent, wxWindowID id = wxID_ANY,
                        const wxString& title = _("Glissando COMMS"),
                        const wxPoint& pos = wxDefaultPosition,
                        const wxSize& size = wxSize(900, 620),
                        long style = wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    virtual ~TextMessagingDialog();

    // Reloads history and the operator's callsign; called before showing.
    void refreshFromSession();

    // ITextMessagingObserver. These arrive on the session's thread and hand
    // the work to the GUI thread.
    virtual void onMessageAdded(const TextMessaging::TextMessage& message) override;
    virtual void onMessageUpdated(const TextMessaging::TextMessage& message) override;
    virtual void onStationsChanged() override;

private:
    // The console's silver screen greys, as HTML colours for the chat page.
    struct Palette
    {
        wxString page;
        wxString text;
        wxString sentBubble;
        wxString receivedBubble;
        wxString subdued;
    };

    void buildControls();
    Palette palette() const;
    // keepPlace leaves the view where the operator has scrolled it, for a
    // redraw that changes no message, only how a chip looks.
    void renderChat(bool keepPlace = false);
    void updateEngageChips();
    void updateQueueBars();
    wxString queueBarChip(const TextMessaging::TextMessage& message);
    void refreshStations();
    std::string selectedCallsign() const;
    long stationItem(const std::string& callsign) const;
    long stationAt(const wxMouseEvent& event) const;
    void connectStationMouse(bool connect);
    void setStationSelected(long item, bool selected);
    void updateSelectionControls();
    void addStation();
    void send(const std::string& destination);
    void appendMessage(const TextMessaging::TextMessage& message);
    void updateTransmitControls();
    void setColumnIfChanged(long item, int column, const wxString& text);
    int messageAt(const wxPoint& point) const;
    static std::string stationOf(const TextMessaging::TextMessage& message);
    void selectStation(const std::string& callsign, bool addIfMissing);

    // The status line says one of three kinds of thing, and each stops being
    // true at a different moment.
    enum class StatusKind
    {
        Sticky,   // errors and standing notices: stay until something replaces them
        Queued,   // "... queued": stops being true once the transmitter keys
        Activity, // "Transmitting": stops being true when the burst ends
        AckWait,  // "Awaiting ...": stops being true when the cycle ends
    };

    void setStatus(const wxString& status, StatusKind kind = StatusKind::Sticky);
    void updateAckWaitStatus();
    void updateModemStatus();

    // Writes how long the message being typed would take on the air under
    // the send button, in red past the time-out timer. Nothing for codec2
    // or Data2G, which do not say.
    void updateAirTime();
    void updateSendToolTip();

    void OnSend(wxCommandEvent& event);
    void OnPing(wxCommandEvent& event);
    void OnStationSelected(wxListEvent& event);
    void OnStationDeselected(wxListEvent& event);
    void OnStationLeftDown(wxMouseEvent& event);
    void OnStationRightDown(wxMouseEvent& event);
    void OnMenuSelectStation(wxCommandEvent& event);
    void OnMenuRemoveStation(wxCommandEvent& event);
    void OnChatLeftDown(wxMouseEvent& event);
    void OnChatLeftUp(wxMouseEvent& event);
    void OnChatContextMenu(wxContextMenuEvent& event);
    void OnMenuCancelMessage(wxCommandEvent& event);
    void OnMenuClearMessages(wxCommandEvent& event);
    void OnAddStationText(wxCommandEvent& event);
    void OnAddStation(wxCommandEvent& event);
    void OnAutoReplyToggled(wxCommandEvent& event);
    void OnEntryKeyDown(wxKeyEvent& event);
    void OnEntryText(wxCommandEvent& event);
    void OnTimer(wxTimerEvent& event);
    void OnBlinkTimer(wxTimerEvent& event);
    void OnClose(wxCloseEvent& event);

    wxListCtrl* m_stationList;
    wxTextCtrl* m_txtAddStation;
    Chaotica::Button* m_btnAddStation;
    Chaotica::Button* m_btnPing;
    wxHtmlWindow* m_chatWindow;
    wxTextCtrl* m_txtEntry;
    Chaotica::Button* m_btnSend;
    wxString m_sendToolTip;         // where the message goes
    wxString m_airTimeToolTip;      // and how long it takes, when that is long
    Chaotica::Button* m_chkAutoReply;
    WrappingText* m_txtStatus;
    WrappingText* m_txtInhibited;
    WrappingText* m_txtModem;       // Data2G's connection, while chat uses it
    wxTimer m_refreshTimer;
    wxTimer m_blinkTimer;

    // With the console disengaged a queued message's chip reads ENGAGE TO
    // SEND and flashes red, in step with the Engage button.
    bool m_waitingForEngage = false;
    bool m_engageChipLit = false;

    // Otherwise it counts down: a bar the length of the wait it had when it
    // joined the queue, shrinking to nothing as its turn comes. Drawn as a
    // picture the chat page loads from memory, one name per redraw.
    struct QueueBar
    {
        int64_t totalMs = 0;     // the longest wait it has been given
        int64_t remainingMs = 0;
        bool channelBusy = false; // held still, and dimmed
        int fillPixels = -1;      // as last drawn
    };
    std::map<int64_t, QueueBar> m_queueBars;
    std::vector<wxString> m_queueBarImages;     // on the page now
    std::vector<wxString> m_newQueueBarImages;  // for the page being built
    unsigned m_queueBarGeneration = 0;

    // Remembered so the one second timer only touches the controls when the
    // transmitter's state, or whether it may be used, actually changes,
    // rather than on every tick.
    bool m_transmitting;
    bool m_transmitControlsDisabled;
    std::string m_inhibitReason;
    StatusKind m_statusKind;
    TextMessaging::AckWait m_lastAckWait;

    // The station the context menu was opened on. Looked up again by name
    // when an item is chosen, since the list may have changed underneath.
    std::string m_menuCallsign;

    // Where the mouse went down on the chat log, to tell a click on a
    // message from a drag selecting its text.
    wxPoint m_chatPressAt;

    // The message the chat log's menu was opened on, by store id; 0 when it
    // offered nothing to remove or abort.
    int64_t m_menuMessageId = 0;

    std::vector<TextMessaging::TextMessage> m_messages;
};

#endif // __FDV_TEXT_MESSAGING_DIALOG__
