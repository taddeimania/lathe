#pragma once

#include "../Engine/Engine.h"

namespace lathe::assistant
{
//==============================================================================
/** What the AI assistant tools do, against the open project. Message thread only.

    So far they only read the project, plus Play and Stop. Tools that change the
    project will go through undo, one step per call. */
class Tools
{
public:
    Tools (Edit&, Engine&);

    /** Runs a tool. Returns an MCP tool result. `status`, if set, is shown in Lathe's
        status bar so you can see what the assistant did. */
    var call (const String& name, const var& arguments, String& status);

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

    Edit& edit;
    Engine& engine;
};
} // namespace lathe::assistant
