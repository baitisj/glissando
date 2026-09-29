//==========================================================================
// Name:            topFrame.cpp
//
// Purpose:         Implements simple wxWidgets application with GUI.
// Created:         Apr. 9, 2012
// Authors:         David Rowe, David Witten
//
// License:
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//==========================================================================

#include <map>
#include <set>

#include <wx/regex.h>
#include <wx/wrapsizer.h>
#include <wx/aui/tabmdi.h>
#include <wx/numformatter.h>

#include "topFrame.h"

#if !wxCHECK_VERSION(3, 3, 0)
#include <set>
#endif // !wxCHECK_VERSION(3, 3, 0)

#include "gui/util/NameOverrideAccessible.h"
#include "gui/util/LabelOverrideAccessible.h"
#include "util/logging/ulog.h"

#if defined(__WXGTK__) && defined(HAS_GTK3)
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif // GDK_WINDOWING_WAYLAND
#endif // defined(__WXGTK__) && defined(HAS_GTK3)


wxPoint LeftOffsetContextMenuPosition(wxWindow* btn)
{
#if defined(__WXGTK__) && defined(HAS_GTK3) && defined(GDK_WINDOWING_WAYLAND)
    GdkDisplay* display = gdk_display_get_default();
    if (display != nullptr && GDK_IS_WAYLAND_DISPLAY(display))
    {
        return wxDefaultPosition;
    }
#endif // defined(__WXGTK__) && defined(HAS_GTK3) && defined(GDK_WINDOWING_WAYLAND)

    auto sz = btn->GetSize();
    return wxPoint(-sz.GetWidth() - 25, 0);
}

extern int g_recFileFromRadioEventId;
extern int g_playFileFromRadioEventId;
extern int g_recFileFromModulatorEventId;
extern int g_txLevel;

#define MIC_SPKR_LEVEL_FORMAT_STR "%s%s"
#define DECIBEL_STR "dB"

#if !wxCHECK_VERSION(3, 3, 0)
// THIS IS VERY MUCH A HACK! wxTabFrame is not in the public interface and should
// not be here, even named as something else. Unfortunately this is needed to get
// the tab state loaded and saved on wxWidgets versions older than 3.3, which lack
// wxAuiNotebook::SaveLayout()/LoadLayout(). Here's hoping this interface remains stable.
//
// (Last retrieved from wxWidgets 3.0.5.1 on August 8, 2023.)
class wxTabFrameOurs : public wxWindow
{
public:

    wxTabFrameOurs()
    {
        m_tabs = NULL;
        m_rect = wxRect(0,0,200,200);
        m_tabCtrlHeight = 20;
    }

    ~wxTabFrameOurs()
    {
        wxDELETE(m_tabs);
    }

    void SetTabCtrlHeight(int h)
    {
        m_tabCtrlHeight = h;
    }

protected:
    void DoSetSize(int x, int y,
                   int width, int height,
                   int WXUNUSED(sizeFlags = wxSIZE_AUTO))
    {
        m_rect = wxRect(x, y, width, height);
        DoSizing();
    }

    void DoGetClientSize(int* x, int* y) const
    {
        *x = m_rect.width;
        *y = m_rect.height;
    }

public:
    bool Show( bool WXUNUSED(show = true) ) { return false; }

    void DoSizing()
    {
        if (!m_tabs)
            return;

        if (m_tabs->IsFrozen() || m_tabs->GetParent()->IsFrozen())
            return;

        m_tab_rect = wxRect(m_rect.x, m_rect.y, m_rect.width, m_tabCtrlHeight);
        if (m_tabs->GetFlags() & wxAUI_NB_BOTTOM)
        {
            m_tab_rect = wxRect (m_rect.x, m_rect.y + m_rect.height - m_tabCtrlHeight, m_rect.width, m_tabCtrlHeight);
            m_tabs->SetSize     (m_rect.x, m_rect.y + m_rect.height - m_tabCtrlHeight, m_rect.width, m_tabCtrlHeight);
            m_tabs->SetRect     (wxRect(0, 0, m_rect.width, m_tabCtrlHeight));
        }
        else //TODO: if (GetFlags() & wxAUI_NB_TOP)
        {
            m_tab_rect = wxRect (m_rect.x, m_rect.y, m_rect.width, m_tabCtrlHeight);
            m_tabs->SetSize     (m_rect.x, m_rect.y, m_rect.width, m_tabCtrlHeight);
            m_tabs->SetRect     (wxRect(0, 0,        m_rect.width, m_tabCtrlHeight));
        }
        // TODO: else if (GetFlags() & wxAUI_NB_LEFT){}
        // TODO: else if (GetFlags() & wxAUI_NB_RIGHT){}

        m_tabs->Refresh();
        m_tabs->Update();

        auto& pages = m_tabs->GetPages();
        size_t i, page_count = pages.GetCount();

        for (i = 0; i < page_count; ++i)
        {
            wxAuiNotebookPage& page = pages.Item(i);
            int border_space = m_tabs->GetArtProvider()->GetAdditionalBorderSpace(page.window);

            int height = m_rect.height - m_tabCtrlHeight - border_space;
            if ( height < 0 )
            {
                // avoid passing negative height to wxWindow::SetSize(), this
                // results in assert failures/GTK+ warnings
                height = 0;
            }
            int width = m_rect.width - 2 * border_space;
            if (width < 0)
                width = 0;

            if (m_tabs->GetFlags() & wxAUI_NB_BOTTOM)
            {
                page.window->SetSize(m_rect.x + border_space,
                                     m_rect.y + border_space,
                                     width,
                                     height);
            }
            else //TODO: if (GetFlags() & wxAUI_NB_TOP)
            {
                page.window->SetSize(m_rect.x + border_space,
                                     m_rect.y + m_tabCtrlHeight,
                                     width,
                                     height);
            }
            // TODO: else if (GetFlags() & wxAUI_NB_LEFT){}
            // TODO: else if (GetFlags() & wxAUI_NB_RIGHT){}
        }
    }

protected:
    void DoGetSize(int* x, int* y) const
    {
        if (x)
            *x = m_rect.GetWidth();
        if (y)
            *y = m_rect.GetHeight();
    }

public:
    void Update()
    {
        // does nothing
    }

