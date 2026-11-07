#pragma once

#include "Widgets.h"

namespace lathe
{
class ChannelStrip;

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
    String structureSignature() const;

    AppContext& ctx;
    Viewport viewport;
    Component content;
    OwnedArray<ChannelStrip> strips;
    std::unique_ptr<ChannelStrip> masterStrip;
    TextButton addReturnButton { "+ Return" };
    String signature;
};
} // namespace lathe
