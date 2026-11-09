#include "AssistantServer.h"

namespace lathe::assistant
{
Server::Server (Handler h, File e)
    : Thread ("Lathe AI assistant"), handler (std::move (h)), endpoint (std::move (e))
{
}

Server::~Server()
{
    stop();
}

Result Server::start (const String& newToken)
{
    stop();
    token = newToken;

    for (int p = kFirstPort; p < kFirstPort + kPortsToTry; ++p)
    {
        if (listener.createListener (p, "127.0.0.1"))
        {
            port = p;
            break;
        }
    }
    if (port == 0)
        return Result::fail ("Couldn't open a port on this computer (tried " + String (kFirstPort) + "-"
                             + String (kFirstPort + kPortsToTry - 1) + ").");

    if (! writeEndpoint ({ port, token }, endpoint))
    {
        listener.close();
        port = 0;
        return Result::fail ("Couldn't write " + endpoint.getFullPathName());
    }

    startThread();
    return Result::ok();
}

void Server::stop()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();
        stopThread (5000);
    }
    listener.close();
    if (port != 0)
    {
        // Only remove the file if it's still ours (another Lathe may have taken over).
        if (auto e = readEndpoint (endpoint); e && e->port == port && tokensMatch (e->token, token))
            endpoint.deleteFile();
        port = 0;
    }
}

Server::Activity Server::getActivity() const
{
    const ScopedLock sl (activityLock);
    return activity;
}

void Server::run()
{
    while (! threadShouldExit())
    {
        if (listener.waitUntilReady (true, 200) != 1)
            continue;

        std::unique_ptr<StreamingSocket> client (listener.waitForNextConnection());
        if (client == nullptr || threadShouldExit())
            continue;

        HttpMessage request;
        String response;
        if (! readHttp (*client, request, true, 5000))
            response = httpResponse (400, {}, {});
        else
            response = handleHttp (request);
        writeAll (*client, response);
        client->close();
    }
}

String Server::handleHttp (const HttpMessage& r)
{
    auto refuse = [this] (int status, const String& why)
    {
        {
            const ScopedLock sl (activityLock);
            ++activity.refused;
        }
        return httpResponse (status, why, "text/plain; charset=utf-8");
    };

    // DNS rebinding: a web page that resolves its own name to 127.0.0.1 still sends its name.
    const auto host = r.header ("host");
    if (host != "127.0.0.1:" + String (port) && host != "localhost:" + String (port))
        return refuse (403, "Wrong host.");

    // Browsers say where a request comes from. AI apps send no Origin; refuse every web page.
    const auto origin = r.header ("origin");
    auto isLocal = [] (const String& o)
    {
        for (auto* base : { "http://127.0.0.1", "http://localhost" })
            if (o == base || o.startsWith (String (base) + ":"))
                return true;
        return false;
    };
    if (origin.isNotEmpty() && ! isLocal (origin))   // includes "null" (sandboxed pages, local files)
        return refuse (403, "Requests from web pages aren't allowed.");

    const auto auth = r.header ("authorization");
    if (! auth.startsWithIgnoreCase ("Bearer ") || ! tokensMatch (auth.substring (7).trim(), token))
        return refuse (401, "Missing or wrong key. Copy the setup again from Lathe (Options > AI Assistant).");

    if (r.path.upToFirstOccurrenceOf ("?", false, false) != "/mcp")
        return httpResponse (404, {}, {});
    if (r.method != "POST")
        return httpResponse (405, {}, {});   // no server-to-client stream

    var message;
    if (JSON::parse (r.body, message).failed() || ! (message.isObject() || message.isArray()))
        return httpResponse (400, JSON::toString (rpcError ({}, -32700, "Parse error"), true));

    {
        const ScopedLock sl (activityLock);
        ++activity.requests;
        activity.lastRequest = Time::getCurrentTime();
    }

    const auto reply = handleRpc (message);
    if (reply.isVoid())
        return httpResponse (202, {}, {});
    return httpResponse (200, JSON::toString (reply, true));
}

var Server::handleRpc (const var& message)
{
    if (auto* batch = message.getArray())
    {
        Array<var> replies;
        for (auto& m : *batch)
            if (auto r = handleOne (m); ! r.isVoid())
                replies.add (r);
        return replies.isEmpty() ? var() : var (replies);
    }
    return handleOne (message);
}

var Server::handleOne (const var& m)
{
    const auto method = m.getProperty ("method", {}).toString();
    const bool isNotification = ! m.hasProperty ("id");
    const auto id = m.getProperty ("id", {});
    const auto params = m.getProperty ("params", {});

    if (method.isEmpty())
        return isNotification ? var() : rpcError (id, -32600, "Invalid request");

    if (isNotification)
        return {};   // notifications/initialized, cancelled, ...: nothing to say

    if (method == "initialize")
    {
        const auto asked = params.getProperty ("protocolVersion", {}).toString();
        const auto versions = protocolVersions();
        const auto clientName = params.getProperty ("clientInfo", {}).getProperty ("name", "an AI app").toString();
        {
            const ScopedLock sl (activityLock);
            activity.client = clientName;
        }
        if (handler.onConnect)
            handler.onConnect (clientName);

        auto* caps = new DynamicObject();
        auto* tools = new DynamicObject();
        tools->setProperty ("listChanged", false);
        caps->setProperty ("tools", var (tools));
        auto* info = new DynamicObject();
        info->setProperty ("name", "lathe");
        info->setProperty ("title", "Lathe");
        info->setProperty ("version", handler.version);
        auto* result = new DynamicObject();
        result->setProperty ("protocolVersion", versions.contains (asked) ? asked : versions[0]);
        result->setProperty ("capabilities", var (caps));
        result->setProperty ("serverInfo", var (info));
        result->setProperty ("instructions", instructions());
        return rpcResult (id, var (result));
    }

    if (method == "ping")
        return rpcResult (id, var (new DynamicObject()));

    if (method == "tools/list")
    {
        auto* result = new DynamicObject();
        result->setProperty ("tools", toolDefinitions());
        return rpcResult (id, var (result));
    }

    if (method == "tools/call")
    {
        const auto name = params.getProperty ("name", {}).toString();
        bool known = false;
        const auto definitions = toolDefinitions();
        if (auto* defs = definitions.getArray())
            for (auto& d : *defs)
                known = known || d.getProperty ("name", {}).toString() == name;
        if (! known)
            return rpcError (id, -32602, "Unknown tool: " + name);

        {
            const ScopedLock sl (activityLock);
            activity.lastTool = name;
        }
        auto args = params.getProperty ("arguments", {});
        if (! args.isObject())
            args = var (new DynamicObject());
        return rpcResult (id, handler.callTool ? handler.callTool (name, args) : toolText ("Lathe isn't ready.", true));
    }

    return rpcError (id, -32601, "Method not found: " + method);
}
} // namespace lathe::assistant