    wxRect m_rect;
    wxRect m_tab_rect;
    wxAuiTabCtrl* m_tabs;
    int m_tabCtrlHeight;
};
#endif // !wxCHECK_VERSION(3, 3, 0)

TabFreeAuiNotebook::TabFreeAuiNotebook() : wxAuiNotebook()
{
    // XXX - FreeDV only supports English but makes a best effort to at least use regional formatting
    // for e.g. numbers. Thus, we only need to override layout direction.
    SetLayoutDirection(wxLayout_LeftToRight);
}
TabFreeAuiNotebook::TabFreeAuiNotebook(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
        : wxAuiNotebook(parent, id, pos, size, style) { }

bool TabFreeAuiNotebook::AcceptsFocus() const { return false; }
bool TabFreeAuiNotebook::AcceptsFocusFromKeyboard() const { return false; }
bool TabFreeAuiNotebook::AcceptsFocusRecursively() const { return false; }

#if !wxCHECK_VERSION(3, 3, 0)
// SavePerspective and LoadPerspective below credit https://forums.kirix.com/viewtopicdafe.html?f=15&t=542
// with minor modifications to make it compile on modern wxWidgets.
wxString TabFreeAuiNotebook::SavePerspective() {
    // Build list of panes/tabs
    wxString tabs;
    wxAuiPaneInfoArray& all_panes = m_mgr.GetAllPanes();
     const size_t pane_count = all_panes.GetCount();

     for (size_t i = 0; i < pane_count; ++i)
     {
       wxAuiPaneInfo& pane = all_panes.Item(i);
       if (pane.name == wxT("dummy"))
             continue;

         wxTabFrameOurs* tabframe = (wxTabFrameOurs*)pane.window;

       if (!tabs.empty()) tabs += wxT("|");
       tabs += pane.name;
       tabs += wxT("=");
  
       // Add tabs, keyed by caption rather than position. Position (AddPage() call
       // order) isn't stable across app versions if a tab is ever added, removed, or
       // reordered, which would silently corrupt previously-saved layouts.
       size_t page_count = tabframe->m_tabs->GetPageCount();
       for (size_t p = 0; p < page_count; ++p)
       {
          wxAuiNotebookPage& page = tabframe->m_tabs->GetPage(p);
          const size_t page_idx = m_tabs.GetIdxFromWindow(page.window);

          if (p) tabs += wxT(",");

          if ((int)page_idx == m_curPage) tabs += wxT("*");
          else if ((int)p == tabframe->m_tabs->GetActivePage()) tabs += wxT("+");
          tabs += page.caption;
       }
    }
    tabs += wxT("@");

    // Add frame perspective
    tabs += m_mgr.SavePerspective();

    return tabs;
}

bool TabFreeAuiNotebook::LoadPerspective(const wxString& layout) {
    // Remove all tab ctrls (but still keep them in main index)
    const size_t tab_count = m_tabs.GetPageCount();
    std::set<wxString> readdedTabs;

    // Caption -> master index lookup, so saved entries resolve by tab identity
    // rather than by position (see SavePerspective()).
    std::map<wxString, size_t> captionToIdx;
    for (size_t i = 0; i < tab_count; ++i) {
        captionToIdx[m_tabs.GetPage(i).caption] = i;
    }

    for (size_t i = 0; i < tab_count; ++i) {
       wxWindow* wnd = m_tabs.GetWindowFromIdx(i);

       // find out which onscreen tab ctrl owns this tab
       wxAuiTabCtrl* ctrl;
       int ctrl_idx;
       if (!FindTab(wnd, &ctrl, &ctrl_idx))
          return false;

       // remove the tab from ctrl
       if (!ctrl->RemovePage(wnd))
          return false;
    }
    RemoveEmptyTabFrames();

    size_t sel_page = 0;

    // Creates a new (empty) tab group pane, docked at the bottom, named paneName.
    auto createTabGroup = [&](const wxString& paneName) -> wxAuiTabCtrl* {
        wxTabFrameOurs* new_tabs = new wxTabFrameOurs();
        new_tabs->m_tabs = new wxAuiTabCtrl(this, m_tabIdCounter++);
        new_tabs->m_tabs->SetArtProvider(m_tabs.GetArtProvider()->Clone());
        new_tabs->m_tabCtrlHeight = m_tabCtrlHeight;
        new_tabs->m_tabs->SetFlags(m_flags);

        wxAuiPaneInfo pane_info = wxAuiPaneInfo().Name(paneName).Bottom().CaptionVisible(false);
        m_mgr.AddPane(new_tabs, pane_info);

        return new_tabs->m_tabs;
    };

    wxString tabs = layout.BeforeFirst(wxT('@'));
    wxAuiTabCtrl *dest_tabs = nullptr; // last group known to hold >= 1 page
    bool anyEmptyGroupsCreated = false;
    while (1)
     {
       const wxString tab_part = tabs.BeforeFirst(wxT('|'));

       // if the string is empty, we're done parsing
         if (tab_part.empty())
             break;

       // Get pane name
       const wxString pane_name = tab_part.BeforeFirst(wxT('='));
       wxAuiTabCtrl* new_group = createTabGroup(pane_name);

       // Get list of tab id's and move them to pane
       wxString tab_list = tab_part.AfterFirst(wxT('='));
       ssize_t activePage = -1;
       while(1) {
          wxString tab = tab_list.BeforeFirst(wxT(','));
          if (tab.empty()) break;
          tab_list = tab_list.AfterFirst(wxT(','));

          // Check if this page has an 'active' marker
          const wxChar c = tab[0];
          if (c == wxT('+') || c == wxT('*')) {
             tab = tab.Mid(1);
          }

          auto captionIt = captionToIdx.find(tab);
          if (captionIt == captionToIdx.end()) continue; // tab no longer exists (e.g. removed in a newer version)
          const size_t tab_idx = captionIt->second;

          // Move tab to pane
          wxAuiNotebookPage& page = m_tabs.GetPage(tab_idx);
          const size_t newpage_idx = new_group->GetPageCount();
          new_group->InsertPage(page.window, page, newpage_idx);
          readdedTabs.insert(tab);

          if (c == wxT('+')) activePage = newpage_idx;
          else if ( c == wxT('*')) sel_page = tab_idx;
       }

       if (new_group->GetPageCount() == 0)
       {
           // None of this group's saved entries resolved to a current tab (e.g. an
           // old, pre-caption-keyed saved layout - see SavePerspective()). Leaving a
           // zero-page tab group behind confuses wxAuiNotebook's own drag-and-drop
           // handling (crashes with an assertion failure in GetPage() when the user
           // later drags a tab near it), so sweep it up below instead.
           anyEmptyGroupsCreated = true;
       }
       else
       {
           if (activePage >= 0) new_group->SetActivePage(activePage);
           new_group->DoShowHide();
           dest_tabs = new_group;
       }

       tabs = tabs.AfterFirst(wxT('|'));
    }

    if (anyEmptyGroupsCreated)
    {
        RemoveEmptyTabFrames();
    }

    // Load the frame perspective
    const wxString frames = layout.AfterFirst(wxT('@'));
    bool framesLoaded = m_mgr.LoadPerspective(frames);

    bool ok = true;
    if (dest_tabs == nullptr)
    {
        // Saved layout string parsed to zero tab groups (e.g. empty/corrupted). Rather
        // than crash on the dereference below, fall back to one default group holding
        // every tab, matching what a first-run/no-saved-layout state looks like.
        log_warn("Tab layout persistence: saved layout produced no tab groups; falling back to default layout.");
        dest_tabs = createTabGroup(wxT("default"));
        ok = false;
    }
    else if (!framesLoaded)
    {
        log_warn("Tab layout persistence: failed to restore frame perspective; layout may not match what was saved.");
        ok = false;
    }

    // Reinsert tabs that weren't persisted before
    for (size_t i = 0; i < tab_count; ++i) {
        wxAuiNotebookPage& page = m_tabs.GetPage(i);
        if (readdedTabs.find(page.caption) != readdedTabs.end())
        {
            continue;
        }
        const size_t newpage_idx = dest_tabs->GetPageCount();
        dest_tabs->InsertPage(page.window, page, newpage_idx);
    }

    // Force refresh of selection
    m_curPage = -1;
    SetSelection(sel_page);

    return ok;
}
#endif // !wxCHECK_VERSION(3, 3, 0)

//=========================================================================
// Code that lays out the main application window
//=========================================================================
TopFrame::TopFrame(wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style) : wxFrame(parent, id, title, pos, size, style)
{
    // XXX - FreeDV only supports English but makes a best effort to at least use regional formatting
    // for e.g. numbers. Thus, we only need to override layout direction.
    SetLayoutDirection(wxLayout_LeftToRight);
    
#if wxUSE_ACCESSIBILITY
    // Initialize accessibility logic
    SetAccessible(new NameOverrideAccessible([&]() {
        auto labelStr = GetLabel(); // note: should be equivalent to title.

        // Ensures NVDA reads back version numbers as "x point y ..." rather
        // than as a date.
        wxRegEx rePoint("\\.");
        rePoint.ReplaceAll(&labelStr, _(" point "));
        
        return labelStr;
    }));
#endif // wxUSE_ACCESSIBILITY
    
    this->SetSizeHints(wxDefaultSize, wxDefaultSize);
    
    //=====================================================
    // Menubar Setup
    //=====================================================
    m_menubarMain = new wxMenuBar(wxMB_DOCKABLE);
    file = new wxMenu();

#if !defined(__WXGTK__)
    /* "On Top" isn't reliable on Linux, so there's no point in having it visible. */
    wxMenuItem* m_menuItemOnTop;
    m_menuItemOnTop = new wxMenuItem(file, wxID_ANY, wxString(_("Keep &On Top")) , _("Always keeps FreeDV above other windows"), wxITEM_CHECK);
    file->Append(m_menuItemOnTop);
    this->Connect(m_menuItemOnTop->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnTop));
#endif // !defined(__WXGTK__)

    wxMenuItem* m_menuItemExit;
    m_menuItemExit = new wxMenuItem(file, wxID_EXIT, wxString(_("E&xit")) , _("Exit Program"), wxITEM_NORMAL);
    file->Append(m_menuItemExit);

    m_menubarMain->Append(file, _("&File"));

    tools = new wxMenu();
    wxMenuItem* m_menuItemEasySetup;
    m_menuItemEasySetup = new wxMenuItem(tools, wxID_ANY, wxString(_("&Easy Setup...")) , _("Simplified setup of FreeDV"), wxITEM_NORMAL);
    tools->Append(m_menuItemEasySetup);

    wxMenuItem* m_menuItemTextMessaging;
    m_menuItemTextMessaging = new wxMenuItem(tools, wxID_ANY, wxString(_("&Text Chat...")) , _("Opens the text chat window."), wxITEM_NORMAL);
    tools->Append(m_menuItemTextMessaging);

    wxMenuItem* m_menuItemGlissando;
    m_menuItemGlissando = new wxMenuItem(tools, wxID_ANY, wxString(_("&Glissando Console...")) , _("Opens the Glissando melodic chirp mode console; text chat then goes out as Glissando."), wxITEM_NORMAL);
    tools->Append(m_menuItemGlissando);
    
    wxMenuItem* toolsSeparator1 = new wxMenuItem(tools, wxID_SEPARATOR);
    tools->Append(toolsSeparator1);
    
    wxMenuItem* m_menuItemAudio;
    m_menuItemAudio = new wxMenuItem(tools, wxID_ANY, wxString(_("&Audio Config...")) , _("Configures sound cards for FreeDV"), wxITEM_NORMAL);
    tools->Append(m_menuItemAudio);

    wxMenuItem* m_menuItemRigCtrlCfg;
    m_menuItemRigCtrlCfg = new wxMenuItem(tools, wxID_ANY, wxString(_("CAT and P&TT Config...")) , _("Configures FreeDV integration with radio"), wxITEM_NORMAL);
    tools->Append(m_menuItemRigCtrlCfg);

    wxMenuItem* m_menuItemOptions;
    m_menuItemOptions = new wxMenuItem(tools, wxID_ANY, wxString(_("&Options...")) , _("Miscellaneous FreeDV configuration options"), wxITEM_NORMAL);
    tools->Append(m_menuItemOptions);

    
    wxMenuItem* toolsSeparator2 = new wxMenuItem(tools, wxID_SEPARATOR);
    tools->Append(toolsSeparator2);

    m_menuItemPlayFileFromRadio = new wxMenuItem(tools, wxID_ANY, wxString(_("Start &Play File - From Radio...")) , _("Pipes radio sound input from file"), wxITEM_NORMAL);
    g_playFileFromRadioEventId = m_menuItemPlayFileFromRadio->GetId();
    tools->Append(m_menuItemPlayFileFromRadio);

    wxMenuItem* toolsSeparator3 = new wxMenuItem(tools, wxID_SEPARATOR);
    tools->Append(toolsSeparator3);

    m_menuItemExportConfig = new wxMenuItem(tools, wxID_ANY, wxString(_("&Export Configuration...")) , _("Exports the current FreeDV configuration to a file"), wxITEM_NORMAL);
    tools->Append(m_menuItemExportConfig);

    m_menuItemImportConfig = new wxMenuItem(tools, wxID_ANY, wxString(_("&Use Configuration...")) , _("Loads a FreeDV configuration from a file"), wxITEM_NORMAL);
    tools->Append(m_menuItemImportConfig);

    wxMenuItem* m_menuItemLoadDefaultConfig;
    m_menuItemLoadDefaultConfig = new wxMenuItem(tools, wxID_ANY, wxString(_("Load &Default Configuration")) , _("Resets FreeDV to its default configuration"), wxITEM_NORMAL);
    tools->Append(m_menuItemLoadDefaultConfig);

    m_menubarMain->Append(tools, _("&Tools"));

    this->SetMenuBar(m_menubarMain);

    m_panel = new wxPanel(this);

    wxBoxSizer* bSizer1;
    bSizer1 = new wxBoxSizer(wxHORIZONTAL);

    //=====================================================
    // Left side
    //=====================================================
    wxSizer* leftOuterSizer = new wxBoxSizer(wxVERTICAL);
    wxSizer* leftSizer = new wxWrapSizer(wxVERTICAL, wxREMOVE_LEADING_SPACES);

    //------------------------------
    // Signal Level(vert. bargraph)
    //------------------------------
    wxStaticBoxSizer* levelSizer;
    wxStaticBox* levelBox = new wxStaticBox(m_panel, wxID_ANY, _("Level"), wxDefaultPosition, wxSize(100,-1));
    levelSizer = new wxStaticBoxSizer(levelBox, wxHORIZONTAL);

    m_gaugeLevel = new wxGauge(levelBox, wxID_ANY, 100, wxDefaultPosition, wxSize(135,15), wxGA_SMOOTH);
    m_gaugeLevel->SetToolTip(_("Peak of From Radio."));
    levelSizer->Add(m_gaugeLevel, 1, wxALIGN_CENTER_VERTICAL|static_cast<int>(wxALL), 10);
    
    leftSizer->Add(levelSizer, 0, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 2);
    
    leftSizer->SetMinSize(wxSize(-1, 375));
    
#if !wxCHECK_VERSION(3,2,0)
    leftOuterSizer->Add(leftSizer, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND) | wxFIXED_MINSIZE, 1);
