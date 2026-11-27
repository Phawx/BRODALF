# BRODALF design

Agreed with the project owner on 2026-09-28, brought in line with the
owner's answers of 2026-11-25 in 0.7.0, and extended in 0.8.0 with the four
features the owner picked from what 0.7.0 left for later (tabs, starred
files and extra places, encrypted folders, several drives at once) and in
0.9.0 with the six 0.8.0 left (dark mode, translations, hidden names, a
compression helper, shadow copies when asked, the last database in
AppData).

## What it is

BRODALF keeps track of what has been backed up, and to where, from the
original folders. The `.brodalf` file (the database, also called the
catalog) holds no file data. It records every file and folder in the
backup roots (the folders the user chose to back up), every version BRODALF
has seen, and every place a copy of each version was written. The targets
are external and removable disks that are usually offline. (OneDrive and
Dropbox storage existed in 0.3.0 and was taken out in 0.4.0; see the
README.) Windows is the main platform and the GUI is the main way to use
it. Encryption is optional, per drive or per folder (see below).

The owner's answers of 2026-11-25 to 178 yes/no questions (kept with the
project as `brodalf-answers.md`) set what 0.7.0 and later do. Three rules from them
shape everything below: nothing on a drive is ever overwritten, moved or
deleted; nothing is dropped from the database without the user's say; and
nothing starts by itself except the scan when a database opens.

## Flow

1. Open BRODALF. Until a database is open, only New, Open and Recover are
   available.
2. A new database asks for its backup roots first (any number), then where
   to save the `.brodalf` file.
3. Opening a database scans the backup roots: path, size, modified time and
   a BLAKE3 checksum for every file. This scan is the one thing BRODALF
   starts by itself. A root whose folder is not there is left as it was and
   the user is asked where it is now; one that cannot be found is offered a
   restore from the drives instead.
4. Show the ghost tree. Everything starts greyed out.
5. Plug in a drive and back up. Files light up once a correct copy is on a
   connected drive.
6. Plugging a known drive in reads its `BRODALF.media`, free space and
   health data and lights up the matching files; unplugging greys them out.
   A drive whose oldest read-back is older than "check_days" offers a check.
   Otherwise nothing happens until the user presses a button.
7. BRODALF is not resident, schedules nothing and does not care what else
   runs on the PC.

## Nothing automatic

`brodalf.exe` takes one argument, a database to open; an argument that
starts with `--` makes it exit with 1 without a window. The options
"auto_backup", "guard_percent", "keep_versions" and "keep_days" of earlier
versions are gone; `bd_option_get` returns -1 for them. The options left are
"check_days" (180), "verify_days" (30), "reserve_mb" (512),
"low_priority" (1) and "parallel_copies" (0, since 0.8.0), plus the skip
lists and the copies wanted.

There is no preview window before a backup (answer 92). The GUI keeps one
line beside its buttons up to date with `bd_backup_preview` for every
plugged-in drive: what `bd_backup` would copy, how much, and whether it
fits. It reads only the catalog and the drive's free space. The preview also
counts the files whose copy on the drive is good and how many of them
changed or were deleted since (moves aside); a quarter or more of at least
50 is `mass_change`, and the GUI then asks before backing up, with a list
of the changed files (`bd_list_changed`). The GUI also asks before a backup
when nothing has been scanned yet, and says after a stopped scan that a
backup works from what the scans so far found.

`bd_media_status_get` says where a drive stands from the catalog alone:
live files whose current version, an older version or no version is on it
(with sizes), deleted files whose last version is on it, old versions in
`.versions`, bad and never-read-back copies, the last backup job and the
last check job. The `jobs` table keeps those (there is no job history to
browse any more). `bd_export_csv` writes every file with its drives as
UTF-8 CSV with a BOM; every text field is quoted and a leading
`= + - @`, tab or CR gets a `'` so a spreadsheet does not run it as a
formula.

Free space is tracked in `space_log(media_id, at_ms, total_bytes,
free_bytes, event)`, written when a drive is plugged in and before and
after every backup to it; `media` carries the latest reading.

With "low_priority" (the default) a job's threads run in Windows'
background mode (`THREAD_MODE_BACKGROUND_BEGIN`: low CPU, I/O and memory
priority), so the PC stays usable. Every job keeps the PC awake while it
runs.

## Moves and renames

Versions belong to paths, so a renamed folder looks like deleted files plus
new files with the same checksums. After every backup root is scanned, a
file new to the catalog whose hash and size match exactly one file that went
missing, and no other new file, is that file moved or renamed: the new node
takes the old one's versions, and with them its copies wherever they are on
the drives, `nodes.moved_from` records the old path, and the old node goes.
Nothing on a drive is moved (answer 74): the copy keeps its old path on the
drive and the catalog records it, so nothing is copied again. Pairs that are
not unique are left as a delete plus a new file.

On Windows names ignore letter case, so a path that only differs in case
from one the catalog knows (Photo.JPG that is now photo.jpg) is that same
file, renamed: the scan looks it up by `nodes.rel_fold` (the path folded
with Windows' own case mapping, indexed, and only looked at for paths the
catalog does not know exactly) and renames the node, keeping its versions
and copies. Elsewhere paths stay case sensitive and the key is not stored.

