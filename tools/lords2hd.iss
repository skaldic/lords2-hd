; Inno Setup script for the Lords of the Realm II HD installer. Built by tools\make_release.ps1, which
; stages the files in dist\stage and passes the version: ISCC /DAppVer=1.3.0 tools\lords2hd.iss
#ifndef AppVer
  #define AppVer "0.0.0"
#endif

[Setup]
AppId={{7B1F2C64-3E0A-4D2B-9C1E-5A8F4D2E6B10}
AppName=Lords of the Realm II HD
AppVersion={#AppVer}
AppVerName=Lords of the Realm II HD {#AppVer}
AppPublisher=Skaldic (fan patch, not affiliated with the game's owners)
AppPublisherURL=https://github.com/skaldic/lords2-hd
DefaultDirName={code:GameDir}
DirExistsWarning=no
AppendDefaultDirName=no
DisableProgramGroupPage=yes
UsePreviousAppDir=no
OutputDir=..\dist
OutputBaseFilename=Lords2-HD-{#AppVer}-setup
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayName=Lords of the Realm II HD (fan patch)
LicenseFile=..\dist\stage\lords2hd\LICENSE.md
InfoBeforeFile=..\dist\stage\lords2hd\README.txt

[Messages]
SelectDirLabel3=Choose the folder of your Lords of the Realm II game, GOG or Steam (the one holding Lords2.exe). The patch is added to it; none of the game's files are changed.

[InstallDelete]
; the loose art of the zip releases (1.2 and before): the pack replaces it
Type: filesandordirs; Name: "{app}\lords2hd\screens"
Type: filesandordirs; Name: "{app}\lords2hd\wide"
Type: filesandordirs; Name: "{app}\lords2hd\tiles"
Type: filesandordirs; Name: "{app}\lords2hd\sprites"

[Files]
Source: "..\dist\stage\ddraw.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\stage\lords2hd.pak"; DestDir: "{app}"; Flags: ignoreversion
; the multiplayer layer (LotR2 Fix dplayx.dll by Wolfe and Socius): everyone in a game needs the same one
Source: "..\dist\stage\dplayx.dll"; DestDir: "{app}"; Flags: ignoreversion
; the settings are the player's: kept on an update
Source: "..\dist\stage\lords2hd.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist
Source: "..\dist\stage\lords2hd\*"; DestDir: "{app}\lords2hd"; Flags: ignoreversion recursesubdirs

[UninstallDelete]
Type: files; Name: "{app}\lords2hd.log"
Type: files; Name: "{app}\lords2hd.ini"
Type: files; Name: "{app}\lords2hd-remote.txt"
Type: files; Name: "{app}\lords2hd-muted.txt"
; the multiplayer layer's own files, and the multiplayer session logs
Type: files; Name: "{app}\dplayx.log"
Type: files; Name: "{app}\dplayx.prev.log"
Type: files; Name: "{app}\desync.txt"
Type: files; Name: "{app}\lotr2net.ini"
Type: files; Name: "{app}\lotr2net.ini.old"
Type: filesandordirs; Name: "{app}\lords2hd-mp"
; the folders, if nothing of the player's own is left in them (MP3s and music they added stay)
Type: dirifempty; Name: "{app}\lords2hd\music"
Type: dirifempty; Name: "{app}\lords2hd"

[Code]
// The game's folder: GOG's registry key for Lords of the Realm II, else the Steam copy (app 397350, the
// same exe as GOG's, one level down in English\Lords of the Realm II), else GOG's default folder.
function GameDir(Param: String): String;
var P: String;
begin
  if RegQueryStringValue(HKLM32, 'SOFTWARE\GOG.com\Games\1207663263', 'path', P) and FileExists(AddBackslash(P) + 'Lords2.exe') then
    Result := P
  else if RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\1207663263_is1', 'InstallLocation', P) and FileExists(AddBackslash(P) + 'Lords2.exe') then
    Result := RemoveBackslash(P)
  else if RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 397350', 'InstallLocation', P) and FileExists(AddBackslash(P) + 'English\Lords of the Realm II\Lords2.exe') then
    Result := AddBackslash(P) + 'English\Lords of the Realm II'
  else if RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 397350', 'InstallLocation', P) and FileExists(AddBackslash(P) + 'English\Lords of the Realm II\Lords2.exe') then
    Result := AddBackslash(P) + 'English\Lords of the Realm II'
  else
    Result := ExpandConstant('{commonpf32}\GOG Galaxy\Games\Lords of the Realm II');
end;
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not FileExists(AddBackslash(WizardDirValue) + 'Lords2.exe') then begin
    MsgBox('Lords2.exe is not in this folder. Choose the folder your game is installed in.', mbError, MB_OK);
    Result := False;
  end;
end;