#else
    leftOuterSizer->Add(leftSizer, 2, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND) | wxFIXED_MINSIZE, 1);
#endif // !wxCHECK_VERSION(3,2,0)

    bSizer1->Add(leftOuterSizer, 0, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 5);

    //=====================================================
    // Center Section
    //=====================================================
    wxBoxSizer* centerSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* upperSizer = new wxBoxSizer(wxVERTICAL);

    //=====================================================
    // Tabbed Notebook control containing display graphs
    //=====================================================

    long nb_style = wxAUI_NB_BOTTOM | wxAUI_NB_TAB_SPLIT | wxAUI_NB_TAB_MOVE | wxAUI_NB_SCROLL_BUTTONS;
    m_auiNbookCtrl = new TabFreeAuiNotebook(m_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, nb_style);
    // This line sets the fontsize for the tabs on the notebook control
    m_auiNbookCtrl->SetMinSize(wxSize(375,375));

    upperSizer->Add(m_auiNbookCtrl, 1, wxALIGN_TOP|static_cast<int>(wxEXPAND), 1);
    centerSizer->Add(upperSizer, 1, wxALIGN_TOP|static_cast<int>(wxEXPAND), 0);

    // lower middle used for user ID

    wxBoxSizer* lowerSizer;
    lowerSizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer* modeStatusSizer;
    modeStatusSizer = new wxBoxSizer(wxVERTICAL);
    m_txtModeStatus = new wxStaticText(m_panel, wxID_ANY, wxT("unk"), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    m_txtModeStatus->Enable(false); // enabled only if Hamlib is turned on
    m_txtModeStatus->SetMinSize(wxSize(80,-1));
    modeStatusSizer->Add(m_txtModeStatus, 0, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 1);
    lowerSizer->Add(modeStatusSizer, 0, wxALIGN_CENTER_VERTICAL|static_cast<int>(wxALL), 1);

    m_BtnCallSignReset = new wxButton(m_panel, wxID_ANY, _("&Clear"), wxDefaultPosition, wxDefaultSize, 0);
    lowerSizer->Add(m_BtnCallSignReset, 0, wxALIGN_CENTER_HORIZONTAL|wxALIGN_CENTER_VERTICAL|static_cast<int>(wxALL), 1);

    wxBoxSizer* bSizer15;
    bSizer15 = new wxBoxSizer(wxVERTICAL);
    m_txtCtrlCallSign = new wxTextCtrl(m_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    m_txtCtrlCallSign->SetToolTip(_("Call Sign of transmitting station will appear here"));
    m_txtCtrlCallSign->SetSizeHints(wxSize(100,-1));

    m_cboLastReportedCallsigns = new wxComboCtrl(m_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxCB_READONLY);
    m_lastReportedCallsignListView = new wxListViewComboPopup(m_BtnCallSignReset);
    m_cboLastReportedCallsigns->SetPopupControl(m_lastReportedCallsignListView);
    m_cboLastReportedCallsigns->SetSizeHints(wxSize(400,-1));
    m_cboLastReportedCallsigns->SetPopupMaxHeight(150);
    
    m_lastReportedCallsignListView->InsertColumn(0, wxT("Callsign"), wxLIST_FORMAT_LEFT, 100);
    m_lastReportedCallsignListView->InsertColumn(1, wxT("Frequency"), wxLIST_FORMAT_RIGHT, 75);
    m_lastReportedCallsignListView->InsertColumn(2, wxT("Date/Time"), wxLIST_FORMAT_LEFT, 175);
    m_lastReportedCallsignListView->InsertColumn(3, wxT("SNR"), wxLIST_FORMAT_RIGHT, 50);

    bSizer15->Add(m_txtCtrlCallSign, 1, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 5);
    bSizer15->Add(m_cboLastReportedCallsigns, 1, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 5);

    lowerSizer->Add(bSizer15, 1, static_cast<int>(wxEXPAND), 5);
    lowerSizer->SetMinSize(wxSize(375,-1));
    centerSizer->Add(lowerSizer, 0, static_cast<int>(wxEXPAND), 2);
    centerSizer->SetMinSize(wxSize(375,375));
    bSizer1->Add(centerSizer, 1, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 1);
    
    //=====================================================
    // Right side
    //=====================================================
    rightSizer = new wxWrapSizer(wxVERTICAL, wxREMOVE_LEADING_SPACES);

    // Transmit Level slider
    m_txLevelBox = new wxStaticBox(m_panel, wxID_ANY, _("TX &Attenuation"), wxDefaultPosition, wxSize(100,-1));
    wxBoxSizer* txLevelSizer = new wxStaticBoxSizer(m_txLevelBox, wxVERTICAL);
    
    wxBoxSizer* txBtnSizer = new wxBoxSizer(wxHORIZONTAL);
    m_btnTxLevelMM = new wxButton(m_txLevelBox, wxID_ANY, _("<<"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_btnTxLevelM  = new wxButton(m_txLevelBox, wxID_ANY, _("<"),  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_btnTxLevelP  = new wxButton(m_txLevelBox, wxID_ANY, _(">"),  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_btnTxLevelPP = new wxButton(m_txLevelBox, wxID_ANY, _(">>"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_btnTxLevelMM->SetToolTip(_("Decrease output by 1.0dB"));
    m_btnTxLevelM ->SetToolTip(_("Decrease output by 0.2dB"));
    m_btnTxLevelP ->SetToolTip(_("Increase output by 0.2dB"));
    m_btnTxLevelPP->SetToolTip(_("Increase output by 1.0dB"));
    txBtnSizer->Add(m_btnTxLevelMM, 1, static_cast<int>(wxEXPAND), 0);
    txBtnSizer->Add(m_btnTxLevelM,  1, static_cast<int>(wxEXPAND), 0);
    txBtnSizer->Add(m_btnTxLevelP,  1, static_cast<int>(wxEXPAND), 0);
    txBtnSizer->Add(m_btnTxLevelPP, 1, static_cast<int>(wxEXPAND), 0);
    wxString fmtString = wxString::Format(MIC_SPKR_LEVEL_FORMAT_STR, wxNumberFormatter::ToString((double)0, 1), DECIBEL_STR);

    m_txtTxLevelNum = new wxStaticText(m_txLevelBox, wxID_ANY, fmtString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER | wxST_NO_AUTORESIZE);
    m_txtTxLevelNum->SetToolTip(_("Use mouse scroll wheel to adjust up or down\nRight click for more options"));
    m_txtTxLevelNum->SetMinSize(wxSize(100,-1));
    txLevelSizer->Add(m_txtTxLevelNum, 0, static_cast<int>(wxEXPAND), 0);

    txLevelSizer->Add(txBtnSizer, 0, static_cast<int>(wxEXPAND), 0);

    rightSizer->Add(txLevelSizer, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 2);
    
    // Frequency text field (PSK Reporter)
    m_freqBox = new wxStaticBox(m_panel, wxID_ANY, _("Radio Freq. (MHz)"), wxDefaultPosition, wxSize(100,-1));

    wxBoxSizer* reportFrequencySizer = new wxStaticBoxSizer(m_freqBox, wxHORIZONTAL);
    
    //wxStaticText* reportFrequencyUnits = new wxStaticText(m_freqBox, wxID_ANY, wxT(" MHz"), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    wxBoxSizer* txtReportFreqSizer = new wxBoxSizer(wxVERTICAL);
    
    m_cboReportFrequency = new wxComboBox(m_freqBox, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, NULL, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);
    m_cboReportFrequency->SetMinSize(wxSize(150,-1));
    txtReportFreqSizer->Add(m_cboReportFrequency, 1, static_cast<int>(wxALL), 5);
    
    reportFrequencySizer->Add(txtReportFreqSizer, 1, static_cast<int>(wxEXPAND), 1);
    //reportFrequencySizer->Add(reportFrequencyUnits, 0, wxALIGN_CENTER_VERTICAL, 1);
    
    rightSizer->Add(reportFrequencySizer, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 2);
    
    /* new --- */

    //=====================================================
    // Control Toggles box
    //=====================================================
    wxStaticBoxSizer* sbSizer5;
    wxStaticBox* controlBox = new wxStaticBox(m_panel, wxID_ANY, _("Control"), wxDefaultPosition, wxSize(100,-1));
    sbSizer5 = new wxStaticBoxSizer(controlBox, wxVERTICAL);

    //-------------------------------
    // Stop/Stop signal processing (rx and tx)
    //-------------------------------
    m_togBtnOnOff = new wxToggleButton(controlBox, wxID_ANY, _("&Start Modem"), wxDefaultPosition, wxDefaultSize, 0);
    m_togBtnOnOff->SetToolTip(_("Begin/End receiving data."));
    sbSizer5->Add(m_togBtnOnOff, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);

    //------------------------------
    // Tune Toggle
    //------------------------------
    m_btnTogTune = new wxToggleButton(controlBox, wxID_ANY, _("&Tune"), wxDefaultPosition, wxDefaultSize, 0);
    m_btnTogTune->SetToolTip(_("Emits 1500 Hz carrier to enable rig/antenna tuning.\nRight click for more options"));
    sbSizer5->Add(m_btnTogTune, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);
    m_btnTogTune->Enable(false);

    //------------------------------
    // PTT button: Toggle Transmit/Receive mode
    //------------------------------
    m_btnTogPTT = new wxToggleButton(controlBox, wxID_ANY, _("&XMIT"), wxDefaultPosition, wxDefaultSize, 0);
    m_btnTogPTT->SetToolTip(_("Switch between Receive and Transmit. Right-click for additional options."));
    sbSizer5->Add(m_btnTogPTT, 0, static_cast<int>(wxALL) | static_cast<int>(wxEXPAND), 5);

    rightSizer->Add(sbSizer5, 0, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 2);

    bSizer1->Add(rightSizer, 0, static_cast<int>(wxALL)|static_cast<int>(wxEXPAND), 3);
    
    m_panel->SetSizerAndFit(bSizer1);
    this->Layout();

    m_statusBar1 = this->CreateStatusBar(1, wxSTB_DEFAULT_STYLE, wxID_ANY);

    //=====================================================
    // End of layout
    //=====================================================
    
    //-------------------
    // Tab ordering for accessibility
    //-------------------
    m_auiNbookCtrl->MoveBeforeInTabOrder(m_BtnCallSignReset);
    
    m_togBtnOnOff->MoveBeforeInTabOrder(m_btnTogTune);
    m_btnTogTune->MoveBeforeInTabOrder(m_btnTogPTT);
    
    //-------------------
    // Connect Events
    //-------------------
    this->Connect(wxEVT_CLOSE_WINDOW, wxCloseEventHandler(TopFrame::topFrame_OnClose));
    this->Connect(wxEVT_PAINT, wxPaintEventHandler(TopFrame::topFrame_OnPaint));
    this->Connect(wxEVT_SIZE, wxSizeEventHandler(TopFrame::topFrame_OnSize));
    this->Connect(wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::topFrame_OnUpdateUI));
    this->Connect(wxEVT_SYS_COLOUR_CHANGED, wxSysColourChangedEventHandler(TopFrame::OnSystemColorChanged));
    this->Connect(m_menuItemExit->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnExit));

    this->Connect(m_menuItemEasySetup->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsEasySetup));
    this->Connect(m_menuItemEasySetup->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsEasySetupUI));
    this->Connect(m_menuItemTextMessaging->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsTextMessaging));
    this->Connect(m_menuItemTextMessaging->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsTextMessagingUI));
    this->Connect(m_menuItemGlissando->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsGlissando));
    this->Connect(m_menuItemAudio->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsAudio));
    this->Connect(m_menuItemAudio->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsAudioUI));
    this->Connect(m_menuItemRigCtrlCfg->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsComCfg));
    this->Connect(m_menuItemRigCtrlCfg->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsComCfgUI));
    this->Connect(m_menuItemOptions->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsOptions));
    this->Connect(m_menuItemOptions->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsOptionsUI));

    this->Connect(m_menuItemPlayFileFromRadio->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnPlayFileFromRadio));

    this->Connect(m_menuItemExportConfig->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsExportConfig));
    this->Connect(m_menuItemExportConfig->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsExportConfigUI));
    this->Connect(m_menuItemImportConfig->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsImportConfig));
    this->Connect(m_menuItemImportConfig->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsImportConfigUI));
    this->Connect(m_menuItemLoadDefaultConfig->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsLoadDefaultConfig));
    this->Connect(m_menuItemLoadDefaultConfig->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsLoadDefaultConfigUI));

    m_togBtnOnOff->Connect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTogBtnOnOff), NULL, this);
    m_btnTogPTT->Connect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTogBtnPTT), NULL, this);