A backup root's folder on a drive sits next to `.versions`, `BRODALF.media`
and the catalog copies, so a root with one of those names (in any case)
gets a name with `_` in front (`_versions`).

A root is its folder structure: the files the catalog records at their
paths inside it. `sources.path` is only where it was last, and nothing
about the computer is kept. A root that moved as a whole (another user
name, another drive letter) is pointed at its new place with
`bd_source_relocate`, which changes only `sources.path`.

### Finding a root by its files

Before a scan walks its roots, each root whose folder is not there is
looked for (`bd_source_find`): the same folder under this user's home
folder when it was under another user's home folder next to it, the same
disk mounted for this user (`/media/<user>`, `/run/media/<user>`), and on
Windows the same folder on every other local drive letter (fixed,
removable and RAM disks; not network drives, which can hang). A folder
counts when at least half of the root's files the catalog checks are in it
at their path in the root with their size: up to 64 files with contents,
picked by checksum so the same ones are checked each time (a root without
any is checked by the names of its folders and empty files). When exactly
one folder counts, the root is relocated there, the scan goes on there and
`roots_found` counts it; when several do, the log names the best and
nothing is picked. A root whose folder is there is never moved by this:
when the folder is held (below) and another folder holds its files, the
log says so and the user decides.

### Is the folder at the path the root?

After the walk, the files the catalog knew in the root are sorted into
kept (where the catalog has them as it has them, or moved inside the root:
their checksum and size are those of a file new in this scan of it,
`temp.scan_new_hash`), changed (at their path, other contents) and lost
(neither). Files new in the root count for nothing. When `lost > 0`,
`kept * 10 <= known` and `lost > changed`, the scan of that root is rolled
back and it is listed like a missing root (`roots_emptied`,
`bd_source_info.files_gone` = lost) until the user says where it is,
restores it, or says the files were deleted (`bd_source_accept_gone`). An
empty folder made under the same name and a folder of other files with the
same path are both held; a folder whose files were all edited is not.

## Links and reparse points

The scan never follows links. On Windows only a reparse point that stands
in for another path (a symbolic link or a junction, the "name surrogate"
tags) is a link; every other reparse point is an ordinary file or folder
with extra plumbing behind it, above all OneDrive and Dropbox placeholders,
which carry the attribute even when fully downloaded. `bd_walk` reads the
tag from `WIN32_FIND_DATA.dwReserved0`; `bd_stat` asks `FindFirstFile` for
it when a path has the attribute. (Until 0.3.0 every reparse point was
skipped, which would have left a synced folder unprotected.)

## Read-back (verify) pass

Every copy row remembers when it was last fully read back (a copy written
again starts as never read back). A check (`bd_media_check`) reads every
copy and compares its size, time and checksum; there is no quick check any
more. A read back (`bd_media_verify`) does the same for the copies not read
back in "verify_days" days (default 30; 0 every copy), so a full check can
be spread over time. A copy found missing or damaged stays so until a
re-read matches its hash. `bd_media_connect` checks no copies; it only
identifies the drive and records its free space and health data.

## Restoring

`bd_restore` writes files as they are now, or with `deleted_only` the
deleted ones as last seen. Restoring one backup root puts its files straight
into the destination; restoring everything when there is more than one root
gives each root its own folder (`<dest>/<root>/<path>`). Nothing in the
destination is ever overwritten: a file there with the right size and hash
counts as already there, and any other file is left alone and recorded in
`temp.restore_conflicts`, which `bd_list_restore_conflicts` lists until the
next restore. `bd_restore_version` restores one version of one file. A
restore plan orders the drives a restore needs, plugged-in ones first, then
by how many files each covers. Restoring as of a date was taken out in
0.7.0.

## Files in use

Opening a file for reading shares read, write and delete. A sharing or lock
violation is reported through `bd_open_was_in_use()`, the scan or backup
records the path in the temp table `in_use` and leaves the catalog as it
was; `bd_list_in_use` lists them and the GUI says to close the program that
holds them and scan again. (0.5.0 read such files from a VSS shadow copy
through an elevated helper; 0.6.0 took that out, and 0.9.0 brings it back
as an offer, below.)

Since 0.9.0 the app then offers a shadow copy (`src/shadow.c`), never
starting one by itself. On yes it writes the list of files in use to a
new folder under `%TEMP%`, hashes it with BLAKE3 and starts itself
elevated (`ShellExecuteEx` with `runas`) as `brodalf.exe --shadow-copy
<out> <list> <hash>`. The helper runs before COM is initialised, checks
the list against the hash on its command line, accepts only plain
`X:\...` paths inside the backup roots the list names (no `.`/`..`, no
streams, no links or junctions leading elsewhere), makes a shadow copy of
just the volumes involved, copies the files into a folder of its own that
only administrators can write, writes a result file and lets VSS delete
the snapshot. The app then scans with each path read from its copy
(`bd_substitute_add`), backs up again where the backup was going, and
deletes the copies. The CLI's `scan --shadow-copy` and `backup
--shadow-copy` do the same in-process when run as administrator.

OneDrive "online-only" files (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` or
`RECALL_ON_OPEN`) are not read, so they are not downloaded: the scan counts
them in `files_online_only` and leaves the file's catalog entry as it was,
and a file first seen online-only gets its first version once it is on the
PC. A backup does not copy them either.

