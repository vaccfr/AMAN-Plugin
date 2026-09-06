//
// Created by Hawai on 9/5/2026.
//

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
static constexpr const char* IAF_FIELD = "iaf";

/// The bridge is declared missing only after it has had a fair chance to load.
static constexpr int MISSING_TICKS_BEFORE_NOTICE = 10;

const ESB_FieldDecl FIELDS[] = {
    {
        IAF_FIELD,
        ESB_T_STR,
        ESB_SCOPE_GLOBAL,
        0u,
        97u, // 5 Char per IAF + 1 for comma + 1 for null terminator, max 16 IAFs
        "AMAN list of IAFs tracked",
    },
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
            for (int i = pointNumber - 1; i >= std::max(0, pointNumber - MAX_IAF_LOOKUP_IN_ROUTE); --i) {
                std::string fix = route.GetPointName(i);
                if (iafMap.contains(destIcao) && iafMap[destIcao].contains(fix))
                {
                    flightsMap[destIcao].emplace_back(
                        rt.GetCallsign(),
                        fp.GetFlightPlanData().GetAircraftFPType(),
                        fp.GetFlightPlanData().GetOrigin(),
                        fix,
                        "", //TODO: get from CoFrance
                        pos.GetFlightLevel(),
                        pos.GetVerticalSpeed(), // Get from CoFrance
                        fp.GetFlightPlanData().GetFinalAltitude(),
                        pos.GetReportedGS(),
                        pos.GetPosition().m_Latitude,
                        pos.GetPosition().m_Longitude
                    );
                }
                break;
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

        if (counter % PERIODIC_POST_TIME_INTERVAL == 5)
        {
            for (const auto& icao : trackedICAOs_)
            {
                PostSnapshotsToAPI(*cli, icao);
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

    if (res->status != 200) {
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
    nlohmann::json body;
    body["type"] = "SNAPSHOT";
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
        flightJson["iaf"] = flight.iaf;
        flightJson["eta_iaf_utc"] = flight.iafEta;
        flightJson["altitude"] = flight.altitude;
        flightJson["verticalSpeed"] = flight.verticalSpeed;
        flightJson["finalAltitude"] = flight.finalAltitude;
        flightJson["groundspeed"] = flight.groundspeed;
        flightJson["lat"] = flight.latitude;
        flightJson["lon"] = flight.longitude;

        body["flights"].push_back(flightJson);
    }


    httplib::Headers headers = { {"User-Agent", "AMANplugin"}, {"Content-Type", "application/json"}, {"Authorization", "Bearer " + AUTH_SECRET}, {"X-Timestamp", std::to_string(std::time(nullptr))} };
    auto res = cli.Post("/api/" + icao + "/feed", headers, body.dump(), "application/json");

    if (!res) {
        if (printError) {
            printError = false;
            QueueError("Cannot reach the AMAN API server for ICAO " + icao + ": " + httplib::to_string(res.error()));
        }
        return;
    }

    if (res->status != 200) {
        if (printError) {
            printError = false;
            QueueError("Unexpected response from the AMAN API server for ICAO " + icao + ": " + std::to_string(res->status));
        }
        return;
    }

    printError = true; // Reset error flag on successful response
}

bool AMANPlugin::RegisterProvider()
{
    ESB_ProviderDecl decl = {};
    decl.struct_size = sizeof decl;
    decl.provider_id = PROVIDER_ID;
    decl.schema_major = 1;
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

