#pragma once

#include "../Common.h"
#include <optional>

namespace lathe
{
/** VST3 discovery and loading.

    Plug-ins are scanned out of process: each file is opened by the small
    LatheScanner helper that sits next to the Lathe executable. A plug-in that
    crashes or hangs while being scanned only takes the helper down; it's
    blacklisted and the scan carries on. Full scans run on a background thread,
    so the UI never freezes. If the helper is missing, scanning falls back to
    the old in-process way (on the message thread). */
class PluginHost
{
public:
    explicit PluginHost (PropertiesFile& settings);
    ~PluginHost();

    AudioPluginFormatManager formats;
    KnownPluginList known;

    std::unique_ptr<AudioPluginInstance> create (const PluginDescription&, double sampleRate, int blockSize, String& error);

    /** Lathe's built-in devices, then every scanned plug-in. */
    Array<PluginDescription> allTypes() const;

    /** Finds the installed plug-in matching one saved in a project (paths differ between machines).
        Lathe's built-in devices resolve to themselves. */
    std::optional<PluginDescription> resolve (const PluginDescription& saved) const;

    /** Scans one plug-in bundle right away (after installing or updating it). */
    bool scanFile (const File& bundle, OwnedArray<PluginDescription>& found);

    // --- scanning
    void startScan (bool clearFirst);
    bool isScanning() const noexcept        { return scanning.load(); }
    String getScanStatus() const;
    std::function<void()> onScanProgress;   // message thread
    std::function<void()> onScanFinished;   // message thread

    /** Plug-ins skipped because they crashed or hung while being scanned (names + reason). */
    StringArray getSkippedPlugins() const;
    /** Gives one skipped plug-in another chance on the next scan. */
    void unblacklist (const String& fileOrIdentifier);

    StringArray getExtraFolders() const;
    void setExtraFolders (const StringArray&);

    /** Builds a menu of installed plug-ins. Item ids are 1-based indexes into `results`. */
    PopupMenu buildMenu (bool instrumentsFirst, std::vector<PluginDescription>& results) const;

    /** How long one plug-in may take to scan before it's treated as hung. */
    int scanTimeoutMs = 60000;

    /** Where the out-of-process scanner lives (next to the Lathe executable). */
    static File getScannerExecutable();

private:
    class ScanThread;
    class OutOfProcessScanner;
    friend class ScanThread;
    friend class OutOfProcessScanner;

    void saveList();
    void finishScan();
    void recordProblem (const String& file, const String& reason);

    PropertiesFile& props;
    File listFile, deadMansPedal;

    std::unique_ptr<PluginDirectoryScanner> scanner;
    std::unique_ptr<Thread> scanThread;
    std::atomic<bool> scanning { false }, abortScan { false };

    mutable CriticalSection statusLock;
    String currentFile;
    float progress = 0.0f;
    StringPairArray problems;   // file -> reason, from this and earlier scans

    JUCE_DECLARE_WEAK_REFERENCEABLE (PluginHost)
};
} // namespace lathe
