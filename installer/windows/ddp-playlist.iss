; DDP Playlist Player - Windows installer (Inno Setup 6)
; Built automatically by .github/scripts/Package-Windows.ps1
; Defines passed on the command line: MyAppVersion, PluginSrc, OutputDir, OutputBase

#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#ifndef PluginSrc
  #define PluginSrc "..\..\release\Release"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\release"
#endif
#ifndef OutputBase
  #define OutputBase "ddp-playlist-" + MyAppVersion + "-windows-x64-Installer"
#endif

#define MyAppName "DDP Playlist Player for OBS"
#define MyAppPublisher "Drag Drop Production"
#define MyAppURL "https://dragdropproduction.com"
#define PluginFolder "ddp-playlist"

[Setup]
AppId={{7D458C97-121D-47D5-B34C-C819DB4E1305}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
; OBS 28+ loads plugins for all users from C:\ProgramData\obs-studio\plugins
DefaultDirName={commonappdata}\obs-studio\plugins\{#PluginFolder}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBase}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}
LicenseFile=..\..\LICENSE
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#PluginSrc}\{#PluginFolder}\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs

[Messages]
FinishedLabel=DDP Playlist Player is installed.%n%nOpen OBS Studio. The "DDP Playlist" panel appears on the first launch (later you can find it under Docks > DDP Playlist).

[Code]
function IsObsRunning(): Boolean;
var
  Locator, Service, Procs: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Procs := Service.ExecQuery('SELECT Name FROM Win32_Process WHERE Name = ''obs64.exe''');
    Result := (Procs.Count > 0);
  except
    Result := False;
  end;
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  while IsObsRunning() do
  begin
    if MsgBox('OBS Studio is open. Please close OBS, then click OK to continue.' + #13#10 + #13#10 +
              'Click Cancel to stop the installer.', mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  while IsObsRunning() do
  begin
    if MsgBox('OBS Studio is open. Please close OBS, then click OK to continue.', mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;
