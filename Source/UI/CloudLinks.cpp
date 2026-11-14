// Invite links and listening links: Cloud > Invite Someone by Link, Join with an Invite
// Link, and Share a Listening Link. Both kinds of link open a page on getlathe.app
// (join.html, listen.html); the server side is lathe-backend's /v2/invites and
// /v2/songs/{id}/listen.
#include "CloudLinks.h"
#include "CloudUI.h"
#include "CloudUIHelpers.h"

namespace lathe
{
using namespace cloudui;

namespace
{
//==============================================================================
/** A small window: a few lines of explanation, an optional choice, the link (with Copy),
    a main button and an optional second one. Each flow below fills it in. */
class LinkDialog : public Component
{
public:
    LinkDialog (const String& heading, bool editableLink)
    {
        title.setText (heading, dontSendNotification);
        title.setFont (theme::font (16.0f, true));
        addAndMakeVisible (title);

        message.setFont (theme::font (12.5f));
        message.setJustificationType (Justification::topLeft);
        message.setColour (Label::textColourId, theme::textDim);
        addAndMakeVisible (message);

        styleEditor (link);
        link.setReadOnly (! editableLink);
        link.setVisible (editableLink);
        link.onReturnKey = [this] { if (main.isEnabled() && onMain) onMain(); };
        addChildComponent (link);

        addChildComponent (choice);

        copy.setButtonText ("Copy Link");
        copy.onClick = [this]
        {
            SystemClipboard::copyTextToClipboard (link.getText());
            copy.setButtonText ("Copied");
        };
        addChildComponent (copy);

        main.onClick = [this] { if (onMain) onMain(); };
        second.onClick = [this] { if (onSecond) onSecond(); };
        close.setButtonText ("Close");
        close.onClick = [this] { closeWindow(); };
        addAndMakeVisible (main);
        addChildComponent (second);
        addAndMakeVisible (close);
        setSize (500, 250);
    }

    void setMessage (const String& text, bool problem = false)
    {
        message.setText (text, dontSendNotification);
        message.setColour (Label::textColourId, problem ? theme::accent : theme::textDim);
    }

    void setLink (const String& url)
    {
        link.setText (url, false);
        link.setVisible (url.isNotEmpty() || ! link.isReadOnly());
        copy.setVisible (url.isNotEmpty() && link.isReadOnly());
        copy.setButtonText ("Copy Link");
        resized();
    }

    void setMain (const String& text, bool enabled = true)
    {
        main.setButtonText (text);
        main.setEnabled (enabled);
        main.setVisible (text.isNotEmpty());
        resized();
    }

    void setSecond (const String& text)
    {
        second.setButtonText (text);
        second.setVisible (text.isNotEmpty());
        resized();
    }

    void setBusy (bool b)
    {
        main.setEnabled (! b);
        second.setEnabled (! b);
        choice.setEnabled (! b);
    }

    void visibilityChanged() override
    {
        if (isShowing() && ! link.isReadOnly())
            link.grabKeyboardFocus();
    }

