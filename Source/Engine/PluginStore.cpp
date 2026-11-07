#include "PluginStore.h"

namespace lathe
{
namespace
{
constexpr int64 kCacheLifetimeMs = 60 * 60 * 1000;   // GitHub allows 60 anonymous API calls an hour

var fetchJson (const String& url, int& statusCode, String& error)
{
    statusCode = 0;
    auto options = URL::InputStreamOptions (URL::ParameterHandling::inAddress)
                       .withExtraHeaders ("User-Agent: Lathe-DAW\r\nAccept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28")
                       .withConnectionTimeoutMs (15000)
                       .withNumRedirectsToFollow (5)
                       .withStatusCode (&statusCode);

    auto stream = URL (url).createInputStream (options);
    if (stream == nullptr)
    {
        error = "Couldn't reach GitHub. Check your internet connection.";
        return {};
    }

    const auto text = stream->readEntireStreamAsString();
    if (statusCode == 403 || statusCode == 429)
        error = "GitHub's limit for anonymous requests was reached. Try again in an hour.";
    else if (statusCode == 404)
        error = "No published release yet.";
    else if (statusCode >= 400)
        error = "GitHub answered " + String (statusCode) + ".";

    return JSON::parse (text);
}

String stripV (const String& tag)
{
    return tag.trimCharactersAtStart ("vV").trim();
}

var readJson (const File& f)
{
    return f.existsAsFile() ? JSON::parse (f) : var();
}

void writeJson (const File& f, const var& v)
{
    f.getParentDirectory().createDirectory();
    f.replaceWithText (JSON::toString (v));
}

DynamicObject* objectOf (var& v)
{
    if (! v.isObject())
        v = var (new DynamicObject());
    return v.getDynamicObject();
}
} // namespace

//==============================================================================
bool StoreEntry::updateAvailable() const
{
    return isInstalled() && version.isNotEmpty() && installedVersion.isNotEmpty() && installedVersion != "?"
           && PluginStore::compareVersions (version, installedVersion) > 0;
}

std::vector<StoreEntry> PluginStore::defaultCatalog()
{
    struct D { const char* repo; const char* name; const char* kind; const char* description; };
    static const D defaults[] = {
        { "taddeimania/nave",     "Nave",     "Effect",     "Reverb suite" },
        { "taddeimania/tarry",    "Tarry",    "Effect",     "Delay suite" },
        { "taddeimania/mottle",   "Mottle",   "Effect",     "Colour and texture multi-effect" },
        { "taddeimania/planish",  "Planish",  "Effect",     "Mixing suite" },
        { "taddeimania/burnish",  "Burnish",  "Effect",     "Mastering suite" },
        { "taddeimania/lacquer",  "Lacquer",  "Effect",     "One-knob mastering assistant" },
        { "taddeimania/descant",  "Descant",  "Effect",     "Vocoder and vocal harmony" },
        { "taddeimania/tincture", "Tincture", "Instrument", "Wavetable synthesizer" },
        { "taddeimania/fractone", "Fractone", "Instrument", "Subharmonic polyrhythm synth" },
        { "taddeimania/stepkin",  "Stepkin",  "Instrument", "Percussion synth with sequencer" },
        { "taddeimania/seyir",    "Seyir",    "Instrument", "Microtonal melody explorer" },
        { "taddeimania/beadwork", "Beadwork", "MIDI",       "Euclidean drum sequencer" },
        { "taddeimania/dicey",    "Dicey",    "MIDI",       "Random melody sequencer" },
        { "taddeimania/posy",     "Posy",     "MIDI",       "Chord generator" },
    };

    std::vector<StoreEntry> list;
    for (auto& d : defaults)
    {
        StoreEntry e;
        e.repo = d.repo;
        e.name = d.name;
        e.kind = d.kind;
        e.description = d.description;
        list.push_back (e);
    }
    return list;
}

//==============================================================================
PluginStore::PluginStore (PluginHost& h, const File& dataDirectory)
    : host (h), dataDir (dataDirectory)
{
    cache = readJson (cacheFile());
    loadCatalog();
    updateInstalledState();

    // Show what we already know straight away; refresh() fills in the rest.
    for (auto& e : entries)
        if (auto c = cache[Identifier (e.repo)]; c.isObject())
            parseRelease (c["release"], e);
}

PluginStore::~PluginStore()
{
    pool.removeAllJobs (true, 20000);
}

StoreEntry* PluginStore::find (const String& repo)
{
    for (auto& e : entries)
        if (e.repo.equalsIgnoreCase (repo))
            return &e;
    return nullptr;
}

void PluginStore::loadCatalog()
{
    entries.clear();
    auto saved = readJson (catalogFile());

    if (auto* list = saved.getArray())
    {
        for (auto& v : *list)
        {
            StoreEntry e;
            e.repo = v["repo"].toString().trim();
            e.name = v["name"].toString().trim();
            e.kind = v["kind"].toString().trim();
            e.description = v["description"].toString();
            if (e.repo.containsChar ('/') && find (e.repo) == nullptr)
                entries.push_back (e);
        }
    }

    // New built-in entries appear even if the user has their own catalog file.
    bool added = false;
    for (auto& d : defaultCatalog())
    {
        if (find (d.repo) == nullptr)
        {
            entries.push_back (d);
            added = true;
        }
    }

    if (added || ! catalogFile().existsAsFile())
        saveCatalog();
}

void PluginStore::saveCatalog()
{
    Array<var> list;
    for (auto& e : entries)
    {
        DynamicObject::Ptr o = new DynamicObject();
        o->setProperty ("repo", e.repo);
        o->setProperty ("name", e.name);
        o->setProperty ("kind", e.kind);
        o->setProperty ("description", e.description);
        list.add (var (o.get()));
    }
    writeJson (catalogFile(), list);
}

void PluginStore::addRepository (const String& text)
{
    // Accept "owner/repo" or a github.com URL.
    auto repo = text.trim().fromLastOccurrenceOf ("github.com/", false, true).upToFirstOccurrenceOf ("/releases", false, true)
                    .trimCharactersAtEnd ("/").replace (".git", "");
    const auto parts = StringArray::fromTokens (repo, "/", "");
    if (parts.size() != 2 || parts[0].isEmpty() || parts[1].isEmpty())
    {
        status = "Enter a repository as owner/name, e.g. taddeimania/tarry";
        notify();
        return;
    }

    repo = parts[0] + "/" + parts[1];
    if (find (repo) == nullptr)
    {
        StoreEntry e;
        e.repo = repo;
        e.kind = "Effect";
        entries.push_back (e);
        saveCatalog();
    }
    refresh (true);
}

//==============================================================================
String PluginStore::platformTag()
{
   #if JUCE_MAC
    return "macOS";
   #elif JUCE_WINDOWS
    return "Windows";
   #else
    return "Linux";
   #endif
}

File PluginStore::getUserVst3Dir()
{
   #if JUCE_MAC
    return File ("~/Library/Audio/Plug-Ins/VST3");
   #elif JUCE_WINDOWS
    // The per-user VST3 folder (VST 3.7+): no administrator rights needed.
    return File::getSpecialLocation (File::windowsLocalAppData).getChildFile ("Programs").getChildFile ("Common").getChildFile ("VST3");
   #else
    return File ("~/.vst3");
   #endif
}

int PluginStore::compareVersions (const String& a, const String& b)
{
    auto pa = StringArray::fromTokens (stripV (a), ".-", "");
    auto pb = StringArray::fromTokens (stripV (b), ".-", "");
    for (int i = 0; i < jmax (pa.size(), pb.size()); ++i)
    {
        const int x = pa[i].getIntValue(), y = pb[i].getIntValue();
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

void PluginStore::parseRelease (const var& json, StoreEntry& e)
{
    e.version = e.tag = e.releaseUrl = e.notes = e.publishedAt = e.assetName = e.assetUrl = {};
    e.assetSize = 0;

    if (! json.isObject() || (bool) json["draft"])
        return;

    e.tag = json["tag_name"].toString();
    e.version = stripV (e.tag);
    e.releaseUrl = json["html_url"].toString();
    e.notes = json["body"].toString().replace ("\r\n", "\n");
    e.publishedAt = json["published_at"].toString().upToFirstOccurrenceOf ("T", false, false);

    const auto platform = "-" + platformTag();
    if (auto* assets = json["assets"].getArray())
    {
        for (auto& a : *assets)
        {
            const auto name = a["name"].toString();
            const bool archive = name.endsWithIgnoreCase (".zip") || name.endsWithIgnoreCase (".tar.gz") || name.endsWithIgnoreCase (".tgz");
            if (archive && name.contains (platform))
            {
                e.assetName = name;
                e.assetUrl = a["browser_download_url"].toString();
                e.assetSize = (int64) a["size"];
                break;
            }
        }
    }

    // Repositories added by hand get their product name from the asset ("Foo-1.2.0-macOS.zip").
    if (e.name.isEmpty() && e.assetName.isNotEmpty())
        e.name = e.assetName.upToFirstOccurrenceOf ("-", false, false);
    if (e.name.isEmpty())
        e.name = e.repo.fromLastOccurrenceOf ("/", false, false);
}

//==============================================================================
void PluginStore::refresh (bool force)
{
    if (refreshing)
        return;

    refreshing = true;
    status = "Checking GitHub for releases...";
    notify();

    struct Job { String repo; var cached; bool useCache; };
    std::vector<Job> jobs;
    const auto now = Time::currentTimeMillis();
    for (auto& e : entries)
    {
        auto c = cache[Identifier (e.repo)];
        const bool fresh = c.isObject() && now - (int64) c["fetched"] < kCacheLifetimeMs;
        jobs.push_back ({ e.repo, c, fresh && ! force });
    }

    WeakReference<PluginStore> weak (this);
    pool.addJob ([weak, jobs]
    {
        struct Result { String repo; var release; String error; bool fromNetwork; };
        std::vector<Result> results;

        for (auto& j : jobs)
        {
            if (j.useCache)
            {
                results.push_back ({ j.repo, j.cached["release"], {}, false });
                continue;
            }

            int statusCode = 0;
            String error;
            auto json = fetchJson ("https://api.github.com/repos/" + j.repo + "/releases/latest", statusCode, error);
            if (error.isNotEmpty() && j.cached.isObject())
                results.push_back ({ j.repo, j.cached["release"], error, false });   // keep showing the last good answer
            else
                results.push_back ({ j.repo, error.isEmpty() ? json : var(), error, error.isEmpty() });

            if (statusCode == 403 || statusCode == 429)
            {
                // Rate limited: no point asking about the rest.
                for (size_t k = results.size(); k < jobs.size(); ++k)
                    results.push_back ({ jobs[k].repo, jobs[k].cached["release"], error, false });
                break;
            }
        }

        MessageManager::callAsync ([weak, results]
        {
            auto* self = weak.get();
            if (self == nullptr)
                return;

            const auto now2 = Time::currentTimeMillis();
            StringArray errors;
            int available = 0;

            for (auto& r : results)
            {
                auto* e = self->find (r.repo);
                if (e == nullptr)
                    continue;

                parseRelease (r.release, *e);
                e->fetchError = e->hasRelease() ? String() : (r.error.isNotEmpty() ? r.error : String ("No " + platformTag() + " build in the latest release."));
                if (r.error.isNotEmpty())
                    errors.addIfNotAlreadyThere (r.error);
                if (e->hasRelease())
                    ++available;

                if (r.fromNetwork)
                {
                    DynamicObject::Ptr c = new DynamicObject();
                    c->setProperty ("fetched", now2);
                    c->setProperty ("release", r.release);
                    objectOf (self->cache)->setProperty (Identifier (r.repo), var (c.get()));
                }
            }

            writeJson (self->cacheFile(), self->cache);
            self->updateInstalledState();
            self->saveCatalog();
            self->refreshing = false;
            self->status = errors.isEmpty() ? String (available) + " plug-ins available from GitHub."
                                            : errors.joinIntoString (" ");
            self->notify();
        });
    });
}

//==============================================================================
std::optional<PluginDescription> PluginStore::findDescription (const StoreEntry& e) const
{
    const auto bundleName = e.name + ".vst3";
    std::optional<PluginDescription> best;
    for (auto& d : host.known.getTypes())
    {
        if (File::createFileWithoutCheckingPath (d.fileOrIdentifier).getFileName().equalsIgnoreCase (bundleName))
        {
            // Prefer the copy in the per-user folder (that's the one we install/update).
            if (! best.has_value() || File (d.fileOrIdentifier).isAChildOf (getUserVst3Dir()))
                best = d;
        }
    }
    return best;
}

void PluginStore::updateInstalledState()
{
    auto record = readJson (recordFile());

    for (auto& e : entries)
    {
        e.installedVersion = {};
        e.installedBundle = File();
        e.pendingRestart = false;

        if (auto r = record[Identifier (e.repo)]; r.isObject())
        {
            const File bundle (r["bundle"].toString());
            if (bundle.exists() || (bool) r["pending"])
            {
                e.installedBundle = bundle;
                e.installedVersion = r["version"].toString();
                e.pendingRestart = (bool) r["pending"];
                continue;
            }
        }

        if (auto d = findDescription (e))
        {
            e.installedBundle = File (d->fileOrIdentifier);
            e.installedVersion = d->version.isNotEmpty() ? d->version : String ("?");
            continue;
        }

        if (auto b = vst3Dir().getChildFile (e.name + ".vst3"); e.name.isNotEmpty() && b.exists())
        {
            e.installedBundle = b;
            e.installedVersion = "?";
        }
    }
}

//==============================================================================
Result PluginStore::installArchive (const File& archive, const String& expectedName, const File& targetDir,
                                    const File& data, File& installedBundle, bool& pendingRestart)
{
    pendingRestart = false;
    const auto extractDir = archive.getSiblingFile (archive.getFileNameWithoutExtension() + " (unpacked)");
    extractDir.deleteRecursively();
    extractDir.createDirectory();

    const auto name = archive.getFileName();
    if (name.endsWithIgnoreCase (".zip"))
    {
        ZipFile zip (archive);
        if (zip.getNumEntries() == 0)
            return Result::fail ("The download isn't a readable zip file.");

        if (auto r = zip.uncompressTo (extractDir, true); r.failed())
            return Result::fail ("Couldn't unpack: " + r.getErrorMessage());

       #if ! JUCE_WINDOWS
        // ZipFile doesn't restore Unix permissions; plug-in binaries need their execute bit.
        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            if (auto* entry = zip.getEntry (i))
            {
                const auto mode = (entry->externalFileAttributes >> 16) & 0777;
                const auto f = extractDir.getChildFile (entry->filename);
                if ((mode & 0111) != 0 && f.existsAsFile())
                    f.setExecutePermission (true);
            }
        }
       #endif
    }
    else if (name.endsWithIgnoreCase (".tar.gz") || name.endsWithIgnoreCase (".tgz"))
    {
        ChildProcess tar;
        if (! tar.start (StringArray { "tar", "-xzf", archive.getFullPathName(), "-C", extractDir.getFullPathName() })
            || ! tar.waitForProcessToFinish (120000) || tar.getExitCode() != 0)
            return Result::fail ("Couldn't unpack " + name + " (tar failed).");
    }
    else
    {
        return Result::fail ("Unknown archive type: " + name);
    }

    // Find the plug-in bundle: the shallowest <Name>.vst3, else any .vst3.
    auto candidates = extractDir.findChildFiles (File::findDirectories, true, "*.vst3");
    File bundle;
    for (auto& c : candidates)
    {
        if (c.getParentDirectory().getFileExtension().equalsIgnoreCase (".vst3"))
            continue;
        const bool better = bundle == File()
                         || (c.getFileName().equalsIgnoreCase (expectedName + ".vst3") && ! bundle.getFileName().equalsIgnoreCase (expectedName + ".vst3"))
                         || c.getFullPathName().length() < bundle.getFullPathName().length();
        if (better)
            bundle = c;
    }

    if (bundle == File())
    {
        extractDir.deleteRecursively();
        return Result::fail ("The download doesn't contain a .vst3 plug-in.");
    }

    targetDir.createDirectory();
    const auto target = targetDir.getChildFile (bundle.getFileName());
    installedBundle = target;

    if (target.exists())
    {
        // Move the old copy aside first; if it's locked (loaded on Windows), stage the new one for the next launch.
        const auto aside = target.getSiblingFile (target.getFileName() + ".lathe-old");
        aside.deleteRecursively();

        if (! target.moveFileTo (aside))
        {
            const auto staged = data.getChildFile ("Pending").getChildFile (bundle.getFileName());
            staged.getParentDirectory().createDirectory();
            staged.deleteRecursively();
            if (! bundle.moveFileTo (staged))
            {
                extractDir.deleteRecursively();
                return Result::fail ("Couldn't stage the update in " + staged.getFullPathName());
            }

            auto pendingList = readJson (data.getChildFile ("Pending Installs.json"));
            if (! pendingList.isArray())
                pendingList = Array<var>();
            DynamicObject::Ptr o = new DynamicObject();
            o->setProperty ("source", staged.getFullPathName());
            o->setProperty ("target", target.getFullPathName());
            pendingList.append (var (o.get()));
            writeJson (data.getChildFile ("Pending Installs.json"), pendingList);

            pendingRestart = true;
            extractDir.deleteRecursively();
            return Result::ok();
        }

        if (! bundle.moveFileTo (target))
        {
            aside.moveFileTo (target);   // put the old one back
            extractDir.deleteRecursively();
            return Result::fail ("Couldn't install into " + targetDir.getFullPathName());
        }
        aside.deleteRecursively();   // may fail on Windows while loaded; cleaned up at next launch
    }
    else if (! bundle.moveFileTo (target))
    {
        extractDir.deleteRecursively();
        return Result::fail ("Couldn't install into " + targetDir.getFullPathName() + ". Check that the folder is writable.");
    }

    extractDir.deleteRecursively();
    return Result::ok();
}

Array<File> PluginStore::applyPendingInstalls (const File& data)
{
    Array<File> applied;
    const auto pendingFile = data.getChildFile ("Pending Installs.json");
    auto record = readJson (data.getChildFile ("Installed Plugins.json"));
    bool recordChanged = false;

    const auto pending = readJson (pendingFile);
    if (auto* list = pending.getArray())
    {
        for (auto& p : *list)
        {
            const File source (p["source"].toString());
            const File target (p["target"].toString());
            if (! source.exists())
                continue;
            target.deleteRecursively();
            if (source.moveFileTo (target))
                applied.add (target);
        }
        pendingFile.deleteFile();

        if (auto* obj = record.getDynamicObject())
        {
            for (auto& prop : obj->getProperties())
            {
                if (auto* entry = prop.value.getDynamicObject(); entry != nullptr && (bool) entry->getProperty ("pending"))
                {
                    entry->setProperty ("pending", false);
                    recordChanged = true;
                }
            }
        }
    }

    if (recordChanged)
        writeJson (data.getChildFile ("Installed Plugins.json"), record);

    // Old copies that couldn't be deleted last time (they were loaded).
    for (auto& old : getUserVst3Dir().findChildFiles (File::findFilesAndDirectories, false, "*.lathe-old"))
        old.deleteRecursively();

    return applied;
}

//==============================================================================
void PluginStore::install (String repo, InstallDone done)
{
    auto* e = find (repo);
    if (e == nullptr || e->busy)
        return;

    if (! e->hasRelease())
    {
        done (Result::fail (e->fetchError.isNotEmpty() ? e->fetchError : "No " + platformTag() + " download found for " + e->name + "."), nullptr);
        return;
    }

    e->busy = true;
    e->progress = 0.0;
    notify();

    const auto url = e->assetUrl, assetName = e->assetName, name = e->name, version = e->version;
    const auto expectedSize = e->assetSize;
    const auto data = dataDir;
    const auto target = vst3Dir();
    WeakReference<PluginStore> weak (this);

    pool.addJob ([=]
    {
        const auto downloads = data.getChildFile ("Downloads");
        downloads.createDirectory();
        const auto file = downloads.getChildFile (assetName);
        file.deleteFile();

        auto setProgress = [weak, repo] (double p)
        {
            MessageManager::callAsync ([weak, repo, p]
            {
                if (auto* self = weak.get())
                    if (auto* entry = self->find (repo))
                        entry->progress = p;
            });
        };

        Result result = Result::ok();
        {
            int statusCode = 0;
            auto options = URL::InputStreamOptions (URL::ParameterHandling::inAddress)
                               .withExtraHeaders ("User-Agent: Lathe-DAW\r\nAccept: application/octet-stream")
                               .withConnectionTimeoutMs (20000)
                               .withNumRedirectsToFollow (10)
                               .withStatusCode (&statusCode);
            auto in = URL (url).createInputStream (options);
            std::unique_ptr<FileOutputStream> out (file.createOutputStream());

            if (in == nullptr || (statusCode >= 400))
                result = Result::fail ("Couldn't download " + assetName + (statusCode >= 400 ? " (HTTP " + String (statusCode) + ")" : String()));
            else if (out == nullptr)
                result = Result::fail ("Couldn't write " + file.getFullPathName());
            else
            {
                HeapBlock<char> buffer (65536);
                int64 received = 0;
                const int64 total = expectedSize > 0 ? expectedSize : in->getTotalLength();
                uint32 lastReport = 0;
                for (;;)
                {
                    const auto n = in->read (buffer, 65536);
                    if (n <= 0)
                        break;
                    out->write (buffer, (size_t) n);
                    received += n;
                    if (total > 0 && Time::getMillisecondCounter() - lastReport > 80)
                    {
                        lastReport = Time::getMillisecondCounter();
                        setProgress (0.9 * (double) received / (double) total);
                    }
                }
                out->flush();
                if (received == 0 || (expectedSize > 0 && received != expectedSize))
                    result = Result::fail ("The download of " + assetName + " was incomplete. Try again.");
            }
        }

        File bundle;
        bool pending = false;
        if (result.wasOk())
        {
            setProgress (0.95);
            result = installArchive (file, name, target, data, bundle, pending);
        }
        file.deleteFile();

        MessageManager::callAsync ([weak, repo, result, bundle, pending, version, done]
        {
            auto* self = weak.get();
            if (self == nullptr)
                return;

            if (auto* entry = self->find (repo))
            {
                entry->busy = false;
                entry->progress = 0.0;
            }

            OwnedArray<PluginDescription> found;
            if (result.wasOk())
            {
                auto record = readJson (self->recordFile());
                DynamicObject::Ptr r = new DynamicObject();
                r->setProperty ("version", version);
                r->setProperty ("bundle", bundle.getFullPathName());
                r->setProperty ("pending", pending);
                r->setProperty ("installed", Time::getCurrentTime().toISO8601 (true));
                objectOf (record)->setProperty (Identifier (repo), var (r.get()));
                writeJson (self->recordFile(), record);

                if (! pending)
                    self->host.scanFile (bundle, found);

                // Learn the real kind from the plug-in itself.
                if (auto* entry = self->find (repo); entry != nullptr && ! found.isEmpty() && entry->kind == "Effect" && found[0]->isInstrument)
                {
                    entry->kind = "Instrument";
                    self->saveCatalog();
                }
            }

            self->updateInstalledState();
            self->notify();
            if (done)
                done (result, found.isEmpty() ? nullptr : found[0]);
        });
    });
}
} // namespace lathe
