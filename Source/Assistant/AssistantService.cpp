#include "AssistantService.h"
#include "../Cloud/Revisions.h"
#include <thread>

#ifndef JUCE_APPLICATION_VERSION_STRING
 #define JUCE_APPLICATION_VERSION_STRING ""   // the tests build this file without the app's version
#endif

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
            pending->result = runTool (name, args);
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
        String note;   // what the person changed since the assistant last looked
    };
    auto started = std::make_shared<Started>();
    auto flag = alive;

    MessageManager::callAsync ([this, flag, started, name, args]
    {
        if (flag->load())
        {
            noteActivity();
            if (paused.load())
                started->immediate = pausedText();
            else
            {
                // These read the song (or play it): they run on what's there now, and say what changed.
                const auto change = watch.compare (currentProject(), ctx.edit.projectFile);
                started->job = tools.startLongTool (name, args, started->immediate);
                watch.seen (currentProject(), ctx.edit.projectFile);
                if (change.any)
                    started->note = "Since you last looked, the person changed:\n" + change.list();
                if (started->job != nullptr)
                    ++(*longToolsRunning);
            }
        }
        started->done.signal();
    });

    for (int waited = 0; ! started->done.wait (100); waited += 100)
        if (waited > 15000 || ! running.load() || ! flag->load())
            return toolText ("Lathe didn't answer in time (it may be busy loading, or showing a dialog). Try again.", true);

    if (started->job == nullptr)
        return started->immediate.isVoid() ? toolText ("Lathe is closing.", true) : started->immediate;

    // Counted as working until the job ends, even if this wait gives up first.
    std::shared_ptr<void> done (nullptr, [count = longToolsRunning] (void*) { --(*count); });
    for (int waited = 0; waited < kLongToolSeconds * 1000; waited += 100)
    {
        if (started->job->done.wait (100))
            return started->note.isEmpty() ? started->job->result : withNote (started->job->result, started->note);
        if (! running.load() || ! flag->load())
            break;
    }
    // Still running: count it until it finishes.
    std::thread ([job = started->job, done] { job->done.wait (-1); }).detach();
    return toolText (name == "export_audio" ? "The export is still going. Lathe's status bar says when it's done."
                     : name == "analyze_mix" ? "The analysis didn't finish in time. Try fewer bars."
                                             : "The measurement didn't finish in time.", true);
}

//==============================================================================
var Service::currentProject() const
{
    return cloud::Snapshot::capture (ctx.edit).project;
}

void Service::noteActivity (const String& status)
{
    lastCallMs = Time::getMillisecondCounter();
    if (status.isNotEmpty())
        lastStatus = status;
    if (onActivity)
        onActivity();
}

bool Service::isWorking() const
{
    if (longToolsRunning->load() > 0)
        return true;
    return lastCallMs != 0 && Time::getMillisecondCounter() - lastCallMs < 45000;
}

void Service::setPaused (bool p)
{
    paused.store (p);
    ctx.showStatus (p ? "Assistant paused: it stops at its next step"
                      : "Assistant can carry on: tell it to, in your AI app");
    if (onActivity)
        onActivity();
}

var Service::pausedText() const
{
    return toolText ("Paused: the person pressed Pause in Lathe, so nothing ran and nothing changed. Stop here. "
                     "Tell them what you've done so far and what's left, then wait. When they want you to carry on "
                     "they press Continue in Lathe; if they tell you to go on and you get this message again, ask "
                     "them to press Continue. Every Lathe tool answers like this until then.", true);
}

var Service::runTool (const String& name, const var& args)
{
    if (paused.load())
    {
        noteActivity();
        return pausedText();
    }

    const auto file = ctx.edit.projectFile;
    const auto now = currentProject();
    const auto change = watch.compare (now, file);

    // Never change the song on an out-of-date picture of it: the person edited it since the
    // assistant last looked. Faders, pans, mutes, sends and the like don't stop it (people move
    // those while listening); the assistant is just told.
    if (change.structural && SessionWatch::changesSong (name))
    {
        watch.seen (now, file);   // it knows now: the next try goes through
        noteActivity ("Assistant held back: you changed the session since it last looked");
        ctx.showStatus (lastStatus);
        if (change.otherProject.isNotEmpty())
            return toolText ("Nothing was changed. The person opened a different project (\"" + change.otherProject
                             + "\") since you last looked. Ask them whether to carry on in this one before changing anything.", true);
        return toolText ("Nothing was changed. The person has edited the session since you last looked, so this was held "
                         "back rather than made on an out-of-date picture of the song. They changed:\n" + change.list()
                         + "\nLook again (describe_session, or describe_track for those tracks), check your plan still fits, "
                           "then make the change again. If what they did conflicts with what you were about to do, ask them.", true);
    }

    String status;
    auto result = tools.call (name, args, status);
    if (status.isNotEmpty())
        ctx.showStatus (status);
    noteActivity (status);
    watch.seen (currentProject(), ctx.edit.projectFile);

    if (change.any)
        result = withNote (result, "Since you last looked, the person changed:\n" + change.list()
                                       + (SessionWatch::changesSong (name) ? "\n(Your change was made on top of theirs.)" : String()));
    return result;
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
