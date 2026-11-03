# Storage and indexing

[Back to README](../README.md)

Reference notes for backups, scan behavior, cloud sync, and migrations.

## Tag database

Tag analysis and proxy state are stored exclusively in SQLite at
`<local_dir>/.sprintboard/image-index.sqlite3`. Tags are durable data, not a
disposable cache. Schema version 8 migrates versions 6 and 7 transactionally,
preserving successful analyses, retry state, and proxy records. Image metadata
from those versions is rebuilt once because its timestamps used platform-specific
clock formats. The upgrade logs this rebuild; let the initial scan finish so its
results are saved. Rebuilding image metadata does not erase tags. Unsupported
schemas and corrupt databases are left untouched; automatic tagging for that
root stays disabled until the database is repaired and Sprintboard is restarted,
or it is opened with a compatible version of Sprintboard.

Existing `*.sprintboard.tags.json` sidecars are ignored and never imported,
updated, repaired, or deleted, including when gallery images are deleted. Tags
present only in those files require reanalysis. No new sidecars are created.

Each distinct physical `local_dir` owns a database. Bindings resolving to the
same directory share it; with nested roots, the most specific configured root
owns each image's records. Ownership transfers commit the destination record
before removing the previous copy. Browsing and search combine the roots in
memory. Reserved `.sprintboard` directories and legacy sidecars are excluded
from browsing and image processing.

Filesystem paths are stored relative to the image root. Move the database
with its collection to preserve tags; copying an individual image alone does
not carry its database records. Include `.sprintboard` in collection backups.
Stop Sprintboard before copying database files so SQLite can checkpoint and
close its WAL. Writes use `synchronous=FULL`.

## Indexing and refresh

Startup restores the last gallery view from `<cache_dir>/gallery-snapshot.json`
without walking the library or checking individual images. The server starts even
when that snapshot is missing or unusable, showing an empty gallery until the
first background scan completes. Startup scans today's date folders and the
previous four calendar days first, publishes those results, then immediately
reconciles the full library. The gallery shows its startup status and automatically
reloads the current folder, filters, and sort order after each startup phase.
Tagging and AVIF generation wait until the full reconciliation completes.
Cached results can briefly include deleted images or omit new ones.

After the startup scans, indexing uses three tasks:

- **Recent date-folder scanning** checks today's `YYYY-MM-DD` folders and the
  previous four local calendar days every five seconds by default, even without
  visits. It includes nested contents and newly created date folders, discovers
  new images, and reconciles deletions in that window. Archived date folders and
  undated image contents remain cached. Each pass recomputes the local date, so
  the window advances at midnight. New images enter the tagging and AVIF queues.
- **Folder discovery** walks the directory tree every five minutes. It updates
  the folder list without reading image contents, hashing files, or reconciling
  image and tag deletions. Unchanged discoveries do not rewrite the saved gallery.
- **Visited-folder scanning** checks the immediate contents of the opened folder.
  With recursive browsing enabled, it checks the entire selected subtree,
  including newly created folders and the first image in a previously empty
  folder. A recursive visit to the top-level gallery queues every configured root.
  Duplicate requests share one queued scan; recursive requests expand a pending
  shallow scan. Requests inside the minimum interval remain queued until due,
  while other eligible folders can proceed. Content probing uses at most four
  concurrent tasks on the index worker.

The open gallery requests a visit every 15 seconds, even when the index has not
changed. These requests do not replace the loaded pages. Index updates trigger
separate passive reads of the loaded page range. Visits and result updates pause
while a lightbox is open or the browser tab is hidden; already queued server work
can finish. Recursive visits inspect all descendant contents automatically, so
there is no additional folder-batch button.

The gallery shows cached results immediately and refreshes the loaded page range
when background work publishes an update. Refresh waits while a lightbox is open
or the browser tab is hidden. Unvisited contents outside the recent date window
remain cached: global search and counts can omit new images or retain externally
deleted images until their
folders are scanned. Manual refresh still reconciles the complete library.
A startup full reconciliation also remains necessary before tagging and encoding
begin; this scheduling change reduces recurring work, not initial indexing.

These settings reload without a restart and must be finite and at least one
second. The date-scan interval also accepts `0` to disable recurring date scans:

```json
{
  "image_index_date_refresh_interval_seconds": 5,
  "image_index_folder_discovery_interval_seconds": 300,
  "image_index_visit_refresh_interval_seconds": 60
}
```

The date-scan and discovery intervals are measured after each pass completes.
The visit interval is a minimum delay after a folder scan attempt. A queued
request runs when that delay expires, even if the browser has since left; it does
not schedule further scans without another visit request. The former
`image_index_full_refresh_interval_seconds` field
remains readable for compatibility but no longer schedules recurring full scans.
Disabling periodic date scans does not change startup: it still scans today and
the previous four local calendar days first, then the full library.
All discovery and content scans run serially on the dedicated index worker, so a
large individual folder or slow discovery can delay other queued visits.

Tagging and AVIF generation wait for the first full pass.
Unavailable roots retain their cached browsing entries, and
failed background scans retain the last published view and retry. The snapshot is
a disposable, atomically replaced browsing cache; it does not replace the SQLite
tag databases or authorize tagging and encoding. Invalid snapshots or changed
namespace-to-directory mappings cause an empty start and a background rebuild.
Keep `cache_dir` on a fast local disk to avoid slow snapshot reads.

