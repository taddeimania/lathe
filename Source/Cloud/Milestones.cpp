#include "Milestones.h"
#include "CloudClient.h"

#ifndef JUCE_APPLICATION_VERSION_STRING
 #define JUCE_APPLICATION_VERSION_STRING "0.0.0"   // the tests
#endif

namespace lathe::milestones
{
namespace
{
constexpr auto kChoice   = "shareMilestones";     // "yes" / "no"; missing: not asked yet
constexpr auto kCohort   = "milestonesCohort";    // "new" / "existing"
constexpr auto kFirstDay = "firstLaunchDay";      // yyyy-mm-dd, local
constexpr auto kNew      = "installIsNew";
constexpr auto kSent     = "milestonesSent";      // the names already sent, comma-separated
constexpr int  kComeBackDays = 7;

std::function<void (const var&)>& testSender()
{
    static std::function<void (const var&)> s;
    return s;
}

String today()
{
    return Time::getCurrentTime().formatted ("%Y-%m-%d");
}

String system()
{
   #if JUCE_MAC
    return "macos";
   #elif JUCE_WINDOWS
    return "windows";
   #else
    return "linux";
   #endif
}

StringArray sent (PropertiesFile& s)
{
    return StringArray::fromTokens (s.getValue (kSent), ",", "");
}

void send (const var& body)
{
    if (auto& t = testSender())
    {
        t (body);
        return;
    }
    // One try, in the background, short timeout. If it fails it's simply not counted.
    const auto json = JSON::toString (body, true);
    Thread::launch ([json]
    {
        MemoryBlock data (json.toRawUTF8(), json.getNumBytesAsUTF8());
        cloud::httpRequest ("POST", cloud::Config::load().apiUrl + "/hit", &data,
                            "Content-Type: application/json", 8000);
    });
}
} // namespace

void sendOnce (PropertiesFile& s, const String& name)
{
    if (choice (s) != Choice::yes || ! names().contains (name))
        return;
    auto done = sent (s);
    if (done.contains (name))
        return;
    // Marked before sending: at most once, even if this one doesn't get through.
    done.add (name);
    s.setValue (kSent, done.joinIntoString (","));
    s.saveIfNeeded();
    send (payload (s, name));
}

const StringArray& names()
{
    static const StringArray n { "launched", "interface", "played", "recorded", "exported", "signed_in", "pushed", "came_back" };
    return n;
}

String describe (const String& name)
{
    if (name == "launched")  return "You opened Lathe for the first time";
    if (name == "interface") return "Lathe found an audio interface with inputs";
    if (name == "played")    return "You pressed play";
    if (name == "recorded")  return "You recorded something";
    if (name == "exported")  return "You exported a mix";
    if (name == "signed_in") return "You signed in to Lathe Cloud";
    if (name == "pushed")    return "You pushed a song to Lathe Cloud";
    if (name == "came_back") return "You opened Lathe again, a week or more after the first time";
    return {};
}

Choice choice (PropertiesFile& s)
{
    const auto c = s.getValue (kChoice);
    return c == "yes" ? Choice::yes : c == "no" ? Choice::no : Choice::notAsked;
}

void markNeverRunBefore (PropertiesFile& s)
{
    s.setValue ("askMilestones", true);
}

bool shouldAsk (PropertiesFile& s)
{
    return s.getBoolValue ("askMilestones", false) && choice (s) == Choice::notAsked;
}

void startup (PropertiesFile& s, bool newInstall)
{
    if (! s.containsKey (kFirstDay))
    {
        s.setValue (kFirstDay, today());
        s.setValue (kNew, newInstall);
        return;
    }

    const auto first = s.getValue (kFirstDay);
    const Time firstTime (first.substring (0, 4).getIntValue(), first.substring (5, 7).getIntValue() - 1,
                          first.substring (8, 10).getIntValue(), 0, 0);
    if ((Time::getCurrentTime() - firstTime).inDays() >= kComeBackDays)
        sendOnce (s, "came_back");
}

void setChoice (PropertiesFile& s, bool share, bool fromWelcome)
{
    s.setValue (kChoice, share ? "yes" : "no");
    if (share && ! s.containsKey (kCohort))
        s.setValue (kCohort, fromWelcome && s.getBoolValue (kNew, false) ? "new" : "existing");
    s.saveIfNeeded();
    if (share)
        sendOnce (s, "launched");
}

var payload (PropertiesFile& s, const String& name)
{
    auto* o = new DynamicObject();
    o->setProperty ("kind", "app");
    o->setProperty ("milestone", name);
    o->setProperty ("cohort", s.getValue (kCohort, "existing"));
    o->setProperty ("system", system());
    o->setProperty ("version", String (JUCE_APPLICATION_VERSION_STRING));
    return var (o);
}

void reach (PropertiesFile& s, const String& name)
{
    if (name != "came_back")      // that one is startup()'s to decide
        sendOnce (s, name);
}

void setSenderForTests (std::function<void (const var&)> f)
{
    testSender() = std::move (f);
}
} // namespace lathe::milestones
