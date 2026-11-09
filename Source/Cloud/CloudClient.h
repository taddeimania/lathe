#pragma once

#include "../Common.h"
#include <juce_cryptography/juce_cryptography.h>

namespace lathe::cloud
{
//==============================================================================
// Talks to Lathe Cloud: Cognito sign-in, the JSON API, and S3 uploads and
// downloads through presigned links. Every call here blocks, so run them on a
// background thread (Lathe's CloudWorker does), never on the message thread.
//==============================================================================

struct Config
{
    String apiUrl, region, clientId;
    String cognitoUrl;   // derived from region unless overridden (tests)

    bool isValid() const   { return apiUrl.isNotEmpty() && region.isNotEmpty() && clientId.isNotEmpty(); }

    /** Lathe Cloud. (Dev and test builds only: LATHE_API_URL / LATHE_REGION /
        LATHE_CLIENT_ID / LATHE_COGNITO_URL point it at a local backend instead.) */
    static Config load();
};

struct HttpResult
{
    int status = 0;          // 0 = no connection
    MemoryBlock body;
    StringPairArray headers;

    bool ok() const noexcept    { return status >= 200 && status < 300; }
    var json() const            { return JSON::parse (body.toString()); }
    String errorMessage() const;
};

/** One HTTP request. url is used exactly as given (presigned links must not be re-encoded). */
HttpResult httpRequest (const String& method, const String& url, const MemoryBlock* body,
                        const String& extraHeaders, int timeoutMs = 60000);

/** Streams a GET into a file. progress gets 0..1 and returns false to cancel. */
Result httpDownload (const String& url, const File& destination, const std::function<bool (double)>& progress);

//==============================================================================
struct Session
{
    String email, idToken, refreshToken;
    int64 expiresAtMs = 0;

    bool isSignedIn() const noexcept   { return refreshToken.isNotEmpty(); }
};

/** Thrown-free error type for the API: status 409 means "someone pushed first". */
struct ApiError
{
    int status = 0;
    String message;
    var body;
};

class Client
{
public:
    explicit Client (Config c) : config (std::move (c)) {}

    const Config& getConfig() const noexcept   { return config; }

    // --- sign-in -------------------------------------------------------------------
    /** Password sign-in. If Cognito wants a new password (first sign-in with a temporary
        one), newPassword is called; return an empty string to give up. */
    Result signIn (const String& email, const String& password, const std::function<String()>& newPassword);
    /** Restores a saved session (refresh token). */
    void setSession (Session s);
    Session getSession() const;
    void signOut();
    bool isSignedIn() const;
    /** Refreshes the ID token when it's about to expire. */
    Result ensureFreshToken();

    /** Called (on the calling thread) whenever tokens change, so they can be saved. */
    std::function<void (const Session&)> onSessionChanged;

    // --- API ------------------------------------------------------------------------------
    /** JSON call to the API. On failure returns an undefined var and fills error
        (error.status is never 0 on failure: a dropped connection or timeout is 599). */
    var call (const String& method, const String& path, const var& body, ApiError& error, int timeoutMs = 30000);

    // --- blobs ------------------------------------------------------------------------------
    struct UploadItem
    {
        String hash;
        File file;                 // uploaded from here...
        MemoryBlock data;          // ...or from memory when file is empty
        int64 size() const         { return file != File() ? file.getSize() : (int64) data.getSize(); }
    };

    /** Uploads whichever items the server doesn't have yet. progress: 0..1. */
    Result upload (const String& projectId, const std::vector<UploadItem>& items,
                   const std::function<bool (double)>& progress, int& uploadedCount);
    /** How many files (or pieces of a big file) move at once, up and down. Default 4. */
    void setParallelTransfers (int n)    { parallelTransfers = jlimit (1, 16, n); }
    int getParallelTransfers() const     { return parallelTransfers.load(); }

    /** Downloads blobs to files, checking each file's SHA-256. */
    Result download (const String& projectId, const std::vector<std::pair<String, File>>& hashAndDestination,
                     const std::function<bool (double)>& progress);

private:
    Result cognito (const String& target, const var& payload, var& response);
    Result applyAuthResult (const var& authResult, const String& email);

    Config config;
    mutable CriticalSection lock;
    Session session;
    std::atomic<int> parallelTransfers { 4 };
};

namespace testing
{
/** Makes the next `count` transfer requests fail as if the connection dropped. */
void dropNextTransfers (int count);
}

/** SHA-256 of a file, as lowercase hex. Streams in pieces; progress (0..1) can cancel,
    in which case it returns an empty string. */
String sha256OfFile (const File&, const std::function<bool (double)>& progress = {});
String sha256OfData (const MemoryBlock&);
} // namespace lathe::cloud
