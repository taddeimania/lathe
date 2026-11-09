#include "CloudClient.h"
#include <thread>

#ifndef LATHE_CLOUD_API_URL
 #define LATHE_CLOUD_API_URL ""
#endif
#ifndef LATHE_CLOUD_REGION
 #define LATHE_CLOUD_REGION ""
#endif
#ifndef LATHE_CLOUD_CLIENT_ID
 #define LATHE_CLOUD_CLIENT_ID ""
#endif

namespace lathe::cloud
{
namespace
{
constexpr int kPartSizeFallback = 64 * 1024 * 1024;

String trimSlash (String s)
{
    while (s.endsWithChar ('/'))
        s = s.dropLastCharacters (1);
    return s;
}

} // namespace

//==============================================================================
Config Config::fromJson (const String& text)
{
    Config c;
    const auto v = JSON::parse (text);
    c.apiUrl = trimSlash (v["apiUrl"].toString().trim());
    c.region = v["region"].toString().trim();
    c.clientId = v["clientId"].toString().trim();
    return c;
}

String Config::toJson() const
{
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("apiUrl", apiUrl);
    o->setProperty ("region", region);
    o->setProperty ("clientId", clientId);
    return JSON::toString (var (o.get()), true);
}

Config Config::load (PropertiesFile& settings)
{
    Config c;
    c.apiUrl = trimSlash (LATHE_CLOUD_API_URL);
    c.region = LATHE_CLOUD_REGION;
    c.clientId = LATHE_CLOUD_CLIENT_ID;

    const auto saved = settings.getValue ("cloudConfig");
    if (saved.isNotEmpty())
    {
        const auto s = fromJson (saved);
        if (s.isValid())
            c = s;
    }

    if (const auto env = SystemStats::getEnvironmentVariable ("LATHE_API_URL", {}); env.isNotEmpty())
    {
        c.apiUrl = trimSlash (env);
        c.region = SystemStats::getEnvironmentVariable ("LATHE_REGION", c.region.isNotEmpty() ? c.region : "us-east-1");
        c.clientId = SystemStats::getEnvironmentVariable ("LATHE_CLIENT_ID", c.clientId);
    }

    c.cognitoUrl = SystemStats::getEnvironmentVariable ("LATHE_COGNITO_URL",
                                                        "https://cognito-idp." + c.region + ".amazonaws.com/");
    return c;
}

//==============================================================================
String HttpResult::errorMessage() const
{
    if (status == 0)
        return "Couldn't connect. Check your internet connection.";
    const auto v = json();
    if (auto m = v["error"].toString(); m.isNotEmpty()) return m;
    if (auto m = v["message"].toString(); m.isNotEmpty()) return m;
    if (auto m = v["Message"].toString(); m.isNotEmpty()) return m;
    return "The server answered " + String (status) + ".";
}

HttpResult httpRequest (const String& method, const String& url, const MemoryBlock* body,
                        const String& extraHeaders, int timeoutMs)
{
    HttpResult r;
    auto u = URL::createWithoutParsing (url);
    if (body != nullptr)
        u = u.withPOSTData (*body);

    auto options = URL::InputStreamOptions (body != nullptr ? URL::ParameterHandling::inPostData
                                                            : URL::ParameterHandling::inAddress)
                       .withExtraHeaders (extraHeaders)
                       .withConnectionTimeoutMs (timeoutMs)
                       .withStatusCode (&r.status)
                       .withResponseHeaders (&r.headers)
                       .withNumRedirectsToFollow (5)
                       .withHttpRequestCmd (method);

    if (auto stream = u.createInputStream (options))
    {
        MemoryOutputStream out (r.body, false);
        out.writeFromInputStream (*stream, -1);
    }
    else
    {
        r.status = 0;
    }
    return r;
}

Result httpDownload (const String& url, const File& destination, const std::function<bool (double)>& progress)
{
    int status = 0;
    auto options = URL::InputStreamOptions (URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs (30000)
                       .withStatusCode (&status)
                       .withNumRedirectsToFollow (5);
    auto in = URL::createWithoutParsing (url).createInputStream (options);
    if (in == nullptr)
        return Result::fail ("Couldn't connect. Check your internet connection.");
    if (status >= 400)
        return Result::fail ("Download failed (HTTP " + String (status) + ").");

    destination.getParentDirectory().createDirectory();
    const auto temp = destination.getSiblingFile (destination.getFileName() + ".part");
    temp.deleteFile();
    {
        FileOutputStream out (temp);
        if (out.failedToOpen())
            return Result::fail ("Couldn't write " + temp.getFullPathName());

        HeapBlock<char> buffer (65536);
        const auto total = in->getTotalLength();
        int64 received = 0;
        for (;;)
        {
            const auto n = in->read (buffer, 65536);
            if (n <= 0)
                break;
            if (! out.write (buffer, (size_t) n))
                return Result::fail ("Couldn't write the download (is the disk full?).");
            received += n;
            if (progress && total > 0 && ! progress ((double) received / (double) total))
            {
                out.flush();
                temp.deleteFile();
                return Result::fail ("Cancelled.");
            }
        }
        out.flush();
    }
    destination.deleteFile();
    if (! temp.moveFileTo (destination))
        return Result::fail ("Couldn't write " + destination.getFullPathName());
    return Result::ok();
}

namespace
{
/** Incremental SHA-256 (FIPS 180-4), so big files can be hashed in pieces with
    progress and cancellation. Checked against juce::SHA256 in the tests. */
class Sha256
{
public:
    void update (const uint8* data, size_t n)
    {
        total += n;
        while (n > 0)
        {
            const size_t take = jmin (n, (size_t) 64 - used);
            std::memcpy (block + used, data, take);
            used += take;
            data += take;
            n -= take;
            if (used == 64)
            {
                compress (block);
                used = 0;
            }
        }
    }

    String finish()
    {
        const uint64 bits = (uint64) total * 8;
        const uint8 pad = 0x80;
        update (&pad, 1);
        const uint8 zero = 0;
        while (used != 56)
            update (&zero, 1);
        uint8 length[8];
        for (int i = 0; i < 8; ++i)
            length[i] = (uint8) (bits >> (56 - 8 * i));
        update (length, 8);

        static const char* digits = "0123456789abcdef";
        char hex[65];
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                hex[i * 8 + j] = digits[(h[i] >> (28 - 4 * j)) & 0xf];
        hex[64] = 0;
        return String (hex);
    }

private:
    static uint32 rotr (uint32 x, int n) noexcept   { return (x >> n) | (x << (32 - n)); }

    void compress (const uint8* p)
    {
        static constexpr uint32 k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

        uint32 w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32) p[4 * i] << 24) | ((uint32) p[4 * i + 1] << 16) | ((uint32) p[4 * i + 2] << 8) | (uint32) p[4 * i + 3];
        for (int i = 16; i < 64; ++i)
        {
            const uint32 s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32 s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32 S1 = rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25);
            const uint32 ch = (e & f) ^ (~e & g);
            const uint32 t1 = hh + S1 + ch + k[i] + w[i];
            const uint32 S0 = rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22);
            const uint32 maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32 t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    uint32 h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8 block[64] {};
    size_t used = 0;
    uint64 total = 0;
};
} // namespace

