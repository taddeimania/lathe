# Follow-up: explicit state machines

**Status: planned, not started.** Nothing here changes how Lathe behaves today. This page
records the plan so it can be picked up later.

## The problem

Much of Lathe's behaviour is a state machine that isn't written down as one: a few flags and
counters whose combinations mean "counting in", "new revision waiting" and so on. Each place
that needs the state works it out again from those flags, so the UI, the AI assistant tools
and the tests can drift apart, and combinations that shouldn't exist (recording but not
playing) are possible in principle.

## Where they are

| # | Machine | Where | Hidden states | Tracked today by |
|---|---|---|---|---|
| 1 | Transport | `Engine` | stopped, playing, counting in, recording, exporting; Stop twice rewinds | `playing`, `recording`, `exporting`, `countInLeft` (the count-in becomes recording on the audio thread). Worked out again in `TransportBar` and `AssistantTools::play/stop`. |
| 2 | Audio device | `Engine::timerCallback` | open, missing, reconnecting, changing rate | `deviceRate == 0`, `reopenCountdown`, `rebuildAfterExport` |
| 3 | Cloud song sync | `CloudController` | not shared, signed out, not uploaded yet, up to date, new revision waiting, view only, access lost, old server, busy | `busy`, `isLinked`, `isSignedIn`, `lostAccess`, `oldServer`, `waitingHead`, `isUnfinishedShare`, `songRole`, combined three times (status text, colour and tooltip, menu items) |
| 4 | Push / pull | `Revisions` | idle, pushing, conflict (pull first), pulling | `PushOutcome::conflict`, `busy` |
| 5 | Cloud sign-in | `CloudClient::Session` | signed out, signed in, token expiring, refreshing | `refreshToken`, `expiresAtMs` |
| 6 | Notes sync | `Notes` | synced, changes waiting, offline, failing | `outbox`, `flushing`, `uploading`, `syncProblem`, `error` |
| 7 | Plug-in scan | `PluginHost` | idle, scanning, aborting (+ skipped list) | `scanning`, `abortScan` |
| 8 | Plug-in store entry | `PluginStore` | not installed, installing, installed, update available, update waiting for restart | `busy`, `pendingRestart`, `installedVersion`, `assetUrl`; store `refreshing`, `status` |
| 9 | Update check | `UpdateNotice` | checks off, checking, up to date, update available, version skipped | `checking`, `skipUpdateVersion` |
| 10 | Lathe Patcher | `PatcherMain` | checking, nothing new, available, downloading, verifying, unpacking, installing, done, failed, cancelled | only the message text and progress value |
| 11 | AI assistant connection | `AssistantService` | off, starting, on, port taken, stopping | `serverRunning`, `alive`, whether `server` exists |
| 12 | Document | `Edit`, `MainComponent` | clean, changed, autosaved, recovering, asking to discard | `dirty`, `needsRebuild`, the autosave file, `confirmDiscard` |

Already explicit and fine: the arrangement and piano-roll drag modes, the notes filter (`Show`)
and monitoring (`Monitor`).

## The decision

Each machine gets **a named list of states (an `enum class`), one function that makes each
change and refuses impossible ones, and one read-only snapshot that every reader uses**: the
UI, the AI assistant tools and the tests.

- **Users:** buttons, the status bar, menus, tooltips and the warning colour read one value,
  so they always agree, and impossible combinations can't happen.
- **Developers:** each machine fits on one screen. The table of allowed changes is the
  documentation and the test plan. Adding a state makes the compiler point at every `switch`
  that doesn't handle it.
- **AI assistants:** `describe_session` reports named states (`transport.state: "counting_in"`,
  `cloud.state: "new_revision_waiting"`) instead of booleans to combine, and tool errors come
  from the same allowed-changes table as the buttons. That makes write tools safe to add later.

## How to do it without breaking anything

1. **Add calculated states alongside the flags.** For example `Engine::transportState()` and
   `CloudController::songState()` are worked out from the existing flags, with tests that they
   always agree. No behaviour changes.
2. **Move readers over one at a time:** the transport bar, the cloud status and menu, then the
   AI tools. Add `state` to `describe_session` next to the existing booleans, so nothing an AI
   relies on today breaks.
3. **Only then change the storage where it helps.** The transport's three atomic flags become
   one atomic state value, which is still lock-free on the audio thread. The count-in's switch
   to recording uses a single compare-and-swap, as the counter does today.
4. **Order:** transport (1) and cloud sync (3, 4) first: they're the most duplicated, the most
   visible, and what the AI already reads. Then the Patcher (10) and plug-in store entries (8),
   whose states currently exist only as text on screen. The rest as they're touched.
