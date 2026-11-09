#pragma once

#include "AssistantShared.h"

namespace lathe::assistant
{
//==============================================================================
/** The MCP server (Streamable HTTP, JSON responses) on 127.0.0.1. One request per
    connection, handled one at a time on this thread.

    Every request must:
      - be a POST to /mcp,
      - say Host: 127.0.0.1:<port> or localhost:<port> (stops DNS-rebinding tricks),
      - have no Origin header, or a localhost one (a web page can't drive Lathe),
      - carry Authorization: Bearer <key>.
*/
class Server : private Thread
{
public:
    struct Handler
    {
        /** Runs a tool. Called on the server thread: the handler gets to the message
            thread itself. Returns an MCP tool result (see toolText). */
        std::function<var (const String& name, const var& arguments)> callTool;
        /** An AI app said hello (MCP initialize). */
        std::function<void (const String& clientName)> onConnect;
        String version;   // Lathe's, for serverInfo
    };

    Server (Handler, File endpoint = endpointFile());
    ~Server() override;

    /** Opens the port, writes the endpoint file and starts listening. */
    Result start (const String& token);
    void stop();
    bool isRunning() const   { return isThreadRunning(); }
    int getPort() const      { return port; }

    struct Activity
    {
        String client, lastTool;
        Time lastRequest;
        int requests = 0, refused = 0;
    };
    Activity getActivity() const;

    /** One JSON-RPC message (or batch). Returns the response, or a void var for
        notifications. Public for the tests. */
    var handleRpc (const var& message);
    /** One HTTP request, after it was read. Public for the tests. */
    String handleHttp (const HttpMessage&);

    /** For the thread that calls the tool: true once stop() was called. */
    bool stopping() const   { return threadShouldExit(); }

private:
    void run() override;
    var handleOne (const var& message);

    Handler handler;
    File endpoint;
    StreamingSocket listener;
    int port = 0;
    String token;

    mutable CriticalSection activityLock;
    Activity activity;
};
} // namespace lathe::assistant