String sha256OfFile (const File& f, const std::function<bool (double)>& progress)
{
    FileInputStream in (f);
    if (in.failedToOpen())
        return {};

    Sha256 sha;
    HeapBlock<uint8> buffer (1 << 20);
    const auto size = jmax ((int64) 1, in.getTotalLength());
    int64 done = 0;
    for (;;)
    {
        const auto n = in.read (buffer, 1 << 20);
        if (n <= 0)
            break;
        sha.update (buffer, (size_t) n);
        done += n;
        if (progress && ! progress ((double) done / (double) size))
            return {};
    }
    return sha.finish();
}

String sha256OfData (const MemoryBlock& m)
{
    return SHA256 (m).toHexString().toLowerCase();
}

//==============================================================================
Result Client::cognito (const String& target, const var& payload, var& response)
{
    const auto text = JSON::toString (payload, true);
    MemoryBlock body (text.toRawUTF8(), text.getNumBytesAsUTF8());
    const auto r = httpRequest ("POST", config.cognitoUrl, &body,
                                "Content-Type: application/x-amz-json-1.1\r\n"
                                "X-Amz-Target: AWSCognitoIdentityProviderService." + target, 20000);
    response = r.json();
    if (r.ok())
        return Result::ok();

    if (r.status == 0)
        return Result::fail (r.errorMessage());
    const auto type = response["__type"].toString();
    if (type.contains ("NotAuthorized"))
        return Result::fail (response["message"].toString().isNotEmpty() ? response["message"].toString()
                                                                        : String ("Wrong email or password."));
    if (type.contains ("InvalidPassword"))
        return Result::fail ("That password isn't allowed: " + response["message"].toString());
    return Result::fail (r.errorMessage());
}

