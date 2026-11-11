#include "AssistantShared.h"

#include <random>

#if ! JUCE_WINDOWS
 #include <sys/socket.h>
 #include <sys/stat.h>
#endif

namespace lathe::assistant
{
File settingsFolder()
{
    // The same folder PropertiesFile uses for Lathe.settings (see Main.cpp).
    const auto data = File::getSpecialLocation (File::userApplicationDataDirectory);
   #if JUCE_MAC
    return data.getChildFile ("Application Support").getChildFile ("Lathe");
   #else
    return data.getChildFile ("Lathe");
   #endif
}

File endpointFile()
{
    return settingsFolder().getChildFile ("assistant.json");
}

std::optional<Endpoint> readEndpoint (const File& f)
{
    if (! f.existsAsFile())
        return std::nullopt;
    const auto v = JSON::parse (f.loadFileAsString());
    Endpoint e;
    e.port = (int) v.getProperty ("port", 0);
    e.token = v.getProperty ("token", {}).toString();
    if (e.port <= 0 || e.port > 65535 || e.token.length() < 32)
        return std::nullopt;
    return e;
}

bool writeEndpoint (const Endpoint& e, const File& f)
{
    auto* o = new DynamicObject();
    o->setProperty ("port", e.port);
    o->setProperty ("token", e.token);
    o->setProperty ("note", "Written by Lathe while its AI assistant connection is on. Keep this key private.");

    f.getParentDirectory().createDirectory();
    // Make the file private before the key goes in it, then write into that same file
    // (replaceWithText would swap in a new one, with default permissions).
    if (! f.existsAsFile() && ! f.create())
        return false;
   #if ! JUCE_WINDOWS
    if (::chmod (f.getFullPathName().toRawUTF8(), 0600) != 0)
        return false;
   #endif
    FileOutputStream out (f);
    if (! out.openedOk())
        return false;
    out.setPosition (0);
    out.truncate();
    out.writeText (JSON::toString (var (o)), false, false, nullptr);
    out.flush();
    return out.getStatus().wasOk();
}

String makeToken()
{
    std::random_device rd;   // the OS's secure source (getrandom/arc4random/RtlGenRandom)
    String s;
    for (int i = 0; i < 8; ++i)
        s << String::toHexString ((int) rd()).paddedLeft ('0', 8);
    return s;
}

bool tokensMatch (const String& a, const String& b)
{
    const auto x = a.toStdString(), y = b.toStdString();
    unsigned diff = (unsigned) (x.size() ^ y.size());
    for (size_t i = 0; i < x.size(); ++i)
        diff |= (unsigned) (uint8) x[i] ^ (unsigned) (uint8) (i < y.size() ? y[i] : 0);
    return diff == 0 && ! x.empty();
}

//==============================================================================
namespace
{
bool readSome (StreamingSocket& s, MemoryBlock& into, uint32 deadline)
{
    const auto now = Time::getMillisecondCounter();
    if (now >= deadline)
        return false;
    if (s.waitUntilReady (true, (int) (deadline - now)) != 1)
        return false;
    char buf[8192];
    const int n = s.read (buf, (int) sizeof (buf), false);
    if (n <= 0)
        return false;
    into.append (buf, (size_t) n);
    return true;
}
} // namespace

bool readHttp (StreamingSocket& s, HttpMessage& m, bool isRequest, int timeoutMs)
{
    const auto deadline = Time::getMillisecondCounter() + (uint32) timeoutMs;
    MemoryBlock data;
    int headerEnd = -1;

    while (headerEnd < 0)
    {
        if (data.getSize() > 16384 || ! readSome (s, data, deadline))
            return false;
        const String sofar (static_cast<const char*> (data.getData()), data.getSize());
        headerEnd = sofar.indexOf ("\r\n\r\n");
    }

    const String head (static_cast<const char*> (data.getData()), (size_t) headerEnd);
    auto lines = StringArray::fromTokens (head, "\r\n", {});
    lines.removeEmptyStrings();
    if (lines.isEmpty())
        return false;

    const auto first = StringArray::fromTokens (lines[0], " ", {});
    if (first.size() < 2)
        return false;
    if (isRequest)
    {
        m.method = first[0];
        m.path = first[1];
    }
    else
    {
        m.status = first[1].getIntValue();
    }

    for (int i = 1; i < lines.size(); ++i)
    {
        const int colon = lines[i].indexOfChar (':');
        if (colon > 0)
            m.headers.set (lines[i].substring (0, colon).trim().toLowerCase(), lines[i].substring (colon + 1).trim());
    }

    const auto lengthText = m.header ("content-length");
    const int64 length = lengthText.isEmpty() ? (isRequest ? 0 : -1) : lengthText.getLargeIntValue();
    if (length > kMaxBody)
        return false;

    const size_t bodyStart = (size_t) headerEnd + 4;
    if (length < 0)
    {
        // A response without a length: read until the other end closes.
        while (data.getSize() - bodyStart <= (size_t) kMaxBody && readSome (s, data, deadline)) {}
    }
    else
    {
        while (data.getSize() < bodyStart + (size_t) length)
            if (! readSome (s, data, deadline))
                return false;
    }

    const auto bodySize = length < 0 ? data.getSize() - bodyStart : (size_t) length;
    m.body = String::fromUTF8 (static_cast<const char*> (data.getData()) + bodyStart, (int) bodySize);
    return true;
}

bool writeAll (StreamingSocket& s, const String& text)
{
    // Writing to a connection the other end already closed must fail, not raise SIGPIPE
    // (which would quit Lathe or LatheMCP). JUCE's write() doesn't ask for that.
    const int fd = s.getRawSocketHandle();
    if (! s.isConnected() || fd < 0)
        return false;
   #if JUCE_MAC || JUCE_IOS
    int one = 1;
    ::setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
   #endif

    const auto* p = text.toRawUTF8();
    const int total = (int) text.getNumBytesAsUTF8();
    int done = 0;
    while (done < total)
    {
       #if JUCE_LINUX || JUCE_BSD || JUCE_ANDROID
        const int n = (int) ::send (fd, p + done, (size_t) (total - done), MSG_NOSIGNAL);
       #else
        const int n = s.write (p + done, total - done);
       #endif
        if (n <= 0)
            return false;
        done += n;
    }
    return true;
}

String httpResponse (int status, const String& body, const String& contentType)
{
    const char* reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 400 ? "Bad Request"
                       : status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 404 ? "Not Found"
                       : status == 405 ? "Method Not Allowed" : status == 413 ? "Payload Too Large" : "Error";
    String r;
    r << "HTTP/1.1 " << status << " " << reason << "\r\n";
    if (body.isNotEmpty())
        r << "Content-Type: " << contentType << "\r\n";
    r << "Content-Length: " << (int) body.getNumBytesAsUTF8() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "Connection: close\r\n\r\n"
      << body;
    return r;
}

int postToLathe (const Endpoint& e, const String& json, String& responseBody, int timeoutMs)
{
    StreamingSocket s;
    if (! s.connect ("127.0.0.1", e.port, 2000))
        return 0;

    String req;
    req << "POST /mcp HTTP/1.1\r\n"
        << "Host: 127.0.0.1:" << e.port << "\r\n"
        << "Authorization: Bearer " << e.token << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Accept: application/json, text/event-stream\r\n"
        << "Content-Length: " << (int) json.getNumBytesAsUTF8() << "\r\n"
        << "Connection: close\r\n\r\n"
        << json;
    if (! writeAll (s, req))
        return 0;

    HttpMessage m;
    if (! readHttp (s, m, false, timeoutMs))
        return 0;
    responseBody = m.body;
    return m.status;
}

//==============================================================================
namespace
{
var object (std::initializer_list<std::pair<const char*, var>> props)
{
    auto* o = new DynamicObject();
    for (auto& [k, v] : props)
        o->setProperty (k, v);
    return var (o);
}

var schema (std::initializer_list<std::pair<const char*, var>> props, StringArray required = {})
{
    auto* p = new DynamicObject();
    for (auto& [k, v] : props)
        p->setProperty (k, v);
    var s = object ({ { "type", "object" }, { "properties", var (p) } });
    if (! required.isEmpty())
    {
        Array<var> r;
        for (auto& x : required)
            r.add (x);
        s.getDynamicObject()->setProperty ("required", r);
    }
    return s;
}

var prop (const char* type, const char* description)
{
    return object ({ { "type", type }, { "description", description } });
}

var tool (const char* name, const char* title, const char* description, var inputSchema, bool readOnly, bool destructive = false)
{
    return object ({ { "name", name },
                     { "title", title },
                     { "description", description },
                     { "inputSchema", inputSchema },
                     { "annotations", object ({ { "readOnlyHint", readOnly },
                                                { "destructiveHint", destructive },
                                                { "idempotentHint", readOnly },
                                                { "openWorldHint", false } }) } });
}
} // namespace

var toolDefinitions()
{
    Array<var> tools;
    tools.add (tool ("describe_session", "Describe the session",
                     "Overview of the project open in Lathe: tempo, time signature, transport (playing, recording, "
                     "position), every track with its kind, input, output, monitoring, arm/mute/solo, volume and pan, "
                     "plug-ins in signal order (and which ones generate notes), number of clips, the returns and "
                     "the master. Also lists likely reasons a track can't be heard. Start here.",
                     schema ({}), true));
    tools.add (tool ("describe_track", "Describe a track",
                     "Everything about one track (or a return, or \"Master\"): input, output, monitoring, plug-ins "
                     "in signal order, sends, EQ, and every clip with its id, position in bars and length "
                     "(MIDI clips: note count and pitch range; audio clips: file and gain).",
                     schema ({ { "track", prop ("string", "Track name (as shown in Lathe) or numeric id") } }, { "track" }),
                     true));
    tools.add (tool ("get_notes", "Get MIDI notes",
                     "The notes in a MIDI track's clips, in time order: pitch (MIDI number and name, middle C = C3 as in Lathe), "
                     "start as a song position in bars (1-based; 3.5 is halfway through bar 3) and in beats from the song start, "
                     "length in beats, and velocity 1-127. Optionally one clip, or only notes between two bars. "
                     "Returns at most 2000 notes.",
                     schema ({ { "track", prop ("string", "MIDI track name or id") },
                               { "clip", prop ("integer", "Only this clip (id from describe_track)") },
                               { "from_bar", prop ("number", "Only notes starting at or after this bar (1-based)") },
                               { "to_bar", prop ("number", "Only notes starting before this bar (1-based)") } },
                             { "track" }),
                     true));
    {
        auto note = schema ({
            { "pitch", object ({ { "type", var (Array<var> { "integer", "string" }) },
                                 { "description", "MIDI number (60 = C3, middle C) or a name like \"C1\", \"F#2\", \"Bb3\"" } }) },
            { "start_bar", prop ("number", "Song position in bars, 1-based, as get_notes gives it: 1.25 is the second beat of bar 1 in 4/4") },
            { "length_beats", prop ("number", "Length in beats (default 0.25, a 16th note)") },
            { "velocity", prop ("integer", "1-127 (default 100)") } },
            { "pitch", "start_bar" });

        tools.add (tool ("write_notes", "Write MIDI notes",
                         "Writes notes into a MIDI track, as one undo step in Lathe. The notes go in a clip covering from_bar to "
                         "to_bar: with mode \"replace\" (the default) whatever was in those bars on that track goes first; with "
                         "\"add\" they join the notes already there. Every note must start inside the bars. repeat plays the "
                         "notes again that many times back to back (a one-bar beat with repeat 4 fills four bars). "
                         "Lathe's Drums play the usual drum notes: 36 kick, 37 rim, 38 snare, 39 clap, 42 closed hat, "
                         "46 open hat, 41/45/48 low/mid/high tom, 49 crash, 51 ride, 56 cowbell.",
                         schema ({ { "track", prop ("string", "MIDI track name or id") },
                                   { "from_bar", prop ("number", "Where the clip starts (1-based bar)") },
                                   { "to_bar", prop ("number", "Where it ends: from_bar 1, to_bar 2 is one bar") },
                                   { "notes", object ({ { "type", "array" }, { "items", note },
                                                        { "description", "The notes (at most 4000 with the repeats)" } }) },
                                   { "repeat", prop ("integer", "Play the notes this many times in a row, each copy (to_bar - from_bar) bars later, "
                                                                "and make the clip that much longer (default 1)") },
                                   { "mode", prop ("string", "\"replace\" (default) or \"add\"") } },
                                 { "track", "from_bar", "to_bar", "notes" }),
                         false, true));
    }
    tools.add (tool ("play", "Play",
                     "Starts playback, from the given bar or from Lathe's insert marker. With to_bar, Lathe stops by "
                     "itself exactly when the playhead gets there. With loop_times, it plays the loop that many times "
                     "and stops at the loop end (the loop must be on: see set_loop). When it stops, the playhead goes "
                     "back to the insert marker, as with Stop. Without from_bar or to_bar it does nothing if Lathe is "
                     "already playing; from_bar jumps there, to_bar alone sets where the current playback stops.",
                     schema ({ { "from_bar", prop ("number", "Bar to start from (1-based)") },
                               { "to_bar", prop ("number", "Stop when the playhead reaches this bar (1-based; 5 = the start of bar 5)") },
                               { "loop_times", prop ("integer", "Play the loop this many times, then stop at its end. Starts at the loop start unless from_bar is inside the loop") } }),
                     false));
    tools.add (tool ("set_loop", "Set the loop",
                     "Sets the loop region and turns looping on, or turns it off. The loop is saved with the "
                     "project, like moving the loop brackets in Lathe. Bars are 1-based: from_bar 1 to_bar 5 loops "
                     "the first four bars.",
                     schema ({ { "from_bar", prop ("number", "Loop start (1-based)") },
                               { "to_bar", prop ("number", "Loop end: the bar it jumps back from") },
                               { "on", prop ("boolean", "false turns the loop off (the region is kept). Default true") } }),
                     false));
    tools.add (tool ("list_plugins", "List installed plug-ins",
                     "The plug-ins Lathe can load on this computer: Lathe's own built-in devices first (instruments such as "
                     "Multi Synth and Drums, the Random Notes generator, and effects such as Reverb and Mastering), then the installed ones, with name, maker, kind (instrument, note "
                     "generator or effect) and format. Use the names with edit_session's add_plugin and add_track's plugins.",
                     schema ({ { "kind", prop ("string", "Only this kind: \"instrument\", \"note generator\" or \"effect\"") },
                               { "search", prop ("string", "Only plug-ins whose name or maker contains this") } }),
                     true));

    {
        auto change = schema ({
            { "action", object ({ { "type", "string" },
                                  { "enum", Array<var> { "add_track", "remove_track", "rename_track", "move_track",
                                                         "add_plugin", "remove_plugin", "move_plugin", "set_plugin_on",
                                                         "set_input", "set_output", "set_send", "set_mixer", "set_track",
                                                         "set_tempo", "set_time_signature" } },
                                  { "description", "What to change" } }) },
            { "track", prop ("string", "The track it applies to: name or id. Tracks added earlier in the same call can be named") },
            { "kind", prop ("string", "add_track: \"audio\", \"midi\" or \"return\"") },
            { "name", prop ("string", "add_track, rename_track: the track's name (must differ from every other track's)") },
            { "plugins", object ({ { "type", "array" }, { "items", object ({ { "type", "string" } }) },
                                   { "description", "add_track: plug-ins to put on it, in order (names from list_plugins)" } }) },
            { "plugin", prop ("string", "add_plugin: the plug-in's name. remove_plugin, move_plugin, set_plugin_on: its name or 1-based position on the track") },
            { "position", prop ("integer", "add_track, move_track: place in the track list (1 = top). add_plugin, move_plugin: place in the chain (1 = first)") },
            { "on", prop ("boolean", "set_plugin_on: false bypasses it, true switches it back on. add_plugin: false adds it bypassed") },
            { "from_track", prop ("string", "set_input: take this track's notes (MIDI tracks, from a MIDI track) or sound (audio tracks)") },
            { "interface_input", prop ("integer", "set_input (audio tracks): the interface input to record, 1-based") },
            { "midi_input", prop ("string", "set_input (MIDI tracks): \"all\", \"none\" or a MIDI device's name") },
            { "none", prop ("boolean", "set_input: true for no input") },
            { "stereo", prop ("boolean", "with interface_input (default false) or interface_output (default true): use a pair of channels") },
            { "to", prop ("string", "set_output: \"master\" or a return's name") },
            { "interface_output", prop ("integer", "set_output: play out of this interface output instead, 1-based") },
            { "return", prop ("string", "set_send: the return to send to") },
            { "level_db", prop ("number", "set_send: send level in dB (0 = unity, -12 is typical; \"off\" for none)") },
            { "pre_fader", prop ("boolean", "set_send: send before the track's fader") },
            { "volume_db", prop ("number", "set_mixer: fader in dB (-70 to +6)") },
            { "pan", prop ("number", "set_mixer: -1 (left) to 1 (right)") },
            { "mute", prop ("boolean", "set_mixer") },
            { "solo", prop ("boolean", "set_mixer") },
            { "monitor", prop ("string", "set_track (also set_input): \"auto\", \"in\" or \"off\"") },
            { "arm", prop ("boolean", "set_track: arm for recording") },
            { "midi_channel", prop ("integer", "set_track (MIDI tracks): 1-16") },
            { "bpm", prop ("number", "set_tempo") },
            { "numerator", prop ("integer", "set_time_signature") },
            { "denominator", prop ("integer", "set_time_signature: 2, 4, 8 or 16") } },
            { "action" });

        tools.add (tool ("edit_session", "Change the session",
                         "Builds or changes the project: tracks, returns, plug-ins, routing, sends, mixer, tempo. Give every "
                         "change for one request in a single call, in order: they all land as ONE undo step in Lathe, or, if one "
                         "can't be made, none of them do (the reply says which and why). Plug-in placement is automatic unless "
                         "position is given: a note generator goes before the instrument it drives, an instrument before the "
                         "effects. A MIDI track plays one instrument. A track whose input is another track gets monitoring In so "
                         "it plays without being armed.\n"
                         "Routing a note generator into an instrument, two ways: on one track, "
                         "{\"action\":\"add_track\",\"kind\":\"midi\",\"name\":\"Arp\",\"plugins\":[\"<generator>\",\"<instrument>\"]} "
                         "(simplest), or on two, so the generator's notes can be recorded or drive several instruments: "
                         "a MIDI track with the generator, a MIDI track with the instrument, then "
                         "{\"action\":\"set_input\",\"track\":\"<instrument track>\",\"from_track\":\"<generator track>\"}. "
                         "A reverb return: {\"action\":\"add_track\",\"kind\":\"return\",\"name\":\"Verb\",\"plugins\":[\"Reverb\"]}, "
                         "then set_send from each track. Lathe's own effects start 100% wet on a return.\n"
                         "Volume, pan, mute, solo, send levels, monitoring and arm change live and aren't part of the undo step.",
                         schema ({ { "changes", object ({ { "type", "array" }, { "items", change },
                                                          { "description", "The changes, applied in order" } }) } },
                                 { "changes" }),
                         false, true));
    }

    {
        auto unsaved = prop ("string", "If the open project has unsaved changes: \"save\" them first (it must have been saved "
                                       "before), or \"discard\" them. Without it, unsaved changes stop the call. Ask the person "
                                       "before discarding anything");
        tools.add (tool ("new_project", "New project",
                         "Starts a new project, like File > New: empty (an audio track and a MIDI track) or from one of the "
                         "person's templates by name. It replaces the open project, so unsaved changes stop it unless "
                         "\"unsaved\" says what to do. The new project is Untitled until save_project gives it a name.",
                         schema ({ { "template", prop ("string", "A template's name (File > New from Template lists them)") },
                                   { "unsaved", unsaved } }),
                         false, true));
        tools.add (tool ("open_project", "Open a project",
                         "Opens a project, like File > Open: by name from the projects folder (exact, or a unique part of it), "
                         "or by path. Without either it lists the projects, newest first. It replaces the open project, so "
                         "unsaved changes stop it unless \"unsaved\" says what to do.",
                         schema ({ { "name", prop ("string", "A project in the projects folder") },
                                   { "path", prop ("string", "Or the .lathe file (or its folder) anywhere") },
                                   { "unsaved", unsaved } }),
                         false, true));
    }
    tools.add (tool ("save_project", "Save the project",
                     "Saves the open project. Without a name it saves where it is (Cmd/Ctrl+S); a project that's never been "
                     "saved needs one. With a name it's saved as a new project, Name/Name.lathe in the projects folder, with "
                     "its own Audio folder (audio it uses is copied in), like File > Save As. It never saves over another project.",
                     schema ({ { "name", prop ("string", "Save as a new project with this name") } }),
                     false));
    tools.add (tool ("export_audio", "Export audio",
                     "Renders the song to WAV files, like File > Export: the mix, and stems (a file per track and return) if "
                     "asked. From the start to the end of the last clip unless bars are given. Lathe stops playing first. "
                     "Files go in the project's Exports folder; the reply lists them with each one's length and, for the mix, "
                     "its peak, true peak and integrated loudness (LUFS), so you can check the level.",
                     schema ({ { "from_bar", prop ("number", "First bar (1-based)") },
                               { "to_bar", prop ("number", "Where it ends (the bar it stops at)") },
                               { "mix", prop ("boolean", "The stereo mix (default true)") },
                               { "stems", prop ("boolean", "A file per track and return (default false)") },
                               { "bit_depth", prop ("integer", "16, 24 (default) or 32 (float)") },
                               { "tail_seconds", prop ("number", "Time after the end for reverb and delay tails, 0 to 30 (default 2)") } }),
                     false));
    tools.add (tool ("get_plugin_params", "Get a plug-in's settings",
                     "Every setting a plug-in on a track publishes (Lathe's own devices and VST3s): number, name, id, the "
                     "current value as the plug-in shows it, and either its choices (a menu or a switch) or its range "
                     "(min, max and unit). Use search to find one in a plug-in with hundreds.",
                     schema ({ { "track", prop ("string", "Track, return or \"Master\": name or id") },
                               { "plugin", prop ("string", "The plug-in on it: name or 1-based position in the chain") },
                               { "search", prop ("string", "Only settings whose name or id contains this") } },
                             { "track", "plugin" }),
                     true));
    {
        auto param = schema ({
            { "param", object ({ { "type", var (Array<var> { "string", "integer" }) },
                                 { "description", "The setting: its name, id or number from get_plugin_params" } }) },
            { "value", object ({ { "type", var (Array<var> { "number", "string", "boolean" }) },
                                 { "description", "A number in the setting's own units (Hz, dB, %, seconds; min to max), "
                                                  "one of its choices by name (\"Simple 808\"), or true/false for a switch" } }) },
            { "normalized", prop ("number", "Instead of value: 0 to 1 across the setting's whole range") } },
            { "param" });
        tools.add (tool ("set_plugin_params", "Change a plug-in's settings",
                         "Turns a plug-in's knobs, flips its switches and picks from its menus: Lathe's own devices (a Drums kit, "
                         "a synth's wave) and VST3s. Give every change for one plug-in in one call; if one can't be made, none are. "
                         "Each change happens at once, as if turned by hand, and like a knob in Lathe it isn't an undo step: the "
                         "reply gives each old value, so it can be put back. Check names and ranges with get_plugin_params first.",
                         schema ({ { "track", prop ("string", "Track, return or \"Master\": name or id") },
                                   { "plugin", prop ("string", "The plug-in on it: name or 1-based position in the chain") },
                                   { "params", object ({ { "type", "array" }, { "items", param },
                                                         { "description", "The settings to change, in order" } }) } },
                                 { "track", "plugin", "params" }),
                         false));
    }

    tools.add (tool ("measure_performance", "Measure CPU use",
                     "Plays part of the project for real and times every audio block, the way a CPU meter does: mean, "
                     "99th-percentile and peak load (share of each block's deadline; over 100% is a dropout), blocks that "
                     "missed the deadline, dropouts the interface reported, each track's own cost per block (heaviest "
                     "first, with its plug-ins), and the conditions (computer, CPU and cores, interface, buffer size). Bars default to the loop if it's on, else the first four. Takes about "
                     "`seconds` plus a short warm-up; Lathe is stopped first and the playhead goes back afterwards.",
                     schema ({ { "from_bar", prop ("number", "First bar to play (1-based)") },
                               { "to_bar", prop ("number", "Bar to stop at") },
                               { "seconds", prop ("number", "How long to measure, 2 to 20 (default 8). Short bars are played again until it's reached") } }),
                     false));
    tools.add (tool ("compare_processing_cores", "Compare 1 core with all cores",
                     "Measures the same bars with Lathe rendering on 1 core and on all cores (Auto), alternating "
                     "(1, all, 1, all) so a warming or throttling computer doesn't favour either, and returns both side by "
                     "side with the speed-up, every run, each track's cost, and the conditions. The Processing cores "
                     "setting is put back afterwards. Takes about 2 x rounds x seconds, plus warm-ups.",
                     schema ({ { "from_bar", prop ("number", "First bar to play (1-based)") },
                               { "to_bar", prop ("number", "Bar to stop at") },
                               { "seconds", prop ("number", "Seconds per run, 2 to 20 (default 6)") },
                               { "rounds", prop ("integer", "Rounds of 1 core + all cores, 1 to 3 (default 2)") } }),
                     false));

    tools.add (tool ("stop", "Stop",
                     "Stops playback, like pressing Stop. If Lathe is recording, this finishes the recording and "
                     "keeps the take, exactly as the Stop button does.",
                     schema ({}), false));
    return tools;
}

StringArray protocolVersions()
{
    return { "2025-06-18", "2025-03-26", "2024-11-05" };
}

String instructions()
{
    return "Lathe is a small DAW (digital audio workstation). These tools read the project that is open in Lathe, "
           "control its transport, and change it (edit_session: each call is one undo step the person can take back). "
           "They can also start, open and save projects and export audio; new_project and open_project replace the open "
           "project, so they won't throw away unsaved changes unless told to. Positions are in bars (1-based) and beats. A MIDI track's notes go through its "
           "plug-ins in order: a generator plug-in (one that makes its own notes, like an arpeggiator or a sequencer) "
           "comes before the instrument it drives. A MIDI track can take its notes from another MIDI track, and an "
           "audio track can take another track's sound; tracks that take their input from somewhere only hear it "
           "while armed or with monitoring set to In.";
}

var rpcResult (const var& id, const var& result)
{
    return object ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } });
}

var rpcError (const var& id, int code, const String& message)
{
    return object ({ { "jsonrpc", "2.0" }, { "id", id }, { "error", object ({ { "code", code }, { "message", message } }) } });
}

var toolText (const String& text, bool isError)
{
    Array<var> content;
    content.add (object ({ { "type", "text" }, { "text", text } }));
    return object ({ { "content", content }, { "isError", isError } });
}
} // namespace lathe::assistant
