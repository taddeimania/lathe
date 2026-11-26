#pragma once

#include "Widgets.h"

namespace lathe
{
class ChannelStrip;
class MixBar;

class MixerView : public Component
{
public:
    explicit MixerView (AppContext&);
    ~MixerView() override;

    void refresh();
    void tick();
    void resized() override;
    void paint (Graphics&) override;

private:
    /** A viewport that says when it scrolls, so the master strip can follow the others up and down. */
    struct FollowingViewport : public Viewport
    {
        std::function<void()> onScroll;
        void visibleAreaChanged (const Rectangle<int>&) override   { if (onScroll) onScroll(); }
    };

    String structureSignature() const;

    AppContext& ctx;
    FollowingViewport viewport, masterViewport;
    Component content;
    OwnedArray<ChannelStrip> strips;
    std::unique_ptr<ChannelStrip> masterStrip;
    TextButton addReturnButton { "+ Return" };
    std::unique_ptr<MixBar> mixBar;
    String signature;
};
} // namespace lathe
