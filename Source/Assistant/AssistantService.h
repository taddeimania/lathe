#pragma once

#include "../UI/Widgets.h"
#include "AssistantServer.h"
#include "AssistantTools.h"

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

private:
    var callTool (const String& name, const var& args, const std::atomic<bool>& running);
    var callLongTool (const String& name, const var& args, const std::atomic<bool>& running);
    void stopServer();

    AppContext& ctx;
    Tools tools;
    std::unique_ptr<Server> server;
    std::shared_ptr<std::atomic<bool>> serverRunning;   // false once this server is being stopped
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};
} // namespace lathe::assistant