#if defined(__WXGTK__)
    // wxGTK fires wxEVT_CONTEXT_MENU on button-press for these widgets,
    // causing GTK to dismiss PopupMenu on button release; use RIGHT_UP
    // instead. MSW/OSX are unaffected (MSW generates the event on
    // button-up already; OSX uses ctrl-click), so this is GTK-specific.
    // Confirmed present on both wxGTK 3.2 and 3.3+, unlike the windowless
    // widget case below, so no version gate here.
    m_btnTogPTT->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) { wxContextMenuEvent ctx; OnTogBtnPTTRightClick(ctx); });
#else
    m_btnTogPTT->Connect(wxEVT_CONTEXT_MENU, wxContextMenuEventHandler(TopFrame::OnTogBtnPTTRightClick), NULL, this);
#endif

    m_BtnCallSignReset->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnCallSignReset), NULL, this);

    m_btnTxLevelMM->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTxLevelDecrBig), NULL, this);
    m_btnTxLevelM->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTxLevelDecr), NULL, this);
    m_btnTxLevelP->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTxLevelIncr), NULL, this);
    m_btnTxLevelPP->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTxLevelIncrBig), NULL, this);

    m_txLevelBox->Connect(wxEVT_MOUSEWHEEL, wxMouseEventHandler(TopFrame::OnTxLevelMouseWheel), NULL, this);
    m_txtTxLevelNum->Connect(wxEVT_MOUSEWHEEL, wxMouseEventHandler(TopFrame::OnTxLevelMouseWheel), NULL, this);
    m_btnTxLevelMM->Connect(wxEVT_MOUSEWHEEL, wxMouseEventHandler(TopFrame::OnTxLevelMouseWheel), NULL, this);
    m_btnTxLevelM->Connect(wxEVT_MOUSEWHEEL, wxMouseEventHandler(TopFrame::OnTxLevelMouseWheel), NULL, this);
    m_btnTxLevelP->Connect(wxEVT_MOUSEWHEEL, wxMouseEventHandler(TopFrame::OnTxLevelMouseWheel), NULL, this);
    m_btnTxLevelPP->Connect(wxEVT_MOUSEWHEEL, wxMouseEventHandler(TopFrame::OnTxLevelMouseWheel), NULL, this);
    m_btnTogTune->Connect(wxEVT_MOUSEWHEEL, wxMouseEventHandler(TopFrame::OnTxLevelMouseWheel), NULL, this);

