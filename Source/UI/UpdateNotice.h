#pragma once

#include "Widgets.h"

namespace lathe
{
/** A quiet "Update available" in the status bar. Lathe asks GitHub for the latest
    release a little after it starts and then twice a day; if there's a newer one,
    this appears (and nothing else happens: no pop-ups). Click it to update with
    Lathe Patcher; right-click to skip that version or turn the checks off. */
class UpdateNotice : public Label, private Timer
{
public:
    UpdateNotice (PropertiesFile&, std::function<void()> openPatcher);
    ~UpdateNotice() override;

    bool checksAutomatically() const;
    void setChecksAutomatically (bool);

    /** Called when the notice appears or goes away, so the status bar can make room. */
    std::function<void()> onShowOrHide;

    void mouseUp (const MouseEvent&) override;

private:
    void timerCallback() override;
    void check();
    void show (const String& version, const String& pageUrl, const String& notes);

    PropertiesFile& settings;
    std::function<void()> openPatcher;
    String latest, page;
    bool checking = false;
};
} // namespace lathe
