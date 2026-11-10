#include "PluginHost.h"
#include "../Devices/Device.h"

namespace lathe
{
//==============================================================================
/** Describes plug-ins by running LatheScanner in a child process. */
class PluginHost::OutOfProcessScanner : public KnownPluginList::CustomScanner
{
public:
    explicit OutOfProcessScanner (PluginHost& h) : host (h) {}

    bool findPluginTypesFor (AudioPluginFormat& format, OwnedArray<PluginDescription>& result, const String& fileOrIdentifier) override
    {
        const auto exe = getScannerExecutable();
        if (! exe.existsAsFile())
            return scanInProcess (format, result, fileOrIdentifier);

        const auto output = File::createTempFile (".lathe-scan.xml");
        ChildProcess child;
        if (! child.start (StringArray { exe.getFullPathName(), format.getName(), fileOrIdentifier, output.getFullPathName() }, 0))
            return scanInProcess (format, result, fileOrIdentifier);

        // Plug-ins with copy protection can take a while the first time; a minute is generous.
        const auto started = Time::getMillisecondCounter();
        while (child.isRunning())
        {
            if (host.abortScan.load() || shouldExit())
            {
                child.kill();
                output.deleteFile();
                return true;   // cancelled, not the plug-in's fault: don't blacklist
            }

            if ((int) (Time::getMillisecondCounter() - started) > host.scanTimeoutMs)
            {
                child.kill();
                output.deleteFile();
                host.recordProblem (fileOrIdentifier, "stopped responding while being scanned");
                return false;
            }

            Thread::sleep (15);
        }

        const auto exitCode = child.getExitCode();
        auto xml = output.existsAsFile() ? parseXML (output) : nullptr;
        output.deleteFile();

        if (exitCode != 0 || xml == nullptr)
        {
            host.recordProblem (fileOrIdentifier, exitCode == 3 ? "isn't a supported format" : "crashed while being scanned");
            return false;
        }

        for (auto* e : xml->getChildIterator())
        {
            auto d = std::make_unique<PluginDescription>();
            if (d->loadFromXml (*e))
                result.add (d.release());
        }
        return true;
    }

private:
    // Fallback when the helper isn't there (e.g. a build that didn't copy it).
    static bool scanInProcess (AudioPluginFormat& format, OwnedArray<PluginDescription>& result, const String& file)
    {
        auto doScan = [&] { format.findAllTypesForFile (result, file); };
        if (MessageManager::getInstance()->isThisTheMessageThread())
            doScan();
        else
            MessageManager::callSync (doScan);
        return true;
    }

    PluginHost& host;
};

//==============================================================================
class PluginHost::ScanThread : public Thread
{
public:
    explicit ScanThread (PluginHost& h) : Thread ("Lathe plug-in scan"), host (h) {}

