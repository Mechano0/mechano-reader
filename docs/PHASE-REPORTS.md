# Phase reports

## Phase 1: Research and architecture (2026-10-29) DONE, approved by Eslam
Docs: `docs/research/01..03`, `docs/architecture/ARCHITECTURE.md`. Stack: C++20 + Win32 + 7z.dll.

## Phase 2: Project skeleton (2026-10-29) DONE

**Implemented**
- Toolchain: VS 2022 "Desktop development with C++" installed (MSVC 19.43, Windows SDK 10.0.22621, bundled CMake).
- CMake project: `mzr_core` static lib (all testable logic), `MechanoZip.exe` (Win32), `mzr_tests.exe`.
  Hardening flags: `/guard:cf /sdl /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA`, static CRT (no VC++ redist needed).
- `src/app/main.cpp`: main window with the brief's layout (toolbar, list | preview with draggable splitter, status bar).
  Startup calls `SetDefaultDllDirectories` (DLL-planting protection). Manifest: PerMonitorV2 DPI, long paths, UTF-8, asInvoker.
- `core/security/PathSanitizer`: entry-name sanitizing + "stays inside destination" proof.
- `core/security/ExtractGuard`: mandatory bomb limits (entries, disk space, total size, ratio, overflow).
- `utils/Result.h`, `utils/ErrorInfo.h`: friendly message + technical details on every error.
- `tests/mzr_test.h`: tiny in-house test framework (no download).

**Decisions**
- Rejecting every `..` instead of resolving it: simpler and safer, and legit archives never need it.
- `C:x`-style and `x:...` names are rejected as absolute (conservative).
- Ratio check only after 1 GB of output per entry, so big text/log files are never flagged.

**Commands:** `.\build.ps1` (configure + Release build + tests)

**Results**
- Build: OK, 0 errors.
- Tests: 22 cases, 79 checks, 0 failures.
- `MechanoZip.exe` = 106 KB. Launch to idle ≈ 230 ms (includes process creation). Private memory 6.0 MB, working set 20 MB (mostly shared system DLLs).

**Remaining / next (Phase 3 MVP)**
- Needs Eslam's OK to download third-party sources: 7-Zip SDK (7-zip.org), pugixml and Scintilla/Lexilla (official sites/GitHub).
- Then: ArchiveEngine over 7z.dll, ArchiveIndex, virtual list + folder tree, extract/create/add, Smart Extract (F5), basic encoding fix (F12).

## Phase 3: MVP archive manager (2026-10-29) DONE

**Implemented**
- 7-Zip 26.05 `7z.dll` built from the pinned source (`third_party/7zip-26.05`, `nmake PLATFORM=x64`), loaded by full path, used through its COM interfaces.
- `core/archive/ArchiveEngine`: open (all 7-Zip formats, detection by extension then signature, multi-volume restricted to the same folder), list, extract (all / selected), test, create (zip / 7z), update (add / delete / rename) written atomically to a temp file + ReplaceFileW, then reopened.
- Passwords: asked on demand (dialog, wiped after use). ZIP AES-256; 7z AES-256 with encrypted file names. ZIP passwords must be English characters (a 7-Zip limitation); a non-English one gets a clear message suggesting 7z.
- Security on every extract: PathSanitizer + resolved-path check, bomb limits (ExtractGuard), links and alternate streams never created, never writes through an existing link/junction, partial files deleted on failure/cancel, Mark of the Web copied to extracted files, only safe attributes restored, long paths.
- `ArchiveIndex` (folder tree, natural sort; 100k entries in ~172 ms), `SmartExtract` (F5: one top item goes straight in, otherwise a subfolder named after the archive).
- UI: virtual list with system icons and sortable columns, breadcrumb, Up/Backspace, search across the whole archive, info pane (archive properties / selection totals / file card with Extract & Open + risky-file warning), drag & drop (open / create / add), New-archive dialog (format, level, password), Names menu (code page for old non-UTF-8 ZIPs), progress + Esc cancel, error dialogs with a Details section.
- Worker thread for all archive work; the UI never blocks.

**Commands**: `.\build.ps1` (builds 7z.dll if missing, CMake, build, tests).

**Results**
- Build: clean (/W4). Tests: **40 test cases, 226 checks, 0 failures** (zip/7z roundtrip, Arabic names, passwords right/wrong/cancel, Zip Slip names, reserved names, zip bomb ratio + total limit, corrupt/truncated/fake archives, cancel).
- Real app: opens `00-data/test-archives/sample-arabic.zip`, Arabic folder name renders correctly, info pane correct, ~9.5 MB private RAM with the archive open. Screenshot checked.
- Fixed during verification: an archive passed with a relative path on the command line did not open (now resolved with GetFullPathNameW).

**Remaining issues / not done yet**
- .tar.gz shows the inner .tar as one item (opening nested archives comes later).
- No folder-tree pane yet (list + breadcrumb only); no rename (F2) in the UI yet (the engine supports it).
- Dark mode and RTL layout: Phase 5. 32-bit build not tested yet.
- Symlink security test skipped on this PC (Windows does not allow creating symlinks without Developer Mode); the code path is in place.
- Folder rows show no Modified date when the archive has no folder entries.

**Next**: Phase 4 Preview (text, image, info card, PDF, DOCX, XLSX, PPTX, audio, video + F1 Quick Look).

## Phase 4 (part 1): Preview pane + detail column (2026-10-29) DONE

Requested by Eslam after trying the MVP:
1. The Packed / Ratio / Modified / CRC32 columns become ONE column with a small dropdown arrow in its header
   that chooses what it shows (Packed, Ratio, Modified, CRC32, Type, Method). The choice is remembered (HKCU\Software\MechanoZip).
2. The right pane shows the file itself, like the Windows Explorer preview pane, with a short info strip
   (name, type, size, dimensions/pages, date) and the buttons underneath.
3. Test with a PDF, an image and a video in an archive and confirm all appear in the preview area.

**Implemented**
- `Archive::ReadEntry`: decodes ONE entry into RAM (never touches the disk); hard byte limit enforced while
  decoding (never trusts the declared size); "first N bytes" mode for big text files; encrypted entries are not
  previewed unless the password is already known (no prompt on selection).
- `core/preview/PreviewDecode`: kind by extension; WIC image decode (size cap 16k x 16k, reduced to 2560 px while
  decoding, EXIF orientation); text decode (UTF-8 / UTF-16 BOM / ANSI, binary detection, cut UTF-8 at the end kept).
- `core/preview/PdfRender`: Windows.Data.Pdf (built into Windows, no JavaScript), page N rendered to the pane width,
  Previous / Next buttons, "Page x of N". Password PDFs: clear message.
- `ui/MediaPlayer`: Media Foundation (MFPlay) video + audio. The entry is extracted through the normal safe
  extraction path into a private temp folder (`%LOCALAPPDATA%\MechanoZip\tmp\pv-*`), deleted when the selection
  changes / on exit / at next start. Starts playing **muted**; Pause/Play and Sound buttons; click the video to pause.
- Preview requests run on the worker, latest-selection-wins (generation counter + cancel token); a queued preview
  keeps its own archive alive (archive_ is now shared_ptr).
