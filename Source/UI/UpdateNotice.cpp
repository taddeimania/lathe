#include "UpdateNotice.h"
#include "../Patcher/Updater.h"

namespace lathe
{
namespace
{
constexpr int kFirstCheckMs = 10 * 1000;              // after start-up settles
constexpr int kCheckEveryMs = 12 * 60 * 60 * 1000;    // twice a day
} // namespace

UpdateNotice::UpdateNotice (PropertiesFile& s, std::function<void()> open)
    : settings (s), openPatcher (std::move (open))
{
    setFont (theme::font (12.0f));
    setColour (Label::textColourId, theme::accent);
    setJustificationType (Justification::centredRight);
    setMouseCursor (MouseCursor::PointingHandCursor);
    setVisible (false);

    if (checksAutomatically())
        startTimer (kFirstCheckMs);
}

UpdateNotice::~UpdateNotice()
{
    stopTimer();
}

bool UpdateNotice::checksAutomatically() const
{
    return settings.getBoolValue ("autoUpdateCheck", true);
}

void UpdateNotice::setChecksAutomatically (bool on)
{
    settings.setValue ("autoUpdateCheck", on);
    if (on)
    {
        startTimer (kFirstCheckMs / 5);
    }
    else
    {
        stopTimer();
        if (isVisible())
        {
            setVisible (false);
            if (onShowOrHide)
                onShowOrHide();
        }
    }
}

void UpdateNotice::timerCallback()
{
    startTimer (kCheckEveryMs);
    check();
}

void UpdateNotice::check()
{
    if (checking)
        return;
    checking = true;

    Component::SafePointer<UpdateNotice> safe (this);
    Thread::launch ([safe]
    {
        updater::Release release;
        const auto result = updater::fetchLatest (release);
        MessageManager::callAsync ([safe, result, release]
        {
            if (safe == nullptr)
                return;
            safe->checking = false;
            // Offline, rate-limited, no release yet: say nothing, try again later.
            if (result.failed())
                return;

            const String current (JUCE_APPLICATION_VERSION_STRING);
            const bool newer = updater::compareVersions (release.version, current) > 0;
            const bool skipped = release.version == safe->settings.getValue ("skipUpdateVersion");
            if (newer && ! skipped)
                safe->show (release.version, release.pageUrl, release.notes);
            else if (safe->isVisible())
            {
                safe->setVisible (false);
                if (safe->onShowOrHide)
                    safe->onShowOrHide();
            }
        });
    });
}

void UpdateNotice::show (const String& version, const String& pageUrl, const String& notes)
{
    latest = version;
    page = pageUrl;
    setText ("Update available: Lathe " + version, dontSendNotification);

    String tip = "You have " + String (JUCE_APPLICATION_VERSION_STRING) + ". Click to update with Lathe Patcher; right-click for more.";
    if (notes.isNotEmpty())
        tip << "\n\n" << notes.substring (0, 600) << (notes.length() > 600 ? "..." : "");
    setTooltip (tip);

    if (! isVisible())
    {
        setVisible (true);
        if (onShowOrHide)
            onShowOrHide();
    }
}

void UpdateNotice::mouseUp (const MouseEvent& e)
{
    if (! e.mouseWasClicked())
        return;

    if (! e.mods.isPopupMenu())
    {
        if (openPatcher)
            openPatcher();
        return;
    }

    PopupMenu m;
    m.addItem (1, "Update Now...");
    m.addItem (2, "What's New in " + latest + "...", page.isNotEmpty());
    m.addSeparator();
    m.addItem (3, "Skip Lathe " + latest);
    m.addItem (4, "Don't Check for Updates Automatically");

    Component::SafePointer<UpdateNotice> safe (this);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (this), [safe] (int r)
    {
        if (safe == nullptr || r == 0)
            return;
        if (r == 1 && safe->openPatcher)
            safe->openPatcher();
        else if (r == 2)
            URL (safe->page).launchInDefaultBrowser();
        else if (r == 3)
        {
            safe->settings.setValue ("skipUpdateVersion", safe->latest);
            safe->setVisible (false);
            if (safe->onShowOrHide)
                safe->onShowOrHide();
        }
        else if (r == 4)
        {
            safe->setChecksAutomatically (false);
        }
    });
}
} // namespace lathe
