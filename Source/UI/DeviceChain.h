#pragma once

#include "Widgets.h"

namespace lathe
{
/** The selected track's plug-ins, left to right in signal order. */
class DeviceChainView : public Component
{
public:
    explicit DeviceChainView (AppContext&);
    ~DeviceChainView() override;

    void setTrack (std::shared_ptr<Track>);
    void refresh();
    void paint (Graphics&) override;
    void resized() override;

private:
    class DeviceBox;
    AppContext& ctx;
    std::shared_ptr<Track> track;
    Viewport viewport;
    Component content;
    OwnedArray<DeviceBox> boxes;
    TextButton addButton { "+ Add plug-in" }, browseButton { "Browse..." };
    String signature;
};
} // namespace lathe
