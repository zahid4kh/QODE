; QODE installer for Windows (Inno Setup 6.6 or newer). Built by scripts\win\build-installer.ps1, which passes:
;   /DAppVersion=2.0.0  /DFlavor=web|noweb  /DSourceDir=<staged program folder>  /DOutputDir=<dist-win>
;   /DWhatsNew=<text file shown after installing>
;
; - Per user, no admin rights: %LOCALAPPDATA%\Programs\QODE.
; - Both flavours (with and without Qt WebEngine) share one AppId, so installing one replaces the other, like the .deb
;   packages' Conflicts / Replaces. The installed version is uninstalled first (silently, keeping the user's data), so
;   no file of the other flavour is left behind.
; - Transparency: the Ready page lists everything this installs and what QODE itself creates later. The uninstaller
;   removes all of it and then shows QODE's data folders with their sizes (qode.exe --qode-files, see
;   src/app/QodeFiles.h), deleting the ticked ones: all of them by default, except JDKs QODE downloaded. A silent
;   uninstall removes the ticked-by-default ones; /KEEPDATA keeps all data (the .deb's QODE_KEEP_USER_DATA=1).
; - QODE holds the mutex "QODE-AGENTIC-running" (Platform::initGui): setup and uninstall ask to close it first.

#ifndef AppVersion
  #error Pass /DAppVersion=x.y.z (scripts\win\build-installer.ps1 does)
#endif
#ifndef SourceDir
  #error Pass /DSourceDir=<staged program folder>
#endif
#ifndef Flavor
  #define Flavor "web"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#if Flavor == "noweb"
  #define FileSuffix "-noweb"
  #define FlavorText "without the web preview"
#else
  #define FileSuffix ""
  #define FlavorText "with the web preview (Qt WebEngine)"
#endif
; Platform::kInstallerAppId in src/platform/Platform.h: Help > QODE's Files reads the uninstall key by it.
#define AppGuid "{EF440086-6264-4480-A849-B306BDBCF69F}"

[Setup]
AppId={{EF440086-6264-4480-A849-B306BDBCF69F}
AppName=QODE
AppVersion={#AppVersion}
AppVerName=QODE {#AppVersion} ({#FlavorText})
AppPublisher=Zahid Khalilov
AppPublisherURL=https://qodeditor.vercel.app
AppSupportURL=https://github.com/zahid4kh/QODE
AppUpdatesURL=https://qodeditor.vercel.app
AppCopyright=Copyright (c) 2026 Zahid Khalilov
VersionInfoVersion={#AppVersion}
VersionInfoProductName=QODE
VersionInfoDescription=QODE {#AppVersion} installer
DefaultDirName={autopf}\QODE
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=QODE-{#AppVersion}{#FileSuffix}-setup
SetupIconFile=..\..\resources\icons\qode.ico
UninstallDisplayIcon={app}\qode.exe
UninstallDisplayName=QODE
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
AppMutex=QODE-AGENTIC-running
CloseApplications=yes
RestartApplications=no
ChangesEnvironment=yes
UsePreviousTasks=yes
#ifdef WhatsNew
InfoAfterFile={#WhatsNew}
#endif

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "addtopath"; Description: "Add QODE to PATH, so ""qode ."" opens a folder from a terminal (adds QODE's bin folder to your user's PATH)"; GroupDescription: "Command line:"
Name: "explorermenu"; Description: "Add ""Open with QODE"" to Explorer's right-click menu for files and folders"; GroupDescription: "Explorer:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\copyright"; DestDir: "{app}"; DestName: "copyright.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\QODE"; Filename: "{app}\qode.exe"; Comment: "QODE code editor"
Name: "{autodesktop}\QODE"; Filename: "{app}\qode.exe"; Comment: "QODE code editor"; Tasks: desktopicon

[Registry]
; Explorer's right-click menu (per user). %V: the folder clicked or the one shown; %1: the file. The "shell" parents
; are created when missing, so uninstall removes them again when nothing else is in them.
Root: HKCU; Subkey: "Software\Classes\Directory\shell"; Flags: uninsdeletekeyifempty; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell"; Flags: uninsdeletekeyifempty; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\*\shell"; Flags: uninsdeletekeyifempty; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\Directory\shell\QODE"; ValueType: string; ValueName: ""; ValueData: "Open with QODE"; Flags: uninsdeletekey; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\Directory\shell\QODE"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\qode.exe"""; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\Directory\shell\QODE\command"; ValueType: string; ValueName: ""; ValueData: """{app}\qode.exe"" ""%V"""; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\QODE"; ValueType: string; ValueName: ""; ValueData: "Open with QODE"; Flags: uninsdeletekey; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\QODE"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\qode.exe"""; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\QODE\command"; ValueType: string; ValueName: ""; ValueData: """{app}\qode.exe"" ""%V"""; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\*\shell\QODE"; ValueType: string; ValueName: ""; ValueData: "Open with QODE"; Flags: uninsdeletekey; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\*\shell\QODE"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\qode.exe"""; Tasks: explorermenu
Root: HKCU; Subkey: "Software\Classes\*\shell\QODE\command"; ValueType: string; ValueName: ""; ValueData: """{app}\qode.exe"" ""%1"""; Tasks: explorermenu

[Run]
Filename: "{app}\qode.exe"; Description: "Start QODE"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Written by [Code] (CurStepChanged), so not in the install log.
Type: files; Name: "{app}\bin\qode.cmd"
Type: dirifempty; Name: "{app}\bin"
Type: dirifempty; Name: "{app}"

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#AppGuid}_is1';
  EnvKey = 'Environment';
  Tab = #9;

var
  KeepData: Boolean;

function HasParam(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
      Result := True;
end;

function BinDir: String;
begin
  Result := ExpandConstant('{app}\bin');
end;

function SamePathEntry(const A, B: String): Boolean;
begin
  Result := CompareText(RemoveBackslashUnlessRoot(Trim(A)), RemoveBackslashUnlessRoot(Trim(B))) = 0;
end;

// --- PATH (HKCU\Environment\Path, REG_EXPAND_SZ; other entries are kept exactly as they are) ---------------------

function PathHas(const Paths, Dir: String): Boolean;
var
  Rest, Entry: String;
  P: Integer;
begin
  Result := False;
  Rest := Paths;
  while (Rest <> '') and not Result do begin
    P := Pos(';', Rest);
    if P = 0 then begin
      Entry := Rest;
      Rest := '';
    end else begin
      Entry := Copy(Rest, 1, P - 1);
      Delete(Rest, 1, P);
    end;
    Result := SamePathEntry(Entry, Dir);
  end;
end;

procedure AddToPath(const Dir: String);
var
  Paths: String;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Paths) then
    Paths := '';
  if PathHas(Paths, Dir) then
    Exit;
  if (Paths <> '') and (Copy(Paths, Length(Paths), 1) <> ';') then
    Paths := Paths + ';';
  RegWriteExpandStringValue(HKCU, EnvKey, 'Path', Paths + Dir);
end;

procedure RemoveFromPath(const Dir: String);
var
  Paths, Rest, Entry, Kept: String;
  P: Integer;
  Found, First: Boolean;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Paths) then
    Exit;
  Rest := Paths;
  Kept := '';
  Found := False;
  First := True;
  // Split on ';' keeping empty entries, so the rest of the value stays byte for byte as it was.
  repeat
    P := Pos(';', Rest);
    if P = 0 then begin
      Entry := Rest;
      Rest := '';
    end else begin
      Entry := Copy(Rest, 1, P - 1);
      Delete(Rest, 1, P);
    end;
    if (Entry <> '') and SamePathEntry(Entry, Dir) then
      Found := True
    else begin
      if not First then
        Kept := Kept + ';';
      Kept := Kept + Entry;
      First := False;
    end;
  until P = 0;
  if not Found then
    Exit;
  if Kept = '' then
    RegDeleteValue(HKCU, EnvKey, 'Path')
  else
    RegWriteExpandStringValue(HKCU, EnvKey, 'Path', Kept);
end;

// --- Install --------------------------------------------------------------------------------------------------

function Line(const S: String): String;
begin
  Result := S + #13#10;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  S: String;
begin
  S := Line('QODE {#AppVersion}, {#FlavorText}. Everything this installs, all for your user only (no admin rights):');
  S := S + Line('');
  S := S + Line('Program files:');
  S := S + Line(Space + ExpandConstant('{app}'));
  S := S + Line('Shortcuts:');
  S := S + Line(Space + ExpandConstant('{autoprograms}\QODE.lnk'));
  if WizardIsTaskSelected('desktopicon') then
    S := S + Line(Space + ExpandConstant('{autodesktop}\QODE.lnk'));
  if WizardIsTaskSelected('addtopath') then begin
    S := S + Line('Your PATH (HKCU\Environment\Path) gets one entry:');
    S := S + Line(Space + ExpandConstant('{app}\bin') + '   (holds qode.cmd)');
  end;
  S := S + Line('Registry (HKEY_CURRENT_USER only):');
  S := S + Line(Space + 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#AppGuid}_is1   (Settings > Apps)');
  if WizardIsTaskSelected('explorermenu') then begin
    S := S + Line(Space + 'Software\Classes\Directory\shell\QODE   ("Open with QODE" on folders)');
    S := S + Line(Space + 'Software\Classes\Directory\Background\shell\QODE   (inside a folder)');
    S := S + Line(Space + 'Software\Classes\*\shell\QODE   (on files)');
  end;
  S := S + Line('');
  S := S + Line('While you use it, QODE keeps its data only in:');
  S := S + Line(Space + ExpandConstant('{userappdata}\QODE-AGENTIC') + '   (settings, API keys, per-project state)');
  S := S + Line(Space + ExpandConstant('{localappdata}\QODE-AGENTIC') + '   (language servers it downloads, local history, caches)');
  S := S + Line(Space + 'and, only if you ask for them, JDK downloads in ' + ExpandConstant('{%USERPROFILE}\.jdks') +
    ' and the Kotlin server command ' + ExpandConstant('{%USERPROFILE}\.local\bin\kotlin-lsp.cmd'));
  S := S + Line('Help > QODE''s Files lists all of it with sizes. Uninstalling removes everything above and lets you');
  S := S + Line('choose which data folders to keep.');
  Result := S;
end;

// The installed QODE (this flavour or the other one) is removed first, keeping the user's data, so no file of the
// old version stays behind.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Uninstaller: String;
  Code, I: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, UninstallKey, 'UninstallString', Uninstaller) then
    Exit;
  Uninstaller := RemoveQuotes(Uninstaller);
  if not FileExists(Uninstaller) then
    Exit;
  if not Exec(Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /KEEPDATA', '', SW_HIDE, ewWaitUntilTerminated, Code) then begin
    Result := 'The installed QODE could not be removed first: ' + SysErrorMessage(Code);
    Exit;
  end;
  // The uninstaller finishes in a copy of itself: wait (at most 60 s) until it has removed its key and, last of all,
  // its own unins000.exe (else this install would name its uninstaller unins001).
  for I := 1 to 240 do begin
    if not RegKeyExists(HKCU, UninstallKey) and not FileExists(Uninstaller) then
      Break;
    Sleep(250);
  end;
  if RegKeyExists(HKCU, UninstallKey) then
    Result := 'The installed QODE could not be removed first. Uninstall it in Settings > Apps, then run this installer again.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    // "qode ." in a terminal: start returns at once (cmd would otherwise wait for the window to close).
    ForceDirectories(BinDir);
    SaveStringToFile(BinDir + '\qode.cmd', '@echo off' + #13#10 + 'start "" "%~dp0..\qode.exe" %*' + #13#10, False);
    if WizardIsTaskSelected('addtopath') then
      AddToPath(BinDir);
  end;
end;

// --- Uninstall ------------------------------------------------------------------------------------------------

function InitializeUninstall: Boolean;
begin
  KeepData := HasParam('/KEEPDATA');
  Result := True;
end;

function Field(const S: String; Index: Integer): String;
var
  Rest: String;
  P, I: Integer;
begin
  Rest := S;
  for I := 0 to Index - 1 do begin
    P := Pos(Tab, Rest);
    if P = 0 then begin
      Rest := '';
      Break;
    end;
    Delete(Rest, 1, P);
  end;
  P := Pos(Tab, Rest);
  if P > 0 then
    Result := Copy(Rest, 1, P - 1)
  else
    Result := Rest;
end;

function SizeText(const Bytes: String): String;
var
  B, Tenths: Int64;
begin
  B := StrToInt64Def(Bytes, -1);
  if B < 0 then
    Result := ''
  else if B < 1024 then
    Result := IntToStr(B) + ' bytes'
  else if B < 1048576 then
    Result := IntToStr(B div 1024) + ' KB'
  else if B < 1073741824 then
    Result := IntToStr(B div 1048576) + ' MB'
  else begin
    Tenths := (B * 10) div 1073741824;
    Result := IntToStr(Tenths div 10) + '.' + IntToStr(Tenths mod 10) + ' GB';
  end;
end;

procedure RemoveUserData;
var
  Report, Qode, Args, Removed, Failed, Kept, Msg: String;
  Lines, Results: TArrayOfString;
  Code, I, N: Integer;
  Form: TSetupForm;
  Intro: TNewStaticText;
  List: TNewCheckListBox;
  RemoveBtn, KeepBtn: TNewButton;
  Selected: array of Boolean;
begin
  Qode := ExpandConstant('{app}\qode.exe');
  Report := ExpandConstant('{tmp}\qode-files.txt');
  if not FileExists(Qode) or not Exec(Qode, '--qode-files list "' + Report + '"', '', SW_HIDE, ewWaitUntilTerminated, Code)
     or not LoadStringsFromFile(Report, Lines) then begin
    if not UninstallSilent then
      MsgBox('QODE could not list its data folders, so they were kept. You can delete them yourself:' + #13#10#13#10 +
        ExpandConstant('{userappdata}\QODE-AGENTIC') + #13#10 + ExpandConstant('{localappdata}\QODE-AGENTIC'), mbInformation, MB_OK);
    Exit;
  end;
  N := 0;
  for I := 0 to GetArrayLength(Lines) - 1 do
    if Trim(Lines[I]) <> '' then begin
      Lines[N] := Lines[I];
      N := N + 1;
    end;
  if N = 0 then
    Exit;
  SetArrayLength(Selected, N);
  for I := 0 to N - 1 do
    Selected[I] := Field(Lines[I], 1) = '1';

  if not UninstallSilent then begin
    Form := CreateCustomForm(ScaleX(640), ScaleY(400), False, False);
    try
      Form.Caption := 'Uninstall QODE: your data';
      Intro := TNewStaticText.Create(Form);
      Intro.Parent := Form;
      Intro.Left := ScaleX(16);
      Intro.Top := ScaleY(14);
      Intro.Width := Form.ClientWidth - ScaleX(32);
      Intro.AutoSize := False;
      Intro.WordWrap := True;
      Intro.Height := ScaleY(64);
      Intro.Caption := 'QODE created these while you used it. The ticked ones are deleted now; untick anything you want to keep ' +
        '(you can delete it yourself later). JDKs QODE downloaded are not ticked: Gradle or Maven projects may still use them. ' +
        'Help > QODE''s Files listed the same.';
      List := TNewCheckListBox.Create(Form);
      List.Parent := Form;
      List.Left := ScaleX(16);
      List.Top := Intro.Top + Intro.Height + ScaleY(8);
      List.Width := Form.ClientWidth - ScaleX(32);
      List.Height := Form.ClientHeight - List.Top - ScaleY(56);
      for I := 0 to N - 1 do
        List.AddCheckBox(Field(Lines[I], 3) + #13#10 + Field(Lines[I], 4), SizeText(Field(Lines[I], 2)), 0, Selected[I], True, False, False, nil);
      RemoveBtn := TNewButton.Create(Form);
      RemoveBtn.Parent := Form;
      RemoveBtn.Caption := 'Delete ticked';
      RemoveBtn.Width := ScaleX(120);
      RemoveBtn.Height := ScaleY(28);
      RemoveBtn.Top := Form.ClientHeight - ScaleY(42);
      RemoveBtn.Left := Form.ClientWidth - ScaleX(16) - ScaleX(120) - ScaleX(8) - ScaleX(120);
      RemoveBtn.ModalResult := mrOk;
      RemoveBtn.Default := True;
      KeepBtn := TNewButton.Create(Form);
      KeepBtn.Parent := Form;
      KeepBtn.Caption := 'Keep all';
      KeepBtn.Width := ScaleX(120);
      KeepBtn.Height := ScaleY(28);
      KeepBtn.Top := RemoveBtn.Top;
      KeepBtn.Left := Form.ClientWidth - ScaleX(16) - ScaleX(120);
      KeepBtn.ModalResult := mrCancel;
      KeepBtn.Cancel := True;
      if Form.ShowModal = mrOk then begin
        for I := 0 to N - 1 do
          Selected[I] := List.Checked[I];
      end else begin
        for I := 0 to N - 1 do
          Selected[I] := False;
      end;
    finally
      Form.Free;
    end;
  end;

  Args := '';
  Kept := '';
  for I := 0 to N - 1 do
    if Selected[I] then
      Args := Args + ' ' + Field(Lines[I], 0)
    else
      Kept := Kept + '    ' + Field(Lines[I], 3) + #13#10;
  Removed := '';
  Failed := '';
  if Args <> '' then begin
    Report := ExpandConstant('{tmp}\qode-removed.txt');
    if Exec(Qode, '--qode-files remove "' + Report + '"' + Args, '', SW_HIDE, ewWaitUntilTerminated, Code)
       and LoadStringsFromFile(Report, Results) then begin
      for I := 0 to GetArrayLength(Results) - 1 do
        if Field(Results[I], 0) = 'removed' then
          Removed := Removed + '    ' + Field(Results[I], 2) + '   (' + SizeText(Field(Results[I], 3)) + ')' + #13#10
        else if Field(Results[I], 0) = 'failed' then
          Failed := Failed + '    ' + Field(Results[I], 2) + ': ' + Field(Results[I], 3) + #13#10;
    end else
      Failed := '    QODE could not delete its data (code ' + IntToStr(Code) + ').' + #13#10;
  end;
  if UninstallSilent then
    Exit;
  Msg := '';
  if Removed <> '' then
    Msg := Msg + 'Deleted:' + #13#10 + Removed + #13#10;
  if Failed <> '' then
    Msg := Msg + 'Could not delete (delete them yourself):' + #13#10 + Failed + #13#10;
  if Kept <> '' then
    Msg := Msg + 'Kept:' + #13#10 + Kept;
  if Msg <> '' then
    MsgBox(Trim(Msg), mbInformation, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // usUninstall: before the program files go, so qode.exe can still list and delete QODE's data.
  if CurUninstallStep = usUninstall then begin
    RemoveFromPath(BinDir);
    if not KeepData then
      RemoveUserData;
  end;
end;
