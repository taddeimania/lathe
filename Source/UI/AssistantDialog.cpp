#include "Dialogs.h"
#include "../Assistant/AssistantService.h"

namespace lathe
{
namespace
{
class AssistantPanel : public Component, private Timer
{
public:
    AssistantPanel (AppContext& c, std::weak_ptr<assistant::Service> s) : ctx (c), service (std::move (s))
    {
        heading (intro, "AI assistant (MCP)");
        note (introNote, "Lets an AI app on this computer, like Claude Desktop or Claude Code, look at the open project "
                         "(tracks, routing, plug-ins, clips and notes) and play or stop it. It can't change the project yet. "
                         "It's off until you turn it on here.");

        enable.setButtonText ("Let AI apps on this computer connect to Lathe");
        enable.onClick = [this] { toggle(); };
        addAndMakeVisible (enable);

        status.setFont (theme::font (12.0f));
        status.setJustificationType (Justification::topLeft);
        addAndMakeVisible (status);

        heading (desktopHeading, "Claude Desktop");
        note (desktopNote, "In Claude Desktop: Settings > Developer > Edit Config. Put this in claude_desktop_config.json "
                           "(merge it into \"mcpServers\" if you already have some), save, and restart Claude Desktop.");
        code (desktopCode);
        copyDesktop.onClick = [this] { copy (desktopCode); };
        addAndMakeVisible (copyDesktop);

        heading (codeHeading, "Claude Code");
        note (codeNote, "Run this once in a terminal:");
        code (codeCode);
        copyCode.onClick = [this] { copy (codeCode); };
        addAndMakeVisible (copyCode);

        note (otherNote, {});

        heading (securityHeading, "Keeping it private");
        note (securityNote, "Lathe only listens on this computer (127.0.0.1), never the network, and every request needs a "
                            "secret key. Lathe writes the key to assistant.json in its settings folder, readable only by you, "
                            "and makes a new one each time the connection starts. Web pages can't connect.");
        newKey.setTooltip ("Disconnects anything using the current key. LatheMCP picks up the new key by itself.");
        newKey.onClick = [this]
        {
            if (auto s = service.lock())
                if (auto r = s->newKey(); r.failed())
                    ctx.showStatus ("AI assistant: " + r.getErrorMessage());
            refresh();
        };
        addAndMakeVisible (newKey);

        refresh();
        startTimer (1000);
        setSize (640, 650);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        intro->setBounds (r.removeFromTop (22));
        introNote->setBounds (r.removeFromTop (48));
        enable.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        status.setBounds (r.removeFromTop (52));
        r.removeFromTop (8);

        desktopHeading->setBounds (r.removeFromTop (22));
        desktopNote->setBounds (r.removeFromTop (34));
        auto row = r.removeFromTop (112);
        copyDesktop.setBounds (row.removeFromRight (80).removeFromTop (26));
        row.removeFromRight (8);
        desktopCode.setBounds (row);
        r.removeFromTop (10);

        codeHeading->setBounds (r.removeFromTop (22));
        codeNote->setBounds (r.removeFromTop (20));
        row = r.removeFromTop (26);
        copyCode.setBounds (row.removeFromRight (80));
        row.removeFromRight (8);
        codeCode.setBounds (row);
        r.removeFromTop (6);
        otherNote->setBounds (r.removeFromTop (34));
        r.removeFromTop (10);

        securityHeading->setBounds (r.removeFromTop (22));
        securityNote->setBounds (r.removeFromTop (50));
        newKey.setBounds (r.removeFromTop (26).removeFromLeft (230));
    }

private:
    void heading (Label*& l, const String& text)
    {
        l = labels.add (new Label ({}, text));
        l->setFont (theme::font (13.0f, true));
        addAndMakeVisible (l);
    }

    void note (Label*& l, const String& text)
    {
        l = labels.add (new Label ({}, text));
        l->setFont (theme::font (12.0f));
        l->setColour (Label::textColourId, theme::textDim);
        l->setJustificationType (Justification::topLeft);
        l->setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (l);
    }

    void code (TextEditor& e)
    {
        e.setMultiLine (true, false);
        e.setReadOnly (true);
        e.setScrollbarsShown (true);
        e.setFont (Font (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain)));
        addAndMakeVisible (e);
    }

    void copy (TextEditor& e)
    {
        SystemClipboard::copyTextToClipboard (e.getText());
        ctx.showStatus ("Copied.");
    }

    void toggle()
    {
        if (auto s = service.lock())
        {
            const bool on = ! s->isOn();
            if (auto r = s->setOn (on); r.failed())
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "AI assistant", r.getErrorMessage());
            else
                ctx.showStatus (on ? "AI assistant connection on." : "AI assistant connection off.");
        }
        refresh();
    }

    void refresh()
    {
        auto s = service.lock();
        if (s == nullptr)
            return;

        enable.setToggleState (s->isOn(), dontSendNotification);
        status.setText (s->statusText(), dontSendNotification);
        status.setColour (Label::textColourId, s->isOn() ? theme::accent : theme::textDim);
        newKey.setEnabled (s->isOn());

        if (desktopCode.getText() != s->desktopConfig())
            desktopCode.setText (s->desktopConfig(), false);
        if (codeCode.getText() != s->claudeCodeCommand())
            codeCode.setText (s->claudeCodeCommand(), false);

        const auto bridge = s->bridgeFile();
        otherNote->setText ("Other MCP apps: add a local (stdio) server that runs " + bridge.getFullPathName() + "."
                            + (bridge.existsAsFile() ? String() : "\n(LatheMCP is missing next to Lathe: reinstall Lathe.)"),
                            dontSendNotification);
    }

    void timerCallback() override   { refresh(); }

    AppContext& ctx;
    std::weak_ptr<assistant::Service> service;
    OwnedArray<Label> labels;
    Label *intro {}, *introNote {}, *desktopHeading {}, *desktopNote {}, *codeHeading {}, *codeNote {}, *otherNote {},
          *securityHeading {}, *securityNote {};
    ToggleButton enable;
    Label status;
    TextEditor desktopCode, codeCode;
    TextButton copyDesktop { "Copy" }, copyCode { "Copy" }, newKey { "New key (disconnects everyone)" };
};
} // namespace

void showAssistantDialog (AppContext& ctx, std::weak_ptr<assistant::Service> service)
{
    DialogWindow::LaunchOptions o;
    o.content.setOwned (new AssistantPanel (ctx, std::move (service)));
    o.dialogTitle = "AI Assistant";
    o.dialogBackgroundColour = theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}
} // namespace lathe
