//
// Created by Hawai on 9/5/2026.
//

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "AMANPlugin.h"

#include "Secret.h"
#include "version.h"
#include "core/CompileCommands.h"

#define ESB_CLIENT_SHIM
#include "esbridge.h"

using namespace amanplugin;
using namespace EuroScopePlugIn;

/// Claimed namespace on the bridge. A conflict is settled with the other
/// author, not retried around, so a taken id is reported once and dropped.
static constexpr const char* PROVIDER_ID = "amanplugin";
static constexpr const char* RWY_FIELD = "rwy";
static constexpr const char* IAF_FIELD = "iaf";

/// CoFrance owns these. It integrates a wind-aware trajectory per flight, which
/// this plugin does not, so the ETA over an IAF is read rather than computed.
static constexpr const char* COFRANCE_ETA_IAF = "cofrance/eta_iaf";
static constexpr const char* COFRANCE_IAF = "cofrance/iaf";

/// The bridge is declared missing only after it has had a fair chance to load.
static constexpr int MISSING_TICKS_BEFORE_NOTICE = 10;

/// Fix names are five characters; this is only ever read into for a comparison.
static constexpr uint32_t VALUE_BUFFER = 32;

/// Render UTC Unix seconds as the "HH:MM:SS" the feed API expects. Empty on a
/// timestamp the CRT will not convert, which the caller turns into a JSON null.
static std::string FormatUtcHms(int64_t epochSeconds)
{
    const std::time_t seconds = static_cast<std::time_t>(epochSeconds);
    std::tm utc = {};
    if (gmtime_s(&utc, &seconds) != 0) return {};

    char buf[16];
    const int written = std::snprintf(buf, sizeof buf, "%02d:%02d:%02d",
                                      utc.tm_hour, utc.tm_min, utc.tm_sec);
    if (written <= 0) return {};
    return std::string(buf, static_cast<size_t>(written));
}

/// Parse the runway API's timestamps - "2026-09-18T14:32:07.310Z", fraction
/// optional - into UTC milliseconds since the epoch. False on anything else
/// rather than a guess: a wrong expiry is worse than a skipped command.
static bool ParseIsoUtcMs(const std::string& text, int64_t& outMs)
{
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T'
        || text[13] != ':' || text[16] != ':')
        return false;

    const auto number = [&text](size_t pos, size_t width, int& out) {
        out = 0;
        for (size_t i = pos; i < pos + width; ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            out = out * 10 + (text[i] - '0');
        }
        return true;
    };

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!number(0, 4, year) || !number(5, 2, month) || !number(8, 2, day)
        || !number(11, 2, hour) || !number(14, 2, minute) || !number(17, 2, second))
        return false;

    // Any number of fraction digits; milliseconds are kept.
    size_t pos = 19;
    int millis = 0;
    if (text[pos] == '.') {
        const size_t first = ++pos;
        for (int scale = 100; pos < text.size() && text[pos] >= '0' && text[pos] <= '9'; ++pos, scale /= 10)
            millis += (text[pos] - '0') * scale;
        if (pos == first) return false;
    }
    if (pos + 1 != text.size() || text[pos] != 'Z') return false;

    const std::chrono::year_month_day date{std::chrono::year{year},
                                           std::chrono::month{static_cast<unsigned>(month)},
                                           std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok() || hour > 23 || minute > 59 || second > 60) return false;

    const auto days = std::chrono::sys_days{date}.time_since_epoch();
    outMs = std::chrono::duration_cast<std::chrono::milliseconds>(days).count()
          + ((hour * 60LL + minute) * 60 + second) * 1000 + millis;
    return true;
}

