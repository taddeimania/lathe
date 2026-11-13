#include "CrashReports.h"
#include "CloudClient.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <dlfcn.h>
 #include <execinfo.h>
 #include <csignal>
#endif
#include <cstdio>

#ifndef JUCE_APPLICATION_VERSION_STRING
 #define JUCE_APPLICATION_VERSION_STRING "0.0.0"   // the tests
#endif

namespace lathe::crashes
{
namespace
{
File& folderRef()
{
    static File f;
    return f;
}

std::function<void (const var&)>& testSender()
{
    static std::function<void (const var&)> s;
    return s;
}

const double startedAt = Time::getMillisecondCounterHiRes();
constexpr int maxReport = 16 * 1024;

String systemName()
{
   #if JUCE_MAC
    return "macos";
   #elif JUCE_WINDOWS
    return "windows";
   #else
    return "linux";
   #endif
}

/** Each step of the stack as "<module> + 0x<offset> (<symbol>)": the module's own file name
    and how far into it, which matches the release's binary whatever address it loaded at. */
StringArray frames()
{
    StringArray out;
   #if JUCE_WINDOWS
    void* stack[64];
    const auto n = (int) CaptureStackBackTrace (2, 64, stack, nullptr);
    for (int i = 0; i < n; ++i)
    {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH] = {};
        String name ("?");
        if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR) stack[i], &module) && module != nullptr
            && GetModuleFileNameW (module, path, MAX_PATH) > 0)
            name = File (String (path)).getFileName();
        const auto offset = (uint64) ((const char*) stack[i] - (const char*) module);
        out.add (name + " + 0x" + String::toHexString ((int64) offset));
    }
   #else
    void* stack[64];
    const auto n = backtrace (stack, 64);
    for (int i = 2; i < n; ++i)
    {
        Dl_info info {};
        if (dladdr (stack[i], &info) != 0 && info.dli_fbase != nullptr)
        {
            String line = File (String::fromUTF8 (info.dli_fname != nullptr ? info.dli_fname : "?")).getFileName()
                        + " + 0x" + String::toHexString ((int64) ((const char*) stack[i] - (const char*) info.dli_fbase));
            if (info.dli_sname != nullptr)
                line << " (" << String::fromUTF8 (info.dli_sname) << ")";
            out.add (line);
        }
        else
        {
            out.add ("? " + String::toHexString ((pointer_sized_int) stack[i]));
        }
    }
   #endif
    return out;
}

String describe (void* info)
{
   #if JUCE_WINDOWS
    if (auto* e = static_cast<EXCEPTION_POINTERS*> (info); e != nullptr && e->ExceptionRecord != nullptr)
        return "exception 0x" + String::toHexString ((int) e->ExceptionRecord->ExceptionCode);
    return "exception";
   #else
    const auto sig = (int) (pointer_sized_int) info;
    switch (sig)
    {
        case SIGSEGV: return "SIGSEGV (bad memory access)";
        case SIGBUS:  return "SIGBUS (bad memory access)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGFPE:  return "SIGFPE (arithmetic error)";
        default:      return "signal " + String (sig);
    }
   #endif
}

void handler (void* info)
{
    // Not much is safe in here, but the process is going anyway: write the report as plainly
    // as possible, then let the system's own crash handling run.
    const auto text = scrub (compose (describe (info), frames(), (Time::getMillisecondCounterHiRes() - startedAt) / 1000.0));
    const auto file = folderRef().getChildFile ("crash-" + String (Time::currentTimeMillis()) + ".txt");
    if (auto* f = std::fopen (file.getFullPathName().toRawUTF8(), "wb"))
    {
        std::fwrite (text.toRawUTF8(), 1, text.getNumBytesAsUTF8(), f);
        std::fclose (f);
    }
   #if ! JUCE_WINDOWS
    const auto sig = (int) (pointer_sized_int) info;
    std::signal (sig, SIG_DFL);
    std::raise (sig);
   #endif
}
} // namespace

String scrub (const String& text)
{
    auto out = text;
    const auto home = File::getSpecialLocation (File::userHomeDirectory).getFullPathName();
    if (home.length() > 1)
        out = out.replace (home, "~");
    const auto user = SystemStats::getLogonName();
    if (user.length() > 3)
        out = out.replace (user, "<user>", true);
    // Any remaining absolute path: keep only the last part.
    StringArray lines;
    lines.addLines (out);
    for (auto& l : lines)
        for (auto sep : { String ("~/"), String ("/Users/"), String ("/home/"), String ("C:\\Users\\"), String ("c:\\users\\") })
            if (l.contains (sep))
                l = l.upToFirstOccurrenceOf (sep, false, false) + "<path>";
    return lines.joinIntoString ("\n");
}

String compose (const String& what, const StringArray& steps, double secondsOpen)
{
    String r;
    r << "Lathe " << JUCE_APPLICATION_VERSION_STRING << " crashed: " << what << "\n"
      << SystemStats::getOperatingSystemName() << ", " << SystemStats::getCpuModel().trim()
      << ", " << SystemStats::getNumCpus() << " cores\n"
      << "Open for " << String (roundToInt (secondsOpen)) << " s\n\n";
    for (int i = 0; i < steps.size(); ++i)
        r << i << "  " << steps[i] << "\n";
    return r.substring (0, maxReport);
}

void install (const File& folder)
{
    folderRef() = folder;
    folder.createDirectory();
    SystemStats::setApplicationCrashHandler (handler);
}

File defaultFolder()
{
    return folderRef();
}

Array<File> pending (const File& folder)
{
    auto files = folder.findChildFiles (File::findFiles, false, "crash-*.txt");
    std::sort (files.begin(), files.end(), [] (const File& a, const File& b) { return a.getFileName() < b.getFileName(); });
    return files;
}

String reportText (const File& f)
{
    return scrub (f.loadFileAsString()).substring (0, maxReport);
}

Choice choice (PropertiesFile& s)
{
    const auto v = s.getValue ("crashReports");
    return v == "always" ? Choice::always : v == "never" ? Choice::never : Choice::ask;
}

void setChoice (PropertiesFile& s, Choice c)
{
    s.setValue ("crashReports", c == Choice::always ? "always" : c == Choice::never ? "never" : "ask");
    s.saveIfNeeded();
}

var payload (const String& report)
{
    auto* o = new DynamicObject();
    o->setProperty ("version", String (JUCE_APPLICATION_VERSION_STRING));
    o->setProperty ("system", systemName());
    o->setProperty ("report", report.substring (0, maxReport));
    return var (o);
}

void send (const File& report)
{
    const auto body = payload (reportText (report));
    report.deleteFile();
    if (auto& t = testSender())
    {
        t (body);
        return;
    }
    const auto json = JSON::toString (body, true);
    Thread::launch ([json]
    {
        MemoryBlock data (json.toRawUTF8(), json.getNumBytesAsUTF8());
        cloud::httpRequest ("POST", cloud::Config::load().apiUrl + "/crash", &data, "Content-Type: application/json", 15000);
    });
}

void dismiss (const File& report)
{
    report.deleteFile();
}

String captureForTests()
{
    return scrub (compose ("test", frames(), (Time::getMillisecondCounterHiRes() - startedAt) / 1000.0));
}

void setSenderForTests (std::function<void (const var&)> f)
{
    testSender() = std::move (f);
}
} // namespace lathe::crashes
