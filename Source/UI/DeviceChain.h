#pragma once

#include "EqEditor.h"

namespace lathe
{
/** The selected track's plug-ins, left to right in signal order, then its channel EQ. */
class DeviceChainView : public Component
{
public:
    explicit DeviceChainView (AppContext&);
    ~DeviceChainView() override;

    void setTrack (std::shared_ptr<Track>);
    void refresh();
    void paint (Graphics&) override;
    void resized() override;

    // Drag a card (a plug-in or the EQ) by its name bar to move it along the chain. A line
    // shows where it will land; Esc, or letting go away from the chain, leaves it where it was.
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    bool keyPressed (const KeyPress&) override;

private:
    class DeviceBox;
    /** The cards in chain order (plug-ins with the EQ at its place). */
    std::vector<Component*> chainCards() const;
    int gapAt (int contentX) const;          // 0..cards: the gap nearest x
    void endDrag (bool drop);

    AppContext& ctx;
    std::shared_ptr<Track> track;
    Viewport viewport;
    Component content;
    OwnedArray<DeviceBox> boxes;
    std::unique_ptr<EqDevice> eqDevice;
    TextButton addButton { "+ Add plug-in" }, browseButton { "Browse..." };
    String signature;

    Label dropLine;                          // where a dragged card will land
    Component* dragCard = nullptr;
    int dragFrom = -1, dragTo = -1;           // chain indexes; dragTo -1: not over the chain
    bool dragging = false;
};
} // namespace lathe