/// @p key of @p object when it is a string, else empty. Absent, null and
/// mistyped all read the same, which is what every caller wants.
static std::string StringField(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

/// How long a written runway is remembered past its command's expiry. Only a
/// repeat of that very decision is suppressed, so being generous costs nothing,
/// and it covers expiry being worked out from a whole-second serverTime.
static constexpr auto RWY_WRITTEN_GRACE = std::chrono::seconds(60);

const ESB_FieldDecl FIELDS[] = {
    {
        IAF_FIELD,
        ESB_T_STR,
        ESB_SCOPE_GLOBAL,
        0u,
        97u, // 5 Char per IAF + 1 for comma + 1 for null terminator, max 16 IAFs
        "AMAN list of IAFs tracked",
    },
    {
    RWY_FIELD,
        ESB_T_STR,
        ESB_SCOPE_AIRCRAFT,
        0u,
        // "<runway>/<reason>": 3 char runway, separator, one reason letter is 5.
        // max_bytes caps the payload length, which ESB_Str reports without the
        // terminator, so this is characters and not sizeof a buffer.
        8u,
        "Runway AMAN assigns, as \"<runway>/<reason>\" (S sequencer, C config, I invalid)",
    }
};

std::unique_ptr<amanplugin::AMANPlugin> myPluginInstance = nullptr;


AMANPlugin::AMANPlugin() : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, "AMAN", PLUGIN_VERSION, "French vACC", "Open Source")
{
    m_stop.store(false, std::memory_order_relaxed);
    Initialize();
}

AMANPlugin::~AMANPlugin()
{
    Shutdown();
}

void AMANPlugin::Initialize()
{
    try
    {
        initialized_ = true;

        // Start the persistent worker thread
        m_stop.store(false, std::memory_order_release);
        m_thread = std::thread(&AMANPlugin::WorkerThread, this);
    }
    catch (const std::exception& e)
    {
        DisplayError("Failed to initialize AMAN Plugin: " + std::string(e.what()));
    }
}

void AMANPlugin::Shutdown()
{
    if (initialized_)
    {
        initialized_ = false;
    }

    // Signal worker thread to stop with proper memory ordering
    m_stop.store(true, std::memory_order_release);

    // Wait for worker thread to finish
    if (m_thread.joinable())
        m_thread.join();

    // Unregistering drops every value we own, which is what should happen: a
    // dupe flag with nobody left to maintain it is worse than no flag at all.
    if (api_ != nullptr && provider_ != nullptr) api_->unregister_provider(provider_);

    DisplayMessage("AMAN Plugin shutdown complete");
};

void __declspec (dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
    myPluginInstance.reset();
    myPluginInstance = std::make_unique<AMANPlugin>();
    *ppPlugInInstance = myPluginInstance.get();
}


void __declspec (dllexport) EuroScopePlugInExit()
{
    myPluginInstance.reset();
}

void AMANPlugin::DisplayMessage(const std::string& message) {
    DisplayUserMessage("AMAN Plugin", "", message.c_str(), true, true, false, false, false);
}

void AMANPlugin::DisplayError(const std::string& message)
{
    DisplayUserMessage("AMAN Plugin", "ERROR", message.c_str(), true, true, true, true, true);
}

void AMANPlugin::QueueError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(messageQueueMutex_);
    messageQueue_.emplace_back<std::pair<std::string, bool>>({message, true});
}