    void run() override
    {
        WeakReference<PluginHost> weak (&host);

        while (! threadShouldExit() && ! host.abortScan.load())
        {
            {
                const ScopedLock sl (host.statusLock);
                host.currentFile = host.scanner->getNextPluginFileThatWillBeScanned();
                host.progress = host.scanner->getProgress();
            }

            MessageManager::callAsync ([weak]
            {
                if (auto* h = weak.get(); h != nullptr && h->onScanProgress)
                    h->onScanProgress();
            });

            String name;
            if (! host.scanner->scanNextFile (true, name))
                break;
        }

        MessageManager::callAsync ([weak]
        {
            if (auto* h = weak.get())
                h->finishScan();
        });
    }

private:
    PluginHost& host;
};

//==============================================================================
PluginHost::PluginHost (PropertiesFile& settings)
    : props (settings)
{
    formats.addDefaultFormats();

    auto dir = settings.getFile().getParentDirectory();
    listFile = dir.getChildFile ("Lathe Plugins.xml");
    deadMansPedal = dir.getChildFile ("Lathe Plugin Scan.txt");

    if (auto xml = parseXML (listFile))
        known.recreateFromXml (*xml);

    if (auto saved = props.getXmlValue ("pluginScanProblems"))
        for (auto* e : saved->getChildIterator())
            problems.set (e->getStringAttribute ("file"), e->getStringAttribute ("reason"));

    known.setCustomScanner (std::make_unique<OutOfProcessScanner> (*this));
}

PluginHost::~PluginHost()
{
    abortScan = true;
    if (scanThread != nullptr)
        scanThread->stopThread (5000);
    scanThread.reset();
    scanner.reset();
}

File PluginHost::getScannerExecutable()
{
    const auto exe = File::getSpecialLocation (File::currentExecutableFile);
   #if JUCE_WINDOWS
    return exe.getSiblingFile ("LatheScanner.exe");
   #else
    return exe.getSiblingFile ("LatheScanner");
   #endif
}

void PluginHost::saveList()
{
    if (auto xml = known.createXml())
        xml->writeTo (listFile);

    XmlElement p ("PROBLEMS");
    {
        const ScopedLock sl (statusLock);
        for (auto& key : problems.getAllKeys())
        {
            auto* e = p.createNewChildElement ("PLUGIN");
            e->setAttribute ("file", key);
            e->setAttribute ("reason", problems[key]);
        }
    }
    props.setValue ("pluginScanProblems", &p);
    props.saveIfNeeded();
}

void PluginHost::recordProblem (const String& file, const String& reason)
{
    const ScopedLock sl (statusLock);
    problems.set (file, reason);
}

StringArray PluginHost::getSkippedPlugins() const
{
    StringArray out;
    const ScopedLock sl (statusLock);
    for (auto& file : known.getBlacklistedFiles())
    {
        const auto reason = problems[file];
        out.add (File::createFileWithoutCheckingPath (file).getFileNameWithoutExtension()
                 + (reason.isNotEmpty() ? " (" + reason + ")" : String()));
    }
    return out;
}

void PluginHost::unblacklist (const String& file)
{
    known.removeFromBlacklist (file);
    const ScopedLock sl (statusLock);
    problems.remove (StringRef (file));
}

std::unique_ptr<AudioPluginInstance> PluginHost::create (const PluginDescription& desc, double sampleRate, int blockSize, String& error)
{
    if (devices::isBuiltIn (desc))
    {
        auto device = devices::create (desc);
        if (device == nullptr)
            error = desc.name + " isn't in this version of Lathe";
        return device;
    }

    auto instance = formats.createPluginInstance (desc, sampleRate, blockSize, error);
    if (instance == nullptr)
        return {};

    // Ask for plain stereo on the main buses; anything else keeps the plug-in's default.
    auto layout = instance->getBusesLayout();
    if (! layout.inputBuses.isEmpty())
        layout.inputBuses.getReference (0) = AudioChannelSet::stereo();
    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference (0) = AudioChannelSet::stereo();

    if (instance->checkBusesLayoutSupported (layout))
        instance->setBusesLayout (layout);

    return instance;
}

Array<PluginDescription> PluginHost::allTypes() const
{
    Array<PluginDescription> types;
    for (auto& info : devices::all())
        types.add (devices::describe (info));
    types.addArray (known.getTypes());
    return types;
}

std::optional<PluginDescription> PluginHost::resolve (const PluginDescription& saved) const
{
    if (devices::isBuiltIn (saved))
    {
        if (auto* info = devices::find (saved))
            return devices::describe (*info);
        return std::nullopt;
    }

    if (auto exact = known.getTypeForIdentifierString (saved.createIdentifierString()))
        return *exact;

    for (auto& d : known.getTypes())
        if (d.uniqueId == saved.uniqueId && d.name == saved.name)
            return d;

    for (auto& d : known.getTypes())
        if (d.name == saved.name && d.manufacturerName == saved.manufacturerName)
            return d;

    // Never hand back a plug-in that crashed the scanner, even if the file is there.
    if (saved.fileOrIdentifier.isNotEmpty() && File::isAbsolutePath (saved.fileOrIdentifier)
         && File (saved.fileOrIdentifier).exists() && ! known.getBlacklistedFiles().contains (saved.fileOrIdentifier))
        return saved;

    return std::nullopt;
}

bool PluginHost::scanFile (const File& bundle, OwnedArray<PluginDescription>& found)
{
    for (auto* format : formats.getFormats())
    {
        if (format->getName() != "VST3")
            continue;

        // Forget the old version's entry so the new one's details replace it.
        for (auto& d : known.getTypes())
            if (File::createFileWithoutCheckingPath (d.fileOrIdentifier) == bundle)
                known.removeType (d);

        unblacklist (bundle.getFullPathName());
        known.scanAndAddFile (bundle.getFullPathName(), false, found, *format);
        saveList();
    }
    return ! found.isEmpty();
}

//==============================================================================
StringArray PluginHost::getExtraFolders() const
{
    return StringArray::fromTokens (props.getValue ("pluginFolders"), ";", "");
}

void PluginHost::setExtraFolders (const StringArray& folders)
{
    props.setValue ("pluginFolders", folders.joinIntoString (";"));
}

void PluginHost::startScan (bool clearFirst)
{
    AudioPluginFormat* vst3 = nullptr;
    for (auto* f : formats.getFormats())
        if (f->getName() == "VST3")
            vst3 = f;

    if (vst3 == nullptr || scanning.load())
        return;

    if (clearFirst)
    {
        known.clear();
        known.clearBlacklistedFiles();
        const ScopedLock sl (statusLock);
        problems.clear();
    }

    // Only matters for the in-process fallback: if Lathe itself crashed on a
    // plug-in last time, that file is blacklisted now.
    PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal (known, deadMansPedal);

    auto paths = vst3->getDefaultLocationsToSearch();
    for (auto& f : getExtraFolders())
        if (File::isAbsolutePath (f.trim()) && File (f.trim()).isDirectory())
            paths.addIfNotAlreadyThere (File (f.trim()));

    scanner = std::make_unique<PluginDirectoryScanner> (known, *vst3, paths, true, deadMansPedal, false);
    abortScan = false;
    scanning = true;
    progress = 0.0f;
    scanThread = std::make_unique<ScanThread> (*this);
    scanThread->startThread (Thread::Priority::low);
}

String PluginHost::getScanStatus() const
{
    if (! scanning.load())
        return {};

    const ScopedLock sl (statusLock);
    return "Scanning plug-ins " + String (roundToInt (progress * 100.0f)) + "%  "
           + File::createFileWithoutCheckingPath (currentFile).getFileNameWithoutExtension();
}

void PluginHost::finishScan()
{
    if (scanThread != nullptr)
        scanThread->stopThread (1000);
    scanThread.reset();
    scanner.reset();
    {
        const ScopedLock sl (statusLock);
        currentFile.clear();
    }
    deadMansPedal.deleteFile();
    saveList();
    scanning = false;

    if (onScanFinished)
        onScanFinished();
}

//==============================================================================
PopupMenu PluginHost::buildMenu (bool instrumentsFirst, std::vector<PluginDescription>& results) const
{
    results.clear();

    auto types = known.getTypes();
    std::sort (types.begin(), types.end(), [] (const PluginDescription& a, const PluginDescription& b)
    {
        if (a.manufacturerName != b.manufacturerName)
            return a.manufacturerName.compareIgnoreCase (b.manufacturerName) < 0;
        return a.name.compareIgnoreCase (b.name) < 0;
    });

    auto makeGroup = [&] (bool instruments)
    {
        PopupMenu menu;
        PopupMenu currentMaker;
        String currentName;

        // Lathe's own first, then the installed plug-ins by maker.
        for (auto& info : devices::all())
        {
            if ((info.kind == devices::Kind::instrument) != instruments)
                continue;
            results.push_back (devices::describe (info));
            menu.addItem ((int) results.size(), info.name);
        }
        if (menu.getNumItems() > 0 && ! types.isEmpty())
            menu.addSeparator();

        auto flush = [&]
        {
            if (currentMaker.getNumItems() > 0)
                menu.addSubMenu (currentName.isNotEmpty() ? currentName : String ("Unknown"), currentMaker);
            currentMaker = PopupMenu();
        };

        for (auto& t : types)
        {
            if (t.isInstrument != instruments)
                continue;

            if (t.manufacturerName != currentName)
            {
                flush();
                currentName = t.manufacturerName;
            }

            results.push_back (t);
            currentMaker.addItem ((int) results.size(), t.name);
        }
        flush();
        return menu;
    };

    auto instruments = makeGroup (true);
    auto effects = makeGroup (false);

    PopupMenu menu;
    if (instrumentsFirst)
    {
        menu.addSubMenu ("Instruments", instruments, instruments.getNumItems() > 0);
        menu.addSubMenu ("Audio Effects", effects, effects.getNumItems() > 0);
    }
    else
    {
        menu.addSubMenu ("Audio Effects", effects, effects.getNumItems() > 0);
        menu.addSubMenu ("Instruments", instruments, instruments.getNumItems() > 0);
    }

    return menu;
}
} // namespace lathe
