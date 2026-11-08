; =====================================================================
;  Archan VR Capture -- the release installer
; =====================================================================
;
;  Built from the stage that tools\packaging\make_release.ps1 produces
;  (explicit file list, our two binaries signed, hashed). Build with:
;
;      powershell -ExecutionPolicy Bypass -File tools\packaging\make_release.ps1 -Version <v>
;
;  Signing is the same system as Archan XR (Microsoft Artifact Signing,
;  Archan Systems AS, through sign_release.ps1). make_release.ps1 defines the
;  Sign Tool "archan" on ISCC's command line; this script names it, so a plain
;  ISCC run without that definition stops with an error: no unsigned
;  installer by accident.
;
;  ONE KIND OF INSTALL: the layer (with the performance overlay) + the OBS capture plugin. The
;  "Overlay only" install was removed (user decision 2026-11-07, "remove the standalone for now").
;
;  WHAT GOES WHERE (the same places install.ps1 has always used)
;    {app} = %LOCALAPPDATA%\ArchanVRCapture
;      layer\    the OpenXR API layer + its manifest, registered for this
;                user (HKCU implicit layers)
;      ArchanVRCaptureSettings.exe   the settings window (overlay on/off + key), Start menu
;      tools\    restore_audio_sync.ps1, run by the uninstaller (full install)
;      logs\     written at run time, NOT installed and NOT removed
;      overlay.ini  the overlay settings, written at run time, NOT removed
;    %ProgramData%\obs-studio\plugins\archan-vr-capture\   the OBS plugin (full install)
;  No admin rights are needed: Users may create folders in ProgramData.

#define MyAppName "Archan VR Capture"
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#define MyAppPublisher "Archan Simulation"
#define MyStage "stage"
#ifndef MyOutputDir
  #define MyOutputDir "E:\ArchanVRCapture_Release_Installer"
#endif
#define MyLayerJson "XR_APILAYER_ARCHAN_vr_capture.json"
#define MyPluginDir "{commonappdata}\obs-studio\plugins\archan-vr-capture"

