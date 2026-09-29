//==========================================================================
// Name:            ReportingConfiguration.h
// Purpose:         Implements the reporting configuration for FreeDV
// Created:         July 2, 2023
// Authors:         Mooneer Salem
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

#include <wx/tokenzr.h>
#include <wx/numformatter.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <inttypes.h>

#include "../defines.h"
#include "ReportingConfiguration.h"

namespace
{

// The list inherited from FreeDV: its voice calling frequencies, nearly all in
// phone segments where the data segment check will not let chat transmit. A
// saved list still equal to it was never edited, so it gets the new defaults.
const uint64_t FREEDV_FREQUENCY_LIST_HZ[] = {
    1870000, 3625000, 3643000, 3693000, 3697000, 3803000, 5403500, 5368500, 7177000,
    7197000, 14236000, 14240000, 18118000, 21313000, 24933000, 28330000, 28720000, 10489640000ULL,
};

// A saved frequency, in MHz in US format whatever the locale, to Hz.
uint64_t storedFrequencyHz(wxString const& val)
{
    wxStringTokenizer tok(val, ".");
    wxString wholeNumber = tok.GetNextToken();
    wxString fraction = "0";
    if (tok.HasMoreTokens())
    {
        fraction = tok.GetNextToken();
    }

    if (fraction.Length() < 6)
    {
        fraction += wxString('0', 6 - fraction.Length());
    }

    long long hz = 0;
    long long mhz = 0;
    wholeNumber.ToLongLong(&mhz);
    fraction.ToLongLong(&hz);
    return mhz * 1000000 + hz;
}

}

ReportingConfiguration::ReportingConfiguration()
    : reportingCallsign("/Reporting/Callsign", _(""))
    , reportingFrequency("/Reporting/Frequency", 0)
    , reportingFrequencyList("/Reporting/FrequencyList", {
        // Where Glissando calls: inside the data segments (US 47 CFR 97.305
        // and the IARU Region 2 plan), just below the FT8/JS8/PSK31 crowd, so
        // the whole signal -- 330 Hz to 2.9 kHz above the dial with the duet
        // and a full melody offset -- stays clear of them. docs/APP.md has
        // the reasons band by band.
        _("1.8460"),
        _("3.5700"),
        _("7.0670"),
        _("10.1330"),
        _("14.0670"),
        _("18.0970"),
        _("21.0670"),
        _("24.9110"),
        _("28.0670"),
    })
        
    , reportingFrequencyAsKhz("/Reporting/FrequencyAsKHz", false)
    , csvLogFilePath("/Reporting/CSV/LogFilePath", _(""))
{
    // Special handling for the frequency list to properly handle locales
    reportingFrequencyList.setLoadProcessor([this](std::vector<wxString> const& list) {
        std::vector<wxString> newList;
        for (auto& val : list)
        {
            // Frequencies are unfortunately saved in US format (legacy behavior). We need 
            // to manually parse and convert to Hz, then output MHz values in the user's current
            // locale.
            uint64_t freq = storedFrequencyHz(val);

            if (reportingFrequencyAsKhz)
            {
                double khzFloat = freq / 1000.0;
                newList.push_back(wxNumberFormatter::ToString(khzFloat, 1));
            }
            else
            {
                double mhzFloat = freq / 1000000.0;
                newList.push_back(wxNumberFormatter::ToString(mhzFloat, 4));
            }
        }

        return newList;
    });

    reportingFrequencyList.setSaveProcessor([this](std::vector<wxString> const& list) {
        std::vector<wxString> newList;
        for (auto& val : list)
        {
            // Frequencies are unfortunately saved in US format (legacy behavior). We need 
            // to manually parse and convert to Hz, then output MHz values in US format.
            double mhz = 0.0;
            wxNumberFormatter::FromString(val, &mhz);
            
            if (reportingFrequencyAsKhz)
            {
                // Frequencies are in kHz, so divide one more time to get MHz.
                mhz /= 1000.0;
            }

            uint64_t hz = mhz * 1000000;
            uint64_t mhzInt = hz / 1000000;
            hz = hz % 1000000;

            wxString newVal = wxString::Format("%" PRIu64 ".%06" PRIu64, mhzInt, hz);
            newList.push_back(newVal);
        }
        return newList;
    });
}

void ReportingConfiguration::load(wxConfigBase* config)
{
    load_(config, reportingCallsign);

    // Note: this needs to be loaded before the frequency list so that
    // we get the values formatted as kHz (if so configured).
    load_(config, reportingFrequencyAsKhz);
    
    load_(config, reportingFrequencyList);

    std::vector<wxString> saved = reportingFrequencyList.getWithoutProcessing();
    bool untouchedFreeDvList = saved.size() == sizeof(FREEDV_FREQUENCY_LIST_HZ) / sizeof(FREEDV_FREQUENCY_LIST_HZ[0]);
    for (size_t i = 0; untouchedFreeDvList && i < saved.size(); i++)
    {
        // Saving truncates MHz times a million, so allow a few Hz of rounding.
        uint64_t hz = storedFrequencyHz(saved[i]);
        uint64_t freeDvHz = FREEDV_FREQUENCY_LIST_HZ[i];
        untouchedFreeDvList = (hz > freeDvHz ? hz - freeDvHz : freeDvHz - hz) <= 5;
    }
    if (untouchedFreeDvList)
    {
        reportingFrequencyList.setWithoutProcessing(reportingFrequencyList.getDefaultVal());
    }

    load_(config, csvLogFilePath);

    // Set default CSV log file path to Documents/glissando_rx_log.csv if not configured.
    if (csvLogFilePath->IsEmpty())
    {
        wxString defaultPath;
        wxString logFileName = "glissando_rx_log.csv";

#if defined(__linux__)
        // Special logic to force use of XDG_DATA_HOME as wxWidgets doesn't currently
        // provide this in wxStandardPaths.
        wxString xdgDataHome;
        if (!wxGetEnv("XDG_DATA_HOME", &xdgDataHome))
        {
            // Default to $HOME/.local/share per XDG specification.
            wxString home = wxGetHomeDir();
            xdgDataHome = wxString::Format("%s/.local/share", home);
        }

        defaultPath = wxString::Format("%s/glissando", xdgDataHome);
#else
        defaultPath = wxStandardPaths::Get().GetDocumentsDir() + wxFILE_SEP_PATH + "glissando";
#endif // wxCHECK_VERSION(3,1,0)

        // Make folder (including parents as needed)
        wxFileName dn = wxFileName::DirName(defaultPath);
        dn.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

        csvLogFilePath.setWithoutProcessing(defaultPath + wxFILE_SEP_PATH + logFileName);
    }

    // Special load handling for reporting below.
    wxString freqStr = config->Read(reportingFrequency.getElementName(), wxT("0"));
    reportingFrequency.setWithoutProcessing(atoll(freqStr.ToUTF8()));
}

void ReportingConfiguration::save(wxConfigBase* config)
{
 
    save_(config, reportingCallsign);

    save_(config, reportingFrequencyAsKhz);
    save_(config, reportingFrequencyList);

    save_(config, csvLogFilePath);

    // Special save handling for reporting below.
    wxString tempFreqStr = wxString::Format(wxT("%" PRIu64), reportingFrequency.getWithoutProcessing());
    config->Write(reportingFrequency.getElementName(), tempFreqStr);
}