    void paint (Graphics& g) override   { g.fillAll (theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        title.setBounds (r.removeFromTop (26));
        r.removeFromTop (8);
        auto buttons = r.removeFromBottom (30);
        close.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        if (main.isVisible())
        {
            main.setBounds (buttons.removeFromRight (jmax (110, main.getBestWidthForHeight (30) + 24)));
            buttons.removeFromRight (8);
        }
        if (second.isVisible())
            second.setBounds (buttons.removeFromLeft (jmax (110, second.getBestWidthForHeight (30) + 24)));
        r.removeFromBottom (12);
        if (link.isVisible())
        {
            auto row = r.removeFromBottom (30);
            if (copy.isVisible())
            {
                copy.setBounds (row.removeFromRight (100));
                row.removeFromRight (8);
            }
            link.setBounds (row);
            r.removeFromBottom (10);
        }
        if (choice.isVisible())
        {
            choice.setBounds (r.removeFromBottom (28));
            r.removeFromBottom (10);
        }
        message.setBounds (r);
    }

    void launch (const String& windowTitle)
    {
        DialogWindow::LaunchOptions o;
        o.content.setOwned (this);   // deleted with its window
        o.dialogTitle = windowTitle;
        o.dialogBackgroundColour = theme::panel;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.escapeKeyTriggersCloseButton = true;
        o.launchAsync();
    }

    void closeWindow()
    {
        if (auto* w = findParentComponentOfClass<DialogWindow>())
            w->exitModalState (0);
    }

    Label title, message;
    TextEditor link;
    ComboBox choice;
    TextButton copy, main, second, close;
    std::function<void()> onMain, onSecond;

};

String songTitle (const String& name)   { return String (CharPointer_UTF8 ("\xe2\x80\x9c")) + name + String (CharPointer_UTF8 ("\xe2\x80\x9d")); }

/** WAV to FLAC (lossless, about half the size), which every current browser plays. */
Result toFlac (const File& wav, const File& flac, double& seconds)
{
    WavAudioFormat wavFormat;
    std::unique_ptr<AudioFormatReader> reader (wavFormat.createReaderFor (wav.createInputStream().release(), true));
    if (reader == nullptr || reader->sampleRate <= 0)
        return Result::fail ("Couldn't read the rendered mix.");
    seconds = (double) reader->lengthInSamples / reader->sampleRate;

    flac.deleteFile();
    std::unique_ptr<OutputStream> stream (flac.createOutputStream());
    if (stream == nullptr)
        return Result::fail ("Couldn't write " + flac.getFullPathName());
    FlacAudioFormat flacFormat;
    auto writer = flacFormat.createWriterFor (stream, AudioFormatWriterOptions()
                                                          .withSampleRate (reader->sampleRate)
                                                          .withNumChannels ((int) reader->numChannels)
                                                          .withBitsPerSample (16));
    if (writer == nullptr)
        return Result::fail ("Couldn't make a FLAC file at " + String (reader->sampleRate) + " Hz.");
    if (! writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples))
        return Result::fail ("Couldn't write the FLAC file.");
    return Result::ok();
}
} // namespace

namespace cloudlinks
{
String inviteCode (String text)
{
    static const String allowed ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-");
    // A link that went through a social site may have picked up tracking (?trk=, &fbclid=, utm_
    // tags), even after the code, or had its "#" encoded as %23. The code is what follows the "#",
    // up to the first character that can't be part of one.
    text = text.trim().replace ("%23", "#", true);
    if (text.containsChar ('#'))
        text = text.fromFirstOccurrenceOf ("#", false, false).initialSectionContainingOnly (allowed);
    else if (text.containsChar ('/'))
        text = text.upToFirstOccurrenceOf ("?", false, false).fromLastOccurrenceOf ("/", false, false);
    text = text.trim();
    if (text.length() < 8 || text.length() > 40 || ! text.containsOnly (allowed))
        return {};
    return text;
}

Result shareMix (cloud::Client& c, Engine& engine, Engine::ExportSettings s, double bpm, const String& songId,
                 const cloud::Progress& progress, String& url)
{
    auto report = [&] (const String& what, double p) { return progress == nullptr || progress (what, p); };
    const auto temp = s.folder;
    temp.createDirectory();
    s.mix = true;
    s.stems = false;
    StringArray files;
    auto result = engine.exportAudio (s, [&] (double p) { return report ("Rendering the mix", p * 0.6); }, files);
    if (result.wasOk() && files.size() != 1)
        result = Result::fail ("The render didn't make a mix file.");
    double seconds = 0.0;
    const auto flac = temp.getChildFile ("mix.flac");
    if (result.wasOk())
        result = toFlac (File (files[0]), flac, seconds);
    if (result.wasOk() && ! report ("Uploading the mix", 0.6))
        result = Result::fail ("Cancelled.");
    if (result.wasOk())
    {
        cloud::Client::UploadItem item;
        item.hash = cloud::sha256OfFile (flac);
        item.file = flac;
        int sent = 0;
        result = c.upload (songId, { item }, [&] (double p) { return report ("Uploading the mix", 0.6 + p * 0.4); }, sent);
        if (result.wasOk())
        {
            DynamicObject::Ptr body = new DynamicObject();
            body->setProperty ("blob", item.hash);
            body->setProperty ("seconds", seconds);
            body->setProperty ("bpm", bpm);
            cloud::ApiError err;
            const auto resp = c.call ("POST", "/v2/songs/" + songId + "/listen", var (body.get()), err);
            if (err.status != 0)
                result = Result::fail (err.message);
            else
                url = resp["url"].toString();
        }
    }
    temp.deleteRecursively();
    return result;
}
} // namespace cloudlinks
using cloudlinks::inviteCode;

//==============================================================================
void CloudController::showInviteLink()
{
    if (! isSignedIn())
    {
        showSignIn ([this] { showInviteLink(); });
        return;
    }

    auto* d = new LinkDialog ("Invite someone by link", false);
    const bool songInvite = isLinked() && songRole == "owner" && ! lostAccess;
    const auto name = songInvite ? store->getCloudName() : String();
    d->choice.addItem ("To be friends with you on Lathe Cloud", 1);
    if (songInvite)
    {
        d->choice.addItem ("To work on " + songTitle (name) + " with you", 2);
        d->choice.addItem ("To listen to " + songTitle (name) + " and pull it (view only)", 3);
    }
    d->choice.setSelectedId (songInvite ? 2 : 1, dontSendNotification);
    d->choice.setVisible (true);
    const String about = "Send the link to them however you like. On the page it opens they type their email: if they're "
                         "new, Lathe Cloud makes them an account and emails a password. Then you're friends"
                         + String (songInvite ? ", and the song is waiting in Cloud > Projects." : ".")
                         + "\n\nA link works for 14 days, for up to 10 people.";
    d->setMessage (about);
    d->setMain ("Make Link");
    d->choice.onChange = [d] { d->setLink ({}); d->setMain ("Make Link"); };

    Component::SafePointer<LinkDialog> safe (d);
    const auto songId = songInvite ? store->getProjectId() : String();
    d->onMain = [this, safe, songId, about]
    {
        if (safe == nullptr)
            return;
        auto* body = new DynamicObject();
        const int which = safe->choice.getSelectedId();
        if (which != 1 && songId.isNotEmpty())
        {
            body->setProperty ("song", songId);
            body->setProperty ("role", which == 3 ? "viewer" : "editor");
        }
        safe->setBusy (true);
        request ("POST", "/v2/invites", var (body), [safe, about] (const var& resp, const cloud::ApiError& err)
        {
            if (safe == nullptr)
                return;
            safe->setBusy (false);
            if (err.status != 0)
            {
                safe->setMessage (err.message, true);
                return;
            }
            safe->setMessage (about);
            safe->setLink (resp["url"].toString());
            safe->setMain ({});
        });
    };
    d->launch ("Invite Someone");
}

void CloudController::joinWithLink()
{
    if (! isSignedIn())
    {
        showSignIn ([this] { joinWithLink(); });
        return;
    }

    auto* d = new LinkDialog ("Join with an invite link", true);
    d->setMessage ("Paste the link someone sent you. You'll be friends, and if it's for a song, you can open it right away.");
    const auto clip = SystemClipboard::getTextFromClipboard().trim();
    if ((clip.contains ("join.html#") || clip.containsIgnoreCase ("join.html%23")) && inviteCode (clip).isNotEmpty())
        d->link.setText (clip, false);
    d->setLink (d->link.getText());
    d->setMain ("Join");

    Component::SafePointer<LinkDialog> safe (d);
    WeakReference<CloudController> weak (this);
    d->onMain = [this, safe, weak]
    {
        if (safe == nullptr)
            return;
        const auto code = inviteCode (safe->link.getText());
        if (code.isEmpty())
        {
            safe->setMessage ("That doesn't look like an invite link. It ends in join.html# and a code.", true);
            return;
        }
        safe->setBusy (true);
        request ("POST", "/v2/invites/" + code + "/accept", {}, [safe, weak] (const var& resp, const cloud::ApiError& err)
        {
            auto* self = weak.get();
            if (safe == nullptr || self == nullptr)
                return;
            safe->setBusy (false);
            if (err.status != 0)
            {
                safe->setMessage (err.message, true);
                return;
            }
            safe->closeWindow();
            self->refreshWindows();
            self->reloadFriendsWindow();
            const auto inviter = resp["inviter"].toString();
            const auto song = resp["song"];
            if (! song.isObject())
            {
                alert ("You're friends", resp["gone"] ? "You and " + inviter + " are friends on Lathe Cloud now. The song "
                                                        "that link was for isn't theirs to share any more."
                                                      : "You and " + inviter + " are friends on Lathe Cloud now.");
                return;
            }
            const bool viewer = song["role"].toString() == "viewer";
            AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::NoIcon)
                                        .withTitle ("You're in")
                                        .withMessage (inviter + " shared " + songTitle (song["name"].toString()) + " with you"
                                                      + (viewer ? " to listen to and pull." : ".")
                                                      + " It's in Cloud > Projects whenever you want it.")
                                        .withButton ("Open the Song").withButton ("Later"),
                                    [weak, song] (int button)
                                    {
                                        if (auto* me = weak.get(); me != nullptr && button == 1)
                                            me->openCloudSong (song);
                                    });
        });
    };
    d->launch ("Join");
}

