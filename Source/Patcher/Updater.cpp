#include "Updater.h"

namespace lathe::updater
{
namespace
{
String stripV (const String& tag)
{
    return tag.trim().trimCharactersAtStart ("vV");
}

bool isArchiveName (const String& name)
{
    return name.endsWithIgnoreCase (".zip") || name.endsWithIgnoreCase (".tar.gz") || name.endsWithIgnoreCase (".tgz");
}

String executableName()
{
   #if JUCE_WINDOWS
    return "Lathe.exe";
   #else
    return "Lathe";
   #endif
}

/** The shallowest match below dir, or an invalid File. */
File shallowest (const File& dir, int whatToFind, const String& pattern)
{
    File best;
    int bestDepth = std::numeric_limits<int>::max();
    for (auto& f : dir.findChildFiles (whatToFind, true, pattern))
    {
        const int depth = f.getRelativePathFrom (dir).retainCharacters ("/\\").length();
        if (depth < bestDepth)
        {
            bestDepth = depth;
            best = f;
        }
    }
    return best;
}

Result run (const StringArray& command, int timeoutMs)
{
    ChildProcess p;
    if (! p.start (command, ChildProcess::wantStdErr))
        return Result::fail ("Couldn't run " + command[0]);
    const auto output = p.readAllProcessOutput();
    if (! p.waitForProcessToFinish (timeoutMs))
    {
        p.kill();
        return Result::fail (command[0] + " took too long.");
    }
    if (p.getExitCode() != 0)
        return Result::fail (command[0] + " failed: " + output.trim());
    return Result::ok();
}
} // namespace

//==============================================================================
String platformTag()
{
   #if JUCE_MAC
    return "macOS";
   #elif JUCE_WINDOWS
    return "Windows";
   #else
    return "Linux";
   #endif
}

int compareVersions (const String& a, const String& b)
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

Result parseRelease (const var& json, const String& platform, Release& out)
{
    out = {};
    if (! json.isObject() || ! json.hasProperty ("tag_name"))
        return Result::fail ("GitHub sent something unexpected.");

    out.tag = json["tag_name"].toString();
    out.version = stripV (out.tag);
    out.notes = json["body"].toString().trim();
    out.pageUrl = json["html_url"].toString();

    const var assets = json["assets"];
    if (auto* list = assets.getArray())
    {
        for (auto& a : *list)
        {
            const auto name = a["name"].toString();
            if (! name.startsWithIgnoreCase ("Lathe-") || ! isArchiveName (name)
                || ! name.containsIgnoreCase ("-" + platform))
                continue;

            out.assetName = name;
            out.assetUrl = a["browser_download_url"].toString();
            out.assetSize = (int64) a["size"];
            const auto digest = a["digest"].toString();
            if (digest.startsWithIgnoreCase ("sha256:"))
                out.sha256 = digest.fromFirstOccurrenceOf (":", false, false).toLowerCase();
            break;
        }
    }

    if (out.assetUrl.isEmpty())
        return Result::fail ("Lathe " + out.version + " has no " + platform + " build.");
    return Result::ok();
}

Result fetchLatest (Release& out, const String& repository)
{
    int statusCode = 0;
    auto options = URL::InputStreamOptions (URL::ParameterHandling::inAddress)
                       .withExtraHeaders ("User-Agent: Lathe-Patcher\r\nAccept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28")
                       .withConnectionTimeoutMs (15000)
                       .withNumRedirectsToFollow (5)
                       .withStatusCode (&statusCode);

    // LATHE_UPDATE_URL points the patcher at another release feed (for testing).
    const auto feed = SystemStats::getEnvironmentVariable ("LATHE_UPDATE_URL",
                                                           "https://api.github.com/repos/" + repository + "/releases/latest");
    auto stream = URL (feed).createInputStream (options);
    if (stream == nullptr)
        return Result::fail ("Couldn't reach GitHub. Check your internet connection.");

    const auto text = stream->readEntireStreamAsString();
    if (statusCode == 404)
        return Result::fail ("No published release yet.");
    if (statusCode == 403 || statusCode == 429)
        return Result::fail ("GitHub's limit for anonymous requests was reached. Try again in an hour.");
    if (statusCode >= 400)
        return Result::fail ("GitHub answered " + String (statusCode) + ".");

    return parseRelease (JSON::parse (text), platformTag(), out);
}

Result download (const Release& r, const File& destination, const std::function<bool (double)>& progress)
{
    destination.getParentDirectory().createDirectory();
    destination.deleteFile();

    int statusCode = 0;
    auto options = URL::InputStreamOptions (URL::ParameterHandling::inAddress)
                       .withExtraHeaders ("User-Agent: Lathe-Patcher\r\nAccept: application/octet-stream")
                       .withConnectionTimeoutMs (20000)
                       .withNumRedirectsToFollow (10)
                       .withStatusCode (&statusCode);

    auto in = URL (r.assetUrl).createInputStream (options);
    if (in == nullptr || statusCode >= 400)
        return Result::fail ("Couldn't download " + r.assetName + (statusCode >= 400 ? " (HTTP " + String (statusCode) + ")." : "."));

    std::unique_ptr<FileOutputStream> out (destination.createOutputStream());
    if (out == nullptr || out->failedToOpen())
        return Result::fail ("Couldn't write " + destination.getFullPathName());

    HeapBlock<char> buffer (65536);
    int64 received = 0;
    const int64 total = r.assetSize > 0 ? r.assetSize : in->getTotalLength();
    for (;;)
    {
        const auto n = in->read (buffer, 65536);
        if (n <= 0)
            break;
        if (! out->write (buffer, (size_t) n))
            return Result::fail ("Couldn't write the download (is the disk full?).");
        received += n;
        if (progress && total > 0 && ! progress ((double) received / (double) total))
        {
            out.reset();
            destination.deleteFile();
            return Result::fail ("Cancelled.");
        }
    }
    out->flush();

    if (received == 0)
        return Result::fail ("The download was empty. Try again.");
    return Result::ok();
}

Result verify (const Release& r, const File& archive)
{
    if (r.assetSize > 0 && archive.getSize() != r.assetSize)
        return Result::fail ("The download was incomplete (" + String (archive.getSize()) + " of "
                             + String (r.assetSize) + " bytes). Try again.");

    if (r.sha256.isNotEmpty() && SHA256 (archive).toHexString().toLowerCase() != r.sha256)
        return Result::fail ("The download doesn't match GitHub's checksum. Try again.");

    return Result::ok();
}

Result unpack (const File& archive, const File& destinationDir)
{
    destinationDir.deleteRecursively();
    if (! destinationDir.createDirectory())
        return Result::fail ("Couldn't create " + destinationDir.getFullPathName());

    const auto name = archive.getFileName();
    if (name.endsWithIgnoreCase (".tar.gz") || name.endsWithIgnoreCase (".tgz"))
        return run ({ "tar", "-xzf", archive.getFullPathName(), "-C", destinationDir.getFullPathName() }, 300000);

    if (! name.endsWithIgnoreCase (".zip"))
        return Result::fail ("Unknown archive type: " + name);

   #if JUCE_MAC
    // ditto restores everything the app bundle needs, signature included.
    return run ({ "/usr/bin/ditto", "-x", "-k", archive.getFullPathName(), destinationDir.getFullPathName() }, 300000);
   #else
    ZipFile zip (archive);
    if (zip.getNumEntries() == 0)
        return Result::fail ("The download isn't a readable zip file.");
    if (auto r = zip.uncompressTo (destinationDir, true); r.failed())
        return Result::fail ("Couldn't unpack: " + r.getErrorMessage());

    #if ! JUCE_WINDOWS
    // ZipFile doesn't restore Unix permissions; the executables need theirs.
    for (int i = 0; i < zip.getNumEntries(); ++i)
        if (auto* entry = zip.getEntry (i))
            if (((entry->externalFileAttributes >> 16) & 0111) != 0)
                destinationDir.getChildFile (entry->filename).setExecutePermission (true);
    #endif
    return Result::ok();
   #endif
}

//==============================================================================
Layout defaultLayout()
{
   #if JUCE_MAC
    return Layout::bundle;
   #else
    return Layout::folder;
   #endif
}

bool isInstall (const File& f, Layout layout)
{
    if (layout == Layout::bundle)
        return f.isDirectory() && f.getChildFile ("Contents").getChildFile ("MacOS").getChildFile ("Lathe").existsAsFile();
    return f.isDirectory() && f.getChildFile (executableName()).existsAsFile();
}

File findInstall()
{
   #if JUCE_MAC
    // Normally .../Lathe.app/Contents/Helpers/LathePatcher.app
    const auto self = File::getSpecialLocation (File::currentApplicationFile);
    const auto outer = self.getParentDirectory().getParentDirectory().getParentDirectory();
    if (isInstall (outer, Layout::bundle))
        return outer;
    for (auto f : { self.getSiblingFile ("Lathe.app"), File ("/Applications/Lathe.app") })
        if (isInstall (f, Layout::bundle))
            return f;
    return {};
   #else
    const auto dir = File::getSpecialLocation (File::currentExecutableFile).getParentDirectory();
    return isInstall (dir, Layout::folder) ? dir : File();
   #endif
}

File stagingDirFor (const File& install, Layout layout)
{
    return layout == Layout::bundle ? install.getSiblingFile (".lathe-update")
                                    : install.getChildFile (".lathe-update");
}

Result install (const File& unpackedDir, const File& installLocation, Layout layout)
{
    if (layout == Layout::bundle)
    {
        const auto newApp = shallowest (unpackedDir, File::findDirectories, "Lathe.app");
        if (! isInstall (newApp, Layout::bundle))
            return Result::fail ("The download doesn't contain Lathe.app.");

        const auto aside = installLocation.getSiblingFile ("." + installLocation.getFileName() + ".old");
        aside.deleteRecursively();
        if (! installLocation.moveFileTo (aside))
            return Result::fail ("Couldn't replace " + installLocation.getFullPathName()
                                 + ". Check that you can write to " + installLocation.getParentDirectory().getFullPathName() + ".");

        if (! newApp.moveFileTo (installLocation))
        {
            aside.moveFileTo (installLocation);   // put the old one back
            return Result::fail ("Couldn't move the new Lathe.app into place. The old one is untouched.");
        }

        aside.deleteRecursively();
        return Result::ok();
    }

    // Folder layout: the archive holds a "Lathe-<version>-<platform>" folder.
    const auto exe = shallowest (unpackedDir, File::findFiles, executableName());
    if (! exe.existsAsFile())
        return Result::fail ("The download doesn't contain " + executableName() + ".");
    const auto root = exe.getParentDirectory();

    struct Moved { File target, aside; };
    std::vector<Moved> done;
    auto rollback = [&]
    {
        for (auto it = done.rbegin(); it != done.rend(); ++it)
        {
            it->target.deleteRecursively();
            if (it->aside != File())
                it->aside.moveFileTo (it->target);
        }
    };

    for (auto& f : root.findChildFiles (File::findFilesAndDirectories, false))
    {
        Moved m { installLocation.getChildFile (f.getFileName()), {} };

        if (m.target.exists())
        {
            // Renaming works even for a running .exe; overwriting it wouldn't.
            m.aside = m.target.getSiblingFile (m.target.getFileName() + ".old");
            if (m.aside.exists() && ! m.aside.deleteRecursively())
                m.aside = m.aside.getNonexistentSibling (false);
            if (! m.target.moveFileTo (m.aside))
            {
                rollback();
                return Result::fail ("Couldn't replace " + m.target.getFileName() + ". Close it and try again.");
            }
        }

        if (! f.moveFileTo (m.target))
        {
            if (m.aside != File())
                m.aside.moveFileTo (m.target);
            rollback();
            return Result::fail ("Couldn't copy " + f.getFileName() + " into " + installLocation.getFullPathName() + ". Nothing was changed.");
        }
        done.push_back (m);
    }

    for (auto& m : done)
        if (m.aside != File())
            m.aside.deleteRecursively();   // fails quietly for files still running; cleaned up next time
    return Result::ok();
}

void cleanUpOldFiles (const File& installLocation, Layout layout)
{
    if (layout == Layout::bundle)
    {
        installLocation.getSiblingFile ("." + installLocation.getFileName() + ".old").deleteRecursively();
        installLocation.getSiblingFile (".lathe-update").deleteRecursively();
        return;
    }

    for (auto& f : installLocation.findChildFiles (File::findFilesAndDirectories, false, "*.old"))
        f.deleteRecursively();
}
} // namespace lathe::updater
