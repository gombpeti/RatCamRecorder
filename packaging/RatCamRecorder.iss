; RatCam Recorder -- Inno Setup script
;
; Produces a single RatCamRecorder-<version>-Setup.exe that installs the
; application, Qt, FFmpeg and pylon's user-mode DLLs into one folder, adds a
; desktop icon and a Start Menu entry, and registers an uninstaller.
;
; What it deliberately does NOT do: install Basler's pylon. The cameras bind to
; plnu3v (oem66.inf), a signed kernel-mode driver that only Basler's installer
; can place, and whose redistribution their licence governs. The setup checks
; for that driver and says plainly what is missing rather than installing an
; application that would then find no cameras.
;
; Build:  "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" RatCamRecorder.iss

#define AppName      "RatCam Recorder"
#define AppVersion   "1.0.0"
#define AppPublisher "Peter Gombkoto"
#define AppExe       "RatCamRecorder.exe"
#define BuildDir     "..\build\bin\Release"

[Setup]
AppId={{8F3A6C21-4D5E-4B7A-9C1F-RATCAM000001}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppCopyright={#AppPublisher}
VersionInfoCompany={#AppPublisher}
VersionInfoProductName={#AppName}
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
OutputDir=.\dist
OutputBaseFilename=RatCamRecorder-{#AppVersion}-Setup
SetupIconFile=..\gui\resources\ratcam.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; 64-bit only: pylon's C++ libraries and the camera buffers rule out 32-bit,
; where 6 cameras x 300 buffers would not even fit in the address space.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
LicenseFile=
PrivilegesRequired=admin

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Shortcuts:"

[Files]
; The whole build output, including the Qt plugin folders (platforms\, styles\,
; imageformats\ ...) which Qt needs at run time or the app will not start.
Source: "{#BuildDir}\{#AppExe}";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\record.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\usbcheck.exe";DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\*.dll";       DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\platforms\*";           DestDir: "{app}\platforms";           Flags: ignoreversion recursesubdirs
Source: "{#BuildDir}\styles\*";              DestDir: "{app}\styles";              Flags: ignoreversion recursesubdirs
Source: "{#BuildDir}\imageformats\*";        DestDir: "{app}\imageformats";        Flags: ignoreversion recursesubdirs
Source: "{#BuildDir}\iconengines\*";         DestDir: "{app}\iconengines";         Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "{#BuildDir}\networkinformation\*";  DestDir: "{app}\networkinformation";  Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "{#BuildDir}\tls\*";                 DestDir: "{app}\tls";                 Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "{#BuildDir}\generic\*";             DestDir: "{app}\generic";             Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "..\gui\resources\ratcam.ico";       DestDir: "{app}"; Flags: ignoreversion
Source: "..\DESIGN.md";                      DestDir: "{app}\docs"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#AppName}";        Filename: "{app}\{#AppExe}"; IconFilename: "{app}\ratcam.ico"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\{#AppExe}"; IconFilename: "{app}\ratcam.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent

[Code]
{ Basler's pylon is a prerequisite, not something this installer can provide.
  Checked before any file is copied, so the operator is told up front rather
  than discovering it when the camera list comes back empty. }
function PylonDriverPresent(): Boolean;
var
  Names: TArrayOfString;
begin
  Result := RegGetSubkeyNames(HKEY_LOCAL_MACHINE,
              'SYSTEM\CurrentControlSet\Services\plnu3v', Names)
            or RegKeyExists(HKEY_LOCAL_MACHINE,
              'SYSTEM\CurrentControlSet\Services\plnu3v');
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not PylonDriverPresent() then
  begin
    if MsgBox('Basler pylon does not appear to be installed on this computer.'#13#10#13#10 +
              '{#AppName} will install and start, but no cameras will be found '+
              'until pylon is installed. This setup cannot install it: the '+
              'cameras need Basler''s kernel driver (plnu3v), which only '+
              'Basler''s own installer can provide.'#13#10#13#10 +
              'Continue anyway?',
              mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
  end;
end;
