#pragma once

#include "../Engine/Engine.h"

namespace lathe::assistant
{
//==============================================================================
/** What the AI assistant tools do, against the open project. Message thread only.

    They read the project, control the transport, and change the project. Each call
    that changes the project is one undo step in Lathe (Cmd/Ctrl+Z takes it back). */
class Tools
{
public:
    Tools (Edit&, Engine&);
    ~Tools();

    /** A tool that takes a while (measure_performance, compare_processing_cores): Lathe plays
        for real meanwhile, so it runs on the message thread's timer, not in one call. */
    struct Job
    {
        WaitableEvent done { true };   // stays signalled once it's finished
        var result;                     // set before done is signalled
    };
    static bool isLongTool (const String& name);
    /** Starts one. Returns null with `immediate` set if it can't start (the reason) . */
    std::shared_ptr<Job> startLongTool (const String& name, const var& args, var& immediate);
    /** Lathe's status bar, for news during a long tool. */
    std::function<void (const String&)> showStatus;

    /** Runs a tool. Returns an MCP tool result. `status`, if set, is shown in Lathe's
        status bar so you can see what the assistant did. */
    var call (const String& name, const var& arguments, String& status);

    /** How edit_session loads a plug-in. The app's own loader; tests put in their own. */
    std::function<std::shared_ptr<PluginSlot> (const PluginDescription&, String& error)> loadPlugin;

    // Building blocks (public for the tests).
    var describeSession() const;
    var describeTrack (const Track&) const;
    StringArray findProblems() const;
    /** By id, or by name (any case); also "Master" and returns. */
    std::shared_ptr<Track> findTrack (const var& nameOrId) const;

private:
    var trackSummary (const Track&) const;
    var pluginList (const Track&) const;
    String describeInput (const Track&) const;
    String describeOutput (const Track&) const;
    double toBar (double beats) const     { return 1.0 + beats / edit.beatsPerBar(); }
    double toBeats (double bar) const     { return (bar - 1.0) * edit.beatsPerBar(); }

    var getNotes (const var& args) const;
    var play (const var& args, String& status);
    var stop (String& status);
    var setLoop (const var& args, String& status);
    var listPlugins (const var& args) const;
    var editSession (const var& args, String& status);

    /** The installed plug-in a name means: exact name first, then a unique partial match.
        Sets `error` (naming the candidates when it's ambiguous) and returns nothing otherwise. */
    std::optional<PluginDescription> findInstalledPlugin (const String& name, String& error) const;
    /** "instrument", "note generator" or "effect", as far as Lathe can tell without loading it. */
    String pluginKind (const PluginDescription&) const;
    String barText (double beats) const;

    class Measurer;
    friend class Measurer;
    std::unique_ptr<Measurer> measurer;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    void measurerFinished (const Measurer*);

    Edit& edit;
    Engine& engine;
};
} // namespace lathe::assistant
