# BRODALF design

Agreed with the project owner on 2026-09-28.

## What it is

BRODALF is a catalog of where your bits live. The `.brodalf` file holds no file
data. It records every file and folder you protect, every version BRODALF has
seen, and every place a copy of each version was written. The main targets are
hard drives that are usually offline; OneDrive and Dropbox come later as
optional hooks. Windows is the main platform and the GUI is the main way to use
it. Encryption is optional, per drive (see below).

## Flow

1. Open BRODALF. It asks for a `.brodalf` file, or creates a new one.
2. Choose the folders to protect.
3. Scan them: path, size, modified time and a BLAKE3 checksum for every file.
4. Show the ghost tree. Everything starts greyed out.
5. Plug in a drive and back up. Files light up once a correct copy is on a
   connected drive.
6. Later sessions rescan and watch for drives. Plugging a known drive in checks
   its copies and lights up the matching files; unplugging greys them out.

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

Checking a copy:

- **Quick** (on every connect): the file exists with the recorded size and
  modified time. If only the time differs, it is rehashed.
- **Full** (on demand): every copy is rehashed.

## Layout on a drive

```
X:\BRODALF\<catalog id>\BRODALF.media
X:\BRODALF\<catalog id>\<source name>\<path>              newest version on this drive
X:\BRODALF\<catalog id>\.versions\<source name>\<dir>\<name>.v<N><ext>   older versions
X:\BRODALF\<catalog id>\catalog-backup.brodalf           copy of the catalog
```

Files stay browsable without BRODALF. A new version is written to a temp file
and checked against its hash first; only then is the previous copy moved into
`.versions` and the new one renamed into place, so a failed write can never
destroy a good copy. A file already at the destination that the catalog does
not know about is adopted if its hash matches (for example after a crash
before the catalog was saved), otherwise it is moved aside, never overwritten.

## The .brodalf file

16-byte header (`BRODALF\x1a`, format version, flags) followed by one zstd
stream of a SQLite database. While open it is unpacked to a working copy in the
temp folder and `<file>.lock` prevents a second window from editing it. Saving
writes a new file and swaps it in atomically.

Tables: `sources`, `nodes`, `versions`, `media`, `cloud_accounts`, `copies`,
`jobs`, `settings`, `meta`.

## Cloud (next)

OneDrive and Dropbox first. The catalog records provider, username and root
path. Sign-in goes through the provider's browser login; BRODALF saves the
refresh token in Windows Credential Manager and the catalog only keeps a
reference to it. A local synced OneDrive or Dropbox folder can also be used as
a plain drive today.

## Encryption

Optional, per drive, and it encrypts file contents only: names and folders
stay readable so a drive can still be browsed (Phawx's call, 2026-09-28).
Built on [Monocypher](https://monocypher.org) 4.0.2, two vendored files.

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
- Hashes in the catalog are always of the plaintext, so a copy on an
  encrypted drive and one on a plain drive are the same version.
- The key is needed to write, full-check or restore an encrypted copy, not to
  see it in the tree. A quick check works without it (size and time), and a
  copy that was touched since it was written is left as it was until a check
  with the passphrase.
- The `.brodalf` file can be encrypted too (flag bit 0 in the header). The
  header is then followed by the key block, and the zstd stream is sealed with
  the master key in the same chunk format. The catalog backup on an encrypted
  drive is always written encrypted.

## GUI

Native Win32 in `gui/main.c`, with comctl32 v6 and a DPI-aware manifest.

- Startup: a task dialog offers the last catalog (kept in
  `HKCU\Software\BRODALF\LastCatalog`), another one, or a new one. A path on
  the command line opens directly. A stale lock can be removed after a crash.
- The tree loads folders lazily and draws each state with custom draw: grey
  italic for no reachable copy, amber for older version only, red bold for a
  bad copy, strikethrough for deleted. Folder and source rows show "x of y
  available" and, when offline, the drive that holds the copies.
- The detail panel lists every version and each copy: drive, state, last
  checked, size and path on the drive.
- Scan, backup, check and restore run one at a time on a worker thread, which
  reports progress to the status bar and log and saves the catalog after each
  job. The tree is refreshed when a job ends and cannot be expanded while one
  runs, since the catalog belongs to the worker then.
- Drives: on startup and on `WM_DEVICECHANGE`, drives that disappeared are
  disconnected, every drive letter with a `BRODALF.media` for this catalog is
  connected (with a quick check), and folders or shares used as storage are
  retried where they were last seen.
