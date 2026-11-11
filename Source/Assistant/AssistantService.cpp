#include "AssistantService.h"

namespace lathe::assistant
{
Service::Service (AppContext& c) : ctx (c), tools (c.edit, c.engine)
{
    tools.showStatus = [this] (const String& s) { ctx.showStatus (s); };
    if (ctx.settings.getBoolValue ("assistantOn", false))
        if (auto r = setOn (true); r.failed())
            ctx.showStatus ("AI assistant connection: " + r.getErrorMessage());
}

Service::~Service()
{
    alive->store (false);
    stopServer();   // a request waiting for the message thread gives up
}

Result Service::setOn (bool on)
{
    ctx.settings.setValue ("assistantOn", on);
    ctx.settings.saveIfNeeded();

    if (! on)
    {
        stopServer();
        return Result::ok();
    }
    if (isOn())
        return Result::ok();

    Server::Handler h;
    auto flag = alive;
    auto running = std::make_shared<std::atomic<bool>> (true);
    serverRunning = running;
    h.callTool = [this, flag, running] (const String& name, const var& args) -> var
    {
        if (! flag->load() || ! running->load())
            return toolText ("Lathe is closing.", true);
        return callTool (name, args, *running);
    };
    h.onConnect = [this, flag] (const String& client)
    {
        MessageManager::callAsync ([this, flag, client]
        {
            if (flag->load())
                ctx.showStatus ("AI assistant connected: " + client);
        });
    };

    h.version = JUCE_APPLICATION_VERSION_STRING;
    server = std::make_unique<Server> (std::move (h));
    auto r = server->start (makeToken());
    if (r.failed())
        stopServer();
    return r;
}

Result Service::newKey()
{
    if (! isOn())
        return Result::ok();
    stopServer();
    return setOn (true);
}

void Service::stopServer()
{
    if (serverRunning != nullptr)
        serverRunning->store (false);
    server.reset();
    serverRunning.reset();
}

var Service::callTool (const String& name, const var& args, const std::atomic<bool>& running)
{
    if (Tools::isLongTool (name))
        return callLongTool (name, args, running);

    // The project belongs to the message thread: run the tool there and wait for it.
    struct Pending
    {
        WaitableEvent done;
        var result;
    };
    auto pending = std::make_shared<Pending>();
    auto flag = alive;

    MessageManager::callAsync ([this, flag, pending, name, args]
    {
        if (flag->load())
        {
            String status;
            pending->result = tools.call (name, args, status);
            if (status.isNotEmpty())
                ctx.showStatus (status);
        }
        pending->done.signal();
    });

    for (int waited = 0; waited < 15000; waited += 100)
    {
        if (pending->done.wait (100))
            return pending->result.isVoid() ? toolText ("Lathe is closing.", true) : pending->result;
        if (! running.load() || ! flag->load())
            break;
    }
    return toolText ("Lathe didn't answer in time (it may be busy loading, or showing a dialog). Try again.", true);
}

var Service::callLongTool (const String& name, const var& args, const std::atomic<bool>& running)
{
    // Started on the message thread, which stays free while Lathe plays; this thread waits.
    struct Started
    {
        WaitableEvent done;
        std::shared_ptr<Tools::Job> job;
        var immediate;
    };
    auto started = std::make_shared<Started>();
    auto flag = alive;

    MessageManager::callAsync ([this, flag, started, name, args]
    {
        if (flag->load())
            started->job = tools.startLongTool (name, args, started->immediate);
        started->done.signal();
    });

    for (int waited = 0; ! started->done.wait (100); waited += 100)
        if (waited > 15000 || ! running.load() || ! flag->load())
            return toolText ("Lathe didn't answer in time (it may be busy loading, or showing a dialog). Try again.", true);

    if (started->job == nullptr)
        return started->immediate.isVoid() ? toolText ("Lathe is closing.", true) : started->immediate;

    for (int waited = 0; waited < kLongToolSeconds * 1000; waited += 100)
    {
        if (started->job->done.wait (100))
            return started->job->result;
        if (! running.load() || ! flag->load())
            break;
    }
    return toolText (name == "export_audio" ? "The export is still going. Lathe's status bar says when it's done."
                                            : "The measurement didn't finish in time.", true);
}

String Service::statusText() const
{
    if (! isOn())
        return "Off. AI apps can't see or control Lathe.";

    const auto a = server->getActivity();
    String s = "On, listening on 127.0.0.1:" + String (server->getPort()) + " (this computer only).";
    if (a.requests == 0)
        s << "\nNo AI app has connected yet.";
    else
    {
        s << "\nLast request " << a.lastRequest.toString (false, true, true) << " from " << (a.client.isNotEmpty() ? a.client : String ("an AI app"));
        if (a.lastTool.isNotEmpty())
            s << " (last tool: " << a.lastTool << ")";
        s << ".";
    }
    if (a.refused > 0)
        s << "\nRefused " << a.refused << (a.refused == 1 ? " request" : " requests") << " without the right key.";
    return s;
}

File Service::bridgeFile() const
{
    const auto exe = File::getSpecialLocation (File::currentExecutableFile);
   #if JUCE_WINDOWS
    return exe.getSiblingFile ("LatheMCP.exe");
   #else
    return exe.getSiblingFile ("LatheMCP");
   #endif
}

String Service::desktopConfig() const
{
    auto* server = new DynamicObject();
    server->setProperty ("command", bridgeFile().getFullPathName());
    auto* servers = new DynamicObject();
    servers->setProperty ("lathe", var (server));
    auto* root = new DynamicObject();
    root->setProperty ("mcpServers", var (servers));
    return JSON::toString (var (root));
}

String Service::claudeCodeCommand() const
{
    return "claude mcp add lathe -- " + bridgeFile().getFullPathName().quoted();
}
} // namespace lathe::assistant