- Text preview: Consolas for code; Arabic text switches to the UI font + right-to-left reading.
- `main.cpp`: MFStartup/MFShutdown (media preview only; the app still works without Media Foundation).

**Test archive**: `00-data/test-archives/sample-preview.zip` (folder "معاينة": clip.mp4 8 s 1280x720 H.264+AAC,
tone.mp3, render.jpg 3000x2000, report.pdf 3 pages, ملاحظات.txt). Video/audio were generated with the ffmpeg
already on this PC (dev tool only, not used by the app).

**Results**
- Build clean (/W4). Tests: **46 test cases, 274 checks, 0 failures** (new: kind detection, text decoding incl. Arabic
  and cut UTF-8, image decode + reduce + 20000x20000 header refused, PDF render page 1/3/clamp + broken PDF,
  ReadEntry full / over limit / truncated / cancel / bad index, encrypted entry not previewed).
- Screen check (keyboard only): video plays inside the pane, photo fits the pane (3000 x 2000 shown), PDF page 1 and
  page 2 via Next, MP3 plays (icon + "Playing (sound off)"), Arabic text readable RTL. Temp folder empty after leaving media.
- Found and fixed while testing: truncated text read returned nothing (7-Zip writes big chunks; now the part that
  fits is kept); Arabic text was tiny in Consolas (font switch).

**Remaining / notes**
- RAM with video/PDF engines loaded: ~78 MB private (Media Foundation + WinRT PDF stay loaded after first use).
  Above the 15 MB idle target only after a video/PDF was previewed; to review in Phase 6 (performance).
- Office (DOCX/XLSX/PPTX) preview, F1 Quick Look, image zoom, PDF zoom/search: next parts of Phase 4.
- Video seek bar / time display not done yet (Play/Pause + Sound only).

## Phase 4 (part 2): Office preview: Word, Excel, PowerPoint (2026-10-29) DONE

No Microsoft Office needed. An Office file is a ZIP: it is opened **in RAM** with the same 7z.dll engine
(`Archive::OpenMemory`, ZIP format only) and its XML parts are read with **pugixml 1.16** (no DTD / external
entities, so no XXE or "billion laughs"). Macros, OLE objects, ActiveX and external links are never read or fetched;
every part has a byte limit (file 128 MB, XML part 48 MB, picture 24 MB, sheet 10 000 rows x 100 columns).

**Implemented** (`core/preview/Ooxml`)
- **Word** (.docx/.docm/.dotx/.dotm): converted to RTF that we generate ourselves, shown in a read-only RichEdit:
  headings (with the style colour), bold / italic / underline / strike, colours, sizes, alignment, right-to-left
  paragraphs, bullets, tables with borders, PNG / JPEG / EMF pictures. Tables and pictures wider than the pane are
  shrunk to fit ("fit to window").
- **Excel** (.xlsx/.xlsm/.xltx/.xltm): one sheet at a time in a virtual grid (row numbers + A, B, C ...): shared
  strings, numbers, dates (1900 system), percent, booleans, inline strings, formula results. Previous / Next switch
  sheets; footer shows "Sheet x of N: name". Chart sheets: a note.
- **PowerPoint** (.pptx/.pptm/.potx/.ppsx ...): each slide drawn to a picture (text boxes, placeholders inherited from
  layout/master, shapes and fills, pictures, tables, background). Previous / Next, "Slide x of N".
- `main.cpp`: `OleInitialize` instead of plain COM init (RichEdit needs OLE for pictures) + a minimal in-memory
  `IRichEditOleCallback` (no paste / drop / context menu).

**Test archive**: `00-data/test-archives/sample-office.zip` (folder "مستندات": تقرير المشروع.docx, حصر الكميات.xlsx,
العرض.pptx; Arabic + English content, table, bullets, picture, 2 sheets, 3 slides). Built from Office Open XML
parts by a script (generating them by driving Word through COM hung on save, see below).

**Results**
- Build clean (/W4). Tests: **52 test cases, 328 checks, 0 failures** (new: column letters + Excel dates, Word ->
  RTF incl. Arabic / RTL / table / external link ignored, Excel grid strings / numbers / date / percent / bool /
  rich-text shared string / float noise / second sheet / index clamp, PowerPoint slide pixels + 2nd slide,
  damaged / non-Office / DTD-entity files refused safely, real sample docx: picture present and fitted).
- Screen check (keyboard only, no mouse): all 3 slides, Word document (headings, RTL Arabic, bullets, table, picture),
  Excel sheet 1 (Arabic headers, 40 rows) and sheet 2 via Next. PDF re-checked after the refactor.
- Found and fixed while testing:
  - `<p:sldId id="256" r:id="rId2">` has two attributes named "id": the slide list was empty for every real
    PowerPoint file. Relationship ids are now read from the prefixed attribute only (also for Excel sheets).
  - Word tables wider than the pane were cut; heading colours from styles were ignored.
  - Pictures in Word were missing: a read-only RichEdit drops pictures while loading RTF (now read-only after load).
  - Arabic sheet name swapped places with the date in the footer (LRM mark added).

**Remaining / notes**
- Old binary formats (.doc / .xls / .ppt) are not previewed (message shown); possible later via a converter.
- PowerPoint: charts, SmartArt, gradients and EMF pictures show as simple placeholders; text auto-fit not applied.
- Excel: no cell colours / merged cells / column widths yet (plain grid); formulas show the saved result only.
- Next parts of Phase 4: F1 Quick Look, image / PDF zoom, video seek bar.

## Phase 4 (part 3): Zoom + media controls (2026-10-29) DONE
- Zoom: pictures, PDF pages and slides (1x to 8x real size, sharp re-render of PDF/slides when zoomed); text, Word and Excel by font size.
  Buttons [-][%][+], Ctrl+wheel at the cursor, Ctrl+= / Ctrl+- / Ctrl+0, drag to pan, double-click toggles fit / actual size.
- Video: VLC-style control bar (src/ui/MediaBar.*): time | seek bar | length, then play/pause, stop, -10 s, +10 s, mute and the volume wedge.
  Clicking anywhere on the seek bar jumps straight to that point; dragging scrubs.
- Audio: the simple row stays ([<< 10 s] [slider] [10 s >>] + Pause / Sound), and a click on the slider now jumps to the clicked point too.
- MediaPlayer: Duration/Position/Seek/Stop/Volume. A refused pause/stop/seek no longer ends the preview with "cannot play".
- Checked on screen (messages only, no mouse): seek click 0:07 and 0:01, pause holds the time, stop back to 0:00, volume 63% + sound on, audio slider jump.
- Build OK, 52 test cases / 328 checks pass.
- Not done: full-screen video (VLC has it).

## F1 Quick Look (2026-10-29) DONE
- Space on a selected entry opens a big resizable window (75% of the screen, centred on the main window).
  The preview pane itself is moved into it (SetParent), so every type (picture, text, PDF, Word, Excel, PowerPoint, video, audio),
  zoom and the media bar work there with no second copy of the code. The pane goes back to the main window on close.