`GET /api/images/index/status` returns a JSON `state` of `pending`, `running`,
`recent_ready`, `ready`, or `failed` for initial reconciliation. Once ready, it
stays ready through later periodic scans. `recent_ready` means the five-day startup
results are available while the full reconciliation is still pending or running.
The gallery polls every two seconds until ready (five seconds after failures).
The status response also includes an index `generation`; the gallery checks it
while open to pick up completed discovery and content scans. List requests use
`visit=0` for passive result refreshes and otherwise queue visited folders.
Responses retain `indexGeneration` and `unscannedFolderCount` for compatibility.
The latter counts discovered folders whose contents have not yet been scanned;
successful recursive scans clear their entire scope. The existing manual refresh
API remains available.

## Portable files and cloud sync

Image metadata stores file size, modification time as Unix nanoseconds, and
SHA-256. Unchanged size and time reuse metadata without reading image contents.
When only the timestamp changes (including sync-related precision changes), a
matching content hash reuses metadata and saves the new timestamp. New or changed
files are inspected and hashed. Relative paths and these portable fingerprints
allow Windows and macOS to reuse the same collection database.
Tag and proxy validation also saves timestamp-only changes after a matching hash,
so subsequent checks can reuse the size-and-time fast path.

For a Dropbox-synced collection, update Sprintboard on both computers before
opening a schema-eight database. Keep images and the hidden `.sprintboard` folder
available locally. Close Sprintboard on the current computer, let Dropbox finish
uploading, then wait for syncing to finish on the other computer before starting
Sprintboard there. Only run one copy against the collection at a time. Dropbox
does not merge SQLite transactions; concurrent or unsynced edits can create
[conflicted copies](https://help.dropbox.com/organize/conflicted-copy).
SQLite's [WAL files](https://www.sqlite.org/walformat.html) are part of an open
database, so syncing the main database alone while Sprintboard is running is not
a supported handoff. Do not delete WAL files manually.

## Fallback storage and write failures

When a root cannot host a writable database, Sprintboard uses
`<cache_dir>/<SHA-256 of canonical root path>/image-index.sqlite3`. A restart
tries the local directory again. Readable local and fallback tags are reconciled
by the latest analysis time, preferring local results on ties; retry state comes
from the latest attempt, and proxy records must validate against the file.
Back up an external database too when that is the active storage location.

If neither location can save tags, browsing and existing committed tags remain
available. New tagging and tag-dependent proxy generation pause for that root;
other healthy roots keep processing. Results whose writes fail after inference
remain pending in memory and are retried on refresh, without repeating inference.
New results become searchable and authorize proxies only after their database
commit succeeds. Pending results can be lost if the process exits before storage
recovers. Logs report pause, failure, and recovery; `persistent` is true only when
all configured roots have working persistent stores, and false with no roots.
Tagging-disabled proxy generation retains its existing behavior.

## Older database locations

The former working-directory `.sprintboard/image-index.sqlite3` remains untouched
and is not imported. Records present only in that old database require reanalysis.
Size and the portable Unix timestamp provide the normal fingerprint validation
fast path. If a copied file's timestamp changes, Sprintboard checks its SHA-256
digest before reusing analysis.

## AVIF work scheduling

AVIF generation uses a queue populated by image-content scans, committed tag
updates, and encoding settings changes. It no longer walks the library every
three seconds. Idle polling checks only the queue; new files and externally
removed proxies are picked up when their folders are scanned.

Each worker pass handles at most two images concurrently, with one encoder thread
per image. Failed jobs retry individually after 30 seconds, increasing to a
five-minute maximum. New scan or tag events can retry them sooner. Generation
still waits for startup reconciliation and, when tagging is enabled, committed
analysis. Disabling AVIF stops further jobs after the current batch finishes.

## AVIF metadata

The `.sprintboard.avif` suffix is reserved for generated proxies. Legacy
same-stem files such as `img.png` and `img.avif` are treated as independent
source images by the server. If a source is removed outside Sprintboard, its
remaining proxy continues to appear as an independent image after the next
index refresh.

The AVIF encoder embeds the scored analysis as a Sprintboard-specific
`tagAnalysis` JSON value in XMP while encoding directly from the source PNG.
It does not publish standard `dc:subject` keywords and does not put the source's
absolute path in the AVIF. No ExifTool process or post-encode metadata rewrite
is required.

## Deleting images

If the source is deleted but its managed proxy remains, Sprintboard keeps the
searchable SQLite analysis and reports the source as missing in Image Details.
If the source later returns, its fingerprint is checked before the
analysis is reused. Tag records are removed only after a successful scan confirms
that neither source nor proxy exists, or following gallery deletion. Temporarily
unavailable roots retain their last known state. The gallery delete action removes
the source, managed proxy, and their database tag records; legacy sidecars remain
untouched. It deletes only images: a request naming a non-image file, anything
under `.sprintboard`, or a proxy whose source is not an image is refused without
deleting anything. If a file cannot be removed, the request fails and the image
keeps its index entry and tag records.

After files are deleted successfully, the gallery entry is removed immediately.
Database and saved-snapshot cleanup runs on the index worker, so deletion does
not wait for a full library scan or a cache-wide storage check. In-progress scans
cannot restore entries awaiting deletion cleanup.

## Migrating legacy AVIF names

The optional [migration script](../scripts/migrate_avif_proxy_names.py) recursively renames
legacy AVIFs such as `img.avif` to `img.png.sprintboard.avif`. Edit its
`WORKING_DIRECTORIES` list before running it. It previews changes by default;
after reviewing the output, change `DRY_RUN` to `False` to apply them.

Stop Sprintboard before applying the migration. The script intentionally treats
every AVIF that does not already have the reserved suffix as a legacy proxy,
including genuine AVIF source images. Existing targets and ambiguous PNG pairs
are reported and never overwritten.