Result Client::applyAuthResult (const var& result, const String& email)
{
    if (! result.isObject() || result["IdToken"].toString().isEmpty())
        return Result::fail ("Sign-in didn't return a token.");

    Session s;
    {
        const ScopedLock sl (lock);
        s = session;
    }
    s.email = email;
    s.idToken = result["IdToken"].toString();
    if (result["RefreshToken"].toString().isNotEmpty())
        s.refreshToken = result["RefreshToken"].toString();
    s.expiresAtMs = Time::currentTimeMillis() + 1000 * (int64) jmax (60, (int) result["ExpiresIn"]) - 60000;

    {
        const ScopedLock sl (lock);
        session = s;
    }
    if (onSessionChanged)
        onSessionChanged (s);
    return Result::ok();
}

Result Client::signIn (const String& email, const String& password, const std::function<String()>& newPassword)
{
    if (! config.isValid())
        return Result::fail ("Lathe isn't set up for the cloud yet (Cloud > Cloud Settings).");

    DynamicObject::Ptr params = new DynamicObject();
    params->setProperty ("USERNAME", email);
    params->setProperty ("PASSWORD", password);
    DynamicObject::Ptr payload = new DynamicObject();
    payload->setProperty ("AuthFlow", "USER_PASSWORD_AUTH");
    payload->setProperty ("ClientId", config.clientId);
    payload->setProperty ("AuthParameters", var (params.get()));

    var resp;
    if (auto r = cognito ("InitiateAuth", var (payload.get()), resp); r.failed())
        return r;

    if (resp["ChallengeName"].toString() == "NEW_PASSWORD_REQUIRED")
    {
        const auto chosen = newPassword ? newPassword() : String();
        if (chosen.isEmpty())
            return Result::fail ("You need to choose a new password the first time you sign in.");

        DynamicObject::Ptr answers = new DynamicObject();
        const auto userId = resp["ChallengeParameters"]["USER_ID_FOR_SRP"].toString();
        answers->setProperty ("USERNAME", userId.isNotEmpty() ? userId : email);
        answers->setProperty ("NEW_PASSWORD", chosen);
        DynamicObject::Ptr challenge = new DynamicObject();
        challenge->setProperty ("ChallengeName", "NEW_PASSWORD_REQUIRED");
        challenge->setProperty ("ClientId", config.clientId);
        challenge->setProperty ("Session", resp["Session"]);
        challenge->setProperty ("ChallengeResponses", var (answers.get()));

        if (auto r = cognito ("RespondToAuthChallenge", var (challenge.get()), resp); r.failed())
            return r;
    }

    if (resp["ChallengeName"].toString().isNotEmpty())
        return Result::fail ("Sign-in needs a step Lathe doesn't support (" + resp["ChallengeName"].toString() + ").");

    return applyAuthResult (resp["AuthenticationResult"], email.trim().toLowerCase());
}

void Client::setSession (Session s)
{
    const ScopedLock sl (lock);
    session = std::move (s);
}

Session Client::getSession() const
{
    const ScopedLock sl (lock);
    return session;
}

void Client::signOut()
{
    {
        const ScopedLock sl (lock);
        session = {};
    }
    if (onSessionChanged)
        onSessionChanged ({});
}

bool Client::isSignedIn() const
{
    const ScopedLock sl (lock);
    return session.isSignedIn();
}

