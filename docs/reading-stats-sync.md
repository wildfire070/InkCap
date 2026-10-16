---
title: Reading Stats Sync
nav_order: 7
---

# Reading Stats Sync

CrossInk can keep your reading progress, reading stats, and clippings in step across devices in two ways:

- **Sync Server**: your readers share progress, stats, and clippings through an online sync server (CrossPoint Sync, or a KOReader-compatible server for progress). See [Server Sync Settings](#server-sync-settings) and [Ways to Sync](#ways-to-sync).
- **Nearby Stats Sync**: two CrossInk readers swap all-time reading totals directly, with no server or account. See [Nearby Stats Sync](#nearby-stats-sync). To copy a reading position between two readers instead, see [Nearby Position Sync](./nearby-position-sync.md).

## Server Sync Settings

To set up server sync:

1. Open **Settings > System > Sync Server**.
2. Enter a **Username** and **Password**. If you do not have an account yet, choose **Sign Up**.
3. Choose **Authenticate** to check that the details work.

The default server is CrossPoint Sync (`https://sync.crosspointreader.com`). You can also use a self-hosted copy or a KOReader-compatible server by changing **Sync Server URL**.

This screen only holds settings. The sync actions themselves are in the **Sync Server** section of **File Transfer** (see [Ways to Sync](#ways-to-sync)). The screen has three groups:

- **Account**: username, password, server URL, **Sign Up**, and **Authenticate**.
- **What to Sync**: **Reading Stats** and **Clippings**.
- **Sync Options**: **Sync Behavior** (**Smart sync** or **Ask on conflicts**), **Document Matching** (**Filename** or **Binary**), and **Send Document Metadata**.

Reading progress always syncs. **Reading Stats** and **Clippings** are off until
you turn them on. They only work with CrossPoint Sync servers:

- The default server always supports them.
- On a custom server, CrossInk checks when you **Authenticate**, and learns from
  the first stats or clippings upload. The authentication screen says whether
  stats and clippings can sync too.
- On a KOReader-only server, both rows show **Requires CrossPoint Sync** and
  are skipped, even if turned on. Syncs finish normally instead of reporting a
  failed upload.

Changing the server URL clears what was learned about the old server.

## Ways to Sync

**Sync Book**, **Sync Folder**, and **Sync Books** send the same data: reading
progress (EPUB only), plus reading stats and clippings when they are on under
**What to Sync**. Only the scope differs:

| Action | Where | Books |
| --- | --- | --- |
| **Sync Book** | Reader menu, frontlight panel, or a book's context menu in Library / Browse Files | One book |
| **Sync Folder** | Long-press a folder in Browse Files | That folder and its subfolders |
| **Sync Books** | **File Transfer**, **Sync Server** section | Every book on the SD card |

Two more actions upload a single kind of data and never read or change reading
positions:

| Action | Where | Sends |
| --- | --- | --- |
| **Sync Reading Stats** | **File Transfer**, **Sync Server** section, or the Reading Stats **This Device** page (Confirm on button devices, the upload icon on touch) | Overall stats, plus the saved stats of every book on the SD card, several books per request |
| **Sync Clippings** | **File Transfer**, **Sync Server** section | The clippings of every book that has any |

These two appear only when their **What to Sync** toggle is on and the server is
known to accept stats and clippings (the default server always is; a custom
server after **Authenticate** or a first upload).

They send only what changed. CrossInk keeps a short list of books whose
position, stats or clippings changed since they were last uploaded, and these
actions upload just those books. The first time each runs (and again after its
**What to Sync** toggle is turned back on, or after switching server or
account) it runs a one-time catch-up instead, because changes made before the
list existed were never recorded:

- **Sync Reading Stats** catch-up: "Scanning your SD card for reading stats...".
  It checks each book on the card for a stats file (no book is opened) and
  sends every book's stats, several books per request.
- **Sync Clippings** catch-up: "Checking clippings...". It only reads the
  clippings folder and sends each book's clippings.

A catch-up re-sends everything of its type, so with hundreds of annotated books
turning Clippings back on means one longer sync. Exit stops it safely; the next
run continues where it stopped. A finished **Sync Books** also counts as both
catch-ups. Without an account, **Sync Books**
asks you to set up an account in **Settings > System > Sync Server** first.

Syncing from Reading Stats inside a book reopens that book afterwards (EPUB
books reopen Reading Stats too), the same way **Sync Book** returns to the
reader.

Overall stats are sent once per action. XTC books have no KOReader position, so
they sync reading stats only; their **Sync Book** runs in place instead of
restarting into the reader. TXT and Markdown books have nothing to sync and are
skipped.

Every result screen lists **All-time Stats**, **Reading Stats**, and **Clippings**
with the same wording: **Done**, a count, **OFF** when that data is turned off,
**Not supported** on a progress-only server, or the error.

## Sync Books

**Sync Books** walks the SD card directly, so it does not depend on the
Library: books copied since the last Library refresh, and readers who only use
Browse Files, are included. Each EPUB follows the same rules
as **Sync Folder** below (**Smart sync** or **Ask on conflicts**, failures listed at the end, Back to stop).
Missing files, unreadable book stats, and books with tracking disabled are
skipped. Nearby readers' totals are never uploaded as your own.

### Incremental syncs (CrossPoint Sync)

After the first full pass, later syncs only handle what changed, so they are much faster. The details below explain how this works.

On a CrossPoint Sync server with the progress change feed, **Sync Books** and
**Sync Folder** only walk their books once:

- **First sync** of the card or a folder: "Syncing books on your SD card..."
  (or "Syncing books in this folder...") with a progress bar; the books are
  counted by name first, without opening them. Every book is synced as above. The
  walk is resumable: Exit stops it, and the next run skips books it finished.
  If a book failed, the next run resumes the walk once to retry it, then
  switches to incremental syncs either way. Books with no saved stats or clippings are
  not hashed for them.
- **Later syncs**: "Checking for updates..." reads only the positions changed
  on the server since the last sync (10 per request) and applies those to the
  matching books, with the same Smart or Ask rules. Then "Uploading changes..."
  sends only books whose position, stats or clippings changed on this reader.
  No per-book request is made for unchanged books. Positions are sent up to 8 per
  request (stats were already batched); a server without the batch route gets
  one request per book instead.
- A folder inside an already-synced scope (for example any folder after a
  completed **Sync Books**) starts incremental right away.
- A remote position for a book this reader does not have, a book never opened
  here, a failed apply, a conflict waiting for its choice, or **Skip book** in
  Ask on conflicts is kept on the
  reader (up to 32, oldest dropped first). Before that book's progress is
  uploaded, it is compared with the kept position (Smart or Ask, as above);
  any sync that reconciles the book, including **Sync Book**, drops it.
  Switching server or account clears them.
- Opening a book with a kept position, if it is inside a synced folder (or the
  card after a completed **Sync Books**), asks "Continue from 45% (Phone)?"
  in both Smart and Ask on conflicts, without using the network. **Continue**
  moves to that position; **Stay here** (or Back) keeps this reader's position
  and uploads it with the next sync. When this reader is already at or past
  the kept position, nothing is asked: the kept position is dropped, and a
  position further along is uploaded with the next sync. Books outside synced
  folders are never asked about. Opening a book costs nothing extra when no
  position is kept; otherwise its id comes from the book's cache, and binary
  matching reads the book only when that copy is missing or the file changed.

Stock KOSync servers, and CrossPoint Sync servers without the feed, keep the
per-book walk every time.

This action runs **only** after manual confirmation. There are no uploads on
book close, sleep, or a timer. It does not enable device-wide tracking.
Historical totals already included in overall stats remain part of that snapshot,
even if tracking for an individual book is now disabled.

Book matching uses the **Document Matching** setting (**Filename** or **Binary**). Keep it consistent across
readers; changing it or renaming a file can create another server record.
Changing **Document Matching** (in **Sync Options** or the web settings page) asks
first, because every book gets a new id: on confirm the reader forgets its
book id map, kept positions, and each synced folder's progress, so the next
**Sync Books** or **Sync Folder** checks every book again. Progress already on
the server stays under the old ids until you merge the books in the web app.
**Send Document Metadata** controls title, author, and filename sharing as it does for
normal progress uploads.

Press Back to stop between books. A network request has a bounded timeout.
Accepted snapshots remain on the server after cancellation or a later failure;
retrying replaces them rather than adding the same reading time again.

Local deletion does not delete server snapshots. Removing stats files causes
uploads to skip them; an explicit device-wide stats reset writes an empty valid
snapshot, which replaces that device's overall server totals on the next upload.
Do not copy another reader's local stats files onto this reader and upload both:
those copied histories cannot be distinguished from reading performed locally.

> [!NOTE]
> Advanced: stats uploads use the same security policy as KOReader progress
> sync. HTTPS encrypts traffic, but server certificates are not verified. A
> server set up with plain HTTP sends your login and stats without encryption.

## Progress and Stats Together

With **Reading Stats** on, the reader's manual **Sync Book** action also uploads
saved overall and current-book stats. It also runs when Smart Sync finds equal
progress or applies a newer server position. Disabled tracking, missing stats,
and invalid stats files do not upload replacement zeros. Other readers' stats are
never merged into local files.

These are separate API requests within the same user action, not one transaction.
Stats and clippings are attempted independently. If one upload fails, the other
is still attempted and the screen reports each result. Folder sync also attempts
these saved snapshots when progress sync fails. Retry the manual sync to resend the current snapshots.

## Catch Up a Folder

In **Browse Files**, long-press a folder (hold Confirm on button devices), select
**Sync Folder**, then confirm. This syncs saved EPUB positions in that
folder and its subfolders. It reads the SD folders directly; no Library refresh is
needed. XTC books in the folder sync their reading stats only. The **Sync Behavior** setting applies to every EPUB: **Smart sync** keeps the further
progress, uploading local progress or applying remote progress as needed; **Ask
on conflicts** asks only when the server's position differs from the reader's,
showing the book's filename at the top. A book the server has no position for
uploads without asking, and matching positions count as already synced.

The whole run stays on one screen: the book being synced, a progress bar (for
the first sync of a folder and for "Uploading changes..."), and the running
counts, redrawn with fast refreshes instead of a screen per book. Ask on
conflicts does not stop the run: each conflicting book's server position is
kept ("Conflicts to review: 2"), its stats and clippings still upload, and the
choices are shown one after another once every book is done. Exit from a choice
ends the sync; the remaining positions stay kept and are compared again at the
next sync or offered when the book is opened.

EPUBs need saved progress; missing EPUB metadata is rebuilt before syncing.
Unread books and old or invalid progress without a usable page count skip position sync;
their saved stats and clippings are still considered when enabled.
A book that fails (a metadata, mapping, or network error) does not stop the run:
its saved stats and clippings are still attempted, and the sync moves to the next
book. Hidden entries and
files other than EPUB and XTC are excluded. The folder's
name does not mark books as finished, and this cannot reconstruct deleted history.
Traversal is bounded to 16 folder levels and 1024-byte paths; exceeding a limit or
an unreadable folder stops the sync rather than silently reporting success.

**Reading Stats** and **Clippings** apply to each synced book when enabled.
Overall stats are attempted once after confirmation and Wi-Fi connection, even if
the folder is empty or every book skips or fails position sync. A failure does not
block per-book uploads. The result shows overall stats, book stats, and clippings
separately; a successful position may also appear in the failed-book count if an
extra upload failed. The final screen lists up to five failed books under
**Failed books**, each with its title (or file name) and what failed and why,
for example "Progress sync failed: Sync server returned HTTP 500"; any more
show as "+3 more". **Sync Book** on a single book shows the same heading, the
title, and a Progress, Reading Stats and Clippings result. Missing data and disabled tracking show no upload rather
than replacing server data with zeros. **Send Document Metadata** controls title,
author, and filename sharing just as it does for normal progress uploads.
Press Back to stop, including from a conflict choice; a book with many clippings
can take longer. Completed uploads and downloads remain applied if a later book
fails or you cancel.

## Clippings Upload

With **Clippings** on, manual book sync and folder catch-up include the book's
saved clippings. Clippings are upload-only: CrossInk does not download
server clippings or propagate local deletions. Notes and colors already on the
server are preserved. Server-deleted IDs stay deleted; this uploader never revives
a server tombstone.

> [!NOTE]
> Advanced: IDs follow the server's SHA-256 rule using the uploaded creation value and exact
> text, so unchanged clippings keep the same ID across retries. New clippings save
> UTC creation time from the available clock, or zero when no clock is available.
> Older uptime-based timestamps are exported as zero (unknown), not as dates near
> 1970. Identical undated quote text within one book shares one server ID; local
> records remain separate. No historical dates are invented and no clipping file
> migration is performed. One clipping is read and sent at a
> time, including its chapter and saved layout hints, without loading all clippings
> into memory. The server's 4096-byte text limit matches the local clipping limit.

## Sync a Selected Book

Choose **Sync Book** from an EPUB or XTC book's context menu in **Library** or **Browse Files**. For an EPUB this uses the same sync behavior as the reader menu, including **Reading Stats** and **Clippings** only when enabled, and resumes in the selected book's reader. For an XTC book it sends that book's reading stats and overall stats, then returns Home. If account credentials are missing, account settings open first; return to the book menu to retry after setup. TXT and Markdown books do not show this action.

## Nearby Stats Sync

Nearby Stats Sync works directly from one reader to another; it does not use a server, account, or one "main" reader. It only works with CrossInk reading stats, not with any other firmware's reading stats.

### What Gets Synced

Nearby Stats Sync shares only each reader's all-time totals:

- total reading sessions
- total reading time
- total pages turned
- completed books

It does not sync per-book reading position, bookmarks, files, KOReader progress, settings, Wi-Fi passwords, or OPDS servers.

Each device owns one record, and the Reading Stats screen sums all the records it can read. The device does not merge another reader's totals into its local `global_stats.bin`.

### How Nearby Sync Works

1. On both readers, open **File Transfer** and select **Nearby Stats Sync** (in the **Nearby Device** section). It only appears while reading stats tracking is turned on.
2. Press **Sync** on one reader only.
3. The readers find each other and swap their totals.
4. From then on, the Reading Stats screen on each reader shows its own stats plus the other reader's.

> [!NOTE]
> Advanced: the readers connect over ESP-NOW and exchange their local
> `/.crosspoint/global_stats.bin` payloads. Each reader saves the other's payload under
> `/.crosspoint/synced_stats/device_<mac>.bin`, and Reading Stats sums this reader's
> local stats with the valid files in that folder.

The sync screen creates `/.crosspoint/synced_stats/` the first time you use it. That folder stores snapshots received from other readers. This reader's own all-time totals stay in `/.crosspoint/global_stats.bin`; CrossInk does not write this reader's own `device_<mac>.bin` file into `synced_stats/`.

### SD Card Folder Structure (Advanced)

The reading-stats files live under `.crosspoint` on the SD card:

```text
.crosspoint/
├── global_stats.bin
├── global_stats.bin.bak
└── synced_stats/
    ├── device_aabbccddeeff.bin
    ├── device_112233445566.bin
    └── manual-copy.bin
```

`global_stats.bin` is this reader's local all-time stats. The `.bak` file is a
backup used if the main file is corrupt.

`synced_stats/` contains one contribution file per other synced reader. Each file is a snapshot from the last time this reader received that device's stats; it is not a live copy of that other reader's current `global_stats.bin`. Files created by nearby sync use the peer reader's hardware MAC address in the file
name: `device_<mac>.bin`, with no colons or dashes.

When displaying aggregated stats, CrossInk reads every non-folder file in `synced_stats/` that has a valid CrossInk stats payload. The file name does not have to match `device_<mac>.bin` for manual imports. If a manually copied file matches this reader's own `device_<local-mac>.bin`, CrossInk skips it so local stats are not counted twice.

### Manually Removing Synced Stats (Advanced)

Back up the SD card first if the stats matter to you.

To remove one old reader from the aggregate total, delete that reader's file from `/.crosspoint/synced_stats/` on every device that has it. For example:

```text
/.crosspoint/synced_stats/device_aabbccddeeff.bin
```

To stop using synced aggregate totals on a reader, delete the whole `/.crosspoint/synced_stats/` folder from that reader. As long as the folder is gone, local stats saves will not recreate it. Opening Nearby Stats Sync again will recreate the folder.

To reset only this reader's local all-time totals, delete both:

```text
/.crosspoint/global_stats.bin
/.crosspoint/global_stats.bin.bak
```

Do not delete `/.crosspoint/epub_<hash>/stats.bin` or versioned files such as
`/.crosspoint/epub_<hash>/stats_v5.bin` unless you also want to remove the
per-book stats for that book.

If a sync was interrupted, a temporary `*.part` file may be left in `synced_stats/`. CrossInk ignores invalid stats files while aggregating, so it is safe to delete leftover `.part` files.

### Manual Copying Between Devices (Advanced)

You can manually copy a valid stats file into another reader's `/.crosspoint/synced_stats/` folder. A descriptive file name such as `old_reader.bin` can work because CrossInk validates the file contents, not the name.

Use normal `device_<mac>.bin` names when possible. Do not copy this reader's own local `global_stats.bin` into `synced_stats/` under a different name; CrossInk will treat it like another reader and the all-time total will be too high.