- Keys inside Quick Look: Space / Esc / X close it; Up / Down / Home / End / PgUp / PgDn move to the previous / next file;
  Left / Right = previous / next page (PDF, slides) or -5 s / +5 s (video, audio); Enter closes it and opens the file; Ctrl +/-/0 zoom.
- Title shows the file name (Arabic OK). The window is hidden, not destroyed, so its size / maximised state is kept.
  Closed automatically when another archive is opened or the app closes.
- Maximising Quick Look also gives a near full-screen video.
- Checked with keyboard messages only: open, Down through mp4 / jpg / pdf / mp3 (all shown), Right to PDF page 3 of 3, Space and Esc close, pane back in the main window.
- Build OK, 52 test cases / 328 checks pass.

## Phase 5: Advanced features + DWG preview (2026-10-31) DONE (drag-out not checked on screen)
Already in place from Phase 3 (checked, not rewritten): search-as-you-type in the archive, password on new archives
(ZIP AES-256, 7z AES-256 + encrypted names), archive Test, drop files on the window to open / create / add, accelerators.
New:
- DWG preview (src/core/preview/Dwg.*): reads the preview picture AutoCAD saves inside the file (BMP or PNG, R13 and later)
  and the AutoCAD version (AC1012 ... AC1032 -> "AutoCAD 2018 - 2025"). Only the first 16 MB are decompressed, so big
  drawings preview fast. The drawing itself is not parsed (no free, license-compatible DWG engine); WMF thumbnails are
  skipped on purpose. No picture -> clear message with the version + "Use Extract & Open".
  Checked with 3 real DWGs (AutoCAD 2007, 2004, 2018; 68.6 MB file with an Arabic name): all show the picture + version.
- Right-click menu on the list (also Shift+F10 / Menu key): Extract & Open / Open folder, Quick Look, Extract selected to,
  Extract here, Test, Add, Delete, Copy name, Copy path inside archive, Select all. Items grey out when they cannot run.
- Drag out: drag files / folders from the list to Explorer or the desktop. The selection is extracted with the normal
  safe extractor to a private temp folder, then handed to Explorer as a shell data object (copy only). The drag starts
  only if the mouse button is still down when the files are ready. Temp folder removed at the next start (Explorer may
  still be copying). A drop back on our own window is ignored (no "add temp copies").
- F1: list of all keyboard shortcuts.
- Tests: 3 new DWG test cases (version names, BMP thumbnail decoded, hostile input: bad seeker, broken sentinel, sizes
  past the end, giant image header, cut file, every-3rd-byte mutation) -> 55 test cases / 349 checks, 0 failures.
- Checked in the app (keyboard messages only): DWG previews, right-click menu open + Esc, Copy name / path with an
  Arabic name, F1 dialog.
- NOT checked: drag-out (needs a real mouse drag). Eslam to try by hand.
- Test archive added: 00-data/test-archives/sample-dwg.zip (copies of 3 DWGs, originals untouched).

## Rename inside the archive (2026-10-31) DONE
- F2, right-click > Rename, or a slow second click on a selected row: the name is edited in place (Explorer style,
  extension not selected). Files and folders (a folder rename moves every entry below it). Read-only formats are refused.
- New name check (security::CheckNewEntryName): empty, too long, \ / : * ? " < > |, control characters, hidden
  text-direction characters (RLO "invoice<RLO>fdp.exe" trick), trailing dot/space, CON/NUL/COM1..., "." / "..".
  Same name already in the folder -> refused (a case-only change is allowed).
- Saved with the existing atomic Update (temp archive, then replace): the original is untouched if anything fails.
  The renamed item stays selected after the reload.
- Tests: new unit test for the name check -> 56 test cases / 366 checks, 0 failures.
- Checked in the app on a COPY of sample-preview.zip: Arabic folder "معاينة" -> "معاينة 2" (all 5 files moved, verified
  by reading the saved zip with .NET), clip.mp4 -> "clip renamed.mp4", "a:b" refused with a clear message.
- CAD layouts: not done. The DWG preview only has the one picture AutoCAD saves; switching layouts needs a full
  DWG/DXF reader. Options sent to Eslam for a decision.

## Phase 6: Optimization (2026-10-31) DONE (RAM target for 100k entries revised, see below)
Startup:
- Only the frame, toolbar, list and status bar are created before the window shows. RichEdit, the Excel grid and the
  system icon list are created right after (WM_APP_LATEINIT). The icon list loads on a background thread (WM_APP_ICONS).
- Media Foundation (mfplay, mfplat) and msimg32 are delay-loaded. MFStartup runs on the first video/audio preview, not
  at start. If Media Foundation is missing (Windows N without the Media Feature Pack) a clear message is shown.
- The app log now records "Startup: window shown at N ms" measured from process creation.
Memory (index of an open archive):
- Node names point into the entry path instead of a second copy (only folder names that are not the last part of a
  path go into a small pool).
- The compression method text ("Deflate", "LZMA2:24"...) is stored once per archive and each entry keeps a 2-byte id.
Benchmark: benchmarks\bench.ps1 (read-only, makes a 100,000-entry ZIP in %TEMP% once). Median of 5 runs, Release x64:
| Measure | Before | After | Target |
|---|---|---|---|
| Startup, window visible | 230 - 290 ms | ~120 - 150 ms | < 200 ms |
| Loader + OLE part of startup | 40 - 55 ms | ~22 ms | - |
| RAM idle (private) | 5.6 MB | 5.2 MB | <= 15 MB |
| Open 100,000-entry ZIP, RAM | 57.7 MB | ~49 MB | <= 40 MB (revised to <= 50 MB) |
- Split of the 100k number (tests/stress/test_memory.cpp, run with MZR_BENCH_ZIP): 7z.dll's own ZIP handler keeps
  ~21 MB for the open archive (not in our control, it needs it to extract), our index went from ~30 MB to ~21 MB.
  40 MB would need 7z.dll to use less, so the target is revised to 50 MB. Possible later saving (~3 MB): flat child
  arrays and dropping sizes duplicated between file node and entry. Not done: small gain, more risk.
- Timings on this machine are noisy (+-30 ms between runs); the numbers above are medians.
Checks:
- Build OK, 57 test cases / 367 checks, 0 failures.
- Regression check in the app (keyboard messages only): video plays (delay-loaded Media Foundation), PowerPoint,
  Word (late RichEdit) and Excel (late grid) previews render, file icons show in the list.

## Phase 7: Security audit (2026-11-01) DONE
Full findings, fixes and checks: `docs/SECURITY-AUDIT-PHASE7.md`.
Toolchain after the Windows reinstall: VS 2022 Build Tools (MSVC 14.44.35207 / cl 19.44, SDK 10.0.26100, bundled CMake).
The old build folder pointed to the removed VS Community: moved to `build/x64-old-2026-10-31` (can be deleted), fresh build.
Reviewed: archive input, paths, extraction, preview engines, temp files, external content. 8 findings, all fixed:
- S1 (High): deeply nested XML in a .docx/.pptx crashed the app on selection (stack overflow, PoC 11 KB file).
  Now: depth limit 256 checked after parsing + non-recursive `Find`.
- S2 (Medium): every big non-first file of a solid 7z was stopped as a "zip bomb" (packed size 0 used as 1 byte).
  Now: unknown packed size uses the archive size.