## Progress

Long jobs report through `bd_catalog_set_progress`: phase (scan, backup,
verify, check, restore), files done and expected, bytes done and expected,
and the current path, about ten times a second; the hash loop adds bytes
as it reads. The GUI shows a job bar with a percentage (bytes when the total
is known, else files, else a marquee), the counts, a data rate over the
last few seconds, and a second bar for the file being copied or read.

## Stopping a job

`bd_catalog_cancel` sets a flag on the catalog that any thread may set; each
job clears it when it starts and returns `BD_ERR_CANCELLED` when it saw it.
Scan, backup, check, read-back and restore look at it between files, and a
copy or hash looks at it between 1 MiB blocks (the scan's hashing threads
too). Stopping keeps everything already done: a scan drops only what it
found in the backup root it was in, which reads as it did before; a backup
still syncs, renames and records the copies already written, and a copy cut
off halfway is removed, never renamed into place; a check keeps the results
so far and leaves the copy it was reading as it was; a restore removes the
file it was writing. The job is recorded as `stopped`. The GUI's Stop button
and Close (which asks first) set the flag and drop the queued jobs, and the
GUI asks again from the progress callback, so a request made between two
steps of a job is not lost when the next step clears the flag.

## Speed

- BLAKE3 is built with its SSE2, SSE4.1, AVX2 and AVX-512 code on x86-64
  (the C intrinsics files from BLAKE3 1.5.4) and picks the best the CPU has
  at run time; other targets use the portable code. `tests/test_blake3.c`
  checks the official test vectors and every path against the portable one.
- A copy longer than four 1 MiB blocks hands its blocks to a writer thread
  through a ring of three buffers (`bd_hash_copy`), so reading and hashing
  one block overlaps writing or sealing the previous one. Smaller files use
  the plain loop and start no thread.
- A scan hashes new and changed files on a few worker threads (one fewer
  than the processors, at most four, at least one), started with the first
  file that needs hashing. Results come back in walk order through a
  bounded ring of 64 slots and only the scanning thread writes the catalog,
  so it ends up exactly as a one-at-a-time scan leaves it. Workers pass
  their byte counts to the scanning thread (`bd_io_redirect`), which alone
  calls the progress callback.

## Ghost tree states

| State | Meaning |
| --- | --- |
| Available | A good copy of the current version is on connected storage |
| Older version available | The file changed since its last backup; only older versions are reachable |
| Offline | Copies exist but none of their drives is connected; the UI names a drive to plug in |
| No copy | Never backed up |
| Missing or damaged | The drive is connected but the copy is gone or fails its check |
| Deleted | Gone from the source folder; its copies are still tracked |
| Partly available | Folders only |

A version number is shown only when a file has more than one version.

## Drives

Each drive gets `BRODALF/<catalog id>/BRODALF.media`, holding a random drive
ID. Drives are recognised by that ID, not by drive letter.

Copies carry the original file's modified time, and so do restored files
(the catalog's time for that version). The time recorded for a copy is
always the one read back from the drive after it was renamed into place, so
a check compares the drive with itself: it works the same on drives that
round times (FAT keeps 2 seconds).

A drive can be marked lost or broken (`bd_media_mark_lost`, `media.lost_why`
and `media.retired_ms`; a drive retired by 0.5.0's Replace drive opens as
broken). Its copies stop counting (`BD_SQL_COUNTS`), so the full copies
drop and the files it held are backed up again to other drives; nothing is
backed up to it, and its copies still restore if it is plugged in.
`bd_media_found` undoes it. Replace drive was taken out in 0.7.0.

The GUI offers to set up a disk it does not know when one is plugged in
(not the system disk); "Never ask about this disk" keeps the volume serial
under `HKCU\Software\BRODALF\IgnoredDisks`.

## Layout on a drive

```
X:\BRODALF\<catalog id>\BRODALF.media
X:\BRODALF\<catalog id>\<root name>\<path>              first copy of a file on this drive
X:\BRODALF\<catalog id>\.versions\<root name>\<dir>\<stem>.v<N><ext>   later versions
X:\BRODALF\<catalog id>\catalog-r<revision>.brodalf     copies of the database
```

Files stay browsable without BRODALF, unless the drive hides names (an
encrypted drive set up so, since 0.9.0): then each part of the copies'
paths below `<catalog id>`, `<root name>` and `.versions` included, is 24
letters and digits (see Encryption), while `BRODALF.media` and the
database copies keep their names. Nothing on a drive is ever
overwritten, moved or deleted (answers 95 and 97). A version goes to the
file's usual path when nothing is there; otherwise (an older version's copy,
a damaged copy, or a file BRODALF did not write) it goes alongside, under
`.versions` as `<stem>.v<N><ext>`, or `<stem>.v<N>-2<ext>` and so on if
that is taken. A copy
row whose file has gone from the drive is kept and marked missing. A file
already at the path that the catalog does not know about is adopted if its
hash matches (for example after a crash before the catalog was saved);
otherwise it is left alone. A file whose size or time changed since the scan
is read again, and its new version is recorded and copied.

