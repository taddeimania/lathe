#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace lathe::assistant
{
using namespace juce;

//==============================================================================
// Shared by Lathe (the server), LatheMCP (the bridge an AI app starts) and the tests.
//
// Lathe's AI assistant connection is an MCP server inside Lathe. It is off until you
// turn it on (Options > AI Assistant), listens only on 127.0.0.1, and every request
// must carry a secret key. Lathe writes the port and key to assistant.json in its
// settings folder (readable only by you); LatheMCP reads them from there.
//==============================================================================

/** Lathe's settings folder (where Lathe.settings lives). */
File settingsFolder();
/** Where a running Lathe with the connection on writes its port and key. */
File endpointFile();

struct Endpoint
{
    int port = 0;
    String token;
};

std::optional<Endpoint> readEndpoint (const File& = endpointFile());
bool writeEndpoint (const Endpoint&, const File& = endpointFile());

/** A new random key (256 bits, hex). */
String makeToken();
/** Compares without leaking where they differ through timing. */
bool tokensMatch (const String& a, const String& b);

constexpr int kFirstPort = 47960;
constexpr int kPortsToTry = 10;
constexpr int kMaxBody = 1024 * 1024;
/** The most a long tool (measure_performance, compare_processing_cores) may take. */
constexpr int kLongToolSeconds = 120;
inline bool isLongTool (const String& name)   { return name == "measure_performance" || name == "compare_processing_cores"; }

//==============================================================================
// Just enough HTTP/1.1 for one request and one response per connection.
struct HttpMessage
{
    String method, path;               // requests
    int status = 0;                    // responses
    StringPairArray headers;           // names in lower case
    String body;

    String header (const String& name) const   { return headers.getValue (name.toLowerCase(), {}); }
};

/** Reads a request (isRequest) or a response from the socket. False on a timeout, a
    malformed message, or a body larger than kMaxBody. */
bool readHttp (StreamingSocket&, HttpMessage&, bool isRequest, int timeoutMs);
bool writeAll (StreamingSocket&, const String& text);
String httpResponse (int status, const String& body, const String& contentType = "application/json");

/** POSTs one JSON-RPC message to a running Lathe. Returns the HTTP status (0 when Lathe
    can't be reached) and the body. */
int postToLathe (const Endpoint&, const String& json, String& responseBody, int timeoutMs);

//==============================================================================
/** The tools Lathe offers (MCP tool definitions). LatheMCP can list them while Lathe
    is closed, so an AI app doesn't lose them when you quit and reopen Lathe. */
var toolDefinitions();
/** The MCP protocol versions Lathe understands, newest first. */
StringArray protocolVersions();
String instructions();

/** A JSON-RPC result or error for the request id. */
var rpcResult (const var& id, const var& result);
var rpcError (const var& id, int code, const String& message);
/** A tool result: one text item. */
var toolText (const String& text, bool isError = false);
} // namespace lathe::assistant
