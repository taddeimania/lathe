#include "CloudClient.h"

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

/** Splits a body over the progress range of one item among several. */
std::function<bool (double)> subProgress (const std::function<bool (double)>& progress, double start, double span)
{
    return [=] (double p) { return progress == nullptr || progress (start + span * jlimit (0.0, 1.0, p)); };
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

String sha256OfFile (const File& f)
{
    FileInputStream in (f);
    if (in.failedToOpen())
        return {};
    return SHA256 (in).toHexString().toLowerCase();
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

var Client::call (const String& method, const String& path, const var& body, ApiError& error)
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

    const auto r = httpRequest (method, config.apiUrl + path, hasBody ? &data : nullptr, headers, 30000);
    if (r.ok())
        return r.json();

    error.status = r.status;
    error.message = r.errorMessage();
    error.body = r.json();
    if (r.status == 401)
        error.message = "The server didn't accept your sign-in. Sign in again (Cloud > Sign In).";
    return {};
}

//==============================================================================
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

    // Ask in batches (the API takes up to 500 at a time).
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

    int64 total = 0, done = 0;
    for (auto& u : uploads)
        if (auto it = byHash.find (u["hash"].toString()); it != byHash.end())
            total += it->second->size();

    const String octet = "Content-Type: application/octet-stream";
    for (auto& u : uploads)
    {
        const auto hash = u["hash"].toString();
        auto it = byHash.find (hash);
        if (it == byHash.end())
            continue;
        const auto& item = *it->second;
        const int64 size = item.size();

        auto readRange = [&item] (int64 start, int64 length, MemoryBlock& out) -> bool
        {
            out.reset();
            if (item.file == File())
            {
                out.append (static_cast<const char*> (item.data.getData()) + start, (size_t) length);
                return true;
            }
            FileInputStream in (item.file);
            if (in.failedToOpen() || ! in.setPosition (start))
                return false;
            out.setSize ((size_t) length);
            return in.read (out.getData(), (int) length) == (int) length;
        };

        if (u["method"].toString() == "PUT")
        {
            MemoryBlock data;
            if (! readRange (0, size, data))
                return Result::fail ("Couldn't read " + item.file.getFullPathName());
            const auto r = httpRequest ("PUT", u["url"].toString(), &data, octet, 300000);
            if (! r.ok())
                return Result::fail ("Upload failed: " + r.errorMessage());
            done += size;
        }
        else
        {
            const int64 partSize = u["partSize"].isVoid() ? (int64) kPartSizeFallback : (int64) u["partSize"];
            Array<var> etags;
            if (auto* parts = u["parts"].getArray())
            {
                for (auto& p : *parts)
                {
                    const int number = (int) p["number"];
                    const int64 start = (int64) (number - 1) * partSize;
                    const int64 length = jmin (partSize, size - start);
                    MemoryBlock data;
                    if (length <= 0 || ! readRange (start, length, data))
                        return Result::fail ("Couldn't read " + item.file.getFullPathName());
                    const auto r = httpRequest ("PUT", p["url"].toString(), &data, octet, 600000);
                    if (! r.ok())
                        return Result::fail ("Upload failed: " + r.errorMessage());
                    DynamicObject::Ptr e = new DynamicObject();
                    e->setProperty ("number", number);
                    e->setProperty ("etag", r.headers.getValue ("ETag", {}));
                    etags.add (var (e.get()));
                    done += length;
                    if (progress && total > 0 && ! progress ((double) done / (double) total))
                        return Result::fail ("Cancelled.");
                }
            }
            DynamicObject::Ptr req = new DynamicObject();
            req->setProperty ("hash", hash);
            req->setProperty ("uploadId", u["uploadId"]);
            req->setProperty ("parts", etags);
            ApiError err;
            call ("POST", "/projects/" + projectId + "/blobs/complete", var (req.get()), err);
            if (err.status != 0)
                return Result::fail (err.message);
        }

        ++uploadedCount;
        if (progress && total > 0 && ! progress ((double) done / (double) total))
            return Result::fail ("Cancelled.");
    }
    return Result::ok();
}

Result Client::download (const String& projectId, const std::vector<std::pair<String, File>>& wanted,
                         const std::function<bool (double)>& progress)
{
    if (wanted.empty())
        return Result::ok();

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
            const auto& [hash, dest] = wanted[j];
            const auto url = resp["urls"][Identifier (hash)].toString();
            if (url.isEmpty())
                return Result::fail ("The server didn't offer " + hash.substring (0, 12));

            const double start = (double) j / (double) wanted.size(), span = 1.0 / (double) wanted.size();
            if (auto r = httpDownload (url, dest, subProgress (progress, start, span)); r.failed())
                return r;
            if (sha256OfFile (dest) != hash)
            {
                dest.deleteFile();
                return Result::fail ("A downloaded file was damaged (" + dest.getFileName() + "). Try again.");
            }
        }
    }
    if (progress)
        progress (1.0);
    return Result::ok();
}
} // namespace lathe::cloud