- S3 (Medium): `invoice.exe.` / `invoice.exe ` opened with Extract & Open without the "can run code" warning.
  Now: `security::IsRiskyFileName` (Windows-style name), checked again on the file actually launched. S6: list extended.
- S4 (Medium): a failed Overwrite deleted the user's original file. Now: temp file + replace on success only.
- S5 EMF in Word not rendered (GDI parser), S7 random + always-new temp folders (BCryptGenRandom), S8 overflow guards.
Files: `core/preview/Ooxml.*`, `core/security/ExtractGuard.*`, `core/security/PathSanitizer.*`, `core/archive/ArchiveEngine.cpp`,
`core/filesystem/FileSystem.*`, `ui/MainWindow.cpp`, `tests/security/test_phase7.cpp` (new), `tests/unit/test_office.cpp`, `CMakeLists.txt`.
**Results**: build clean, **63 test cases / 419 checks / 0 failures**. Real app: hostile archive crashes the old build
(0xC00000FD), the new one stays open; sample-preview.zip previews OK, temp folder names random.
**Remaining**: drag-out still to be tried by hand (Eslam). **Next**: Phase 8 Packaging.

## Phase 8: Packaging (2026-11-02) DONE (GitHub upload waits for Eslam)
Decisions (Eslam): Inno Setup inside the project, version **0.9.0 Beta**, icon drawn by script, **MIT** license.
**Implemented**
- Version in ONE place: `assets/version.h` -> exe file properties (0.9.0 / "0.9.0 Beta", author, MIT copyright,
  pre-release flag), the F1 dialog footer (version, author, license, 7-Zip version, "Portable mode"), and the installer
  (reads it back from the built exe). CMake project version 0.9.0.