Result Client::ensureFreshToken()
{
    Session s = getSession();
    if (! s.isSignedIn())
        return Result::fail ("Sign in first (Cloud > Sign In).");
    if (s.idToken.isNotEmpty() && Time::currentTimeMillis() < s.expiresAtMs)
        return Result::ok();

    DynamicObject::Ptr params = new DynamicObject();
    params->setProperty ("REFRESH_TOKEN", s.refreshToken);
    DynamicObject::Ptr payload = new DynamicObject();
    payload->setProperty ("AuthFlow", "REFRESH_TOKEN_AUTH");
    payload->setProperty ("ClientId", config.clientId);
    payload->setProperty ("AuthParameters", var (params.get()));

    var resp;
    if (auto r = cognito ("InitiateAuth", var (payload.get()), resp); r.failed())
    {
        if (r.getErrorMessage().containsIgnoreCase ("refresh token") || r.getErrorMessage().containsIgnoreCase ("expired"))
        {
            signOut();
            return Result::fail ("Your sign-in expired. Sign in again (Cloud > Sign In).");
        }
        return r;
    }
    return applyAuthResult (resp["AuthenticationResult"], s.email);
}

var Client::call (const String& method, const String& path, const var& body, ApiError& error, int timeoutMs)
{
    error = {};
    if (auto r = ensureFreshToken(); r.failed())
    {
        error.status = 401;
        error.message = r.getErrorMessage();
        return {};
    }

    const auto token = getSession().idToken;
    String headers = "Authorization: Bearer " + token + "\r\nAccept: application/json";
    MemoryBlock data;
    const bool hasBody = ! body.isVoid();
    if (hasBody)
    {
        const auto text = JSON::toString (body, true);
        data.append (text.toRawUTF8(), text.getNumBytesAsUTF8());
        headers << "\r\nContent-Type: application/json";
    }

    const auto r = httpRequest (method, config.apiUrl + path, hasBody ? &data : nullptr, headers, timeoutMs);
    if (r.ok())
        return r.json();

    // Never report a failure as status 0, which callers read as "no error".
    error.status = r.status != 0 ? r.status : 599;
    error.message = r.errorMessage();
    error.body = r.json();
    if (r.status == 401)
        error.message = "The server didn't accept your sign-in. Sign in again (Cloud > Sign In).";
    return {};
}

//==============================================================================
//==============================================================================
namespace testing
{
std::atomic<int> droppedTransfers { 0 };
void dropNextTransfers (int count)   { droppedTransfers = count; }
} // namespace testing

namespace
{
/** Runs numTasks tasks on up to `concurrency` threads. The calling thread waits,
    calling poll() about ten times a second (return false to cancel: no new tasks
    start, running ones finish). The first failure stops the rest. */
Result runParallel (int numTasks, int concurrency,
                    const std::function<Result (int index, const std::atomic<bool>& stop)>& task,
                    const std::function<bool()>& poll)
{
    if (numTasks <= 0)
        return Result::ok();

    std::atomic<int> next { 0 }, running { 0 };
    std::atomic<bool> stop { false };
    CriticalSection errorLock;
    Result firstError = Result::ok();
    WaitableEvent wake;

    auto worker = [&]
    {
        for (;;)
        {
            if (stop.load())
                break;
            const int i = next.fetch_add (1);
            if (i >= numTasks)
                break;
            auto r = task (i, stop);
            if (r.failed())
            {
                const ScopedLock sl (errorLock);
                if (firstError.wasOk())
                    firstError = r;
                stop = true;
            }
        }
        --running;
        wake.signal();
    };

    const int n = jlimit (1, numTasks, concurrency);
    running = n;
    std::vector<std::thread> threads;
    threads.reserve ((size_t) n);
    for (int i = 0; i < n; ++i)
        threads.emplace_back (worker);

    bool cancelled = false;
    while (running.load() > 0)
    {
        wake.wait (100);
        if (! cancelled && poll && ! poll())
        {
            cancelled = true;
            stop = true;
        }
    }
    for (auto& t : threads)
        t.join();

    if (firstError.failed())
        return firstError;
    return cancelled ? Result::fail ("Cancelled.") : Result::ok();
}

/** Retries a request that failed in a way worth retrying (dropped connection,
    timeout, throttling, server error), waiting 1 s, then 3 s. */
HttpResult withRetries (const std::function<HttpResult()>& attempt, const std::atomic<bool>& stop)
{
    HttpResult r;
    for (int tries = 0; tries < 3; ++tries)
    {
        if (tries > 0)
        {
            for (int waited = 0; waited < (tries == 1 ? 1000 : 3000) && ! stop.load(); waited += 100)
                Thread::sleep (100);
            if (stop.load())
                break;
        }

        if (testing::droppedTransfers.load() > 0 && testing::droppedTransfers.fetch_sub (1) > 0)
            r = {};   // simulated dropped connection (tests)
        else
            r = attempt();

        const bool worthRetrying = r.status == 0 || r.status == 408 || r.status == 429 || r.status >= 500;
        if (r.ok() || ! worthRetrying)
            break;
    }
    return r;
}
} // namespace

