// LatheScanner: describes one plug-in file, in its own process.
//
//   LatheScanner <format> <plug-in path> <output.xml>
//
// Lathe runs this once per plug-in while scanning. Loading an unknown plug-in
// can crash or hang (VCV Rack, for one, crashes when it's unloaded right after
// loading), and when that happens here it only takes this helper down: Lathe
// sees the failed exit, skips that plug-in, and carries on.
//
// Exit codes: 0 = wrote the output file, 2 = bad arguments, 3 = unknown format,
// 4 = couldn't write the output. A crash or timeout means "skip this plug-in".

#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdlib>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

using namespace juce;

static int scan (const StringArray& args)
{
    if (args.size() < 3)
        return 2;

    ScopedJuceInitialiser_GUI juceInit;

    AudioPluginFormatManager formats;
    formats.addDefaultFormats();

    AudioPluginFormat* format = nullptr;
    for (auto* f : formats.getFormats())
        if (f->getName() == args[0])
            format = f;

    if (format == nullptr)
        return 3;

    OwnedArray<PluginDescription> found;
    format->findAllTypesForFile (found, args[1]);

    XmlElement root ("PLUGINS");
    for (auto* d : found)
        root.addChildElement (d->createXml().release());

    if (! root.writeTo (File (args[2])))
        return 4;

    return 0;
}

#if JUCE_WINDOWS
int wmain (int argc, wchar_t* argv[])
{
    // A crashing plug-in should just end this process, not pop up an error dialog
    // (which would also leave the scan waiting until it times out).
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (String (argv[i]));

    // Skip static teardown: some plug-ins misbehave when unloaded at exit.
    std::_Exit (scan (args));
}
#else
int main (int argc, char* argv[])
{
    StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (String::fromUTF8 (argv[i]));

    std::_Exit (scan (args));
}
#endif
