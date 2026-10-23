; Windows installer for GazCast Clean Record (Inno Setup 6).
; Built by .github/workflows/build.yml:
;   ISCC /DVersion=<x.y.z> /DSourceDir=<release/<config>/gazcast-clean-record> /DOutputDir=<dir> gazcast-clean-record.iss

#ifndef Version
  #define Version "0.0.0"
#endif

[Setup]
AppId={{6F1C2E7A-3B4D-4C59-9E21-7A0D5C8B4F13}
AppName=GazCast Clean Record for OBS Studio
AppVersion={#Version}
AppPublisher=GazCast
AppPublisherURL=https://gazgoc.com.ng/gazcast/
AppSupportURL=https://github.com/GAZMEDIA/gazcast-clean-record/issues
; OBS loads plugins for all users from here.
DefaultDirName={commonappdata}\obs-studio\plugins\gazcast-clean-record
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=gazcast-clean-record-{#Version}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=GazCast Clean Record for OBS Studio
; If OBS is open it holds the plugin file; offer to close it.
CloseApplications=yes

[Messages]
WelcomeLabel2=This installs the GazCast Clean Record plugin for OBS Studio 30 or newer.%n%nPlease close OBS before you continue.
FinishedLabel=GazCast Clean Record is installed.%n%nOpen OBS, click Docks, then GazCast Clean Record.

[InstallDelete]
; Remove files from an older version first.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"

[Files]
Source: "{#SourceDir}\bin\64bit\gazcast-clean-record.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#SourceDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs
