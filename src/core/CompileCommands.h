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

    if (sub == "version")
    {
        DisplayMessage(std::string("AMAN Plugin version: ") + PLUGIN_VERSION);
        return true;
    }

    DisplayMessage("Commands: .aman version");
    return true;
}


}


#endif //AMAN_PLUGIN_COMPILECOMMANDS_H