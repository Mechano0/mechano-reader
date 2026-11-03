# MECHANO ZIP READER: Architecture (Phase 1 proposal, waiting for approval)

Stack: C++20, Win32, MSVC, CMake, 7-Zip `7z.dll`. See `../research/03-stack-and-libraries.md`.

## 1. Modules and boundaries

```
            ┌──────────────────────── UI Layer (UI thread only) ─────────────────────────┐
            │ MainWindow · ArchiveListView (virtual) · FolderTree · PreviewHost ·          │
            │ Toolbar/Breadcrumb/Search · Dialogs · ProgressPanel · QuickLook (F1) · Theme │
            └───────────────▲──────────────────────────────┬───────────────────────────────┘
                 PostMessage│ results / progress           │ commands (Job objects)
            ┌───────────────┴──────────── Workers (thread pool + job queue F14) ───────────┐
            │ JobQueue · CancellationToken · ProgressReporter                              │
            └──────▲───────────────▲──────────────────▲──────────────────▲────────────────┘
                   │               │                  │                  │
         ┌─────────┴──────┐ ┌──────┴────────┐ ┌───────┴────────┐ ┌───────┴───────┐
         │ Archive Engine │ │ Preview Engine│ │ Search / Index │ │  Settings /   │
         │ (7z.dll COM)   │ │ (providers)   │ │ (F3, F4, F2)   │ │ Recent / Log  │
         └──────┬─────────┘ └──────┬────────┘ └────────────────┘ └───────────────┘
                │                  │
         ┌──────┴──────────────────┴──────┐
         │ Security Layer (gate for ALL   │  path normalize · limits · MotW · temp files
         │ disk writes and temp files)    │
         └──────────────┬─────────────────┘
                 ┌──────┴──────┐
                 │ FileSystem  │  long paths \\?\ · atomic writes · watcher (F17)
                 └─────────────┘
```

Rules: the UI never touches 7z.dll or the disk. Only the Security Layer creates files. Every module is a static lib with unit tests.

## 2. Source layout (adjusted from the brief)

```
01-wip/
  CMakeLists.txt
  third_party/        7zip-sdk (headers), pugixml, scintilla, lexilla, doctest   (pinned versions)
  src/
    app/              main.cpp, App, command routing, shortcuts
    core/archive/     ArchiveEngine, SevenZipLoader, ArchiveIndex, Entry, Update(Add/Delete/Rename)
    core/compression/ Presets (F15), CompressionSettings, Split (F16)
    core/filesystem/  Path, LongPath, TempManager, FileWatcher, AtomicWrite
    core/security/    PathSanitizer, ExtractGuard (limits/bomb), MotW, InspectWarnings (F8), SecureTemp
    preview/          PreviewEngine, ProviderRegistry, EntryStream, PreviewCache
    preview/text|image|pdf|office|audio|video   one provider per folder
    ui/               windows, controls, theme (light/dark), RTL, i18n (ar/en)
    workers/          JobQueue, Job types, Progress
    utils/            Result<T>, ErrorInfo (friendly + details), Log (bounded), Encoding (F12)
  tests/unit|integration|stress|security
  benchmarks/
  assets/             icons, toolbar SVG→PNG, strings_ar.rc, strings_en.rc
  installer/          Inno Setup script, portable packaging (F11)
```

## 3. Data flow

**Open:** UI → Job(Open) → 7z.dll `IInArchive::Open` (reads the Central Directory only) → `ArchiveIndex`
(flat arrays: name pool + parent index + sizes + CRC + time, about 60-80 bytes per entry, so 100k entries ≈ 8 MB)
→ UI gets a snapshot → the virtual list view draws only visible rows.

**Preview:** select → 150 ms debounce → cancel the previous preview → `PreviewEngine.Request(entry)`
→ pick a provider by **content sniffing** (magic bytes) + extension → provider asks `EntryStream`
→ 7z.dll extracts **that one index** into a bounded stream:
- small (≤ 32 MB): RAM buffer in the `PreviewCache` (LRU, 128 MB total by default)
- large or needs seek (video/PDF): `SecureTemp` file (random name, `FILE_FLAG_DELETE_ON_CLOSE`, user-only ACL)
→ decoder (WIC / MF / Windows.Data.Pdf / Scintilla / OOXML) → render on the UI thread.

**Extract:** Job → for each selected index → `ExtractGuard` checks limits → `PathSanitizer` builds the final path
(Smart Extract F5 applied here) → `FileSystem` opens the file → 7z.dll writes into our stream → CRC check
→ set times/attributes → apply Mark of the Web.

**Create/Add/Delete/Rename:** 7z.dll `IOutArchive::UpdateItems` writes a **new temp archive next to the original**,
then atomic replace (`ReplaceFileW`). The original is never half-written.

## 4. Threading model
- **UI thread:** messages and painting only. Never does I/O.
- **Job queue:** 1 heavy-job worker by default (compress/extract/test), queued and cancellable (F14).
  7z.dll uses its own internal threads for compression, capped by settings (default = cores - 1, min 1).
- **Preview worker:** 1 thread with the latest-request-wins policy (older requests cancelled).
- **Search worker:** 1 thread. Name search runs over the in-memory index (fast). Content search (F4) streams entries.
- Communication: `PostMessage` with heap-owned result objects. Progress throttled to ≤ 20 updates/s.
- Cancel = `Esc`. Every job polls its `CancellationToken` between blocks (≤ 100 ms latency).

