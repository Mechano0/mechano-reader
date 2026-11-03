Mechano Reader 0.9.0 Beta
=============================
A light, offline archive manager for Windows 10/11 (64-bit) by Eng. Eslam Abdelqader (Mechano).
Free and open source (MIT License).

What it does
- Open, browse, extract, create and edit ZIP and 7z archives; open RAR, TAR, GZ, BZ2, XZ, ZST,
  ISO, CAB, WIM and split archives (.001, .z01, .part1.rar) to view and extract.
- Preview files INSIDE the archive without extracting them: pictures, text, PDF, Word, Excel,
  PowerPoint (no Microsoft Office needed), video (VLC-style bar), audio (AIMP-style player: cover,
  title, artist, album, previous / next song), and AutoCAD drawings (DWG / DXF):
  Model and every Layout with ◀ ▶, zoom, layers and colours, Arabic text (trial).
- BIM info cards (a quick look, no Autodesk software needed): Revit (.rvt .rfa .rte .rft: thumbnail,
  version, project info, family types, links), IFC (tool, project, storeys, element counts) and
  Navisworks (.nwd .nwc .nwf: thumbnail, version, referenced files when readable).
- Double-click a file (or Enter, or right-click > "Open in main program") to open it in the program
  Windows uses for it (AutoCAD, Word, a PDF reader ...).
- Explorer right-click menu "Mechano Reader" (chosen in the setup; on Windows 11 under
  "Show more options"): Extract here, Extract to folder, Open on archives; Add to archive... and
  Compress to ZIP on any files and folders (several selected = one archive), Compress each to its own ZIP.
- Many archives at once: select several archives in Explorer > Extract here / Extract to folder, or
  drop / open several archives in the window: extracted one after the other, one summary at the end.
- Passwords (AES-256), Arabic / Unicode file names, Smart Extract, Quick Look (Space),
  drag files out to Explorer, rename inside the archive (F2). Press F1 for all shortcuts.
- Works offline. The only thing that can go online is the optional app update (Menu > App updates:
  Tell me / Install automatically / Off): a signed update from GitHub, checked before it installs.
  Menu > Restart opens the app again with the same archive. No account, no ads, no telemetry.

Safety
- Every archive is treated as untrusted: names that try to write outside the destination
  folder (Zip Slip), links, device names and hidden text-direction tricks are blocked;
  zip bombs are stopped; files that can run programs ask before opening.
- Mark of the Web is kept on extracted files.

Files and settings
- Installed copy: settings in HKCU\Software\MechanoZip, temp files and log in
  %LOCALAPPDATA%\MechanoZip. The uninstaller removes both.
- Portable copy (the folder has a "portable.flag" file): no registry writes; settings and log in
  the "Data" folder next to MechanoZip.exe; temp files in %TEMP%\MechanoZip (removed at the
  next start). Delete the folder to remove it.

Note
- This beta is not code-signed yet, so Windows SmartScreen may show "Windows protected your PC".
  Click "More info" > "Run anyway" only if you downloaded it from the official GitHub page.

License: MIT (see LICENSE.txt). 7z.dll is 7-Zip by Igor Pavlov (GNU LGPL + unRAR restriction),
pugixml by Arseny Kapoulkine (MIT), and the "cad" folder holds GNU LibreDWG's dwg2dxf (GPL-3, a
separate program used for the DWG view): see THIRD-PARTY-NOTICES.txt and cad\README-LibreDWG.txt.