[Setup]
AppId={{F744916B-2546-4E9D-8529-70283527C1D3}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
VersionInfoVersion={#MyAppVersion}
DefaultDirName={localappdata}\ArchanVRCapture
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#MyOutputDir}
OutputBaseFilename=ArchanVRCapture-{#MyAppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#MyAppName} {#MyAppVersion}
; Signed: Microsoft Artifact Signing, Archan Systems AS (Sign Tool defined by make_release.ps1).
SignTool=archan
SignedUninstaller=yes
SetupLogging=yes
; OBS holds the plugin and every OpenXR program holds the layer; [Code] CloseHolders closes the
; known ones first, this page is the fallback for anything else.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
ApplicationsFound=These programs are using Archan VR Capture's files and must be closed before Setup can continue. They are usually OBS, a simulator, Pimax Home or another VR program. Please close them yourself, or let Setup close them for you, then start them again after the installation.

[Files]
; Our binaries, signed by make_release.ps1; `signcheck` makes the compiler refuse an unsigned one.
Source: "{#MyStage}\layer\XR_APILAYER_ARCHAN_vr_capture.dll"; DestDir: "{app}\layer"; Flags: ignoreversion signcheck
Source: "{#MyStage}\layer\{#MyLayerJson}"; DestDir: "{app}\layer"; Flags: ignoreversion
Source: "{#MyStage}\ArchanVRCaptureSettings.exe"; DestDir: "{app}"; Flags: ignoreversion signcheck
Source: "{#MyStage}\obs-plugin\bin\64bit\archan-vr-capture.dll"; DestDir: "{#MyPluginDir}\bin\64bit"; Flags: ignoreversion signcheck
; Valve's OpenVR loader (SteamVR mirror path): third party, shipped as Valve built it, never re-signed.
Source: "{#MyStage}\obs-plugin\bin\64bit\openvr_api.dll"; DestDir: "{#MyPluginDir}\bin\64bit"; Flags: ignoreversion
Source: "{#MyStage}\obs-plugin\data\locale\en-US.ini"; DestDir: "{#MyPluginDir}\data\locale"; Flags: ignoreversion
Source: "{#MyStage}\tools\restore_audio_sync.ps1"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "{#MyStage}\third_party\*"; DestDir: "{app}\third_party"; Flags: ignoreversion recursesubdirs
Source: "{#MyStage}\payload.manifest.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Archan VR Capture Settings"; Filename: "{app}\ArchanVRCaptureSettings.exe"

[InstallDelete]
; The installs from before the renames (install.ps1 removed them the same way).
Type: filesandordirs; Name: "{localappdata}\QuestVDtoOBS\layer"
Type: filesandordirs; Name: "{localappdata}\VRCapture\layer"
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\quest-vd-obs"
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\vr-capture"

[UninstallDelete]
; Empty folders the uninstaller leaves behind (files are removed by Inno itself). Inno removes
; only folders it created; these may come from install.ps1 (seen 2026-11-05: an empty layer\ stayed).
; {app} itself stays when logs\ is in it: the logs are not removed.
Type: dirifempty; Name: "{app}\layer"
Type: dirifempty; Name: "{app}\tools"
Type: dirifempty; Name: "{app}\third_party"
Type: dirifempty; Name: "{#MyPluginDir}\data\locale"
Type: dirifempty; Name: "{#MyPluginDir}\data"
Type: dirifempty; Name: "{#MyPluginDir}\bin\64bit"
Type: dirifempty; Name: "{#MyPluginDir}\bin"
Type: dirifempty; Name: "{#MyPluginDir}"

[Registry]
; The OpenXR implicit-layer registration: value name = manifest path, DWORD 0 = enabled.
; `uninsdeletevalue` removes exactly this value and nothing else.
Root: HKCU; Subkey: "SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"; ValueType: dword; ValueName: "{app}\layer\{#MyLayerJson}"; ValueData: 0; Flags: uninsdeletevalue

[Code]
const
  ImplicitKey = 'SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit';

{ The registrations of the installs from before the renames. }
procedure RemoveOldRegistrations();
var
  Old: TArrayOfString;
  I: Integer;
begin
  SetArrayLength(Old, 2);
  Old[0] := ExpandConstant('{localappdata}\QuestVDtoOBS\layer\XR_APILAYER_QUESTVD_obs_capture.json');
  Old[1] := ExpandConstant('{localappdata}\VRCapture\layer\XR_APILAYER_VRCAPTURE_obs.json');
  for I := 0 to GetArrayLength(Old) - 1 do
    if RegValueExists(HKCU, ImplicitKey, Old[I]) then
    begin
      RegDeleteValue(HKCU, ImplicitKey, Old[I]);
      Log('Removed the old layer registration: ' + Old[I]);
    end;
end;

{ ---------------------------------------------------------------- }
{ Closing the programs that hold our files (as Archan XR does)      }
{ ---------------------------------------------------------------- }
procedure HolderList(var Exes, Names: TArrayOfString);
begin
  SetArrayLength(Exes, 10);
  SetArrayLength(Names, 10);
  Exes[0] := 'obs64.exe';                     Names[0] := 'OBS Studio';
  Exes[1] := 'DCS.exe';                       Names[1] := 'DCS World';
  Exes[2] := 'FlightSimulator2024.exe';       Names[2] := 'Microsoft Flight Simulator 2024';
  Exes[3] := 'FlightSimulator.exe';           Names[3] := 'Microsoft Flight Simulator 2020';
  Exes[4] := 'Falcon BMS.exe';                Names[4] := 'Falcon BMS';
  Exes[5] := 'PimaxHome-Win64-Shipping.exe';  Names[5] := 'Pimax Home';
  Exes[6] := 'PimaxClient.exe';               Names[6] := 'Pimax Client';
  Exes[7] := 'PimaxToolkit.exe';              Names[7] := 'Pimax Toolkit';
  Exes[8] := 'OFXRBridgeTray.exe';            Names[8] := 'OFXR Bridge';
  Exes[9] := 'ArchanVRCaptureSettings.exe';   Names[9] := 'Archan VR Capture Settings';
end;

function IsRunning(const Exe: string): Boolean;
var
  Locator, Service, Found: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Found := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = "' + Exe + '"');
    Result := Found.Count > 0;
  except
    Log('Could not ask Windows whether ' + Exe + ' is running: ' + GetExceptionMessage);
  end;
end;

{ Names the programs, then closes them. OBS is asked to close first (so it saves its scene
  collection) and is ended only if it is still running after 15 s. }
procedure CloseHolders(const Silent: Boolean);
var
  Exes, Names: TArrayOfString;
  Running: string;
  I, Round, Code: Integer;
  Any: Boolean;
begin
  HolderList(Exes, Names);
  Running := '';
  for I := 0 to GetArrayLength(Exes) - 1 do
    if IsRunning(Exes[I]) then
      Running := Running + '    ' + Names[I] + #13#10;
  if Running = '' then
    Exit;
  Log('Closing the programs that hold Archan VR Capture''s files:' + #13#10 + Running);
  if not Silent then
    MsgBox('These programs are using Archan VR Capture''s files and will be closed now:' + #13#10#13#10
      + Running + #13#10
      + 'Anything unsaved in them will be lost, and a running OBS recording stops. Start them again when this is finished.',
      mbInformation, MB_OK);
  if IsRunning('obs64.exe') then
  begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM "obs64.exe"', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Log(Format('asked OBS to close -> %d', [Code]));
    for Round := 1 to 30 do
    begin
      if not IsRunning('obs64.exe') then
        Break;
      Sleep(500);
    end;
  end;
  for Round := 1 to 5 do
  begin
    Any := False;
    for I := 0 to GetArrayLength(Exes) - 1 do
      if IsRunning(Exes[I]) then
      begin
        Any := True;
        Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM "' + Exes[I] + '"', '', SW_HIDE, ewWaitUntilTerminated, Code);
        Log(Format('taskkill %s -> %d', [Exes[I], Code]));
      end;
    if not Any then
      Exit;
    Sleep(700);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseHolders(WizardSilent());
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    RemoveOldRegistrations();
end;

{ Uninstall: close OBS and the games, then give every audio source its own Sync Offset back in
  every scene collection (the plugin set own offset + video delay), then the files go. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
  Script: string;
begin
  if CurUninstallStep <> usUninstall then
    Exit;
  CloseHolders(UninstallSilent());
  Script := ExpandConstant('{app}\tools\restore_audio_sync.ps1');
  if FileExists(Script) then
  begin
    if Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
        '-NoProfile -ExecutionPolicy Bypass -File "' + Script + '"', '', SW_HIDE, ewWaitUntilTerminated, Code) then
      Log(Format('restore_audio_sync.ps1 -> %d', [Code]))
    else
      Log('COULD NOT run restore_audio_sync.ps1');
  end;
end;
