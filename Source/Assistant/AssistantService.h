#pragma once

#include "../UI/Widgets.h"
#include "AssistantServer.h"
#include "AssistantTools.h"
#include "SessionWatch.h"

namespace lathe::assistant
{
//==============================================================================
/** Lathe's AI assistant connection: the MCP server, switched on and off from
    Options > AI Assistant. Off until you turn it on; remembers your choice. A new
    key is made every time it starts, so an old copy of assistant.json is useless. */
class Service
{
public:
    explicit Service (AppContext&);
    ~Service();

    bool isOn() const   { return server != nullptr && server->isRunning(); }
    Result setOn (bool);
    /** Starts again with a new key: anything connected must read assistant.json again
        (LatheMCP does that by itself on its next request). */
    Result newKey();

    String statusText() const;
    /** LatheMCP, next to Lathe (inside Lathe.app on the Mac). */
    File bridgeFile() const;
    String desktopConfig() const;
    String claudeCodeCommand() const;

    /** How the tools make, open and save projects: the app fills these in. */
    Tools::ProjectHooks& projectHooks()   { return tools.project; }

    /** The person's side of working together (message thread). While paused, every tool call is
        turned away with a message telling the assistant to stop and wait. */
    bool isPaused() const   { return paused.load(); }
    void setPaused (bool);
    /** An AI app has called a tool lately, or a long one (an export, say) is still running. */
    bool isWorking() const;
    /** What the assistant did last, as Lathe's status bar said it. */
    String lastAction() const   { return lastStatus; }
    /** Called on the message thread when the assistant does something, or pause changes. */
    std::function<void()> onActivity;

    /** One tool call, on the message thread (the server's thread hands calls here): the pause,
        then whether the person changed the session since the assistant last looked, then the tool. */
    var runTool (const String& name, const var& args);

private:
    var callTool (const String& name, const var& args, const std::atomic<bool>& running);
    var callLongTool (const String& name, const var& args, const std::atomic<bool>& running);
    var pausedText() const;
    void noteActivity (const String& status = {});
    var currentProject() const;
    void stopServer();

    AppContext& ctx;
    Tools tools;
    SessionWatch watch;   // the session as the assistant last saw it
    std::atomic<bool> paused { false };
    std::shared_ptr<std::atomic<int>> longToolsRunning = std::make_shared<std::atomic<int>> (0);
    uint32 lastCallMs = 0;
    String lastStatus;
    std::unique_ptr<Server> server;
    std::shared_ptr<std::atomic<bool>> serverRunning;   // false once this server is being stopped
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};
} // namespace lathe::assistant
