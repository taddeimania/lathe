#pragma once

#include "PluginHost.h"

namespace lathe
{
/** One plug-in published as GitHub releases. */
struct StoreEntry
{
    // --- catalog (Plugin Catalog.json)
    String repo;            // "owner/name"
    String name;            // product name, also the bundle name: <name>.vst3
    String kind;            // "Effect", "Instrument" or "MIDI"
    String description;

    // --- latest published release for this platform
    String version, tag, releaseUrl, notes, publishedAt;
    String assetName, assetUrl;
    int64 assetSize = 0;
    String fetchError;

    // --- this machine
    String installedVersion;     // empty = not installed
    File installedBundle;
    bool pendingRestart = false; // an update is waiting for the next launch

    // --- an install in progress (UI only)
    bool busy = false;
    double progress = 0.0;       // read by a ProgressBar on the message thread

    bool isInstalled() const    { return installedVersion.isNotEmpty() || installedBundle.exists(); }
    bool hasRelease() const     { return assetUrl.isNotEmpty(); }
    bool updateAvailable() const;
};

/** Finds, downloads and installs the user's own plug-ins from GitHub releases.
    Network and disk work happens on a background pool; everything in `entries`
    is only touched on the message thread. */
class PluginStore
{
public:
    PluginStore (PluginHost&, const File& dataDirectory);
    ~PluginStore();

    std::vector<StoreEntry> entries;
    std::function<void()> onChanged;

    /** Looks up the latest release of every catalog entry (cached for an hour unless forced). */
    void refresh (bool force);
    bool isRefreshing() const noexcept     { return refreshing; }
    String getStatus() const               { return status; }
    StoreEntry* find (const String& repo);   // repo names are always passed by value elsewhere: rows get rebuilt

    /** Downloads and installs the latest release. `done` runs on the message thread
        with the installed bundle (already scanned into the plug-in list). */
    using InstallDone = std::function<void (const Result&, const PluginDescription*)>;
    void install (String repo, InstallDone done);

    /** Adds an "owner/repo" to the catalog and looks it up. */
    void addRepository (const String& ownerSlashRepo);

    /** Re-reads what's installed (install records, the plug-in list, the VST3 folder). */
    void updateInstalledState();

    /** The matching scanned plug-in, if it's installed and scanned. */
    std::optional<PluginDescription> findDescription (const StoreEntry&) const;

    // --- static helpers (also used by the tests)
    static File getUserVst3Dir();
    static String platformTag();   // "macOS", "Windows" or "Linux": the release asset suffix
    /** Finishes updates staged last session. Call before any plug-in loads. Returns the bundles replaced. */
    static Array<File> applyPendingInstalls (const File& dataDirectory);
    static void parseRelease (const var& releaseJson, StoreEntry&);
    static int compareVersions (const String& a, const String& b);
    /** Unpacks a release archive and puts the .vst3 into vst3Dir. If the old copy is
        locked (Windows, plug-in loaded), the new one is staged for the next launch. */
    static Result installArchive (const File& archive, const String& expectedName, const File& vst3Dir,
                                  const File& dataDirectory, File& installedBundle, bool& pendingRestart);
    static std::vector<StoreEntry> defaultCatalog();

    File vst3DirOverride;   // tests install somewhere harmless

private:
    File catalogFile() const    { return getUserDir().getChildFile ("Plugin Catalog.json"); }
    File cacheFile() const      { return dataDir.getChildFile ("GitHub Releases Cache.json"); }
    File recordFile() const     { return dataDir.getChildFile ("Installed Plugins.json"); }
    File vst3Dir() const        { return vst3DirOverride != File() ? vst3DirOverride : getUserVst3Dir(); }

    void loadCatalog();
    void saveCatalog();
    void notify()               { if (onChanged) onChanged(); }

    PluginHost& host;
    File dataDir;
    ThreadPool pool { 2 };
    bool refreshing = false;
    String status;
    var cache;   // repo -> { "fetched": ms, "release": json }

    JUCE_DECLARE_WEAK_REFERENCEABLE (PluginStore)
    JUCE_DECLARE_NON_COPYABLE (PluginStore)
};
} // namespace lathe
