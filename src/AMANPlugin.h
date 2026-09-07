//
// Created by Hawai on 9/5/2026.
//

#ifndef AMAN_PLUGIN_AMANPLUGIN_H
#define AMAN_PLUGIN_AMANPLUGIN_H


#include <windows.h>

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <utility>
#include <unordered_set>

#include <EuroScopePlugIn.h>

#include <httplib.h>

using namespace EuroScopePlugIn;

struct ESB_Api_v1;
struct ESB_Provider;

namespace amanplugin
{

class AMANPlugin : public CPlugIn
{
    static constexpr int PERIODIC_POST_TIME_INTERVAL = 5; // seconds
    static constexpr int CONFIG_REFRESH_INTERVAL = 60; // seconds
    static constexpr const char* API_URL = "aman-vatsim.lunair.fr";
    static constexpr int MAX_IAF_LOOKUP_IN_ROUTE = 20; // Max number of waypoint after IAF (used to limit search time per route)

    struct Flight
    {
        std::string callsign;   // required every snapshot
        std::string aircraType;  // required every snapshot
        std::string departure;  // required every snapshot
        std::string runwayId;   // required every snapshot
        std::string iaf;        // required every snapshot
        std::string iafEta;     // required every snapshot
        int altitude;
        int verticalSpeed;
        int finalAltitude;
        int groundspeed;
        double latitude;
        double longitude;

        Flight(const std::string& callsign, const std::string& aicraType, const std::string& departure, const std::string& runwayId, const std::string& iaf, const std::string& iafEta, int altitude = 0, int verticalSpeed = 0, int finalAltitude = 0, int groundspeed = 0, double latitude = 0.0, double longitude = 0.0)
            : callsign(callsign), aircraType(aicraType), departure(departure), runwayId(runwayId), iaf(iaf), iafEta(iafEta), altitude(altitude), verticalSpeed(verticalSpeed), finalAltitude(finalAltitude), groundspeed(groundspeed), latitude(latitude), longitude(longitude) {}
    };

public:
    AMANPlugin();
    ~AMANPlugin();

    // Plugin lifecycle methods
    void Initialize();
    void Shutdown();

    //Message management
    void DisplayMessage(const std::string& message);
    void QueueMessage(const std::string& message); // Needed since Euroscope is not threadsafe
    void DisplayError(const std::string& message);
    void QueueError(const std::string& message); // Needed since Euroscope is not threadsafe

    // Scope events
    void OnTimer(int Counter) override;
    bool OnCompileCommand(const char* sCommandLine) override;

private:
    void WorkerThread();
    void GetCompatibleICAOs(httplib::Client& cli);
    void GetIAFsForICAO(httplib::Client& cli, const std::string& icao);
    void RefreshConfig(httplib::Client& cli);
    void PostSnapshotsToAPI(httplib::Client& cli, const std::string& icao);

    bool RegisterProvider();
    void PublishIAFsToBridge();

    /// Resolve the fields we read from CoFrance. Retried from OnTimer until they
    /// land: EuroScope loads plugins in the order the user's settings file lists
    /// them, so CoFrance may well register after we do.
    void ResolveConsumedFields();

    /// CoFrance's predicted ETA over @p iaf for @p callsign, formatted "HH:MM:SS"
    /// UTC. Empty whenever there is no honest answer - bridge absent, CoFrance not
    /// loaded, nothing published for this flight, or the aircraft has passed the
    /// fix - and empty is serialised as a bare null rather than "".
    ///
    /// The published fix is checked against @p iaf before the time is accepted.
    /// CoFrance scans the whole remaining route while this plugin scans a window at
    /// the end of it, so the two can legitimately land on different fixes; an ETA
    /// over a fix we are not reporting is worse than none.
    std::string GetIafEtaFromBridge(const std::string& callsign,
                                    const std::string& iaf) const;

private:
    // Plugin state
    bool initialized_ = false;
    bool printError = true;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::atomic<bool> iafUpdateRequired{false};

    // Bridge
    const ESB_Api_v1* api_ = nullptr;
    ESB_Provider* provider_ = nullptr;
    bool providerConflict_ = false;   // Another module owns "amanplugin"
    int missingTicks_ = 0;
    bool missingReported_ = false;    // ESB_MISSING_MESSAGE is said once
    uint32_t iafField_ = 0;          // 0 == still unresolved

    // Read from CoFrance, which owns them. eta_iaf is UTC Unix seconds; iaf names
    // the fix that time is over. 0 == still unresolved.
    uint32_t cofranceEtaField_ = 0;
    uint32_t cofranceIafField_ = 0;

    // Message management
    std::mutex messageQueueMutex_;
    std::vector<std::pair<std::string, bool>> messageQueue_; // Pair of message and isError flag

    std::mutex compatibleICAOsMutex_;
    std::unordered_set<std::string> compatibleICAOs_; // ICAOs that are compatible

    std::mutex trackedICAOsMutex_;
    std::unordered_set<std::string> trackedICAOs_; // ICAOs for which to send snapshots

    std::mutex snapshotMapMutex_;
    std::unordered_map<std::string, std::vector<Flight>> snapshotMap_; // Map of ICAOs to their snapshots

    std::mutex iafMapMutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> iafMap_; // Map of ICAOs to their IAFs
};

} // namespace amanplugin

#endif //AMAN_PLUGIN_AMANPLUGIN_H