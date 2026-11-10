; Inno Setup script for the Windows installer.
;
; Built in CI (see .github/workflows/build.yml):
;   iscc /DMyAppVersion=1.0.0 /DSourceDir=<repo root> installer\ASampler.iss
;
; Inno Setup's wizard is a standard Win32 dialog, so NVDA and JAWS read every
; page and button without any extra work - unlike most custom installers.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir ".."
#endif
#ifndef BuildConfig
  #define BuildConfig "Release"
#endif

[Setup]
AppId={{6E0C4D1B-3B8A-4F0E-9B7C-2B5D8A1E4F70}
AppName=ASampler
AppVersion={#MyAppVersion}
AppPublisher=Hermis Kasperavičius
DefaultDirName={autopf}\ASampler
DefaultGroupName=ASampler
DisableProgramGroupPage=yes
OutputDir={#SourceDir}\package
OutputBaseFilename=ASampler-{#MyAppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; VST3 plug-ins live in the shared Common Files folder, which needs admin.
PrivilegesRequired=admin
UninstallDisplayName=ASampler {#MyAppVersion}
WizardStyle=modern
; The REAPER script goes into the installing user's REAPER folder on purpose.
UsedUserAreasWarning=no

[Tasks]
; Shown only when REAPER has been run on this account. Checked by default:
; without it the user has to find and load the script through the Actions
; list by hand.
Name: "reaperaction"; Description: "Add the ""Create pad tracks"" action to REAPER's Actions list"; \
    Check: ReaperInstalled

[Files]
; The whole VST3 bundle folder, into the standard VST3 location every host scans.
Source: "{#SourceDir}\build\ASampler_artefacts\{#BuildConfig}\VST3\ASampler.vst3\*"; \
    DestDir: "{commoncf64}\VST3\ASampler.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\docs\manual.html"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.md";        DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\CHANGELOG.md";     DestDir: "{app}"; Flags: ignoreversion
; The REAPER routing script: always next to the manual, and into REAPER's
; Scripts folder (registered as an action, see [Code]) when the task is chosen.
Source: "{#SourceDir}\scripts\ASampler - Create pad tracks.lua"; DestDir: "{app}\REAPER"; Flags: ignoreversion
Source: "{#SourceDir}\scripts\ASampler - Create pad tracks.lua"; DestDir: "{userappdata}\REAPER\Scripts\ASampler"; \
    Flags: ignoreversion; Tasks: reaperaction

[Icons]
Name: "{group}\ASampler Manual"; Filename: "{app}\manual.html"
Name: "{group}\Uninstall ASampler"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\manual.html"; Description: "Open the ASampler manual"; \
    Flags: postinstall shellexec skipifsilent unchecked

[UninstallDelete]
Type: dirifempty; Name: "{userappdata}\REAPER\Scripts\ASampler"

[Code]
const
  // The line REAPER itself writes to reaper-kb.ini when this script is loaded
  // from Scripts\ASampler (same command ID), so a later manual load is seen as
  // the same action rather than a duplicate.
  ScriptFileName = 'ASampler - Create pad tracks.lua';
  ActionLine = 'SCR 4 0 RS8fbce66813089a3b2463981f9d9180baf10de0c0 "Custom: ASampler - Create pad tracks.lua" "ASampler/ASampler - Create pad tracks.lua"';

function ReaperInstalled: Boolean;
begin
  Result := DirExists(ExpandConstant('{userappdata}\REAPER'));
end;

function KeyMapFile: String;
begin
  Result := ExpandConstant('{userappdata}\REAPER\reaper-kb.ini');
end;

function ReaperRunning: Boolean;
begin
  Result := FindWindowByClassName('REAPERwnd') <> 0;
end;

// REAPER keeps its action list in memory and writes it back, so the key map
// must be edited while REAPER is closed. Returns False if the user cancels.
function WaitForReaperClosed(const Why: String): Boolean;
begin
  Result := True;
  while ReaperRunning do
  begin
    if MsgBox('REAPER is running. ' + Why + #13#10#13#10 +
              'Close REAPER, then choose Retry.', mbInformation, MB_RETRYCANCEL) <> IDRETRY then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

// The key map is edited as raw bytes, line by line, so the user's other
// entries (which may hold non-ASCII paths) are written back exactly as they
// were.

// Splits off the first line of Rest, including its line break.
function TakeLine(var Rest: AnsiString): AnsiString;
var
  P: Integer;
begin
  P := Pos(#10, Rest);
  if P = 0 then
  begin
    Result := Rest;
    Rest := '';
  end else
  begin
    Result := Copy(Rest, 1, P);
    Rest := Copy(Rest, P + 1, Length(Rest));
  end;
end;

// The command ID of a script line ("SCR 4 0 RS1234... ..." -> "RS1234...").
function ScriptCommandId(const Line: AnsiString): AnsiString;
var
  Rest: AnsiString;
  I, P: Integer;
begin
  Rest := Trim(Line);
  for I := 1 to 3 do
  begin
    P := Pos(' ', Rest);
    if P = 0 then
    begin
      Result := '';
      Exit;
    end;
    Rest := TrimLeft(Copy(Rest, P + 1, Length(Rest)));
  end;
  P := Pos(' ', Rest);
  if P > 0 then
    Result := Copy(Rest, 1, P - 1)
  else
    Result := Rest;
end;

function IsScriptLine(const Line: AnsiString): Boolean;
begin
  Result := (Pos('SCR ', Line) = 1) and (Pos(ScriptFileName, Line) > 0);
end;

// The key map without the script's actions, and without any shortcut keys
// bound to them, which would otherwise point at nothing once it is gone.
function WithoutScriptActions(const Content: AnsiString): AnsiString;
var
  Rest, Line, Ids: AnsiString;
begin
  // Pass 1: which command IDs (as "_RS...|") belong to the script.
  Ids := '';
  Rest := Content;
  while Length(Rest) > 0 do
  begin
    Line := TakeLine(Rest);
    if IsScriptLine(Line) and (ScriptCommandId(Line) <> '') then
      Ids := Ids + '_' + ScriptCommandId(Line) + '|';
  end;

  // Pass 2: drop those script lines and the KEY lines that run them.
  Result := '';
  Rest := Content;
  while Length(Rest) > 0 do
  begin
    Line := TakeLine(Rest);
    if IsScriptLine(Line) then
      Continue;
    // A KEY line names the command as "_RS...", the fourth field like SCR's.
    if (Pos('KEY ', Line) = 1) and (Ids <> '')
       and (Pos(ScriptCommandId(Line) + '|', Ids) > 0) then
      Continue;
    Result := Result + Line;
  end;
end;

procedure AddReaperAction;
var
  Content: AnsiString;
  Rest: AnsiString;
begin
  Content := '';
  if FileExists(KeyMapFile) then
    LoadStringFromFile(KeyMapFile, Content);

  // Already in the Actions list (an earlier install, or loaded by hand,
  // perhaps with a shortcut on it): leave it as it is.
  Rest := Content;
  while Length(Rest) > 0 do
    if IsScriptLine(TakeLine(Rest)) then
      Exit;

  if (Length(Content) > 0) and (Content[Length(Content)] <> #10) then
    Content := Content + #13#10;
  Content := Content + ActionLine + #13#10;
  if not SaveStringToFile(KeyMapFile, Content, False) then
    Log('Could not write ' + KeyMapFile);
end;

procedure RemoveReaperAction;
var
  Content: AnsiString;
begin
  if not FileExists(KeyMapFile) then Exit;
  if not LoadStringFromFile(KeyMapFile, Content) then Exit;
  if Pos(ScriptFileName, Content) = 0 then Exit;
  SaveStringToFile(KeyMapFile, WithoutScriptActions(Content), False);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and WizardIsTaskSelected('reaperaction') then
    Result := WaitForReaperClosed('It must be closed so the ASampler action can be added to its Actions list.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('reaperaction') then
    AddReaperAction;
end;

function InitializeUninstall: Boolean;
begin
  Result := True;
  if FileExists(KeyMapFile) and not UninstallSilent then
    Result := WaitForReaperClosed('It must be closed so the ASampler action can be removed from its Actions list.');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveReaperAction;
end;
