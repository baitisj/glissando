//=========================================================================
// Name:            dlg_snoop.h
// Purpose:         The snooping window: every message the station hears,
//                  whoever it was sent to, in the console's Chaotica dress.
//
// Written for Glissando. The chat window only shows traffic for this
// station; this one listens in on everyone else's too, from the SnoopFeed.
//=========================================================================

#ifndef __GLISSANDO_SNOOP_DIALOG__
#define __GLISSANDO_SNOOP_DIALOG__

#include <vector>

#include <wx/dialog.h>
#include <wx/html/htmlwin.h>
#include <wx/stattext.h>

#include "text_messaging/SnoopFeed.h"

namespace Chaotica
{
class Button;
}

class SnoopDialog : public wxDialog
{
public:
    SnoopDialog(wxWindow* parent, wxWindowID id = wxID_ANY,
                const wxString& title = _("Glissando Snooper"),
                const wxPoint& pos = wxDefaultPosition,
                const wxSize& size = wxSize(760, 520),
                long style = wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    virtual ~SnoopDialog();

private:
    void buildControls();
    void add(const TextMessaging::SnoopEvent& event);
    bool shown(const TextMessaging::SnoopEvent& event) const;
    void render();
    void updateTally();

    void OnClose(wxCloseEvent& event);

    wxHtmlWindow* m_log;
    Chaotica::Button* m_btnFrames;
    Chaotica::Button* m_btnClear;
    wxStaticText* m_txtTally;

    std::vector<TextMessaging::SnoopEvent> m_events;
    int m_listenerId;
};

#endif // __GLISSANDO_SNOOP_DIALOG__