//==============================================================================
void CloudController::shareListeningLink()
{
    if (! isSignedIn())
    {
        showSignIn ([this] { shareListeningLink(); });
        return;
    }
    if (! isLinked())
    {
        alert ("Put the song in the cloud first",
               "A listening link plays this song's mix from Lathe Cloud. Choose Cloud > Put This Song in the Cloud, "
               "then share the link.");
        return;
    }
    if (lostAccess || (songRole.isNotEmpty() && songRole != "owner"))
    {
        alert ("Only the song's owner shares listening links",
               "A listening link is public, so it's up to the song's owner. Ask them to share the mix.");
        return;
    }

    auto* d = new LinkDialog ("Share a listening link", false);
    const auto songId = store->getProjectId();
    const String about = "Anyone with the link can play this song's mix in a browser and leave comments at a time in it. "
                         "They don't need Lathe or an account. Their comments show up in Comments, here and for everyone on the song.";
    d->setMessage (about + "\n\nLathe renders the mix as it sounds now and uploads it.");
    d->setMain ("Share the Mix");

    Component::SafePointer<LinkDialog> safe (d);
    request ("GET", "/v2/songs/" + songId + "/listen", {}, [safe, about] (const var& resp, const cloud::ApiError& err)
    {
        if (safe == nullptr || err.status != 0)
            return;   // 404: not shared yet
        safe->setLink (resp["url"].toString());
        safe->setMessage (about + "\n\nThe mix there was shared " + timeAgo ((int64) resp["updatedAt"])
                          + ". Share it again after changes: the link stays the same.");
        safe->setMain ("Share the Latest Mix");
        safe->setSecond ("Stop Sharing");
    });

    d->onSecond = [this, safe, songId, about]
    {
        if (safe == nullptr)
            return;
        safe->setBusy (true);
        request ("DELETE", "/v2/songs/" + songId + "/listen", {}, [safe, about] (const var&, const cloud::ApiError& err)
        {
            if (safe == nullptr)
                return;
            safe->setBusy (false);
            if (err.status != 0)
            {
                safe->setMessage (err.message, true);
                return;
            }
            safe->setLink ({});
            safe->setSecond ({});
            safe->setMain ("Share the Mix");
            safe->setMessage ("Stopped. That link doesn't play anything now. Sharing again makes a new one.\n\n" + about);
        });
    };

    d->onMain = [this, safe, songId, about]
    {
        if (safe == nullptr)
            return;
        if (busy)
        {
            safe->setMessage ("Lathe Cloud is busy with something else. Try again when it's finished.", true);
            return;
        }
        if (ctx.engine.isPlaying())
            ctx.engine.stop();

        auto& edit = ctx.edit;
        Engine::ExportSettings s;
        s.mix = true;
        s.stems = false;
        s.bitDepth = 16;
        s.tailSeconds = 2.0;
        s.startBeat = 0.0;
        s.endBeat = jmax (edit.beatsPerBar(), edit.getEndBeat());
        const auto temp = File::getSpecialLocation (File::tempDirectory).getNonexistentChildFile ("lathe-listen", "");
        s.folder = temp;
        s.baseName = "mix";
        const double bpm = edit.tempo;

        auto c = client;
        Engine* engine = &ctx.engine;
        auto flag = cancelFlag;
        auto progress = progressReporter();
        auto url = std::make_shared<String>();
        auto result = std::make_shared<Result> (Result::ok());
        safe->setBusy (true);
        safe->setMessage ("Rendering the mix...");

        run ("Rendering the mix", [c, engine, flag, progress, url, result, s, songId, bpm]
        {
            *result = cloudlinks::shareMix (*c, *engine, s, bpm, songId, [&] (const String& what, double p)
            {
                return progress (what, p) && ! *flag;
            }, *url);
        },
        [this, safe, url, result, about]
        {
            if (result->failed())
            {
                hooks.setStatus ("Couldn't share the mix: " + result->getErrorMessage());
                if (safe != nullptr)
                {
                    safe->setBusy (false);
                    safe->setMessage ("Couldn't share the mix: " + result->getErrorMessage(), true);
                }
                return;
            }
            hooks.setStatus ("Shared the mix. Anyone with the listening link can play it.");
            if (safe == nullptr)
                return;
            safe->setBusy (false);
            safe->setLink (*url);
            safe->setMessage (about + "\n\nShared just now. Share it again after changes: the link stays the same.");
            safe->setMain ("Share the Latest Mix");
            safe->setSecond ("Stop Sharing");
        });
    };
    d->launch ("Listening Link");
}
} // namespace lathe
