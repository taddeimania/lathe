# Sharing projects with Lathe Cloud

Lathe can share a project with collaborators through Lathe Cloud. It works like git, not like a shared document:
- Everyone works offline on their own copy.
- **Push** sends a revision up. **Pull** brings collaborators' revisions down.

**Getting set up**
- Release builds are already set up for Lathe Cloud.
- **Cloud > Sign In** with your Lathe Cloud account. The first time, Lathe asks you to choose your own password.

**Friends, projects and songs**
- A **song** is one Lathe project file. A **project** is a group of songs: an album, a band, a client.
- You share with **friends** only. **Cloud > Friends** sends a request to someone's email; once they accept (in their Lathe), you can share with each other. Removing a friend doesn't unshare anything.
- **Cloud > Put This Song in the Cloud** uploads the open song (r1) into one of your projects, or a new one.
- **Cloud > Projects** lists your projects and their songs, and songs shared with you on their own. Select one to see who has it. **Open** downloads a song, audio included, into `Documents/Lathe/Projects/<name>`.
- Share a whole **project** (friends get every song in it, including ones added later) or a single **song**. Each person **can edit** (pull and push) or **can view** (pull only; they can still change their own copy, but can't push).
- **Share only some tracks.** In **Cloud > Projects**, Share > a friend > **Can push only some tracks...** (the song has to be open, and pushed at least once). Tick the tracks they may push. They clone the whole song and can change anything on their computer, but a push sends only those tracks, put on top of the newest revision; everything else they changed stays on their computer. Lathe Cloud refuses any other change, so it holds whatever version of Lathe they use. Change the tracks later from the person's menu in the song's people list (**Pushes Guitar**), or make them a full editor.
- What they see: a bar under the transport says "In “Song” you push Guitar. Everything else you change stays on this computer." Their tracks say **Yours** and are outlined in the arrangement and the mixer; the rest are dimmed (still playable and editable). **Cloud > Push Guitar...** pushes. Someone who can also edit the whole project isn't limited.
- Only the owner manages a project or song: share, change someone's access, **Stop Sharing** (with one person or everyone), **Rename** (shares stay), **Move to Project** (a song; people who had it only through its old project, or shared on its own, lose it), and **Delete from Cloud**. Anyone can **Leave**.
- When you lose access to something or become a viewer, Lathe tells you. Your copy on your computer is never touched: you just can't pull or push it anymore. **Cloud > Disconnect This Song** stops it asking.

**Notes and conversations**
- Leave a note on a track at a moment in the song: right-click the lane (or a clip) and choose **Add Note Here**, or **Track > Add Note at Start Marker** (Cmd/Ctrl+Alt+N). It shows as a speech bubble on the lane; the number is how many messages are in the thread.
- **Song notes** are about the whole song (the plan so far, what's left): **Track > Add Song Note**, or **Add Song Note** in the Notes window.
- Click a bubble to open the conversation: reply, **Resolve** (or Reopen), **Edit** your own note, **Delete**. You can delete your own notes and replies; the song's owner can delete any. Everyone the song is shared with can write notes, viewers too.
- **Notes** in the top bar (Cmd/Ctrl+Shift+N) lists every note, song notes first then in time order. Click one to jump there. It lights up when someone left something you haven't read, and the status bar tells you who. **On tracks** (also View > Show on Tracks) shows every note, hides resolved ones, or hides them all; that only changes your screen.
- A cloud song's notes live on the server: collaborators see them within a minute, with no Push needed and no conflicts. A song that isn't in the cloud keeps its notes in the project file, and they move to the server when you put it in the cloud.
- **Offline:** when Lathe can't reach the cloud, notes keep working. New notes and changes show at once (faded, "not sent yet") and are sent in order when the cloud is back; Lathe retries every 15 s. The Notes button reads **Notes offline** (its tooltip and the Notes window say why and how many changes are waiting). The last copy of the notes and the unsent changes are kept in the song's `.lathe/notes.json`, so they survive quitting.

**Push**
- Uploads what you have open as a new revision, with your description and an automatic summary of what changed (faders, pans, mutes, sends, EQ bands, plug-in knobs, clips, new or removed tracks, tempo, monitor mixes).
- Audio files are stored once under their content hash, so a take is never uploaded twice.
- Takes are compressed before they go up, losslessly (FLAC): a recorded take is typically about half the size, and the file you pull back is identical, byte for byte, to the one that was recorded (every download is checked). Compressing runs several hundred times faster than real time, so it saves more upload time than it takes. Files FLAC can't hold (32-bit float) go up as they are.
- Up to 4 files (or 64 MB pieces of a big file) move at once, in both directions, and a dropped connection is retried. Change this under **Cloud > Transfers at Once**: 1 sends one file at a time, which is gentler on a slow connection.

**Pull**
- Downloads new revisions and the latest one's audio.
- Doesn't change what you have open. Lathe offers to open the newest revision.

**History** (or click the cloud label in the status bar)
- Lists every revision: pushed, pulled, and **Local** ones (kept on this computer, not pushed).
- Select one to see what it changed and what opening it would change.
- **Open This Revision** first keeps what you have open as a Local revision, so switching never loses work.
- The cloud keeps each song's newest 5 revisions (20 on Pro). **Keep** a revision (a mix you sent, say) and it stays however many come after. Revisions the cloud let go stay in History on your computer ("only on this computer").

**Plans**
- **Free**: join songs friends share with you, pull them, leave notes, and push if they made you an editor.
- **Starter**: your own songs and projects in the cloud, shared with friends. Each song up to 5 GB (its current version), 50 GB for all your songs together, 2 free accounts can edit your songs (any number can view).
- **Pro**: songs up to 25 GB, 250 GB in all, 20 revisions kept, any number of free editors.
- Your plan shows in the Cloud menu next to your email. When something doesn't fit, Lathe says what and why before anything uploads.

**If someone pushed first**
- Lathe refuses to overwrite their revision and asks you to choose:
  - **Pull theirs first**, to look at it.
  - **Push mine on top**: yours becomes the latest, and theirs stays in History.
- There's no automatic merging of changes yet. The change summaries show what each side did.

**Other things to know**
- While a shared song is open, Lathe checks every two minutes for new revisions, friend requests and notices, and the status bar tells you when something is waiting.
- The history lives in a hidden `.lathe` folder inside the project folder, next to `Audio/`. Copying the folder copies the history.
