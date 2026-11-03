<p align="center"><img src="assets/logo.png" width="520" alt="Mechano Reader logo: view, extract, compress"></p>

# Mechano Reader

A light, **offline** archive manager for Windows 10/11 in the style of WinRAR / 7-Zip, with one difference:
you can **see the files inside an archive without extracting it**.

Free and open source (MIT) · by **Eng. Eslam Abdelqader (Mechano)** · version **0.9.0 Beta**

## Features
- Open, browse, extract, create and edit **ZIP and 7z**; open **RAR, TAR, GZ, BZ2, XZ, ZST, ISO, CAB, WIM** and
  split archives (`.001`, `.z01`, `.part1.rar`) to view and extract.
- **Preview inside the archive**: pictures, text, PDF, **Word / Excel / PowerPoint without Microsoft Office**,
  video (VLC-style bar, full screen) and audio (AIMP-style player: cover, title / artist / album, previous / next song), and **AutoCAD drawings (DWG / DXF)**: Model and Layouts, zoom, layers, colours,
  Arabic text (trial; our own light CAD core + GNU LibreDWG's `dwg2dxf` as a separate process).
- **BIM info cards** (a quick look, no Autodesk software needed): **Revit** (.rvt .rfa .rte .rft: thumbnail, version,
  project info, family types, links), **IFC** (authoring tool, project, storeys, element counts) and **Navisworks**
  (.nwd .nwc .nwf: thumbnail, version, referenced files when readable).
  Zoom, **Quick Look** (Space). Double-click opens a file in its main program.
- **Explorer right-click menu** (optional in the setup; Windows 11: "Show more options"): *Extract here*,
  *Extract to folder*, *Open* on archives; *Add to archive...*, *Compress to ZIP* and *Compress each to its own ZIP*
  on any files / folders.
- **Many archives at once**: select several in Explorer (Extract here / Extract to folder) or drop / open several in
  the window: extracted one after the other, one summary at the end (Esc stops the rest).
- Passwords (AES-256), **Arabic / Unicode names** (and a code-page fix for old ZIPs), Smart Extract,
  search, rename inside the archive (F2), drag files out to Explorer, right-click menu. F1 lists all shortcuts.
- **Optional app updates**, signed (like Mechano DM): Menu → App updates (Tell me / Install automatically / Off).
  `update.txt` in each release carries the Setup's size and SHA-256 with an ECDSA P-256 signature; the app has only
  the public key and checks both before installing. **Menu → Restart** reopens the same archive (and installs a
  waiting update). Nothing else ever goes online.
- Fast and small: starts in ~130 ms, ~5 MB RAM when idle, ~49 MB with a 100 000-file ZIP open.
- **Installer** (per-user, no admin needed, optional "Open with" entries) or **portable** (runs from a USB stick,
  no registry writes).

## Security
Every archive is treated as untrusted: Zip Slip / absolute / device / reserved names blocked, links and
alternate streams never created, zip bombs stopped, Mark of the Web kept, risky files ask before opening,
Office files parsed as data only (no macros, no external links, no DTD). A full audit is in
[`docs/SECURITY-AUDIT-PHASE7.md`](docs/SECURITY-AUDIT-PHASE7.md).

## Download
See the **Releases** page: `Mechano Reader Setup.exe` (one file: installs and opens the app, no admin needed) or `Mechano Reader.zip` (portable), with `SHA256SUMS.txt`.
The beta is not code-signed yet, so SmartScreen may warn: "More info" → "Run anyway".

## Build from source
Requirements: Visual Studio 2022 (or Build Tools) with the C++ workload (MSVC, Windows SDK, CMake).
Clone into a short path (for example `C:\src\MechanoZipReader`): MSBuild fails on very long paths.
```
.\build.ps1            # builds 7z.dll from third_party\7zip-26.05 if needed, then Release x64 + tests
.\release.ps1          # installer + portable zip (needs Inno Setup 6 in tools\InnoSetup6)
```
Stack: C++20, Win32, 7-Zip 26.05 `7z.dll`, pugixml 1.16. No Electron, no runtime internet.

## License
MIT for this project ([LICENSE.txt](LICENSE.txt)). `7z.dll` is 7-Zip by Igor Pavlov under the GNU LGPL with the
unRAR restriction for the RAR code; pugixml is MIT. The DWG view uses GNU LibreDWG's `dwg2dxf.exe` (GPL-3), shipped
unchanged in the `cad` folder and started as a separate program (never linked); its exact source is
`third_party/libredwg-0.14.8739/libredwg-0.14.8739.tar.xz`. The LibreDWG Windows binaries are not in this repository:
download `libredwg-0.14.8739-win64.zip` from the LibreDWG releases page into `third_party/libredwg-0.14.8739/win64/`
(without them the app still builds and shows the DWG thumbnail only). See `THIRD-PARTY-NOTICES.txt` in the release.
