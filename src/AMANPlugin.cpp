//
// Created by Hawai on 9/5/2026.
//

#include "AMANPlugin.h"
#include "version.h"
#include "core/CompileCommands.h"


using namespace amanplugin;
using namespace EuroScopePlugIn;

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
    //TODO:
}

void AMANPlugin::QueueMessage(const std::string& message)
{
    std::lock_guard<std::mutex> lock(messageQueueMutex_);
    messageQueue_.emplace_back<std::pair<std::string, bool>>({message, false});
}

void AMANPlugin::WorkerThread()
{
    size_t counter = 0;

    while (m_stop.load(std::memory_order_acquire) == false)
    {
        //TODO:
        counter++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Avoid busy waiting
    }
}