Result Client::upload (const String& projectId, const std::vector<UploadItem>& items,
                       const std::function<bool (double)>& progress, int& uploadedCount)
{
    uploadedCount = 0;
    if (items.empty())
        return Result::ok();

    std::map<String, const UploadItem*> byHash;
    Array<var> list;
    for (auto& it : items)
    {
        if (byHash.count (it.hash) != 0)
            continue;
        byHash[it.hash] = &it;
        DynamicObject::Ptr o = new DynamicObject();
        o->setProperty ("hash", it.hash);
        o->setProperty ("size", it.size());
        list.add (var (o.get()));
    }

    // Ask which files the server still needs (up to 500 at a time).
    Array<var> uploads;
    for (int i = 0; i < list.size(); i += 400)
    {
        Array<var> batch;
        for (int j = i; j < jmin (list.size(), i + 400); ++j)
            batch.add (list[j]);
        DynamicObject::Ptr req = new DynamicObject();
        req->setProperty ("blobs", batch);
        ApiError err;
        const auto resp = call ("POST", "/projects/" + projectId + "/blobs/uploads", var (req.get()), err);
        if (err.status != 0)
            return Result::fail (err.message);
        if (auto* u = resp["uploads"].getArray())
            uploads.addArray (*u);
    }

    // Every PUT is a task: a whole small file, or one piece of a big one.
    struct Piece
    {
        const UploadItem* item;
        String url;
        int64 start, length;
        int fileIndex, partNumber;   // partNumber 0 = a whole file
    };
    struct BigFile
    {
        String hash, uploadId;
        std::vector<String> etags;   // by part number - 1
    };

    std::vector<Piece> pieces;
    std::vector<BigFile> bigFiles;
    int64 total = 0;
    for (auto& u : uploads)
    {
        auto it = byHash.find (u["hash"].toString());
        if (it == byHash.end())
            continue;
        const auto* item = it->second;
        const int64 size = item->size();
        total += size;

        if (u["method"].toString() == "PUT")
        {
            pieces.push_back ({ item, u["url"].toString(), 0, size, -1, 0 });
            continue;
        }

        const int64 partSize = u["partSize"].isVoid() ? (int64) kPartSizeFallback : (int64) u["partSize"];
        BigFile big { item->hash, u["uploadId"].toString(), {} };
        if (auto* parts = u["parts"].getArray())
        {
            big.etags.resize ((size_t) parts->size());
            for (auto& p : *parts)
            {
                const int number = (int) p["number"];
                const int64 start = (int64) (number - 1) * partSize;
                pieces.push_back ({ item, p["url"].toString(), start, jmin (partSize, size - start),
                                    (int) bigFiles.size(), number });
            }
        }
        bigFiles.push_back (std::move (big));
    }

    std::atomic<int64> done { 0 };
    std::atomic<int> wholeFilesDone { 0 };
    CriticalSection etagLock;
    const String octet = "Content-Type: application/octet-stream";

    auto r = runParallel ((int) pieces.size(), parallelTransfers.load(), [&] (int index, const std::atomic<bool>& stop)
    {
        const auto& piece = pieces[(size_t) index];
        const auto& item = *piece.item;
        if (piece.length <= 0)
            return Result::fail ("Couldn't read " + item.file.getFullPathName());

        // Read just this piece, right before sending it.
        MemoryBlock data;
        if (item.file == File())
        {
            data.append (static_cast<const char*> (item.data.getData()) + piece.start, (size_t) piece.length);
        }
        else
        {
            FileInputStream in (item.file);
            data.setSize ((size_t) piece.length);
            if (in.failedToOpen() || ! in.setPosition (piece.start)
                || in.read (data.getData(), (int) piece.length) != (int) piece.length)
                return Result::fail ("Couldn't read " + item.file.getFullPathName());
        }

        const auto res = withRetries ([&] { return httpRequest ("PUT", piece.url, &data, octet, 600000); }, stop);
        if (! res.ok())
            return Result::fail ("Upload failed: " + res.errorMessage());

        if (piece.partNumber > 0)
        {
            const ScopedLock sl (etagLock);
            bigFiles[(size_t) piece.fileIndex].etags[(size_t) piece.partNumber - 1] = res.headers.getValue ("ETag", {});
        }
        else
        {
            ++wholeFilesDone;
        }
        done += piece.length;
        return Result::ok();
    },
    [&] { return progress == nullptr || total <= 0 || progress ((double) done.load() / (double) total); });

    if (r.failed())
        return r;

    // Join each big file's pieces (S3 can take a while for very big ones).
    for (auto& big : bigFiles)
    {
        Array<var> parts;
        for (size_t i = 0; i < big.etags.size(); ++i)
        {
            DynamicObject::Ptr e = new DynamicObject();
            e->setProperty ("number", (int) i + 1);
            e->setProperty ("etag", big.etags[i]);
            parts.add (var (e.get()));
        }
        DynamicObject::Ptr req = new DynamicObject();
        req->setProperty ("hash", big.hash);
        req->setProperty ("uploadId", big.uploadId);
        req->setProperty ("parts", parts);
        ApiError err;
        call ("POST", "/projects/" + projectId + "/blobs/complete", var (req.get()), err, 600000);
        if (err.status != 0)
            return Result::fail (err.message);
    }

    uploadedCount = wholeFilesDone.load() + (int) bigFiles.size();
    if (progress)
        progress (1.0);
    return Result::ok();
}