Every copy is written to `<path>.brodalf-tmp` while it is hashed, checked
against the hash, synced, and only then renamed into place, so a failed
write never leaves a file that looks good (kept against answer 102, which
suggested writing directly: a cut-off direct write looks like a real file).
Copies are synced in groups (up to the 256 files of a batch or about 64 MB):
every temp file of the group is written and checked, then the group is
synced (`bd_fsync_paths`: on Windows each file is written out without a
drive cache flush and one full flush ends the group; elsewhere one fsync
each), then each copy is renamed into place and recorded, then on POSIX the
folders that got new names are synced. A batch's catalog rows are committed
in one transaction after its last group, so no copy is recorded before its
data is on the drive.

A backup never writes into the last "reserve_mb" of a drive (default
512 MB), which keeps room for the database's own copies; a file that
doesn't fit is counted as "no room" and skipped, and waits for another
drive. Every drive gets whatever it is missing, so a large set of folders
spreads across drives with no follow-up mode.

At the end of every backup the database is saved and copied to the drive as
`catalog-r<revision>.brodalf` (`bd_catalog_revision`: how many times the
database has been saved), never over an existing copy. An encrypted drive
always gets an encrypted copy.

A scan also leaves out BRODALF's own store inside a backup root: a folder
named `BRODALF` that holds this catalog's `BRODALF.media` (an extra place
inside a backed-up Dropbox folder, say). Any other folder of that name is
scanned as usual.

Scans apply two skip lists: the catalog's ("skip_list", default: temp
files, system files and rebuildable caches such as `node_modules/`) and
each root's own (`bd_source_skip_set`). Patterns are case-insensitive globs
matched against each name, or against the path from the backup root when
they contain a `/`; a trailing `/` means folders. A file that a list newly
leaves out is marked deleted, for the user to keep tracking or forget.

## The .brodalf file