void AMANPlugin::OnTimer(int Counter)
{
    // bridge connection
    if (api_ == nullptr) {
        api_ = ESB_Attach();
        if (api_ == nullptr) {
            // Said once. Without the bridge the DUPE flag simply does not reach
            // CoFrance -- there is no annotation fallback any more -- so the
            // controller does need to be told, but only the one time.
            if (!missingReported_ && ++missingTicks_ >= MISSING_TICKS_BEFORE_NOTICE) {
                missingReported_ = true;
                DisplayError(ESB_MISSING_MESSAGE);
            }
            return;
        }
    }

    if (provider_ == nullptr && !providerConflict_) RegisterProvider();

    if (cofranceEtaField_ == 0 || cofranceIafField_ == 0) ResolveConsumedFields();


    // Update connection Type
    const int type = GetConnectionType();
    connectionType.store(type, std::memory_order_release);

    // Only a controller the servers accept may amend flight plans, and there is
    // no such thing offline or in a sim session.
    const bool connected = type == CONNECTION_TYPE_DIRECT || type == CONNECTION_TYPE_SWEATBOX;
    isController.store(connected && ControllerMyself().IsController(), std::memory_order_release);


    {
        // Drain message queue
        std::lock_guard<std::mutex> lock(messageQueueMutex_);
        for (const auto& msgPair : messageQueue_)
        {
            if (msgPair.second) DisplayError(msgPair.first);
            else DisplayMessage(msgPair.first);
        }
        messageQueue_.clear();
    }

    if (iafUpdateRequired.load(std::memory_order_acquire))
    {
        PublishIAFsToBridge();
        iafUpdateRequired.store(false, std::memory_order_release);
    }

    // Every tick rather than on a flag: a command that could not be written yet
    // stays cached, and this is where it is retried.
    PublishRunwaysToBridge();

    // populate snapshot map with current flights
    if (Counter % PERIODIC_POST_TIME_INTERVAL == 0 )
    {
        std::unordered_set<std::string> trackedIcao;
        {
            std::lock_guard<std::mutex> lock(trackedICAOsMutex_);
            trackedIcao = trackedICAOs_;
        }

        if (trackedIcao.empty()) return;

        std::unordered_map<std::string, std::unordered_set<std::string>> iafMap;
        {
            std::lock_guard<std::mutex> lock(iafMapMutex_);
            iafMap = iafMap_;
        }


        std::unordered_map<std::string, std::vector<Flight>> flightsMap;

        for (auto rt = RadarTargetSelectFirst(); rt.IsValid(); rt = RadarTargetSelectNext(rt))
        {
            auto pos = rt.GetPosition();
            if (!pos.IsValid()) continue;
            auto fp = rt.GetCorrelatedFlightPlan();
            if (!fp.IsValid()) continue;

            std::string destIcao = fp.GetFlightPlanData().GetDestination();
            std::transform(destIcao.begin(), destIcao.end(), destIcao.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (trackedIcao.contains(destIcao) == false) continue;

            auto route = fp.GetExtractedRoute();
            int pointNumber = route.GetPointsNumber();
            // Hoisted: the destination decides the IAF set, not the individual fix,
            // and the old form re-hashed destIcao on every point it looked at.
            const auto iafsForDest = iafMap.find(destIcao);
            if (iafsForDest == iafMap.end()) continue;

            const std::string callsign = rt.GetCallsign();

            for (int i = pointNumber - 1; i >= std::max(0, pointNumber - MAX_IAF_LOOKUP_IN_ROUTE); --i) {
                std::string fix = route.GetPointName(i);
                if (iafsForDest->second.contains(fix) == false) continue;

                flightsMap[destIcao].emplace_back(
                    callsign,
                    fp.GetFlightPlanData().GetAircraftFPType(),
                    fp.GetFlightPlanData().GetOrigin(),
                    fp.GetFlightPlanData().GetArrivalRwy(),
                    fix,
                    GetIafEtaFromBridge(callsign, fix),
                    pos.GetFlightLevel(),
                    rt.GetVerticalSpeed(),
                    fp.GetFlightPlanData().GetFinalAltitude(),
                    pos.GetReportedGS(),
                    pos.GetPosition().m_Latitude,
                    pos.GetPosition().m_Longitude
                );
                break;   // first IAF found scanning back from the destination
            }
        }

        {
            std::lock_guard<std::mutex> lock(snapshotMapMutex_);
            snapshotMap_ = std::move(flightsMap);
        }
    }
}

void AMANPlugin::QueueMessage(const std::string& message)
{
    std::lock_guard<std::mutex> lock(messageQueueMutex_);
    messageQueue_.emplace_back<std::pair<std::string, bool>>({message, false});
}

void AMANPlugin::WorkerThread()
{
    size_t counter = 0;

    std::string baseUrl = std::string("https://") + API_URL;

    // One client for every call. The scheme in the URL decides whether TLS is
    // used, so http:// and https:// endpoints are both reachable.
    auto cli = std::make_unique<httplib::Client>(baseUrl);
    cli->set_connection_timeout(2, 0);		 // 2s
    cli->set_read_timeout(3, 0);             // 3s
    cli->set_write_timeout(3, 0);            // 3s
    cli->set_keep_alive(true);


    RefreshConfig(*cli);


    while (m_stop.load(std::memory_order_acquire) == false)
    {

        if (counter % PERIODIC_POST_TIME_INTERVAL == 0)
        {
            std::unordered_set<std::string> trackedIcaos;
            {
                std::lock_guard<std::mutex> lock(trackedICAOsMutex_);
                trackedIcaos = trackedICAOs_;
            }
            for (const auto& icao : trackedIcaos)
            {
                PostSnapshotsToAPI(*cli, icao);
            }

            if (isController.load(std::memory_order_acquire) == true) {
                // Only controllers can assign runways
                PollRunwayAssignRequests(*cli);
            }
        }

        if (counter % CONFIG_REFRESH_INTERVAL == 1)
        {
            RefreshConfig(*cli);
        }

        counter++;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // Avoid busy waiting
    }
}

void AMANPlugin::GetCompatibleICAOs(httplib::Client& cli)
{
    httplib::Headers headers = { {"User-Agent", "AMANplugin"} };
    auto res = cli.Get("/api/icao", headers);

    if (!res) {
        if (printError) {
            printError = false;
            QueueError("Cannot reach the AMAN API server: " + httplib::to_string(res.error()));
        }
        return;
    }



    if (res->status != 200) {
        if (res->status == 401) {
            if (printError) {
                printError = false;
                QueueError("Invalid Authorization key, make sure you are using latest plugin version.");
            }
            return;
        }

        if (printError) {
            printError = false;
            QueueError("Unexpected response from the AMAN API server: " + std::to_string(res->status));
        }
        return;
    }

    // Parse the JSON response
    std::unordered_set<std::string> compatibleICAOs;
    try {
        auto json = nlohmann::json::parse(res->body);
        if (json.contains("icao") == false || !json["icao"].is_array()) return;
        for (const auto& icao : json["icao"]) {
            compatibleICAOs.insert(icao.get<std::string>());
        }
    } catch (const std::exception& e) {
        if (printError) {
            printError = false;
            QueueError("Failed to parse the AMAN API response: " + std::string(e.what()));
        }
        return;
    }

    std::lock_guard<std::mutex> lock(compatibleICAOsMutex_);
    compatibleICAOs_ = std::move(compatibleICAOs);
}

void AMANPlugin::GetIAFsForICAO(httplib::Client& cli, const std::string& icao)
{
    httplib::Headers headers = { {"User-Agent", "AMANplugin"} };
    auto res = cli.Get("/api/" + icao + "/feeder_fixes", headers);

    if (!res) {
        if (printError) {
            printError = false;
            QueueError("Cannot reach the AMAN API server for ICAO " + icao + ": " + httplib::to_string(res.error()));
        }
        return;
    }

    if (res->status != 200 && res->status != 202) {
        if (res->status == 401) {
            if (printError) {
                printError = false;
                QueueError("Invalid Authorization key, make sure you are using latest plugin version.");
            }
            return;
        }
        if (printError) {
            printError = false;
            QueueError("Unexpected response from the AMAN API server for ICAO " + icao + ": " + std::to_string(res->status));
        }
        return;
    }

    // Parse the JSON response
    try {
        auto json = nlohmann::json::parse(res->body);
        if (json.contains("icao") == false || !json["icao"].is_string() || json["icao"].get<std::string>() != icao) return;
        if (json.contains("fixes") == false || !json["fixes"].is_array() || json["fixes"].size() == 0) return;

        std::unordered_set<std::string> iafs;
        for (const auto& iafObject : json["fixes"]) {
            if (iafObject.is_object() == false) continue;
            if (!iafObject.contains("iaf") || !iafObject["iaf"].is_string()) continue;
            iafs.insert(iafObject["iaf"].get<std::string>());
        }

        std::lock_guard<std::mutex> lock(snapshotMapMutex_);
        iafMap_[icao] = std::move(iafs);
    } catch (const std::exception& e) {
        if (printError) {
            printError = false;
            QueueError("Failed to parse the AMAN API response for ICAO " + icao + ": " + std::string(e.what()));
        }
        return;
    }
    printError = true; // Reset error flag on successful response
}

void AMANPlugin::RefreshConfig(httplib::Client& cli)
{
    GetCompatibleICAOs(cli);
    {
        std::lock_guard<std::mutex> lock(compatibleICAOsMutex_);
        for (const auto& icao : compatibleICAOs_)
        {
            GetIAFsForICAO(cli, icao);
        }
    }
    iafUpdateRequired.store(true, std::memory_order_release);
}

void AMANPlugin::PostSnapshotsToAPI(httplib::Client& cli, const std::string& icao)
{
    int type = connectionType.load(std::memory_order_acquire);
    if (type != CONNECTION_TYPE_DIRECT && type != CONNECTION_TYPE_SWEATBOX) return;

    nlohmann::json body;
    body["type"] = "SNAPSHOT";
    body["network"] = (type == CONNECTION_TYPE_SWEATBOX ? "sweatbox" : "live");
    body["flights"] = nlohmann::json::array();

    std::vector<Flight> flights;
    {
        // Copy the flights from the snapshot map
        std::lock_guard<std::mutex> lock(snapshotMapMutex_);
        auto it = snapshotMap_.find(icao);
        if (it != snapshotMap_.end())
        {
            flights = it->second;
        }
    }

    // Send snapshot even if empty to keep session alive

    for (const auto& flight : flights)
    {
        nlohmann::json flightJson;
        flightJson["callsign"] = flight.callsign;
        flightJson["aircraftType"] = flight.aircraType;
        flightJson["departure"] = flight.departure;
        flightJson["runwayId"] = flight.runwayId;
        flightJson["iaf"] = flight.iaf;
        if (flight.iafEta.empty()) flightJson["eta_iaf_utc"] = nullptr;
        else flightJson["eta_iaf_utc"] = flight.iafEta;
        flightJson["altitude"] = flight.altitude;
        flightJson["verticalSpeed"] = flight.verticalSpeed;
        flightJson["finalAltitude"] = flight.finalAltitude;
        flightJson["groundspeed"] = flight.groundspeed;
        flightJson["lat"] = flight.latitude;
        flightJson["lon"] = flight.longitude;

        body["flights"].push_back(flightJson);
    }

    std::string endpoint = std::string(type == CONNECTION_TYPE_SWEATBOX ? "/sweatbox" : "") + "/api/" + icao + "/feed";
    httplib::Headers headers = { {"User-Agent", "AMANplugin"}, {"Content-Type", "application/json"}, {"Authorization", "Bearer " + AUTH_SECRET}, {"X-Timestamp", std::to_string(std::time(nullptr))} };
    auto res = cli.Post(endpoint, headers, body.dump(), "application/json");

    if (!res) {
        if (printError) {
            printError = false;
            QueueError("Cannot reach the AMAN API server for ICAO " + icao + ": " + httplib::to_string(res.error()));
        }
        return;
    }

    if (res->status != 200 && res->status != 202) {
        if (res->status == 401) {
            if (printError) {
                printError = false;
                QueueError("Invalid Authorization key, make sure you are using latest plugin version.");
            }
            return;
        }
        if (printError) {
            printError = false;
            QueueError("Unexpected response from the AMAN API server for ICAO " + icao + ": " + std::to_string(res->status));
        }
        return;
    }

    printError = true; // Reset error flag on successful response
}

void AMANPlugin::PollRunwayAssignRequests(httplib::Client& cli)
{
    const int type = connectionType.load(std::memory_order_acquire);
    if (type != CONNECTION_TYPE_DIRECT && type != CONNECTION_TYPE_SWEATBOX) return;

    // Live and sweatbox are separate endpoints. A tag from one says nothing about
    // the other, and neither do the commands it was vouching for.
    if (type != rwyEtagNetwork_) {
        rwyEtagNetwork_ = type;
        rwyEtag_.clear();
        std::lock_guard<std::mutex> lock(rwyCommandsMutex_);
        rwyCommands_.clear();
    }

    const std::string endpoint = std::string(type == CONNECTION_TYPE_SWEATBOX ? "/sweatbox" : "") + "/api/runways";
    httplib::Headers headers = { {"User-Agent", "AMANplugin"} };
    if (!rwyEtag_.empty()) headers.emplace("If-None-Match", rwyEtag_);

    auto res = cli.Get(endpoint, headers);

    // printRwyError rather than printError: PostSnapshotsToAPI resets the shared
    // flag after every successful feed, which would re-raise a poll that keeps
    // failing every five seconds.
    if (!res) {
        if (printRwyError) {
            printRwyError = false;
            QueueError("Cannot reach the AMAN API server for runways: " + httplib::to_string(res.error()));
        }
        return;
    }

    // Unchanged since the last 200, so the cached set is still exactly the
    // server's. Whatever in it has not been written yet, OnTimer keeps retrying.
    if (res->status == 304) {
        printRwyError = true;
        return;
    }

    if (res->status != 200) {
        if (printRwyError) {
            printRwyError = false;
            QueueError("Unexpected response from the AMAN API server for runways: " + std::to_string(res->status));
        }
        return;
    }

    const auto received = std::chrono::steady_clock::now();
    std::vector<RunwayAssign> commands;
    try {
        const auto json = nlohmann::json::parse(res->body);
        const auto& list = json.at("commands");
        if (!list.is_array()) throw std::runtime_error("\"commands\" is not an array");

        // Expiry is carried over as the time left by the server's own clock, so a
        // controller's PC clock being off does not matter. Only needed when there
        // is something to expire.
        int64_t serverTimeMs = 0;
        if (!list.empty() && !ParseIsoUtcMs(StringField(json, "serverTime"), serverTimeMs))
            throw std::runtime_error("missing or malformed \"serverTime\"");

        for (const auto& command : list) {
            if (StringField(command, "type") != "RUNWAY_ASSIGN") continue;

            RunwayAssign assign;
            assign.callsign = StringField(command, "callsign");
            assign.arrivalIcao = StringField(command, "arrivalIcao");
            assign.runwayId = StringField(command, "runwayId");
            assign.since = StringField(command, "since");
            if (assign.callsign.empty() || assign.arrivalIcao.empty() || assign.runwayId.empty() || assign.since.empty()) continue;

            int64_t expiresAtMs = 0;
            if (!ParseIsoUtcMs(StringField(command, "expiresAt"), expiresAtMs) || expiresAtMs <= serverTimeMs) continue;
            assign.expiresAt = received + std::chrono::milliseconds(expiresAtMs - serverTimeMs);

            const std::string reason = StringField(command, "reason");
            if (reason == "sequencer") assign.reason = RunwayAssignReason::SEQUENCER;
            else if (reason == "config") assign.reason = RunwayAssignReason::CONFIG;
            else assign.reason = RunwayAssignReason::INVALID;

            commands.push_back(std::move(assign));
        }
    } catch (const std::exception& e) {
        if (printRwyError) {
            printRwyError = false;
            QueueError("Failed to parse the AMAN API response for runways: " + std::string(e.what()));
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(rwyCommandsMutex_);
        rwyCommands_ = std::move(commands);
    }
    // Only once the set is in place: the tag must never vouch for anything but
    // what is cached, or a 304 would confirm commands this plugin never saw.
    rwyEtag_ = res->get_header_value("ETag");
    printRwyError = true;
}

bool AMANPlugin::RegisterProvider()
{
    ESB_ProviderDecl decl = {};
    decl.struct_size = sizeof decl;
    decl.provider_id = PROVIDER_ID;
    // 2: rwy carries "<runway>/<reason>" where it used to carry the bare runway.
    // A consumer built against schema 1 would read the separator as part of the
    // runway, so this is a break rather than an addition.
    decl.schema_major = 2;
    decl.schema_minor = 0;
    decl.display_name = "AMAN Plugin";
    decl.contact = "https://github.com/vaccfr/AMAN-Plugin";
    decl.fields = FIELDS;
    decl.field_count = static_cast<uint32_t>(std::size(FIELDS));
    decl.module = ESB_SelfModule();

    const ESB_Status status = api_->register_provider(&decl, &provider_);
    if (status != ESB_OK) {
        provider_ = nullptr;

        if (status == ESB_E_PROVIDER_TAKEN) {
            providerConflict_ = true;
            DisplayError("Another loaded plugin already owns the \"" + std::string(PROVIDER_ID) + "\" bridge provider id.");
        }
        return false;
    }

    // Owning the field is what grants write authority; resolve() alone would
    // only ever produce a read handle.
    if (api_->own_field(provider_, IAF_FIELD, &iafField_) != ESB_OK) {
        iafField_ = ESB_FIELD_NONE;
        return false;
    }

    if (api_->own_field(provider_, RWY_FIELD, &rwyField_) != ESB_OK) {
        rwyField_ = ESB_FIELD_NONE;
        return false;
    }

    return true;
}

void AMANPlugin::PublishIAFsToBridge()
{
    if (iafField_ == ESB_FIELD_NONE) return;
    std::unordered_set<std::string> iafList;
    {
        std::lock_guard<std::mutex> lock(iafMapMutex_);
        for (const auto& [icao, iafs] : iafMap_)
        {
            iafList.insert(iafs.begin(), iafs.end());
        }
    }

    if (iafList.empty()) return;
    if (iafList.size() > 16) {
        QueueError("Too many IAFs to publish to the bridge. Max 16 allowed.");
        return;
    }

    std::string iafString;
    for (const auto& iaf : iafList)
    {
        if (!iafString.empty()) iafString += ",";
        iafString += iaf;
    }

    ESB_Value value = ESB_Str(iafString.c_str());
    api_->set_global(provider_, iafField_, &value);
}

void AMANPlugin::PublishRunwaysToBridge()
{
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(rwyWritten_, [&now](const auto& written) { return written.second <= now; });

    if (rwyField_ == ESB_FIELD_NONE || !isController.load(std::memory_order_acquire)) return;

    std::vector<RunwayAssign> commands;
    {
        std::lock_guard<std::mutex> lock(rwyCommandsMutex_);
        commands = rwyCommands_;
    }

    for (const auto& command : commands)
    {
        if (command.expiresAt <= now) continue;

        // The server repeats a decision until the feed catches up with it, so one
        // already written is skipped rather than written again: a controller who
        // turned a config change down would otherwise be asked on every tick.
        std::string key = command.callsign + "|" + command.runwayId + "|" + command.since;
        if (rwyWritten_.contains(key)) continue;

        // Deliberately not limited to the flights in our own snapshot: only the
        // controller feeding the sequencer tracks airports, and every controller
        // applies runways to the flights they own. So: the airport the command was
        // decided for, and only while this controller could amend the flight.
        // Anything else stays cached and is looked at again next tick, which is
        // how a flight handed over to us gets its runway.
        CFlightPlan fp = FlightPlanSelect(command.callsign.c_str());
        if (!fp.IsValid()) continue;

        std::string destination = fp.GetFlightPlanData().GetDestination();
        std::transform(destination.begin(), destination.end(), destination.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (destination != command.arrivalIcao) continue;

        const char* tracker = fp.GetTrackingControllerId();
        if (!fp.GetTrackingControllerIsMe() && tracker != nullptr && tracker[0] != '\0') continue;

        ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
        if (api_->aircraft(command.callsign.c_str(), &aircraft) != ESB_OK) continue;

        // The reason travels with the runway rather than in a field of its own. A
        // consumer applies the sequencer's order without asking and puts anything
        // else to the controller, so reading a new runway against the previous
        // reason would silently amend a flight plan nobody agreed to - which is
        // exactly what two fields and two notifications would allow.
        char reason = 'I';
        switch (command.reason) {
        case RunwayAssignReason::SEQUENCER: reason = 'S'; break;
        case RunwayAssignReason::CONFIG:    reason = 'C'; break;
        default:                            reason = 'I'; break;
        }

        const std::string payload = command.runwayId + "/" + reason;

        ESB_Value value = ESB_Str(payload.c_str());
        if (api_->set_ac(provider_, aircraft, rwyField_, &value) != ESB_OK) continue;

        // Recorded only once the bridge took it; a failed write is retried.
        rwyWritten_.emplace(std::move(key), command.expiresAt + RWY_WRITTEN_GRACE);
    }
}

void AMANPlugin::ResolveConsumedFields()
{
    if (api_ == nullptr) return;

    // ESB_E_NO_PROVIDER simply means CoFrance has not registered yet; leaving the
    // ids at 0 makes the next tick try again. Resolve, not own_field: these are
    // CoFrance's fields and we only ever read them.
    if (cofranceEtaField_ == 0)
        api_->resolve(COFRANCE_ETA_IAF, ESB_T_I64, &cofranceEtaField_);
    if (cofranceIafField_ == 0)
        api_->resolve(COFRANCE_IAF, ESB_T_STR, &cofranceIafField_);
}

std::string AMANPlugin::GetIafEtaFromBridge(const std::string& callsign,
                                            const std::string& iaf) const
{
    if (api_ == nullptr || callsign.empty()) return {};
    if (cofranceEtaField_ == ESB_FIELD_NONE || cofranceIafField_ == ESB_FIELD_NONE) return {};

    ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
    if (api_->aircraft(callsign.c_str(), &aircraft) != ESB_OK) return {};

    // Which fix the published time is over. Checked first so a disagreement costs
    // one read instead of two, and so the ETA is never accepted unlabelled.
    char fixBuf[VALUE_BUFFER];
    ESB_Value published = {};
    uint32_t fixBytes = sizeof fixBuf;

    // ESB_E_UNSET is the ordinary answer: CoFrance clears both fields as soon as a
    // flight passes its IAF or is vectored off the route it was predicted along.
    if (api_->get_ac(aircraft, cofranceIafField_, &published, fixBuf, &fixBytes) != ESB_OK) return {};
    if (published.type != ESB_T_STR) return {};

    const std::string publishedIaf(fixBuf, (std::min)(published.bytes,
                                                      static_cast<uint32_t>(sizeof fixBuf)));
    if (publishedIaf != iaf) return {};

    char etaBuf[VALUE_BUFFER];
    ESB_Value eta = {};
    uint32_t etaBytes = sizeof etaBuf;

    // A scalar needs no payload buffer, but one is passed anyway rather than relying
    // on the bridge tolerating a null for an I64 read.
    if (api_->get_ac(aircraft, cofranceEtaField_, &eta, etaBuf, &etaBytes) != ESB_OK) return {};
    if (eta.type != ESB_T_I64) return {};

    return FormatUtcHms(eta.v.i64);
}

