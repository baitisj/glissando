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

#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/html/htmlwin.h>
#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include "text_messaging/Data2GBroadcast.h"
#include "text_messaging/Data2GFileTransfer.h"
#include "text_messaging/TextMessagingTypes.h"
#include "text_messaging/TextMessagingProtocol.h"

namespace Chaotica
{
class Button;
class Lamp;
}

class TransferStatusArea;

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
    wxString barChip(int64_t messageId, const wxString& label, int fillPixels, const wxColour& filled);
    void refreshStations();
    std::string selectedCallsign() const;
    long stationItem(const std::string& callsign) const;
    long stationAt(const wxMouseEvent& event) const;
    void connectStationMouse(bool connect);
    void setStationSelected(long item, bool selected);
    void updateSelectionControls();
    void addStation();
    void send(const std::string& destination);
    bool queueText(const std::string& text, const std::string& destination);
    void appendMessage(const TextMessaging::TextMessage& message);
    void updateTransmitControls();
    void setColumnIfChanged(long item, int column, const wxString& text);
    int rowAt(const wxPoint& point) const;
    static std::string stationOf(const TextMessaging::TextMessage& message);
    void selectStation(const std::string& callsign, bool addIfMissing);

    // Letting go of the selected station ends a Data2G session with it and
    // cancels the files going to or from it, so whatever would change the
    // selection asks first while either is so. wanted is the station that
    // would be chosen instead; empty for none.
    bool needsAskingToLetGo(const std::string& before) const;
    bool askToLetGo(const std::string& before, const std::string& wanted, const wxString& title);
    // The same, asked once the event that wanted it is over, and the
    // selection then changed if the operator agrees.
    void askToLetGoLater(const std::string& before, const std::string& wanted);
    void chooseStation(const std::string& before, const std::string& wanted);

    // The Data2G session's far end as the station list names it, or as
    // data2g-host does when it is not listed; empty with no session.
    std::string sessionStation() const;
    bool sessionWith(const std::string& callsign) const;
    void updateSessionPlate();

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
    void updateSmokeWarning();
    wxString transmittingStatus() const;

    // Writes how long the message being typed would take on the air under
    // the send button, in red past the time-out timer. Nothing for codec2
    // or Data2G, which do not say.
    void updateAirTime();
    void updatePhraseHighlight();
    void updateSendToolTip();

    // Files through a Data2G session: each one a line of the chat, and an
    // offer to us a box over it until the operator answers.
    // A file to or from the whole GLISS group is a line too, with group
    // set and groupFile, not transfer, telling its story.
    // The chat shows two lines for each: one when it starts, and one
    // saying how it ended. How it is getting on is shown over the chat,
    // in the transfer's status area.
    struct FileLine
    {
        TextMessaging::Data2G::FileTransfer transfer;  // as last seen
        std::time_t at = 0;                             // when it first appeared
        bool group = false;
        TextMessaging::Data2G::GroupFile groupFile;     // as last seen
        double readAt = 0.0;        // steady clock seconds it was last seen
        std::time_t endedAt = 0;    // when it was first seen over; 0 while live
        double endedSteady = 0.0;   // the same on the steady clock
        double rateSince = -1.0;    // steady clock seconds bytes began to move, -1 until they did
        uint64_t rateBytes = 0;     // and how many had moved then
        wxString shown;             // its lines as the chat last drew them

        bool live() const { return group ? groupFile.live() : transfer.live(); }
    };
    void updateFileTransfers();
    void updateGroupFiles(bool& added, bool& changed, bool& incoming);
    void updateOfferBox();
    void paintOfferBox(wxPaintEvent& event);
    void updateTransferStatus();
    // The line it starts with, or (result) the one saying how it ended.
    wxString fileLineHtml(const FileLine& line, const Palette& colors, bool result) const;
    wxString groupLineHtml(const FileLine& line, const Palette& colors, bool result) const;
    // Both, as the chat would draw them now: when this changes, it is drawn again.
    wxString fileLinesShown(const FileLine& line) const;
    // When a line ended, the first time it is seen over.
    static void noteEnded(FileLine& line);
    void sendFileTo(const std::string& callsign);
    // Files going to or from the station, which letting go of it cancels.
    int liveFilesWith(const std::string& callsign) const;
    void sendFileToGroup();
    void receiveGroupFile(uint64_t id);
    void ignoreGroupFile(uint64_t id);

    void OnSend(wxCommandEvent& event);
    void OnPing(wxCommandEvent& event);
    void OnStationSelected(wxListEvent& event);
    void OnStationDeselected(wxListEvent& event);
    void OnStationLeftDown(wxMouseEvent& event);
    void OnStationRightDown(wxMouseEvent& event);
    void OnMenuSelectStation(wxCommandEvent& event);
    void OnMenuRemoveStation(wxCommandEvent& event);
    void OnChatDoubleClick(wxMouseEvent& event);
    void OnDisconnect(wxCommandEvent& event);
    void OnChatContextMenu(wxContextMenuEvent& event);
    void OnMenuCancelMessage(wxCommandEvent& event);
    void OnMenuWoah(wxCommandEvent& event);
    void OnMenuResend(wxCommandEvent& event);
    void OnMenuTempo(wxCommandEvent& event);
    void OnMenuClearMessages(wxCommandEvent& event);
    void OnMenuSendFile(wxCommandEvent& event);
    void OnMenuCancelTransfer(wxCommandEvent& event);
    void OnMenuSendGroupFile(wxCommandEvent& event);
    void OnMenuStopServing(wxCommandEvent& event);
    void OnMenuCancelGroupFile(wxCommandEvent& event);
    void OnMenuReceiveGroupFile(wxCommandEvent& event);
    void OnMenuIgnoreGroupFile(wxCommandEvent& event);
    void OnChatLink(wxHtmlLinkEvent& event);
    void OnOfferSave(wxCommandEvent& event);
    void OnOfferDecline(wxCommandEvent& event);
    void OnStatusCancel(wxCommandEvent& event);
    void OnStatusReceive(wxCommandEvent& event);
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

    // A Data2G session's lamp and Disconnect button on the station plate,
    // shown only while one is connected.
    wxSizer* m_stationSizer = nullptr;
    wxSizer* m_sessionSizer = nullptr;
    Chaotica::Lamp* m_sessionLamp = nullptr;
    WrappingText* m_sessionText = nullptr;
    std::string m_sessionShown;     // the far end the plate shows; empty while hidden
    wxHtmlWindow* m_chatWindow;
    wxTextCtrl* m_txtEntry;
    Chaotica::Button* m_btnSend;
    wxString m_sendToolTip;         // where the message goes
    wxString m_airTimeToolTip;      // and how long it takes, when that is long
    bool m_phraseHighlighted = false; // some of the entry text has the accelerated background
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
        int gear = 0;             // the tempo it will key at; 0 if not Glissando
        bool tempoChosen = false; // the operator moved it to that tempo
    };
    std::map<int64_t, QueueBar> m_queueBars;
    std::vector<wxString> m_queueBarImages;     // on the page now
    std::vector<wxString> m_newQueueBarImages;  // for the page being built
    unsigned m_queueBarGeneration = 0;
    int m_sendFillPixels = -1; // the bar behind SENDING, as last drawn; -1 for none

    // Remembered so the one second timer only touches the controls when the
    // transmitter's state, or whether it may be used, actually changes,
    // rather than on every tick.
    bool m_transmitting;
    bool m_smoking = false;         // the visi-scope is smoking: the status line says so
    bool m_transmitControlsDisabled;
    std::string m_inhibitReason;
    StatusKind m_statusKind;
    TextMessaging::AckWait m_lastAckWait;

    // The station last picked in the list, as the console's map was told:
    // it is told only when that changes.
    std::string m_mapPick;

    // The station the context menu was opened on. Looked up again by name
    // when an item is chosen, since the list may have changed underneath.
    std::string m_menuCallsign;

    // The selection is being changed with the operator's leave (or for
    // a reason that needs none), so letting go of a station asks nothing.
    bool m_lettingGo = false;
    // The selection is being put back while the operator is asked.
    bool m_restoringSelection = false;
    bool m_letGoAsked = false;      // a question is on its way

    // The message the chat log's menu was opened on, by store id; 0 when it
    // offered nothing to remove or abort.
    int64_t m_menuMessageId = 0;
    TextMessaging::TextMessage m_menuResend; // the message Re-send would copy; id 0 for none

    std::vector<TextMessaging::TextMessage> m_messages;

    // Files sent and received this run, oldest first.
    std::vector<FileLine> m_fileLines;
    uint64_t m_fileChanges = 0;     // the transport's count when they were last read
    bool m_fileLinesRead = false;
    std::set<uint64_t> m_clearedFiles; // finished ones Clear Messages took away, not to come back
    uint64_t m_menuTransferId = 0;  // the file line the chat log's menu was opened on; 0 for none

    // Files to and from the group, which the transport numbers apart.
    uint64_t m_groupFileChanges = 0;
    bool m_groupLinesRead = false;
    std::set<uint64_t> m_clearedGroupFiles;
    uint64_t m_menuGroupFileId = 0; // the group file line the chat log's menu was opened on; 0 for none

    // The box over the chat asking whether to save a file offered to us,
    // red, its border flashing, counting down to the offer's expiry.
    wxPanel* m_offerBox = nullptr;
    WrappingText* m_offerText = nullptr;
    wxStaticText* m_offerCountdown = nullptr;
    uint64_t m_offerId = 0;         // the offer it shows; 0 while hidden
    bool m_offerLit = false;        // its border, as last painted
    std::set<uint64_t> m_offersRaised; // offers COMMS has come forward for

    // The transfer under way, over the chat: one at a time, the newest.
    TransferStatusArea* m_transferStatus = nullptr;
    bool m_statusGroup = false;     // the one it shows, or last showed
    uint64_t m_statusId = 0;

    // What each line of the chat is, top to bottom, for a click to find:
    // a message (an index into m_messages) or a file (into m_fileLines).
    struct ChatRow
    {
        bool file = false;
        size_t index = 0;
        bool result = false;    // a file's line saying how it ended
    };
    std::vector<ChatRow> m_rows;
};

#endif // __FDV_TEXT_MESSAGING_DIALOG__