- App icon: `assets/make-icon.ps1` draws `app.ico` (16-256 px) + `app-256.png`; used by the exe, window, Quick Look, installer.
- **Portable mode (F11)**: `portable.flag` next to the exe -> settings in `Data\settings.ini` and log in `Data\`,
  no registry writes; temp files in `%TEMP%\MechanoZip`. New `utils/Settings` (registry or ini); `fs::ExeFolder`, `fs::IsPortable`.
- **Installer + uninstaller**: `installer/MechanoZip.iss` (Inno Setup 6.7.4, downloaded with Eslam's OK from the official
  site, signature checked: Pyrsys B.V. as stated on jrsoftware.org; installed portable in `tools/InnoSetup6`, no registry).
  Per-user by default (no admin), "all users" offered; English + Arabic wizard; x64, Windows 10+; Start menu entry,
  optional desktop icon; optional "Open with" for 13 archive types (never takes over the default app); closes a running
  copy before updating; the uninstaller removes the app, its registry entries, `%LOCALAPPDATA%\MechanoZip` and
  `HKCU\Software\MechanoZip`.
- `release.ps1`: build + tests, stage, THIRD-PARTY-NOTICES.txt (7-Zip License/unRAR/LGPL texts + pugixml, verbatim),
  installer, portable zip, `SHA256SUMS.txt` -> `02-final product/release/`.
- `make-github-copy.ps1`: clean source copy in `02-final product/github` (src, tests, assets, installer, docs without
  research notes, 7-Zip + pugixml sources without build output, README.md, LICENSE.txt, .gitignore). No git, no upload.
**Results**
- Build clean, **63 test cases / 419 checks / 0 failures**.
- Release: `MechanoZipReader-0.9.0-beta-setup-x64.exe` 2.9 MB, `MechanoZipReader-0.9.0-beta-portable-x64.zip` 1.2 MB.
- Silent install test (per-user, "Open with" task): files, Apps & Features entry (0.9.0 Beta, publisher, icon), Start menu,
  `.zip` Open-with entry OK; the installed app opens sample-arabic.zip. Silent uninstall: folder, uninstall key, ProgID,
  Open-with values, settings key, %LOCALAPPDATA%\MechanoZip and the Start menu entry all gone.
- Portable test: runs from the extracted zip, log in `Data\`, no registry key, nothing in %LOCALAPPDATA%.
- GitHub copy built from scratch in a separate folder (7z.dll from source + CMake + tests): **63 cases / 414 checks,
  0 failures** (5 checks use test archives from 00-data, which are not in the copy, and skip). Found and fixed on the way:
  `build.ps1` failed on a first build of 7z.dll (vcvars prints a harmless "vswhere not found" line on stderr and
  PowerShell treated it as an error). A very long clone path also breaks MSBuild: the README says to clone into a short path.
- Release SHA-256: setup `c50b0075…fbb91`, portable `36d8d099…ce83fa` (full values in SHA256SUMS.txt).
**Remaining / notes**
- Not code-signed (no certificate): SmartScreen warns on first run (said in README).
- 32-bit build still not tested. Drag-out still to be tried by hand. Upload to GitHub: waiting for Eslam (account / repo name).
- After Phase 8: **F7 CAD/BIM preview reminder** (see ADDENDUM-01).

## F7 trial: AutoCAD drawing view (DWG / DXF) + "Open in main program" (2026-11-02)
Requested by Eslam after Phase 8: the DWG view must be built in (no separate download). Option 1 approved: the
LibreDWG converter ships inside the installer, our own CAD core draws the drawing.
**Implemented**
- `core/cad` (reusable library, no UI / archive dependency):
  - `CadDxf.cpp`: ASCII DXF reader, streamed line by line (a 185 MB DXF never sits in RAM as text). LINE, LWPOLYLINE /
    POLYLINE (bulges, polyface meshes), CIRCLE, ARC, ELLIPSE, SPLINE (NURBS, de Boor), TEXT, ATTRIB, MTEXT (formatting
    codes stripped, stacked fractions, %%c %%d %%p), SOLID / TRACE / 3DFACE, HATCH (solid + pattern as light fill),
    INSERT / MINSERT (nested blocks), DIMENSION and ACAD_TABLE blocks, LEADER, POINT, VIEWPORT. Layers (off / frozen),
    ACI + true colour, BYLAYER / BYBLOCK, OCS (mirrored entities), Arabic text (UTF-8 in 2007+ files, code page +
    \U+ escapes in older ones). Model + every Layout (names and tab order from LAYOUT objects, viewports).
    Model extents: the biggest connected group of the drawing (64x64 grid, lines walked cell by cell), so far-away
    stray objects do not shrink the drawing to a dot.
  - Limits (hostile files): 1 GB file, 5 M entities, 6 M points, block depth 16, 10 000 MINSERT cells, 4 096 text
    chars, entity-visit budget (block "billion laughs"); a limit skips that part and marks the drawing "partly shown".
  - `CadRender.cpp`: GDI+ (part of Windows, delay-loaded): anti-aliased lines, fills, rotated / aligned text (Arial,
    Arabic shaping), dark model background like AutoCAD, white paper for layouts, viewports clipped.
  - `CadConvert.cpp`: runs `cad\dwg2dxf.exe` in a Job Object (2 GB RAM, 2 min, no child processes, killed with the
    job, no window, below-normal priority, only a NUL handle inherited); cancelled within 100 ms.
- Preview: a DWG shows its saved thumbnail at once ("opening the drawing..."), then the real drawing replaces it.
  DXF is drawn directly. Footer: "Model" / "Layout 2 of 9: Tunnel Layout" + AutoCAD version; ◀ ▶ switch views, zoom
  re-renders sharp (up to 2560 px). The entry goes through the normal safe extraction to a private temp folder with an
  ASCII name (dwg2dxf cannot open Arabic names), deleted right after. A user task (open, extract, test ...) stops a
  running conversion so it never waits.
- "Open in main program": right-click menu item (was "Extract & Open"), double-click / Enter (already did it), the
  preview button "Open in program", F1 text updated.
- Packaging: `cad\` (dwg2dxf.exe, libredwg-0.dll, libiconv-2.dll, COPYING-GPL3.txt, README-LibreDWG.txt with the
  source location + SHA-256) in the installer and the portable zip; THIRD-PARTY-NOTICES lists LibreDWG. Source tarball
  (verified against the official dist.sha256) in `third_party/libredwg-0.14.8739`; the 35 MB binaries stay out of the
  GitHub copy (README says where to get them; without them the build still works with the thumbnail only).
**Results**
- Build clean (/W4, 0 warnings). Tests: **69 test cases / 468 checks / 0 failures** (6 new in `tests/unit/test_cad.cpp`:
  geometry + layers + blocks, layouts + viewports, Arabic / MTEXT, hostile drawings (self-insert, 10^10 block bomb,
  giant MINSERT, absurd counts / NaN / huge bulge / bad spline / 1 MB text, entity limit, garbage, binary DXF),
  colour table, real DWG through the helper + garbage DWG + cancel + size limit).
- Real files: 3 sample DWGs (AutoCAD 2007, 2004, 2018). The 68.6 MB drawing with an Arabic name: thumbnail at once,
  full drawing after ~14 s (dwg2dxf ~13.6 s, 185 MB DXF read in ~0.5 s), 9 layouts with real names; app RAM 5 MB while
  converting (the helper has its own ~290 MB), ~33-37 MB while the drawing is shown. Small drawings: < 0.5 s.
- Release: installer **7.5 MB** (was 2.9), portable zip 9.8 MB; install / uninstall test OK (cad folder installed and removed).
- Note: in an early UI test the selection jumped; it came from the real mouse on the visible test window, not the app
  (tests now run with the window off-screen).
**Remaining / notes (trial)**
- Not drawn yet: linetypes (dashes), lineweights, hatch patterns (light fill instead), wipeouts, images, 3D solids,
  MLINE, inline MTEXT sizes / fonts, non-plan (3D) viewports, viewport layer overrides.
- Zoom beyond 2560 px is pixel zoom; a vector re-render of the visible area would keep lines sharp at any zoom.
- Big DWGs take seconds (the converter); a cache per archive entry would make the second look instant.
- Next requested: Revit and Navisworks options (ADDENDUM-01 F7b / F7c).

## Excel preview looks like Excel (2026-11-02)
Eslam: "Excel files shown with broken formatting, different from the file". The old preview was a plain ListView grid.
**Implemented**
- `core/preview/Ooxml.cpp`: the sheet now carries its look: theme colours (theme1.xml + tint), legacy indexed palette, fonts
  (name, size, bold / italic / underline / strike, colour), fills (solid, patterns as a lighter shade, gradient first stop),
  borders (thin / medium / thick / double + colour), alignment (horizontal incl. center-across, vertical, wrap, indent),
  column widths (hidden = 0), row heights (hidden = 0), merged cells, right-to-left sheets, gridlines on / off, styled empty
  cells. New `FormatExcelNumber`: built-in ids + format codes (sections pos;neg;zero, 0 # ? , . %, thousands, scaling
  commas, E+00, literals / quotes / \x / _x, [$€-x] currency, [Red]-style colours, dates / times incl. [h], AM/PM, month vs
  minute), General like Excel (11 characters, E+NN), built-in 14 / 22 use the Windows short date like Excel.
- `ui/SheetView.*`: new drawn grid (replaces the ListView): only visible cells painted, double-buffered; Excel column /
  row pixel sizes, fills, gridlines, borders (no lines inside merges), text alignment (General: numbers right, text left,
  booleans / errors centre), text running on into empty neighbours, "####" for numbers that do not fit, wrapped text,
  right-to-left sheets mirrored (column A and row numbers on the right, scroll bar mirrored), zoom with the preview
  buttons / Ctrl+wheel, wheel / Shift+wheel / keys scroll.
- Hostile workbooks: values like 1e308 and format codes with 100 000 zeros / long [hhhh] / long exponents are capped
  (found by the new test: a 128-char buffer would have stopped the app with 0xC0000409, fixed before release).
- Test sample: `01-wip/tests/data/make-sample-excel.ps1` -> `00-data/test-archives/sample-excel.zip` (Arabic RTL sheet with
  merged coloured title, theme-tint header, borders, number / date / % / red negative formats, wrap, total row; English sheet
  with many formats, hidden column, indent).
**Results**: build clean, **71 test cases / 510 checks / 0 failures** (new: Excel number formats, Excel look from the
sample). Checked in the app (window off-screen): both sample sheets match the intended Excel look, zoom 156 %, RTL scroll.
**Remaining**: conditional formatting, charts / pictures / shapes on sheets, cell comments, frozen panes, rich text runs
inside one cell (one style per cell), theme fonts (+mn-lt) shown as Calibri.

## Revit / Navisworks study (2026-11-02)
See `docs/research/04-revit-navisworks.md`. Summary: the Revit model itself (Partitions streams) is a closed format;
offline we can show a rich info card (128 px thumbnail, version, project information, family types + parameters, linked
files). Navisworks needs a sample file. A real offline 3D view = an IFC viewer (separate task). Waiting for Eslam's choice.

## Revit info card: .rvt / .rfa / .rte / .rft (2026-11-02)
Eslam chose the info card (option 1) for Revit; families too (the family geometry is in the same closed format).
**Implemented** (`core/preview/Revit.*`)
- Own bounded OLE compound-file reader `CompoundFile` (reusable): header checks, FAT via header + DIFAT chain, directory,
  mini FAT + mini stream, path lookup in the directory tree without recursion (visited set), every sector chain checked
  (index range, step count <= FAT size: no loops, never longer than the stream). Only small streams are read (8 MB cap);
  the model (Partitions) is never read. Not the Windows structured-storage parser.
- Revit streams: BasicFileInfo (version, build, workshared flag, central path, last saved path, locale; new length-prefixed
  layout + the older "Key: value" text layout), RevitPreview4.0 (PNG thumbnail), ProjectInformation (ZIP read in RAM with
  our 7z engine -> Atom XML: project parameters by group), PartAtom (family name, category, parameters, every type with its
  values), TransmissionData (4-byte count + UTF-16 XML: linked files with saved path and load state).
- Card = RTF in the existing read-only rich view (zoom, scroll, Arabic): title, "Revit 2023 project · build · workshared",
  thumbnail (shown 192 px), File table, Project information / Family parameters (grouped), Types table (up to 9 parameter
  columns, 500 types), Linked files (not found in orange), note that the 3D model needs Revit (double-click opens it).
- Preview flow: the entry goes to a private temp copy (shared `ExtractPreviewCopy`, also used by the CAD view), read, deleted.
  Max 1 GB.
**Results**: build clean, **73 test cases / 531 checks / 0 failures** (new `tests/unit/test_revit.cpp`: a synthetic Revit-like
compound file built with Windows storage on the test side (workshared, Arabic path, family types, UTF-16 links, preview,
200 KB stream in a sub-storage) read back field by field; hostile files: self-looping FAT, looping directory tree, absurd
FAT / DIFAT counts, bad sector shift, cut files, 300 rounds of random damage, non-CFB data).
Real copies of Eslam's files (originals untouched): Revit 2023 MECH 243 MB read in 16-47 ms (4 links), ARCH 19.5 MB
(workshared, project name in Arabic, 2 links), STR 10.7 MB (136 project parameters, 1 link), Revit 2025 family TA1.rfa
(Pipe Tags, 2 types). Cards checked on screen. Found and fixed on the way: TransmissionData is UTF-16 with a length prefix
(links were missing); "×" written as UTF-8 inside RTF.
**Remaining**: in the app most of the time for a big .rvt is extracting it from the archive to the temp copy (the reading
itself is milliseconds); IFC 3D viewer = separate task (needs a sample .ifc); Navisworks needs a sample file.

## IFC info card (2026-11-02)
Eslam: no 8 - 10 day 3D viewer; the goal of the app is a **light, fast look at what is inside a file** - for real
work he opens the original program. So IFC gets an info card like Revit (the 3D viewer plan in
`docs/research/05-ifc-viewer-plan.md` is shelved).
**Implemented** (`core/preview/Ifc.*`): one streamed pass over the STEP text (records over several lines, '' quotes,
\X2\ UTF-16 / \X\ / \S\ escapes so Arabic names work): header (schema, model view, author, organization, authoring
tool, application, date), project / site / building, storeys with elevation and unit (mm / cm / m / ft / in), element
counts by type (about 100 IFC element types mapped to plain names: walls, doors, pipes, cable trays ...), elements per
storey (also through spaces; parent hops capped at 16), property sets, element types, record count. Card = RTF in the
rich view. Limits: 2 GB file, 32 MB per record, 50 M records, 64 arguments read per record, 64 K chars per string.
Fixed on the way (also in the Revit card): a table cell starting with digits glued onto the RTF control word before
it ("00 groundfloor" -> \sa4000, a huge row and "00" lost); every cell now starts after a space.
**Results**: build clean, **76 test cases / 562 checks / 0 failures** (new `tests/unit/test_ifc.cpp`: synthetic IFC2x3
with Arabic project / storey names, multi-line record, storeys sorted by elevation, element -> storey through a space;
both buildingSMART samples (IFC4, IFC2x3); hostile: empty, not STEP, unterminated string, 40 MB record, 100 000 nested
parentheses, aggregate loops, bad \X2\ escapes, huge strings). Card checked on screen with the IFC4 sample.
**Remaining**: .ifczip (zipped IFC) and .ifcXML not handled; Navisworks needs a sample file.

## Navisworks info card + rebuilt release (2026-11-02)
Eslam: "rebuild + add Navisworks". No sample .nwd / .nwc / .nwf exists (he has none), and the format is closed and
undocumented, so the card is **best effort** and says so.
**Implemented** (`core/preview/Navisworks.*`, Kind::Navisworks for .nwd / .nwc / .nwf): the file (temp copy, max 2 GB)
is scanned whole up to 24 MB, otherwise its first 16 MB + last 8 MB. Found: a saved thumbnail (PNG walked chunk by
chunk, JPEG walked marker by marker; the biggest 32 - 4096 px one; at most 64 tries, 4 MB per picture), texts that name
Navisworks (the version that wrote the file, max 4), and referenced / appended files (ASCII and UTF-16 text, Arabic
too) with about 35 CAD / BIM extensions (rvt, dwg, ifc, nwc, dgn, skp, step ...), drive / UNC paths, max 300.
The card: type (NWD = published model, NWC = cache of one file, NWF = links only), size, "written by", container
(Navisworks binary / OLE / ZIP / XML), thumbnail, file table (name + folder), a note that the model needs Navisworks
(Freedom is free) and "No readable details" when the data is compressed.
**Results**: build clean, **78 test cases / 582 checks / 0 failures** (new `tests/unit/test_navisworks.cpp`: synthetic
NWD with a small and a big PNG, version text, ASCII / UTF-16 Arabic / UNC paths, duplicates, non-files; hostile:
empty, 3 bytes, 1 MB random, PNG claiming 4 GB chunks, cut PNG, endless JPEG markers, SOS with no end, 3 MB text run,
5000 paths, failing reader, 1 GB file read only partly). Card checked on screen with a synthetic NWD in a zip.
**Release rebuilt** (Excel look, Revit, IFC, Navisworks cards inside; READMEs list the BIM cards):
setup 7.55 MB, SHA256 0e245360855bcca7522e52d1d6b4bd429dd8773e4fd3ecf62b8c9cc356b3f9f5; portable zip 9.93 MB,
SHA256 ee0f15a581f9d36ebbc05e0c7f1e4cf5cc868a6a0c49ddd98bdd60ba40a5f7a6. Silent install + uninstall checked (all files
incl. cad\ installed, nothing left after uninstall). GitHub copy refreshed (1493 files, 22.5 MB, upload still waiting).
**Remaining**: check the Navisworks card with a real file when one is available (real NWD data may be compressed, then
only type + size show).

## Media look: AIMP-style audio, VLC-style video (2026-11-02)
Eslam: audio should look like AIMP, video like VLC.
**Audio** (`MainWindow::PaintAudioPanel` + `MediaBar` Aimp style + `core/preview/AudioTags.*`): a dark song panel
with the embedded cover (Windows property system `PKEY_ThumbnailStream`, decoded by our bounded WIC decoder, max 512 px,
16 MB; transparent covers laid over the panel colour) or a note tile, title (or the file name), artist (Arabic OK),
album / year / genre, a format line ("MP3 :: 44.1 kHz, 320 kbps, stereo"), a big time (click: elapsed / remaining) and
the state. Under it a dark AIMP bar: thin seek line (knob on hover), previous / -10 s / big round play-pause / stop /
+10 s / next, speaker + volume line with the percent; narrower panes drop the slider, then the 10 s buttons.
Previous / next select the previous / next audio file of the list (wraps around, like a playlist) and keep the sound
on if the user had turned it on (`MediaPlayer::StartWithSound`); a preview still never starts with sound by itself.
**Video** (VLC look kept and completed): full-screen button (opens Quick Look), double-click the picture = full
screen, click the right time = remaining time, mouse wheel on the bar = volume +/- 5 % (also for audio).
**Results**: build clean, **79 test cases / 587 checks / 0 failures** (new: song details + format line + a broken MP3
with a huge ID3 size and an Arabic name). Checked on screen: tagged MP3 with cover and Arabic artist, untagged MP3,
narrow pane, next button, remaining time, video bar. Release rebuilt: setup 7.55 MB, SHA256 9a9f7c87df7a6421c5e66a2b1cebadd2d3e205a59b66868d939f7d32c5a1777f; portable SHA256 ccc941d3331c1904baff3f7510a96a2752417552380b3ba3eef4df6ff1fffbd9.

## App theme: Eslam's palette (2026-11-02)
Eslam chose the palette #F6BD60 yellow, #F7EDE2 cream, #F5CAC3 pink, #84A59D sage, #F28482 coral.
**Implemented** (`ui/Theme.*`, one place for every colour): cream window and panes, a darker cream toolbar band /
list header / status bar, warm-white list, warm dark-brown text (never white text on the light fills), pink
selection and hover, coral main action ("Open in program", password OK), sage icons and lines, yellow VLC-style
video bar; the AIMP-style player is a warm dark brown with coral. Owner-drawn flat buttons with Segoe MDL2 icons and
hover (toolbar: Open, New, Add, Extract To, Extract Here, Test, Delete, Names, Up), a rounded search box with a search
icon (coral frame when focused), list rows through NM_CUSTOMDRAW (no blue system highlight), a painted list header
(sort arrow, the detail-column chevron), a painted status bar, sage splitter line, Windows 11 caption colours (main
window, Quick Look, password dialog). File contents (Word, Excel, PDF, slides, text) keep white "paper".
Not themed: Windows' own menus, file dialogs and task-dialog messages.
**Results**: build clean, **79 test cases / 587 checks / 0 failures**. Checked on screen: image, PDF, slides, video,
audio, folder card. Release rebuilt: setup 7.56 MB, SHA256 dbe0c43c0e875b8c0101eb5be309d940db76fd8972c33e55dc3701d9054d4626;
portable SHA256 7c1f30504670cbc718507e22e4100760d0d4c34805fbce9c7af29647afa95112.

## App icon from Eslam's logo (2026-11-02)
Eslam sent his logo (`00-data/brand/mechano-reader-logo.png`: the "M" mark with an eye and a zipper + "MECHANO
READER - VIEW · EXTRACT · COMPRESS"). `assets/make-icon.ps1` now builds the icon from it: finds the divider line,
cuts the mark left of it, removes the white background by flood fill from the border (the white eye and zipper inside
stay), removes the soft shadow and the white gap between the legs (bottom part only, the zipper's pull column kept),
"colour to alpha" on the edges (no white fringe on dark taskbars), then app.ico with 10 sizes (16 ... 256, PNG
entries) + app-256.png + logo-mark.png. The full logo is at the top of the GitHub README (`assets/logo.png`).
The old icon is kept in `00-data/brand/old-icon-2026-11-01.ico`. The name in the app stays "Mechano Zip Reader"
(the logo says "Mechano Reader"; not changed without Eslam's word).
**Results**: 79 test cases / 587 checks / 0 failures; icon checked in the title bar and on MechanoZip.exe. Release
rebuilt: setup 7.9 MB, SHA256 251795539250880e77f20399fcc0a66458f7e5e7e54d522136455f1eb80250e9; portable 10.06 MB,
SHA256 9555900e5ca101464411280c132d7e0caf022691d27fd214af33fe38d1047560.

## Renamed to "Mechano Reader" (2026-11-02)
Eslam: the app takes the logo's name. Every name the user sees is now "Mechano Reader": window title, messages,
about / info card, version info (MZR_PRODUCT_NAME), installer (name, Start-menu / desktop shortcuts, "Open with"
name, uninstall entry), release files (`MechanoReader-<ver>-setup-x64.exe`, `MechanoReader-<ver>-portable-x64.zip`),
READMEs, LICENSE. Kept on purpose (so updates and saved settings keep working): the installer AppId, MechanoZip.exe,
HKCU\Software\MechanoZip, %LOCALAPPDATA%\MechanoZip, window class names, the project folder. An older install keeps
its folder (Inno "previous app dir"); its old-name shortcuts are removed on update ([InstallDelete]).
**Results**: 79 test cases / 587 checks / 0 failures; title bar "... - Mechano Reader"; silent install (Start menu
"Mechano Reader") + uninstall checked. Setup 7.9 MB, SHA256 5bf5fceb058dc2f0eb5ee066e5a023d572b2e95b91cf00cbb09c44f9e64e410e;
portable SHA256 c38b32cd881c996e60fa0185ecadbe6cc57049f3e4a8245a8654f583f6d16ce8.

## One setup file to share, like Mechano DM (2026-11-02)
Eslam: one EXE to share so people install straight away (like "Mechano DM Setup.exe").
release.ps1 now writes `02-final product/release/new v<ver> - <date>/` (older "new ..." folders become "old ..."):
**Mechano Reader Setup.exe** (one file: installs + opens the app) and **Mechano Reader.zip** (portable) + SHA256SUMS.txt.
Shorter wizard for people it is shared with: no welcome, no license page (LICENSE.txt still installed), no
"me / all users" question (per user, no admin; /ALLUSERS still works on the command line), no ready page, desktop
shortcut on by default, "Launch" ticked on the last page. Silent install + uninstall checked (exe, cad\, desktop
shortcut created and removed). Not code-signed: Windows SmartScreen may show "Windows protected your PC" on other PCs
(More info > Run anyway), same as Mechano DM.
0b63cc47752fc6423e29e8b12cab5520fcf1ecd02e9ca016a651be6c836a9881  Mechano Reader Setup.exe
e919c50947420df84ba986bfaa425bb616e1b6d1d1666f450afbf6ededbd14f0  Mechano Reader.zip

## F13 Explorer right-click menu (2026-11-02)
Eslam: first step to replace WinRAR on his PC. Classic (static) verbs, per user (HKCU via Inno `HKA`), new setup task
"shellmenu" (ticked), removed on uninstall. One cascading "Mechano Reader" submenu (MUIVerb + SubCommands="" + Icon):
- archives (`SystemFileAssociations\.<ext>`: zip 7z rar tar gz tgz bz2 xz zst iso cab wim 001): Open, Extract here,
  Extract to folder, Add to archive..., Compress to ZIP. Explorer shows only one submenu (the more specific key wins).
- every file (`*`) and folder (`Directory`): Add to archive..., Compress to ZIP.
Command line (app/main.cpp): `/extract-here <arc>` (Smart Extract next to the archive), `/extract-to <arc>` (always ends in
a folder of its own: `<name>\`, or the archive's single top folder), `/add <paths>` (the usual New-archive dialog),
`/zip <paths>` (`<name>.zip` next to the files, normal level, never overwrites: `name (2).zip`). Unknown switches do nothing.
The window shows the task's progress and closes itself when done (errors / reports are shown first).
Several selected files: Explorer starts one copy per file; the first copy (named mutex) collects the others' paths through
WM_COPYDATA to a message-only window until 0.5 s pass with no new file (10 s max); the others hand over and quit; only
existing absolute paths are kept, duplicates dropped. Explorer limits classic verbs to 15 selected items.
Windows 11's new short menu needs a signed MSIX "sparse package" + IExplorerCommand DLL: not done (the app is unsigned).
**Results**: 79 test cases / 587 checks / 0 failures. Checked: 3 simultaneous `/zip` copies -> one zip with all 3 (Arabic
folder name); extract-here / extract-to on a multi-item and a single-file archive; no overwrite; windows closed by themselves.
Installed silently: the real Explorer menu (IContextMenu + WM_INITMENUPOPUP) shows the submenu on .zip (5 items), a folder
and a .txt (2 items); uninstall removed every key. Not checked by hand: the "Add to archive..." dialog from Explorer.
2a8ccfa02a6204ebf89e0ca9ae62d96a29474e90258520ccfb91f924a5f01d5c  Mechano Reader Setup.exe
84b5fa61fa039b157e68e0c1df249d71239c6d41e668c6d25e7f1443b33a3a68  Mechano Reader.zip

## F14 Batch: many archives at once (2026-11-02)
Eslam: next WinRAR gap. One window does the items one after the other (MainWindow::Batch: StartBatch / NextBatchItem /
FinishBatch); no dialog per item (password prompts still come), one summary at the end: "Extracted N of M archive(s)",
files + size, and the problem list (name: reason) only if something went wrong. Esc (or Cancel on a prompt) stops the
rest ("Stopped: k not done"). Status bar shows "[2 of 5] Extracting ...".
- Explorer: every menu command now collects the selection (CollectSelection, one mutex per verb), so several archives
  + Extract here / Extract to folder = one batch window; new item "Compress each to its own ZIP" (/zip-each).
- In the app: Open now allows several files; dropping several archives (only archives): a choice of Extract each one
  here / Extract all to a folder... (each in its own folder inside it) / Put them all in one new archive / Add them to
  the open archive (only if an editable archive is open).
**Results**: 79 test cases / 587 checks / 0 failures. Checked: 3 simultaneous /zip-each -> 3 zips (Arabic folder ok);
3 simultaneous /extract-here -> one window, all extracted, closed by itself; batch with a broken .zip in the middle ->
the 2 good ones extracted, summary "Extracted 2 of 3 archive(s)" with "broken.zip: not a supported archive or damaged".
Not checked on screen: the in-app choice dialog (drop / Open several).
3f504545f2e91634e10bfff704082d28343335f037d90aba7156629f87676dec  Mechano Reader Setup.exe
cafe75861320701c8c33310f94eab2c90d7179eea56cd0e5dc0cc3937c12bc9b  Mechano Reader.zip

## Revit card redesigned like the approved mockup + 2x thumbnail (2026-11-03)
Eslam: the card on screen did not match the design shown to him (Option 1), and the thumbnail must be twice as big in the
best quality possible. Revit stores only one 128 x 128 PNG (RevitPreview4.0, ~5 KB; checked on 3 real files: no larger
picture anywhere in the file), so it is enlarged once in core/preview/Revit.cpp (ThumbnailTile: WIC high-quality cubic to
256 px, rounded anti-aliased corners over the white card, re-encoded PNG) and shown 1:1 (RichEdit's own stretching is blurry).
The top of the card is now one bordered row: left = picture + "Saved thumbnail · 128 × 128 (shown 2×)"; right = icon + file
name (bold), facts (Revit version · build, File kind (workshared / family category), Project name, Project number, Client,
Building, Last saved "31 Jan 2024, 00:14", Saved at (folder), Central model, Language) and Linked files with "Not found" /
state badges. Below: Project information / Family parameters, Types, "Linked files: full paths", the closing note.
Inner spacing: \trgaph + \sb and an empty small last line (RichEdit ignores \trpadd).
**Results**: 79 test cases / 587 checks / 0 failures; checked on screen with HARBY-VILLA-ARCH.rvt and TA1.rfa.
New release folder "new v0.9.0-beta - 2026-11-03" (the 2026-11-02 one became "old ...").
d6cd70b1ffc4a7bbb853f7a2d3144f5e601c46c286c083125a4188c323771bbf  Mechano Reader Setup.exe
f2660968f9336fa76527f5bd401176750e243e7e155ab49a87998f260c6c2954  Mechano Reader.zip

## App updates + Restart (2026-11-03)
Eslam: "a Restart option and automatic update", like Mechano DM (he chose: online for updates only, Tell me /
Install automatically / Off). This is the ONLY network use in the app.
- core/update/Updater: WinHTTP GET of `update.txt` from https://github.com/Mechano0/mechano-reader/releases/latest/download/
  (64 KB max), ECDSA P-256 / SHA-256 signature check with BCrypt against the release public key built into the app,
  then the Setup download (`Mechano.Reader.Setup.exe`, GitHub's name for "Mechano Reader Setup.exe") to
  %LOCALAPPDATA%\MechanoZip\update\ via .part, size + SHA-256 must equal the signed values (else deleted).
  update.txt = plain "key=value" lines, signature over every byte above the "signature=" line (no JSON needed in C++).
  Test override: MECHANO_READER_UPDATE_FEED (https, or http on 127.0.0.1 only); the signature is still required.
- Signing: sign-release.ps1 (PowerShell, CNG ECDsaP256). Key made once with -NewKey -> 01-wip\.private\update-signing-key.blob
  (inside the project, not mirrored to GitHub, .gitignore'd). release.ps1 -Notes "..." now writes update.txt.
  Note: the first -NewKey run used $null for the key name, which PowerShell turns into "" = a key saved in the
  Windows key store; that copy was deleted (CngKey.Open("").Delete()) and the script now uses [NullString]::Value.
  **Keep a backup of the .blob off this PC**: without it, installed copies cannot verify new releases.
- UI: new toolbar button "Menu" (≡, becomes "Update" / "Update 45%" / "Restart to update"): Restart, Check for updates,
  App updates (Tell me / Install automatically (when the app closes) / Off), Open the download page, shortcuts + version.
  Automatic check 2 min after start, then every 12 h (not when Off). Tell me: a dialog once per version (UpdateAsked),
  the button stays. Install automatically: downloads quietly, installs silently when the app closes. Update now /
  Restart: setup /VERYSILENT /RELAUNCH=1 "/OPEN=<archive>" then the app closes; the setup ([Run] + [Code]
  ShouldRelaunch / RelaunchArgs) starts it again with the same archive. Restart without an update: the app starts
  itself again with the same archive. Portable / not installed copies (no unins000.exe): only "open the download page".
- Settings: AppUpdate (0 Tell me, 1 Install automatically, 2 Off), UpdateAsked.
**Results**: 82 test cases / 613 checks / 0 failures (new test_update: signed file accepted, every changed byte /
broken signature / junk / the release key with the test signature refused; version order; file SHA-256).
End to end with a local server (scratchpad, the installed copy untouched): signed update.txt 0.9.1 -> "Update" button +
dialog -> download + SHA-256 OK -> setup started with `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS
/RELAUNCH=1 "/OPEN=...\revit-test.zip"` -> app closed. Not run yet: the real setup's relaunch (it would have closed
Eslam's open copy) and a real GitHub release (the repository does not exist yet).
ec741b24eef29d2a352a0b1c6eac94a220fd4b26ccd12f2bfca283b1404e788e  Mechano Reader Setup.exe
38a47f8613560d940910f020012c38578d7efadca697af64a36f9f7c08295902  Mechano Reader.zip
