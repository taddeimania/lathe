#pragma once

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <functional>

//==============================================================================
// The update logic behind Lathe Patcher: find the newest published release on
// GitHub, download this platform's build, and swap it in place of the installed
// one. No window code here, so the tests can drive it.
//==============================================================================
namespace lathe::updater
{
using namespace juce;

/** The GitHub repository releases come from. */
inline constexpr const char* kRepository = "taddeimania/lathe";

struct Release
{
    String version;      // "0.3.0"
    String tag;          // "v0.3.0"
    String notes;        // release text (markdown)
    String pageUrl;      // the release page, for "open in browser"
    String assetName, assetUrl;
    int64 assetSize = 0;
    String sha256;       // from GitHub's asset digest, when it gives one
};

/** "macOS", "Windows" or "Linux": the word in the release archive names. */
String platformTag();

/** -1, 0 or 1. Accepts a leading "v". */
int compareVersions (const String& a, const String& b);

/** Reads a GitHub release object. Fails if there's no build for the platform. */
Result parseRelease (const var& json, const String& platform, Release& out);

/** Asks GitHub for the latest published release (drafts and pre-releases are skipped). */
Result fetchLatest (Release& out, const String& repository = kRepository);

/** Downloads the release's archive. progress gets 0..1 and returns false to cancel. */
Result download (const Release&, const File& destination, const std::function<bool (double)>& progress);

/** Checks the size and, if GitHub gave one, the SHA-256 of a downloaded archive. */
Result verify (const Release&, const File& archive);

/** Unpacks a .zip (ditto on the Mac, so the signature survives) or .tar.gz. */
Result unpack (const File& archive, const File& destinationDir);

//==============================================================================
enum class Layout
{
    bundle,   // macOS: the install is Lathe.app, replaced whole
    folder    // Windows / Linux: the install is the folder holding Lathe(.exe)
};

Layout defaultLayout();

/** The installed Lathe this patcher belongs to: the Lathe.app it's inside (Mac),
    or the folder it sits in (Windows, Linux). Invalid File if it can't tell. */
File findInstall();

/** True if the path looks like a Lathe install for the layout. */
bool isInstall (const File&, Layout);

/** Where downloads and unpacking happen: beside the install, so moves are renames
    on the same disk (fast, and file permissions are kept). */
File stagingDirFor (const File& install, Layout);

/** Replaces the install with the release unpacked in unpackedDir.
    Bundle: the new Lathe.app takes the old one's place, the old one is deleted.
    Folder: every file in the release replaces its namesake; files that are in use
    (a running Lathe.exe, this patcher) are renamed to *.old first, which Windows
    allows, and cleaned up the next time. On failure the old files are put back. */
Result install (const File& unpackedDir, const File& installLocation, Layout);

/** Deletes *.old files a previous update left behind (ignores ones still in use). */
void cleanUpOldFiles (const File& installLocation, Layout);
} // namespace lathe::updater
