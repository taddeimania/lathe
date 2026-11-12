#pragma once

#include "../Compare/Compare.h"

namespace lathe
{
/** View > Compare: Before, Now and a Reference, playing in step. Click one to hear it (or
    press 1, 2, 3); Space plays and stops while the window is in front. */
class CompareWindow : public DocumentWindow
{
public:
    static void show (Compare&, std::function<void (const String&)> status);
    static void closeIfOpen();
    /** If the Compare window is the one in front, Space is for it: plays or stops the
        comparison and returns true. */
    static bool takeSpace();

    CompareWindow (Compare&, std::function<void (const String&)> status);
    void closeButtonPressed() override;

private:
    class Content;
    static Component::SafePointer<CompareWindow> current;
};
} // namespace lathe
