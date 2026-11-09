#include "AssistantShared.h"

#include <random>

#if ! JUCE_WINDOWS
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
    const auto* p = text.toRawUTF8();
    const int total = (int) text.getNumBytesAsUTF8();
    int done = 0;
    while (done < total)
    {
        const int n = s.write (p + done, total - done);
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

var tool (const char* name, const char* title, const char* description, var inputSchema, bool readOnly)
{
    return object ({ { "name", name },
                     { "title", title },
                     { "description", description },
                     { "inputSchema", inputSchema },
                     { "annotations", object ({ { "readOnlyHint", readOnly },
                                                { "destructiveHint", false },
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
    tools.add (tool ("play", "Play",
                     "Starts playback, from the given bar or from Lathe's insert marker. Does nothing if Lathe is "
                     "already playing (unless from_bar is given, which jumps there).",
                     schema ({ { "from_bar", prop ("number", "Bar to start from (1-based)") } }), false));
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
    return "Lathe is a small DAW (digital audio workstation). These tools read the project that is open in Lathe "
           "and control its transport. Positions are in bars (1-based) and beats. A MIDI track's notes go through its "
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