#if wxCHECK_VERSION(3, 3, 0) && defined(__WXGTK__)
    // wxGTK 3.3+ fires wxEVT_CONTEXT_MENU on button-press for these widget types,
    // causing GTK to dismiss PopupMenu on button release; use RIGHT_UP instead.
    // MSW/OSX are unaffected (MSW generates the event on button-up already;
    // OSX uses ctrl-click), so this is GTK-specific.
    m_txLevelBox->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) { wxContextMenuEvent ctx; OnTxLevelContextMenu(ctx); });
    m_txtTxLevelNum->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) { wxContextMenuEvent ctx; OnTxLevelContextMenu(ctx); });
    m_btnTogTune->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) { wxContextMenuEvent ctx; OnTuneAttenContextMenu(ctx); });
#else
    // wxGTK < 3.3 does not generate RIGHT_UP for windowless widget types
    // (wxStaticBox, wxStaticText); CONTEXT_MENU works without the dismiss issue.
    // (Also used as-is on MSW/OSX regardless of wx version -- see above.)
    m_txLevelBox->Connect(wxEVT_CONTEXT_MENU, wxContextMenuEventHandler(TopFrame::OnTxLevelContextMenu), NULL, this);
    m_txtTxLevelNum->Connect(wxEVT_CONTEXT_MENU, wxContextMenuEventHandler(TopFrame::OnTxLevelContextMenu), NULL, this);
    m_btnTogTune->Connect(wxEVT_CONTEXT_MENU, wxContextMenuEventHandler(TopFrame::OnTuneAttenContextMenu), NULL, this);
