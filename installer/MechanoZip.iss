; Mechano Reader - installer + uninstaller (Inno Setup 6).
; Built by ..\release.ps1, which stages the files in build\stage and runs:
;   tools\InnoSetup6\ISCC.exe /DStage=<stage folder> /DOutDir=<release folder> installer\MechanoZip.iss
; The version is read from the built exe (single source: assets\version.h).

#ifndef Stage
  #error Stage folder not given (run release.ps1)
#endif
#ifndef OutDir
  #define OutDir "."
#endif

#define AppExe       Stage + "\MechanoZip.exe"
#define AppName      "Mechano Reader"
#define AppVersion   GetVersionNumbersString(AppExe)
#define AppVerText   GetStringFileInfo(AppExe, "ProductVersion")
#define AppPublisher "Eng. Eslam Abdelqader (Mechano)"
; "0.9.0 Beta" -> "0.9.0-beta" for the file name
#define FileVer      StringChange(LowerCase(AppVerText), " ", "-")

[Setup]
; Never change AppId: Windows uses it to find the installed copy for updates / uninstall.
AppId={{6E0B8F3A-4C1D-4E7A-9B52-8D3E1F7A2C90}
AppName={#AppName}
AppVersion={#AppVerText}
AppVerName={#AppName} {#AppVerText}
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
AppPublisher={#AppPublisher}
AppCopyright=Copyright (C) 2026 Eng. Eslam Abdelqader. MIT License.
DefaultDirName={autopf}\Mechano Reader
DefaultGroupName=Mechano Reader
DisableProgramGroupPage=yes
; Per-user install (no admin needed): quick for people the setup is shared with.
PrivilegesRequired=lowest
; No "for me / all users" question when shared: per-user, no admin. (/ALLUSERS on the command line still works.)
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; Short wizard (like Mechano DM Setup): options -> install -> finish (opens the app). LICENSE.txt is still installed.
DisableWelcomePage=yes
DisableDirPage=auto
DisableReadyPage=yes
SetupIconFile=..\assets\app.ico
UninstallDisplayIcon={app}\MechanoZip.exe
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Close a running copy before replacing files, and restart it afterwards if it was running.
CloseApplications=yes
RestartApplications=no
ChangesAssociations=yes
OutputDir={#OutDir}
OutputBaseFilename=Mechano Reader Setup

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ar"; MessagesFile: "compiler:Languages\Arabic.isl"

[CustomMessages]
en.AssocGroup=Archive files:
en.AssocTask=Add "Open with Mechano Reader" for ZIP, 7z, RAR, TAR, GZ, ISO ... (does not change your default app)
ar.AssocGroup=ملفات الأرشيف:
ar.AssocTask=إضافة «فتح بـ Mechano Reader» لملفات ZIP و7z وRAR وTAR وGZ وISO ... (لا يغيّر البرنامج الافتراضي)
en.MenuTask=Add "Mechano Reader" to the Explorer right-click menu (Extract here, Extract to folder, Add to archive)
ar.MenuTask=إضافة «Mechano Reader» لقائمة الزر الأيمن في Explorer (فك هنا، فك في فولدر، إضافة لأرشيف)

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "assoc"; Description: "{cm:AssocTask}"; GroupDescription: "{cm:AssocGroup}"
Name: "shellmenu"; Description: "{cm:MenuTask}"; GroupDescription: "{cm:AssocGroup}"

[Files]
Source: "{#Stage}\MechanoZip.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\7z.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
; F7: DWG drawing view helper (LibreDWG, GPL-3, own process) + its license and source note.
Source: "{#Stage}\cad\*"; DestDir: "{app}\cad"; Flags: ignoreversion

; Renamed to "Mechano Reader" (2026-11-02): remove the shortcuts an older version made under the old name.
[InstallDelete]
Type: files; Name: "{autoprograms}\Mechano Zip Reader.lnk"
Type: files; Name: "{autodesktop}\Mechano Zip Reader.lnk"

[Icons]
Name: "{autoprograms}\Mechano Reader"; Filename: "{app}\MechanoZip.exe"
Name: "{autodesktop}\Mechano Reader"; Filename: "{app}\MechanoZip.exe"; Tasks: desktopicon

[Registry]
; The app's saved settings (written by the app itself): never created here, removed on uninstall.
Root: HKCU; Subkey: "Software\MechanoZip"; Flags: dontcreatekey uninsdeletekey
; "Open with" entries only (Windows 10/11 let the user pick the default app; we never take it over).
; HKA = HKCU for a per-user install, HKLM for an all-users install. Removed on uninstall.
Root: HKA; Subkey: "Software\Classes\MechanoZip.Archive"; ValueType: string; ValueData: "Archive (Mechano Reader)"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\MechanoZip.Archive\DefaultIcon"; ValueType: string; ValueData: "{app}\MechanoZip.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\MechanoZip.Archive\shell\open\command"; ValueType: string; ValueData: """{app}\MechanoZip.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\Applications\MechanoZip.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "Mechano Reader"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\Applications\MechanoZip.exe\shell\open\command"; ValueType: string; ValueData: """{app}\MechanoZip.exe"" ""%1"""; Tasks: assoc
#define Ext(e) \
  'Root: HKA; Subkey: "Software\Classes\.' + e + '\OpenWithProgids"; ValueType: string; ValueName: "MechanoZip.Archive"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc' + NewLine + \
  'Root: HKA; Subkey: "Software\Classes\Applications\MechanoZip.exe\SupportedTypes"; ValueType: string; ValueName: ".' + e + '"; ValueData: ""; Tasks: assoc' + NewLine
#emit Ext("zip")
#emit Ext("7z")
#emit Ext("rar")
#emit Ext("tar")
#emit Ext("gz")
#emit Ext("tgz")
#emit Ext("bz2")
#emit Ext("xz")
#emit Ext("zst")
#emit Ext("iso")
#emit Ext("cab")
#emit Ext("wim")
#emit Ext("001")

; Explorer right-click menu: one "Mechano Reader" submenu (classic menu; on Windows 11 it is under
; "Show more options" / Shift+F10). Archives get Open / Extract here / Extract to folder (+ the Add
; commands, so there is only one submenu); every file and folder gets Add to archive / Compress to ZIP.
; All per user (HKA = HKCU here), removed on uninstall.
#define MenuRoot(k) \
  'Root: HKA; Subkey: "Software\Classes\' + k + '\shell\MechanoReader"; ValueType: string; ValueName: "MUIVerb"; ValueData: "Mechano Reader"; Flags: uninsdeletekey; Tasks: shellmenu' + NewLine + \
  'Root: HKA; Subkey: "Software\Classes\' + k + '\shell\MechanoReader"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\MechanoZip.exe"",0"; Tasks: shellmenu' + NewLine + \
  'Root: HKA; Subkey: "Software\Classes\' + k + '\shell\MechanoReader"; ValueType: string; ValueName: "SubCommands"; ValueData: ""; Tasks: shellmenu' + NewLine
#define MenuItem(k, id, text, sw) \
  'Root: HKA; Subkey: "Software\Classes\' + k + '\shell\MechanoReader\shell\' + id + '"; ValueType: string; ValueName: "MUIVerb"; ValueData: "' + text + '"; Tasks: shellmenu' + NewLine + \
  'Root: HKA; Subkey: "Software\Classes\' + k + '\shell\MechanoReader\shell\' + id + '\command"; ValueType: string; ValueData: """{app}\MechanoZip.exe"" ' + sw + ' ""%1"""; Tasks: shellmenu' + NewLine
#define AddItems(k) \
  MenuItem(k, "4add", "Add to archive...", "/add") + \
  MenuItem(k, "5zip", "Compress to ZIP", "/zip") + \
  MenuItem(k, "6each", "Compress each to its own ZIP", "/zip-each")
#define ArcMenu(e) \
  MenuRoot("SystemFileAssociations\." + e) + \
  MenuItem("SystemFileAssociations\." + e, "1open", "Open", "") + \
  MenuItem("SystemFileAssociations\." + e, "2here", "Extract here", "/extract-here") + \
  MenuItem("SystemFileAssociations\." + e, "3folder", "Extract to folder", "/extract-to") + \
  AddItems("SystemFileAssociations\." + e)
#emit MenuRoot("*")
#emit AddItems("*")
#emit MenuRoot("Directory")
#emit AddItems("Directory")
#emit ArcMenu("zip")
#emit ArcMenu("7z")
#emit ArcMenu("rar")
#emit ArcMenu("tar")
#emit ArcMenu("gz")
#emit ArcMenu("tgz")
#emit ArcMenu("bz2")
#emit ArcMenu("xz")
#emit ArcMenu("zst")
#emit ArcMenu("iso")
#emit ArcMenu("cab")
#emit ArcMenu("wim")
#emit ArcMenu("001")

[Run]
Filename: "{app}\MechanoZip.exe"; Description: "{cm:LaunchProgram,Mechano Reader}"; Flags: nowait postinstall skipifsilent
; App update (Menu > Restart to update): the app runs this setup with /VERYSILENT /RELAUNCH=1 /OPEN="<archive>";
; after installing, Mechano Reader starts again with the same archive.
Filename: "{app}\MechanoZip.exe"; Parameters: "{code:RelaunchArgs}"; Flags: nowait; Check: ShouldRelaunch

[Code]
function ShouldRelaunch: Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:RELAUNCH|0}') = '1');
end;

function RelaunchArgs(Param: String): String;
var
  F: String;
begin
  F := ExpandConstant('{param:OPEN|}');
  if (F <> '') and (Pos('"', F) = 0) and FileExists(F) then Result := '"' + F + '"' else Result := '';
end;

[UninstallDelete]
; The app's own temp files / log / settings for the user who uninstalls (rebuilt if reinstalled).
Type: filesandordirs; Name: "{localappdata}\MechanoZip"

