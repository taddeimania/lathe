#pragma once

#include "../Engine/Engine.h"
#include "../Cloud/CloudClient.h"
#include "../Cloud/Revisions.h"

namespace lathe::cloudlinks
{
/** Cloud > Share a Listening Link, without the window: renders the mix (settings.folder is
    a temporary folder, made and removed here), turns it into FLAC, uploads it to the song
    and registers it as the song's listening mix. Run off the message thread. url: the
    getlathe.app/listen.html link. */
Result shareMix (cloud::Client&, Engine&, Engine::ExportSettings settings, double bpm, const String& songId,
                 const cloud::Progress& progress, String& url);

/** The code from a pasted invite link (getlathe.app/join.html#code), or "" if it isn't one. */
String inviteCode (String text);
} // namespace lathe::cloudlinks
