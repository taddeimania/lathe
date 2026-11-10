// LatheMCP: what an AI app (Claude Desktop, Claude Code, ...) starts to talk to Lathe.
//
// It speaks MCP over stdin/stdout (one JSON-RPC message per line) and passes each
// message on to the Lathe that's running, using the port and key Lathe wrote to
// assistant.json. It reads that file for every message, so it keeps working when you
// restart Lathe. While Lathe is closed (or its AI assistant connection is off) it
// still answers, so the AI app keeps the tools and can tell you what to do.

#include "AssistantShared.h"

#include <iostream>
#include <string>

using namespace juce;
using namespace lathe::assistant;

namespace
{
void send (const var& message)
{
    std::cout << JSON::toString (message, true).toStdString() << "\n";
    std::cout.flush();
}

String notReachable()
{
    return "Lathe isn't open, or its AI assistant connection is off. Open Lathe and turn it on in "
           "Options > AI Assistant, then try again.";
}

/** Answers for when Lathe can't be reached. */
var answerLocally (const var& m)
{
    const auto id = m.getProperty ("id", {});
    const auto method = m.getProperty ("method", {}).toString();
    const auto params = m.getProperty ("params", {});

    if (method == "initialize")
    {
        const auto versions = protocolVersions();
        const auto asked = params.getProperty ("protocolVersion", {}).toString();
        auto* tools = new DynamicObject();
        tools->setProperty ("listChanged", false);
        auto* caps = new DynamicObject();
        caps->setProperty ("tools", var (tools));
        auto* info = new DynamicObject();
        info->setProperty ("name", "lathe");
        info->setProperty ("title", "Lathe");
        info->setProperty ("version", LATHE_VERSION_STRING);
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
        return rpcResult (id, toolText (notReachable(), true));
    return rpcError (id, -32601, "Method not found: " + method);
}

void handle (const String& line)
{
    var message;
    if (JSON::parse (line, message).failed() || ! (message.isObject() || message.isArray()))
    {
        send (rpcError ({}, -32700, "Parse error"));
        return;
    }

    String body;
    int status = 0;
    // A measurement plays part of the song for real: give it the time it needs.
    const bool longCall = message["method"].toString() == "tools/call" && isLongTool (message["params"]["name"].toString());
    if (auto e = readEndpoint())
        status = postToLathe (*e, JSON::toString (message, true), body, longCall ? (kLongToolSeconds + 15) * 1000 : 20000);

    if (status == 200)
    {
        var reply;
        if (JSON::parse (body, reply).wasOk() && ! reply.isVoid())
            send (reply);
        return;
    }
    if (status == 202)
        return;   // a notification

    if (status != 0)
        std::cerr << "LatheMCP: Lathe answered HTTP " << status << ": " << body.toStdString() << std::endl;

    // Lathe can't be reached (or refused): answer what we can ourselves.
    auto answerOne = [] (const var& m) -> var
    {
        return m.hasProperty ("id") ? answerLocally (m) : var();
    };
    if (auto* batch = message.getArray())
    {
        Array<var> replies;
        for (auto& m : *batch)
            if (auto r = answerOne (m); ! r.isVoid())
                replies.add (r);
        if (! replies.isEmpty())
            send (replies);
    }
    else if (auto r = answerOne (message); ! r.isVoid())
    {
        send (r);
    }
}
} // namespace

int main (int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const String arg (argv[i]);
        if (arg == "--version")
        {
            std::cout << "LatheMCP " << LATHE_VERSION_STRING << std::endl;
            return 0;
        }
        if (arg == "--help" || arg == "-h")
        {
            std::cout << "LatheMCP: lets an AI app (an MCP client) see and control Lathe.\n"
                         "Add it to the app as a local (stdio) MCP server, then turn the connection on in\n"
                         "Lathe: Options > AI Assistant. It reads " << endpointFile().getFullPathName().toStdString() << "\n";
            return 0;
        }
    }

    std::ios::sync_with_stdio (false);
    std::string line;
    while (std::getline (std::cin, line))
    {
        const auto text = String::fromUTF8 (line.c_str()).trim();
        if (text.isNotEmpty())
            handle (text);
    }
    return 0;
}
