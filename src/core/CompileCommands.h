//
// Created by Hawai on 9/5/2026.
//

#ifndef AMAN_PLUGIN_COMPILECOMMANDS_H
#define AMAN_PLUGIN_COMPILECOMMANDS_H

#include <string>
#include <algorithm>
#include <sstream>

#include <version.h>

#include "AMANPlugin.h"

namespace amanplugin
{

inline bool TryToParseICAOs(std::unordered_set<std::string>& parsedICAOs, const std::string& line, const std::unordered_set<std::string>& compatibleICAOs)
{
    std::istringstream iss(line);
    std::string icao;
    while (iss >> icao)
    {
        if (icao == "aman") continue; // Skip the command itself

        if (icao.size() != 4 || !std::all_of(icao.begin(), icao.end(), ::isalpha))
        {
            // Invalid ICAO format, skip it
            continue;
        }

        std::transform(icao.begin(), icao.end(), icao.begin(), ::toupper);

        if (!compatibleICAOs.contains(icao))
        {
            // ICAO not in the list of compatible ICAOs, skip it
            continue;
        }

        parsedICAOs.insert(icao);
    }
    return !parsedICAOs.empty();
}

inline bool AMANPlugin::OnCompileCommand(const char* sCommandLine)
{
    if (sCommandLine == nullptr) return false;

    std::string line(sCommandLine);

    auto trim = [](std::string& s) {
        const auto notSpace = [](int ch) { return !std::isspace(ch); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    };
    trim(line);
    if (line.empty()) return false;

    // Remove optional leading '.' (Euroscope command convention)
    if (!line.empty() && line[0] == '.') line.erase(0, 1);

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty()) return false;

    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string lcmd = toLower(cmd);

    if (lcmd != "aman")
        return false;

    std::string sub;
    iss >> sub;
    sub = toLower(sub);


    std::unordered_set<std::string> icaoList;

    std::unordered_set<std::string> compatibleICAOs;
    {
        std::lock_guard<std::mutex> lock(compatibleICAOsMutex_);
        compatibleICAOs = compatibleICAOs_;
    }

    if (sub == "version")
    {
        DisplayMessage(std::string("AMAN Plugin version: ") + PLUGIN_VERSION);
        return true;
    }
    else if (sub == "icao")
    {
        std::lock_guard<std::mutex> lock(trackedICAOsMutex_);
        if (trackedICAOs_.empty())
        {
            DisplayMessage("No ICAOs are currently being tracked.");
        }
        else
        {
            std::string icaoListStr;
            for (const auto& icao : trackedICAOs_)
            {
                if (!icaoListStr.empty()) icaoListStr += " ";
                icaoListStr += icao;
            }
            DisplayMessage("Currently tracked ICAOs: " + icaoListStr);
        }
        return true;
    }
    else if (sub == "clear")
    {
        std::lock_guard<std::mutex> lock(trackedICAOsMutex_);
        trackedICAOs_.clear();
        DisplayMessage("Tracking ICAOs cleared.");
        return true;
    }
    else if (sub == "available")
    {
        std::lock_guard<std::mutex> lock(compatibleICAOsMutex_);
        if (compatibleICAOs_.empty())
        {
            DisplayMessage("No ICAOs are currently available.");
        }
        else
        {
            std::string icaoListStr;
            for (const auto& icao : compatibleICAOs_)
            {
                if (!icaoListStr.empty()) icaoListStr += " ";
                icaoListStr += icao;
            }
            DisplayMessage("Available ICAOs: " + icaoListStr);
        }
        return true;
    }
    else if (TryToParseICAOs(icaoList, line, compatibleICAOs))
    {
        if (connectionType.load(std::memory_order::acquire) != CONNECTION_TYPE_DIRECT
        && connectionType.load(std::memory_order::acquire) != CONNECTION_TYPE_SWEATBOX)
        {
            DisplayError("Cannot update tracked ICAOs: Not connected to network.");
            return true;
        }
        // User provided ICAOs list or Position: example: .aman LFPG LFPO
        std::lock_guard<std::mutex> lock(trackedICAOsMutex_);
        trackedICAOs_ = std::move(icaoList);
        DisplayMessage("Tracking ICAOs updated.");
        return true;
    }

    // no command recognized, display help
    DisplayMessage("Commands: .aman version    Display AMAN Plugin version");
    DisplayMessage("Commands: .aman <ICAO1> <ICAO2> ...    Activate reporting for the specified ICAOs");
    DisplayMessage("Commands: .aman icao    Display currently tracked ICAOs");
    DisplayMessage("Commands: .aman clear    Clear the list of tracked ICAOs");
    DisplayMessage("Commands: .aman available    Display the list of available ICAOs");
    return true;
}


}


#endif //AMAN_PLUGIN_COMPILECOMMANDS_H