#endif


    m_cboReportFrequency->Connect(wxEVT_TEXT_ENTER, wxCommandEventHandler(TopFrame::OnChangeReportFrequency), NULL, this);
    m_cboReportFrequency->Connect(wxEVT_TEXT, wxCommandEventHandler(TopFrame::OnChangeReportFrequencyVerify), NULL, this);
    m_cboReportFrequency->Connect(wxEVT_COMBOBOX, wxCommandEventHandler(TopFrame::OnChangeReportFrequency), NULL, this);
    m_cboReportFrequency->Connect(wxEVT_SET_FOCUS, wxFocusEventHandler(TopFrame::OnReportFrequencySetFocus), NULL, this);
    m_cboReportFrequency->Connect(wxEVT_KILL_FOCUS, wxFocusEventHandler(TopFrame::OnReportFrequencyKillFocus), NULL, this);
    
    m_cboLastReportedCallsigns->Connect(wxEVT_COMBOBOX_DROPDOWN, wxCommandEventHandler(TopFrame::OnOpenCallsignList), NULL, this);
    m_cboLastReportedCallsigns->Connect(wxEVT_COMBOBOX_CLOSEUP, wxCommandEventHandler(TopFrame::OnCloseCallsignList), NULL, this);
    m_cboLastReportedCallsigns->Connect(wxEVT_RIGHT_DOWN, wxMouseEventHandler(TopFrame::OnRightClickCallsignList), NULL, this);

    m_btnTogTune->Connect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTogBtnTune), NULL, this);
}