Result Client::download (const String& projectId, const std::vector<std::pair<String, File>>& wanted,
                         const std::function<bool (double)>& progress)
{
    if (wanted.empty())
        return Result::ok();

    // Links for everything first (cheap), then the files, several at a time.
    std::vector<String> urls (wanted.size());
    for (size_t i = 0; i < wanted.size(); i += 400)
    {
        Array<var> hashes;
        for (size_t j = i; j < jmin (wanted.size(), i + 400); ++j)
            hashes.addIfNotAlreadyThere (wanted[j].first);
        DynamicObject::Ptr req = new DynamicObject();
        req->setProperty ("hashes", hashes);
        ApiError err;
        const auto resp = call ("POST", "/projects/" + projectId + "/blobs/downloads", var (req.get()), err);
        if (err.status != 0)
            return Result::fail (err.message);
        for (size_t j = i; j < jmin (wanted.size(), i + 400); ++j)
        {
            urls[j] = resp["urls"][Identifier (wanted[j].first)].toString();
            if (urls[j].isEmpty())
                return Result::fail ("The server didn't offer " + wanted[j].first.substring (0, 12));
        }
    }

    std::vector<std::atomic<float>> fraction (wanted.size());
    for (auto& f : fraction)
        f = 0.0f;

    auto r = runParallel ((int) wanted.size(), parallelTransfers.load(), [&] (int index, const std::atomic<bool>& stop)
    {
        const auto& [hash, dest] = wanted[(size_t) index];
        Result last = Result::ok();
        for (int tries = 0; tries < 3; ++tries)
        {
            if (tries > 0)
                for (int waited = 0; waited < (tries == 1 ? 1000 : 3000) && ! stop.load(); waited += 100)
                    Thread::sleep (100);
            if (stop.load())
                return Result::fail ("Cancelled.");

            if (testing::droppedTransfers.load() > 0 && testing::droppedTransfers.fetch_sub (1) > 0)
            {
                last = Result::fail ("Couldn't connect (simulated).");
                continue;
            }

            last = httpDownload (urls[(size_t) index], dest, [&] (double p)
            {
                fraction[(size_t) index] = (float) p;
                return ! stop.load();
            });
            if (last.failed())
            {
                if (stop.load())
                    return last;
                continue;   // dropped connection: try again
            }
            if (sha256OfFile (dest) == hash)
            {
                fraction[(size_t) index] = 1.0f;
                return Result::ok();
            }
            dest.deleteFile();
            last = Result::fail ("A downloaded file was damaged (" + dest.getFileName() + ").");
        }
        return last;
    },
    [&]
    {
        if (progress == nullptr)
            return true;
        double sum = 0.0;
        for (auto& f : fraction)
            sum += f.load();
        return progress (sum / (double) wanted.size());
    });

    if (r.failed())
        return r;
    if (progress)
        progress (1.0);
    return Result::ok();
}
} // namespace lathe::cloud