16-byte header (`BRODALF\x1a`, format version, flags) followed by one zstd
stream of a SQLite database. While open, a plain catalog is unpacked to a
working copy in the temp folder (WAL mode). An encrypted one is unpacked into
memory instead (`sqlite3_deserialize`), so nothing of it is ever written to the
disk unencrypted, not even by a crash: the save serializes it under the
connection's mutex (refused while a write transaction is open) and seals it
straight from memory, `PRAGMA secure_delete` keeps its free pages zeroed (so
they compress away without a VACUUM), and the memory database's 1 GiB limit is
lifted. Turning encryption on moves an open catalog into memory and deletes its
working copy. For every catalog SQLite keeps its scratch data in memory
(`temp_store=MEMORY`; a memory database's journal is in memory too). The key's
memory is locked out of the page file, and a process with a catalog in memory
leaves the heap out of Windows Error Reporting's dumps (no core files
elsewhere). On Windows a named mutex, `Local\BRODALF-catalog-<hash>` (the
first 16 bytes of the BLAKE3 hash of the catalog's full path, made long,
upper-cased and without the `\\?\` prefix), keeps a second BRODALF on the
same PC from opening it, and `<file>.lock` is held open without sharing and
deleted on close, which also keeps out a BRODALF on another PC when the
catalog is on a share. Windows drops both when the process ends, so a crash
leaves no stale lock. On other systems (tests, CLI) `<file>.lock` is created
exclusively and removed on close. Saving
writes a new file and swaps it in atomically.

Tables: `sources`, `nodes`, `versions`, `media`, `media_hardware`, `copies`,
`space_log`, `jobs`, `settings`, `meta` (and `cloud_accounts` in catalogs
written by 0.3.0), `stars`, `encrypt_paths` and `damaged_files` (0.10.0:
what the last bit-rot scan found, kept after BRODALF closes; it is created
when a catalog opens and needs no new schema version). Schema version 8,
or 9 once a drive or extra place hides names; see below.
Version 6 added `nodes.rel_fold`, the
case-folded path used on Windows. Version 7 (0.7.0) added
`sources.active`, `nodes.deleted_ack`, `nodes.moved_from`,
`nodes.btime_ns` (the file's creation time, given to copies and restored
files on Windows) and `media.lost_why`,
and the `revision` in `meta`. Version 8 (0.8.0) added `media.secondary`
and `media.limit_bytes` (extra places), `copies.encrypted` (whether that
copy is sealed, since a plain drive can now hold encrypted copies), the
`stars` and `encrypt_paths` tables and the "parallel_copies" option.
Version 9 (0.9.0) adds `media.hide_names`; the column is added to every
catalog 0.9.0 opens, but the catalog is still saved as version 8, which
0.8.0 opens, until some drive or extra place hides names. An older catalog
is upgraded in place when it is opened; the version before then refuses
it as newer.

The database is kept small: no file data, only paths, sizes, times,
checksums and where each copy is, compressed with zstd, so a search works
with the database on its own.

## Recovering a lost database

`bd_find_catalog_copies` lists, for each catalog folder on a drive, its
newest copy: the highest `catalog-r<revision>.brodalf`, or
`catalog-backup.brodalf` (revision 0) on a drive written by 0.6.0 or older.
Revisions of one catalog compare across drives. The GUI's "Recover the
database from a backup drive" looks on every drive letter (C: to Z:, not
CD drives), offers the newest copy of each catalog, or a folder the user
picks, and `bd_catalog_recover` copies it to a new file the user names
(never over an existing one; written as `<name>.recovering` and renamed at
the end). It is then opened as usual (passphrase asked if the copy is
encrypted), which scans the backup roots.

A backup root whose folder is not there, and is not found by its files, is
left as it was by the scan (its files are not marked deleted) and recorded
in `temp.missing_sources`, which `bd_source_info.missing` reports. The
GUI asks where it is now (offering the folder `bd_source_find` gives, with
how many of the files it checked are there); `bd_source_relocate` changes only
`sources.path`, so the root keeps its name on the drives and its files keep
their versions and copies. "I cannot find it" offers a restore of that root
from the drives, with the drives it needs in order. Tools > "Change where
a backup root is" does the same at any time.

## Copies wanted and full copies

The catalog's settings hold a target of N copies in M places (default 2 and
2). For every live file in a root still backed up, the current version's
good copies are counted by distinct drive (drives marked lost or broken do
not count), and places by distinct place key: a drive's location (trimmed,
ignoring case; empty for all drives with none set; a cloud account left by
0.3.0 keeps a key of its own). A file is at risk when copies < N or
places < M. Offline copies count: the target is about what exists, not what
is plugged in.

`bd_backup_sets` gives the progress toward each full copy: copy k is done
when every live file, as the last scan found it, has at least k good copies
that count. A new or changed file lowers every one until it is backed up
again. The GUI shows one bar per full copy, up to N.

`bd_list_pending` lists what the next backup would copy, for one drive (or
extra place) or, with no drive, every file short of the target, with its
copies and places, whether it is starred or in an encrypted folder. The
GUI's Next backup tab shows it with one column per full copy.

## Starred files and extra places

The `stars` table holds starred files and folders by backup root and path
(`''` is the whole root; a folder covers everything under it).
`bd_starred` says whether a path is starred itself (1) or through a folder
above it (2).

An extra place is a `media` row with `secondary = 1`
(`BD_MEDIA_SECONDARY`): a folder such as a Dropbox or OneDrive folder, a
network share or a folder on another disk (answer 54). `bd_place_add` sets
it up like a drive (`BRODALF/<catalog id>/BRODALF.media`, plain copies in
their folders, the catalog's own copy at the end of a backup) and refuses
a description that is too short to tell places apart: at least two words
and 10 characters, 9 of them not spaces. `limit_bytes` caps the space
starred files may use there (`bd_place_set_limit`); a file that would go
past it is counted as over the limit and waits. A place can be encrypted
like a drive. BRODALF talks to no cloud service (answer 13): the folder's
own app syncs it.

A backup to an extra place copies only starred files. Its copies restore
like any other, but they never count toward the copies wanted or the full
copies (`BD_SQL_COUNTS` leaves secondary media out), and no "which drive
would help" suggestion points at one. `bd_place_status_get` says, from the
catalog, how many starred files are there as they are now, what is
waiting, what would go past the limit and the space used.

## Compressing a drive

Optional, and only when the person asks (0.9.0, `src/compress.c`):
`bd_media_set_compression` turns Windows' own NTFS compression on or off
for `<drive>/BRODALF/<catalog uuid>` and every folder and file in it
(`FSCTL_SET_COMPRESSION`). The copies stay ordinary files any Windows PC
reads; a compressed folder makes files copied into it later compressed
too, so later backups need nothing more. It walks the folder twice, once
to count for the progress bar, then to change and measure each file
(size on disk), and records a "compress" or "uncompress" job. A file
another program holds open is counted as failed and left as it was. It
is refused on an encrypted drive (sealed copies cannot be made smaller),
on exFAT or FAT32 (`bd_fs_can_compress`) and outside Windows; making an
encrypted drive's files whole again is allowed. `bd_media_compression`
says whether it can be and is on. Window: a drive's menu and the Local
backups tab; CLI: `brodalf-cli compress`.

## Several drives at once

`bd_backup` backs up to one drive. `bd_backup_many` (up to 16 connected
drives or extra places) reads each file once and writes it to every drive
that needs it at the same time (`bd_hash_copy_multi`, with one writer
thread per drive for a big file); each drive gets the
same rules as `bd_backup` (space kept free, never overwriting, the
catalog's copy at the end) and its own stats. A drive that fails to write
a file does not stop the others. It loads the PC more while it runs (the
USB bus, memory and processor are shared) and goes at the pace of the
slowest drive, so the app uses it only when "parallel_copies" is on
(answer 86); otherwise "Back up to every plugged-in drive" calls
`bd_backup` for one drive after another, stopping on Stop or a passphrase
error. The command line does the same with `backup <catalog> <drive>...`,
and `--parallel` turns it on for one run.

## Deleted files

A file removed from its backup root keeps its node (`deleted<>0`,
`last_seen_ms` the last scan that found it) and its versions and copies.
Nothing is dropped from the database without the user's say (answer 17).
After a scan the GUI says how many files went since the user last looked
(`nodes.deleted_ack=0`) and offers the deleted-files list, where the user
can keep tracking them (`bd_deleted_ack`) or forget them
(`bd_deleted_forget`, which removes them from the database only; their
copies stay on the drives). A file that comes back clears its mark.
`bd_list_deleted` lists them with the drives that hold them. Restoring with
`deleted_only` brings back only deleted files, each as it was last seen.

A bit-rot scan (`bd_scan_ex` with `bitrot`) also reads files whose size and
time did not change. One whose hash changed anyway is not taken as a new
version; it is listed in `temp.damaged` (`bd_list_damaged`) for the user to
restore from a drive or accept as changed on purpose (`bd_damaged_accept`).

A backup root can be stopped (`bd_source_set_active`, `sources.active`): it
is not scanned or backed up and its files do not count toward the full
copies, but they stay in the database and their copies restore.

## Drive hardware

`media.location` holds the optional "where it is kept" text. `media_hardware`
holds the last reading of each drive, taken whenever it is plugged in
(`src/drive_hw.c`): on Windows the volume is mapped to its physical disk
(`IOCTL_STORAGE_GET_DEVICE_NUMBER`), then `IOCTL_STORAGE_QUERY_PROPERTY` gives
vendor, model, firmware, serial and bus; `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`
the size; the temperature property (`StorageDeviceTemperatureProperty`, 52)
the temperature; `IOCTL_STORAGE_PREDICT_FAILURE` the failure prediction and,
for ATA and SATA disks only, the ATA SMART attribute table (ids 5, 9, 12,
190/194, 197, 198, 231; other buses put something else in those bytes); for
NVMe disks only the NVMe health log page through the protocol-specific query; and, as a fallback that needs
administrator rights, `SMART_RCV_DRIVE_DATA`. Health is "failing" when the
drive predicts failure, "warning" with any reallocated, pending or
unreadable sectors, NVMe critical-warning bits or 100% wear, else "good".
The drive's identity stays the `BRODALF.media` UUID; a serial number that
changes under the same UUID is logged.

## Cloud (removed in 0.4.0)

OneDrive and Dropbox storage was built in 0.3.0 and taken out in 0.4.0; the
code is in git history at tag `v0.3.0`. What remains: `media.kind` can still
be `onedrive` or `dropbox` in a catalog written by 0.3.0 (such a row is never
connected, is shown as unsupported, and its copies count as a place of their
own), the empty `cloud_accounts` table in those catalogs, and the unused
`copies.stored_rev` column. Storage still goes through the one interface in
`src/store.h` (stat, local path, download, staging path, upload, move,
remove, space), which local drives implement with plain file operations.

## Encryption

Optional and off unless chosen (answer 78), and it encrypts file contents:
names and folders stay readable so a drive can still be browsed (Phawx's
call, 2026-09-28), unless the drive hides names (answer 79, built in
0.9.0, an option and off unless chosen).
Built on [Monocypher](https://monocypher.org) 4.0.2, two vendored files.
There are four kinds, each on its own: an encrypted drive or extra place,
encrypted folders (since 0.8.0), the `.brodalf` file, and the database
copies on an encrypted drive.

- A catalog can have one passphrase. It protects a random 32-byte master key:
  Argon2id (64 MiB, 3 passes) turns the passphrase into a key that wraps the
  master key with XChaCha20-Poly1305. The wrapped key (the 96-byte "key
  block": salt, cost, nonce, MAC, wrapped key) is kept in `settings`. A wrong
  passphrase fails to unwrap. Changing the passphrase rewraps the same master
  key, so nothing on the drives needs rewriting.
- A drive set up as encrypted (`media.encrypted`, and `encrypted=1` in
  `BRODALF.media`) stores every copy as `<name>.bdenc`. Older versions in
  `.versions` become `<stem>.v<N><ext>.bdenc`. This can't be changed later.
- An encrypted file is a 40-byte header (`BRDLFENC`, version, chunk size,
  random 24-byte nonce), then 64 KiB chunks, each with a 16-byte MAC. Every
  chunk is authenticated with the header and a "last chunk" flag, and the
  stream rekeys after each chunk, so tampering, reordering and truncation all
  show up as a damaged copy.
- Encrypted folders (0.8.0): the `encrypt_paths` table holds files and
  folders, by backup root and path, whose copies are sealed on every drive
  and extra place, plain ones included (`bd_encrypt_path_set`,
  `bd_encrypt_marked`). Each copy records whether it is sealed in
  `copies.encrypted`; a sealed copy on a plain drive is `<name>.bdenc`
  like one on an encrypted drive. Copies written before a folder was
  chosen stay plain, since nothing on a drive is overwritten; new copies
  and new versions are sealed. Taking a folder off the list makes the next
  new copies plain. A backup that would seal a copy needs the passphrase.
- Hidden names (0.9.0, `src/names.c`): an encrypted drive or extra place
  set up with `BD_MEDIA_HIDE_NAMES` (`media.hide_names`, and
  `hide_names=1` in `BRODALF.media`) stores every path under
  `BRODALF/<catalog uuid>/` with each part replaced by 24 base32
  characters: the first 15 bytes of BLAKE3 keyed by a drive key, over the
  hidden path of the folder above, a 0 byte and the real name. The drive
  key is BLAKE3 keyed by the master key over "BRODALF hidden names v1" and
  the drive's UUID, so two drives never share names, and a file always
  gets the same name, which is how a backup finds what is already there. A
  file keeps `.bdenc` at the end. The real names stay in the catalog, whose copy
  on the drive is encrypted. Only an encrypted drive can hide names, and
  it is fixed when the drive is set up. Check and backup messages show the
  real names.
- What it costs (answer 77): `bd_crypto_speed` measures once, for about a
  quarter of a second, how many bytes a second this PC seals, and
  remembers it. `bd_encrypt_cost_text` turns that into a few sentences for
  a given size: sealing overlaps the write, so against a typical USB hard
  drive (taken as 120 MB/s) a backup takes about as long as before unless
  the PC seals slower than that, in which case it says how much it adds;
  checking and restoring decrypt at about the same speed; sealed copies
  take about 0.1% more space. `bd_encrypt_stats_get` gives the files and
  bytes chosen, sealed and plain copies of them, files with no sealed copy
  yet and the encrypted drives in use. The GUI shows both on the
  Encryption tab, for everything and per folder.
- Hashes in the catalog are always of the plaintext, so a copy on an
  encrypted drive and one on a plain drive are the same version.
- The key is needed to write, check or restore an encrypted copy, not to
  see it in the tree.
- The `.brodalf` file can be encrypted too (flag bit 0 in the header). The
  header is then followed by the key block, and the zstd stream is sealed with
  the master key in the same chunk format. The database copy on an encrypted
  drive is always written encrypted.
- The master key, salts and nonces come from the system's random source
  (BCryptGenRandom, getrandom). If it fails, nothing is made or written: no
  key block is stored, the old one stays, a sealed copy or catalog file is
  not started, so a predictable key or a repeated nonce can never happen.
- A key block's Argon2 settings are checked before any work: memory 8 MiB
  to 1 GiB and 1 to 16 passes (BRODALF writes 64 MiB and 3; it always uses
  one lane, which the block does not store). Anything else is refused as a
  damaged catalog instead of trying to allocate 4 GiB or run for hours.
- A recovery key is a second way to unlock: 30 characters from
  `23456789ABCDEFGHJKLMNPQRSTUVWXYZ` (no 0, 1, I or O; 150 bits) in groups of
  five, made only when asked for (the recovery sheet) and shown once. It wraps
  the same master key the same way as the passphrase (Argon2id with its own
  salt, then XChaCha20-Poly1305); the block is kept as `recovery_block` in
  `settings`, and an encrypted `.brodalf` file also carries it in the header
  after the key block, with flag bit 1 set, so it opens a database copy
  when the passphrase is lost. BRODALF before this change refuses such a file as a newer format.
  Typed keys ignore case, dashes and spaces. A new recovery key replaces the
  old one; changing the passphrase keeps it.

## Recovery sheet

`bd_write_recovery_sheet` writes one self-contained HTML page (no scripts,
images or links; every catalog string HTML-escaped): what to do when the catalog file is lost,
each drive with where it is kept, size, free space as last seen, the
backup roots it holds current versions of with file counts and sizes,
and its last backup (from `jobs`), the backup roots, and the recovery
key when one was just made. The passphrase is never written. The GUI saves
it where the user chooses, opens it in the browser for printing and says to
delete the file afterwards; making a key also puts a new database copy
on every plugged-in drive so they know the new key.

## GUI

Native Win32 in `gui/main.c`, with comctl32 v6 and a DPI-aware manifest.

- Menus (answer 152: standard menus, and tabs for the rest): File (new,
  open, recover the database, recovery sheet, CSV export, exit), View (the
  six tabs as radio items, status of every drive, files short of your
  copies, largest, deleted, damaged and in-use files, what the last
  restore left alone, refresh), Tools (add a backup root, scan, bit-rot
  scan, change where a root is, back up, back up to every plugged-in drive,
  check a drive, restore, restore deleted files, set up a disk, add an
  extra place, star, encrypt, a submenu of every drive and extra place with
  its own submenu, settings) and Help (log, reports, about). The drives
  submenu is filled on `WM_INITMENUPOPUP`. File is greyed out while a job
  runs; Tools without a database or while a job runs; View and Help never,
  though View's lists are greyed without a database or during a job.
  Right-click menus on the tree (Explorer, restore, star, encrypt; for a
  root: stop or back up again, its skip list, where it is) and on the
  versions list (restore this version, open its copy in Explorer).
- Tabs (0.8.0): a tab control below the buttons and full-copy bars, with
  one page window per tab (class `BrodalfPage`, `WS_EX_CONTROLPARENT`). A
  page forwards `WM_COMMAND`,
  `WM_NOTIFY` and `WM_CONTEXTMENU` to the main window. The last tab shown
  is kept in `HKCU\Software\BRODALF\LastTab`. Pages are filled from the
  catalog only when no job runs (the catalog belongs to the worker then)
  and are marked stale when a job ends, a database opens or closes, or the
  selection changes. Their buttons are greyed without a database and
  during a job, like the main buttons.
  - Primary Database: the tree, search, results and detail panel, with
    buttons for the database's lists and tools. Starred and encrypted
    entries show "★ starred" and "[encrypted]" after their names.
  - Next backup: a drive, an extra place or any drive in a combo box, a
    summary line, and `bd_list_pending` as a table with a column per full
    copy (answer 93), plus starred and encrypted columns.
  - Local backups: every drive with size, free, BRODALF's copies, other
    files, kept free, health, temperature, hours on, wear, oldest
    read-back, copies and problems (answer 66), and buttons for every
    drive action, plus the "parallel_copies" tick box.
  - Starred files and extra places: the stars, the extra places with
    their status, and the "Extra place for starred files" dialog
    (description, limit, encrypt), which refuses a description that is
    too short.
  - Encryption: passphrase, forgetting it, recovery key and sheet, an
    encrypted drive, the database file, and the encrypted folders with
    what each costs in time.
  - Restore: restore what is selected, everything, deleted files, carry on
    a restore that waits for drives, the plan, and the files the last
    restore left alone beside the backed-up size and time (answer 116).
- "Back up" offers every plugged-in drive and, with two or more, "All N of
  them", which calls `bd_backup_many` with "parallel_copies" on and
  `bd_backup` drive by drive otherwise, then one log line per drive.
- Beside the buttons, one line per plugged-in drive says what a backup to
  it would copy and how the extra places stand. Under them, one bar per
  full copy; under the tabs, the job bar with Stop and its counts, the file
  bar, then the log.
- Startup: the last database, another one, a new one or recovery. The last
  database is kept in `%LOCALAPPDATA%\BRODALF\last-database.txt`
  (`bd_last_database`, since 0.9.0) and still in
  `HKCU\Software\BRODALF\LastCatalog`, where 0.8.0 and older look; the
  file wins. With no database open only Open database and New database
  (and File's recovery) work; every other button, the tab pages and Tools
  are greyed out. A path on the command line opens directly. A database
  open in another BRODALF window is refused with a message naming it.
- Appearance (0.9.0): View > Appearance is like Windows (its app mode,
  `AppsUseLightTheme`), Light or Dark, kept in
  `HKCU\Software\BRODALF\Appearance` (0, 1, 2). Dark uses the
  undocumented uxtheme exports that Explorer uses (ordinals 133, 135, 136,
  looked up at run time and skipped when absent), `DwmSetWindowAttribute`
  for the title bar, the `DarkMode_Explorer`/`DarkMode_CFD`/
  `DarkMode_ItemsView` themes for the common controls, and BRODALF's own
  colours for dialogs, static text, tabs and the status bar. Message boxes
  and the common question dialogs stay light. High contrast always uses
  the system's colours, and a change of the Windows setting applies while
  BRODALF runs.
- Languages (0.9.0): every piece of text the window shows goes through
  `T(L"...")`, `TN(n, L"one", L"many")` for text with a count, or `N_()` in
  a table whose entries get `T()` where they are shown. Translations are
  gettext `.po` files in `gui/lang/` (German and Spanish so far, machine
  translated), embedded in the exe as RCDATA and read at start: entries
  marked fuzzy or with a context are skipped, the plural rule must be
  `(n != 1)` or `(n > 1)`, and an entry whose %-codes differ from the
  English in number, order or kind is dropped and the English shown, so a
  bad translation cannot crash a format call. Dialogs from `brodalf.rc`
  are translated control by control when they open. The language is View
  > Language, kept in `HKCU\Software\BRODALF\Language` (empty: Windows'
  display language), and applies at the next start. `tools/lang.py` makes
  `brodalf.pot` from the source, updates the `.po` files, and checks them
  and every format call's argument count (CI). The engine's own messages,
  the CLI and the installer stay English.
- The tree loads folders lazily and draws each state with custom draw: grey
  italic for no reachable copy, amber for older version only, red bold for a
  bad copy, strikethrough for deleted. Folder and root rows show "x of y
  available" and, when offline, the drive that holds the copies.
- The detail panel lists every version and each copy: drive, state, last
  checked, size and path on the drive.
- Scan, backup, check and restore run one at a time on a worker thread, which
  reports progress and saves the catalog after each job. The tree is
  refreshed when a job ends and cannot be expanded while one runs, since the
  catalog belongs to the worker then. The reverse holds too: while the UI
  thread is in a command or in what follows a job (its end-of-job prompts),
  no job starts; a drive plugged in meanwhile is queued and handled when the
  UI is done.
- Drives: on startup and on `WM_DEVICECHANGE`, drives that disappeared are
  disconnected and every drive letter with a `BRODALF.media` for this
  database is connected (read only: identity, free space, health data). A
  drive due for a check offers one, once a session; a disk BRODALF does not
  know offers to be set up. Nothing is started without the user.

## Installer

`installer/brodalf.nsi` (NSIS 3, `makensis -DVERSION=x.y.z`) installs
brodalf.exe, brodalf-cli.exe and THIRD-PARTY-NOTICES.txt for the current
user into `%LOCALAPPDATA%\Programs\BRODALF` with no administrator rights
(`RequestExecutionLevel user`), adds a Start menu shortcut, a desktop
shortcut if the user ticks it on the components page, and an uninstall
entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\BRODALF`.
The uninstaller removes only those files, the shortcuts and that key: catalogs,
the app log and `HKCU\Software\BRODALF` are left,
since they belong to the user's catalogs. The installer is not signed.