TopFrame::~TopFrame()
{
    //-------------------
    // Disconnect Events
    //-------------------   
    this->Disconnect(wxEVT_CLOSE_WINDOW, wxCloseEventHandler(TopFrame::topFrame_OnClose));
    this->Disconnect(wxEVT_PAINT, wxPaintEventHandler(TopFrame::topFrame_OnPaint));
    this->Disconnect(wxEVT_SIZE, wxSizeEventHandler(TopFrame::topFrame_OnSize));
    this->Disconnect(wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::topFrame_OnUpdateUI));
    this->Disconnect(ID_EXIT, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnExit));
    this->Disconnect(wxID_ANY, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsEasySetup));
    this->Disconnect(wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsEasySetupUI));
    this->Disconnect(wxID_ANY, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsAudio));
    this->Disconnect(wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsAudioUI));
    this->Disconnect(wxID_ANY, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsComCfg));
    this->Disconnect(wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsComCfgUI));
    this->Disconnect(wxID_ANY, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsOptions));
    this->Disconnect(wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsOptionsUI));

    this->Disconnect(wxID_ANY, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnPlayFileFromRadio));

    this->Disconnect(wxID_ANY, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsExportConfig));
    this->Disconnect(wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsExportConfigUI));
    this->Disconnect(wxID_ANY, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsImportConfig));
    this->Disconnect(wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsImportConfigUI));
    this->Disconnect(wxID_ANY, wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(TopFrame::OnToolsLoadDefaultConfig));
    this->Disconnect(wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler(TopFrame::OnToolsLoadDefaultConfigUI));

    m_togBtnOnOff->Disconnect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTogBtnOnOff), NULL, this);
    m_btnTogPTT->Disconnect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTogBtnPTT), NULL, this);