## 5. Security model (every archive is hostile)

| Threat | Control |
|---|---|
| Zip Slip / path traversal | `PathSanitizer`: split on `/` and `\`, drop `.` / `..` / drive letters / UNC / leading separators, then `GetFullPathNameW` + **prefix check against the destination**. Any failure skips the entry and reports it |
| Bad Windows names | Strip NUL and control chars, rename `CON/NUL/COMx/LPTx`, trim trailing dots/spaces, reject `:` (ADS), map illegal chars `<>:"|?*` to `_` |
| Symlinks / junctions / hard links | **Never created.** Link entries are extracted as small text files or skipped (setting) |
| Zip bomb / decompression bomb (mandatory, brief §18) | Limits: total written bytes ≤ min(free disk, user limit), per-entry ratio > 1000:1 **and** > 1 GB triggers stop + ask, max entries 1,000,000, max path depth 64, max name 1024 chars, nested archives never auto-opened |
| Malformed headers / overflow | Engine errors are caught per entry, all sizes `uint64_t` with checked math, Central Directory vs Local Header mismatch is a warning |
| Dangerous content (F8) | Warn on `.exe .dll .bat .cmd .ps1 .vbs .js .msi .lnk .scr .hta`, double extensions, and RTL-override chars (U+202E) in names |
| Mark of the Web | Copy `Zone.Identifier` from the archive to every extracted file (default ON) |
| Temp files | `%LOCALAPPDATA%\MechanoZip\tmp\<random>`, user-only ACL, delete-on-close, wiped at startup |
| Passwords | Held in a `SecureString` buffer, `SecureZeroMemory` after use, **never logged**, never written to disk |
| Preview attack surface | No scripts/macros/network: OOXML parsed as data only (no VBA, no OLE objects, no external links fetched), PDF via the OS renderer (no JS), **no HTML/SVG rendering in v1** (shown as text), images decoded with a size cap (max 16k × 16k) |
| Crashes | Each provider runs inside a try/catch + SEH boundary, so a failed preview shows "Cannot preview" and never kills the app |

## 6. Preview architecture
`IPreviewProvider { bool CanHandle(sniff, ext); Cost EstimateCost(entry); Task Render(EntryStream&, PreviewHost&, Cancel&); }`
Providers register in `ProviderRegistry`, so adding a format later means one new folder.

| Order (size vs value) | Provider | Size cost | Notes |
|---|---|---|---|
| 1 | Text + code + CSV/JSON/XML/INI/MD/LOG | ~1 MB (Scintilla) | First 8 MB shown, "load more"; CSV as grid; MD as text v1 |
| 2 | Image | 0 (WIC) | zoom, fit, rotate, dimensions, alpha checkerboard |
| 3 | Info card (unsupported) | 0 | name, type, sizes, method, date, CRC, path + "Extract & Open" / "Extract" |
| 4 | PDF | 0 (Windows.Data.Pdf) | pages, zoom, fit width/page, next/prev; search later |
| 5 | DOCX → XLSX → PPTX | ~150 KB (pugixml) | DOCX: paragraphs, headings, bold/italic, tables, images. XLSX: sheets grid (first 10k rows). PPTX: slide text + images |
| 6 | Audio | 0 (MF) | play/pause, seek, volume, duration, tags |
| 7 | Video | 0 (MF) | optional component, fullscreen |

## 7. Performance strategy
- Virtual list (`LVS_OWNERDATA`): 1,000,000 rows draw like 30.
- Index stored as flat arrays + one string pool (no per-entry heap objects).
- Icons cached per extension (SHGetFileInfo once per ext), so no per-file icon loading.
- No thumbnails in the list (brief §17). Preview loads only the selected entry.
- Search: lowercase-folded name pool scanned with SIMD-friendly `memchr`-style search, < 50 ms for 100k names.
- Startup: no DLL loaded until needed (`7z.dll`, Scintilla, MF and PDF are delay-loaded), target **< 200 ms cold on HDD-class PC**.
- Memory targets: idle ≤ 15 MB, 100k-entry archive ≤ 40 MB, preview cache cap 128 MB (setting).

## 8. Implementation roadmap (from the brief, plus the extras)

| Phase | Content | Extras added |
|---|---|---|
| 2 | Finalize this doc after approval, set up CMake, folders, CI script | |
| 3 MVP | Open ZIP, browse (tree + list), extract (all/selected), create ZIP, add files, basic UI, friendly errors, log | F5 Smart Extract, F12 encoding (basic) |
| 4 Preview | Text → Image → Info card → PDF → DOCX → XLSX → PPTX → Audio → Video | F1 Quick Look |
| 5 Advanced | Search, passwords, test, delete/rename, drag & drop, context menus, shortcuts, properties, dark/RTL | F3, F4, F8, F13, F14, F15, F16, F17, F18, F19 |
| 6 Optimize | Benchmarks vs WinRAR 7.1 on the same files, profiling | F2 Diff, F6 Repair |
| 7 Security audit | Fuzz the open/extract path with hostile archives, review temp/MotW/preview | |
| 8 Packaging | Release build, Inno installer + uninstaller, portable ZIP, version info | F11 Portable |
| After 8 | **Remind Eslam: F7 CAD/BIM preview (DWG/DXF/IFC/RVT)** | F7 |

Each phase ends with: build, tests, a short report (what, files, decisions, commands, results, open issues, next step).
