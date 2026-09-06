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
    else if (TryToParseICAOs(icaoList, line, compatibleICAOs))
    {
        // User provided ICAOs list or Position: example: .aman LFPG LFPO
        std::lock_guard<std::mutex> lock(trackedICAOsMutex_);
        trackedICAOs_ = std::move(icaoList);
        DisplayMessage("Tracking ICAOs updated.");
        return true;
    }

    // no command recognized, display help
    DisplayMessage("Commands: .aman version");
    DisplayMessage("Commands: .aman <ICAO1> <ICAO2> ...");
    return true;
}


}


#endif //AMAN_PLUGIN_COMPILECOMMANDS_H