#if !defined(__WXGTK__)
    m_btnTogPTT->Disconnect(wxEVT_CONTEXT_MENU, wxContextMenuEventHandler(TopFrame::OnTogBtnPTTRightClick), NULL, this);
#endif
    

    m_btnTxLevelMM->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTxLevelDecrBig), NULL, this);
    m_btnTxLevelM->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTxLevelDecr), NULL, this);
    m_btnTxLevelP->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTxLevelIncr), NULL, this);
    m_btnTxLevelPP->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTxLevelIncrBig), NULL, this);
#if !(wxCHECK_VERSION(3, 3, 0) && defined(__WXGTK__))
    m_txLevelBox->Disconnect(wxEVT_CONTEXT_MENU, wxContextMenuEventHandler(TopFrame::OnTxLevelContextMenu), NULL, this);
    m_txtTxLevelNum->Disconnect(wxEVT_CONTEXT_MENU, wxContextMenuEventHandler(TopFrame::OnTxLevelContextMenu), NULL, this);
    m_btnTogTune->Disconnect(wxEVT_CONTEXT_MENU, wxContextMenuEventHandler(TopFrame::OnTuneAttenContextMenu), NULL, this);
#endif

    
    m_cboReportFrequency->Disconnect(wxEVT_TEXT_ENTER, wxCommandEventHandler(TopFrame::OnChangeReportFrequency), NULL, this);
    m_cboReportFrequency->Disconnect(wxEVT_TEXT, wxCommandEventHandler(TopFrame::OnChangeReportFrequencyVerify), NULL, this);
    m_cboReportFrequency->Disconnect(wxEVT_COMBOBOX, wxCommandEventHandler(TopFrame::OnChangeReportFrequency), NULL, this);
    
    m_cboReportFrequency->Disconnect(wxEVT_SET_FOCUS, wxFocusEventHandler(TopFrame::OnReportFrequencySetFocus), NULL, this);
    m_cboReportFrequency->Disconnect(wxEVT_KILL_FOCUS, wxFocusEventHandler(TopFrame::OnReportFrequencyKillFocus), NULL, this);

    m_cboLastReportedCallsigns->Disconnect(wxEVT_RIGHT_DOWN, wxMouseEventHandler(TopFrame::OnRightClickCallsignList), NULL, this);
    m_cboLastReportedCallsigns->Disconnect(wxEVT_COMBOBOX_DROPDOWN, wxCommandEventHandler(TopFrame::OnOpenCallsignList), NULL, this);
    m_cboLastReportedCallsigns->Disconnect(wxEVT_COMBOBOX_CLOSEUP, wxCommandEventHandler(TopFrame::OnCloseCallsignList), NULL, this);

    m_btnTogTune->Disconnect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(TopFrame::OnTogBtnTune), NULL, this);
}

