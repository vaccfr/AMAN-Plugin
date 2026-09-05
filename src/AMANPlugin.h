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

#include <EuroScopePlugIn.h>

using namespace EuroScopePlugIn;

namespace amanplugin
{

class AMANPlugin : public CPlugIn
{
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

private:
    // Plugin state
    bool initialized_ = false;
    bool printError = true;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    
    // Message management
    std::mutex messageQueueMutex_;
    std::vector<std::pair<std::string, bool>> messageQueue_; // Pair of message and isError flag
};

} // namespace amanplugin

#endif //AMAN_PLUGIN_AMANPLUGIN_H