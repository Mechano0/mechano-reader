# Phase 7: Security audit (2026-11-01): review + fixes

Scope (brief, Phase 7): archive input, paths, extraction, preview engines, temp files, external content.
Method: code review of `src/core/**`, `src/ui/MainWindow.cpp`, `src/app/main.cpp`, plus one proof-of-concept
run (scratch folder, outside the project) for the most serious finding.

Toolchain after the Windows reinstall: VS 2022 Build Tools (MSVC 14.44.35207, SDK 10.0.26100, bundled CMake).
The old build folder pointed to the removed VS Community, so it was moved to `build/x64-old-2026-10-31`
and the project was built fresh: clean build, **57 test cases / 367 checks / 0 failures**.

## Findings (most severe first)

| # | Severity | Area | Finding | Status |
|---|---|---|---|---|
| S1 | **High** (crash) | Preview: Office | Unbounded recursion over XML in `Ooxml.cpp` (`Find`, `DocxConverter::Block`, PPTX `Tree`, `LoadPlaceholders` walk). A hostile .docx/.pptx with deeply nested elements overflows the worker thread stack and **kills the whole app just by selecting the file** (preview runs automatically). PoC: an 11 KB .docx with 200 000 nested `<w:customXml>` -> `0xC00000FD` STATUS_STACK_OVERFLOW. The PPTX `shapes_ > 3000` counter does not bound depth enough for a 1 MB stack. | **Fixed** |
| S2 | **Medium** (reliability, false "zip bomb") | Extraction: ExtractGuard | 7-Zip reports `kpidPackSize` only for the FIRST file of each solid block (`7zHandler.cpp:596`); other files get 0, and `ExtractGuard` then uses packed = 1. Any non-first file larger than 1 GB in a solid .7z is stopped as a "possible zip bomb". | **Fixed** |
| S3 | **Medium** (warning bypass) | Extract & Open | The "can run code" warning checks the raw name. An entry named `invoice.exe.` or `invoice.exe ` has no risky extension, the sanitizer strips the trailing dot/space, the launch path is not found, the fallback picks `invoice.exe` and `ShellExecute` runs it **without the warning**. (Mark of the Web / SmartScreen still applies when the archive itself has MotW.) | **Fixed** |
| S4 | Medium (data loss) | Extraction: Overwrite mode | With "Overwrite", the existing file is opened with `CREATE_ALWAYS` (truncated) before decoding; if the entry then fails (damaged data, wrong password, cancel), the partial file is deleted, so the user's original file is lost. Also writes through an existing hard link. Fix: write to a temp name in the same folder, then replace on success. | **Fixed** |
| S5 | Low-Medium | Preview: Word | EMF pictures inside .docx are passed to RichEdit (GDI EMF parser = historic attack surface). PPTX already shows EMF as a placeholder. Proposal: placeholder in Word too (PNG/JPEG only). | **Fixed** |
| S6 | Low | Extract & Open | Risky-extension list misses some types that run code: .msc, .scf, .sct, .wsc, .psm1, .ps1xml, .vb, .chm, .hlp, .inf, .gadget, .settingcontent-ms, .appref-ms, .xbap, .xll, .diagcab, .website. | **Fixed** |
| S7 | Low | Temp folders | `RandomHex()` (QPC ^ tick + thread id) is predictable and not checked for an existing folder. Only the same user can write in `%LOCALAPPDATA%\MechanoZip\tmp`, so impact is low. Fix: `BCryptGenRandom` + require the folder to be new. | **Fixed** |
| S8 | Low | Preview: Office | Integer overflow possible in picture sizing (`ht * maxW_` with a hostile `cy`), and in `InMemStream::Seek` (`base + offset`). Not memory-unsafe today, but UB. | **Fixed** |

## Checked and OK
- **Paths (Zip Slip):** `..` rejected, absolute/drive/UNC/device forms rejected, NUL rejected, `:` (ADS), control and reserved characters replaced, reserved DOS names (incl. COM0, superscripts) prefixed, trailing dots/spaces made explicit, depth/length limits, then `GetFullPathNameW` + component-boundary "inside destination" proof.
- **Links:** symlinks / hard links / Unix links / reparse attrs detected and skipped by default; alternate streams never created; an existing link or folder at the target is never written through (renamed instead).
- **Bombs:** entry count, free disk space, optional total limit, overflow-safe counters, ratio after 1 GB; preview reads have a hard byte cap that ignores declared sizes.
- **Attributes / times:** only READONLY / HIDDEN / ARCHIVE restored. Mark of the Web copied from the archive.
- **Partial files** deleted on failure / cancel (except S4 case above).
- **Update / rename:** temp archive + `ReplaceFileW`, original untouched on failure.
- **DLL loading:** `SetDefaultDllDirectories`, 7z.dll by full path.
- **Multi-volume:** volume names cannot leave the archive's folder; in-memory archives never look for volumes.
- **Passwords:** never written to disk, wiped after use, preview never prompts.
- **Office:** pugixml (no DTD/entities, no XXE), external relationships never fetched, macros/OLE/ActiveX ignored, size caps per part, image budgets. RTF text escaping is correct (`\ { }` escaped, non-ASCII as `\u`).
- **DWG:** bounds-checked parser, fuzz-style test already present. **PDF:** Windows.Data.Pdf (no JavaScript), password PDFs refused, render size clamped. **Images:** 16k side cap, decode reduced.
- **Temp cleanup:** private folder under `%LOCALAPPDATA%`, removed on selection change / exit / next start.
- **Log:** bounded to 1 MB + one `.old` file.

## Fixes made (approved by Eslam 2026-11-01)
1. **S1** `Ooxml.cpp`: every parsed XML part is checked right after parsing; deeper than 256 levels = treated as
   damaged (so every recursive converter has a bounded depth). `Find` is now a non-recursive walk.
2. **S2** `ExtractGuard`: packed size 0 = unknown, not 1 byte. Those entries use everything written vs the archive
   file size (`kpidPhySize`, else the file size), same 1000x / 1 GB rule. Bombs in solid blocks are still caught.
3. **S3 + S6** new `security::IsRiskyFileName` (trailing dots/spaces and hidden direction marks removed first,
   ~50 extensions). Extract & Open checks the archive name AND the file actually written before `ShellExecute`.
4. **S4** Overwrite writes to `~mzr<random>.tmp` in the same folder (`CREATE_NEW`), then `MoveFileEx` replaces the
   original only after a good decode (read-only/hidden originals handled). On failure the original stays and the
   temp file is deleted; a hard-linked original is replaced, not written through.
5. **S5** Word: EMF pictures shown as "[picture]" (PNG/JPEG only). **S7** `fs::RandomHex` (BCryptGenRandom) +
   `fs::NewTempSubfolder` (the folder must be new) for preview / Extract & Open / drag-out. **S8** picture extents
   clamped; `InMemStream::Seek` overflow-checked.

## Verification
- Build clean (/W4, 0 warnings). Tests: **63 test cases / 419 checks / 0 failures** (6 new cases in
  `tests/security/test_phase7.cpp` + deep-XML case in `tests/unit/test_office.cpp`; the solid-7z test also checks
  that the big file really has packSize 0, i.e. the case that used to fail).
- Real app, same hostile archive (a zip with the 11 KB deep .docx, file selected): **old build crashed
  (0xC00000FD), new build stays open**.
- Real app regression (key messages): sample-preview.zip opens, previews load, the video temp folder now has a
  random name (`pv-` + 16 hex).
- Not covered by tests: S3 dialog itself (UI), S7 fallback when BCrypt fails.
