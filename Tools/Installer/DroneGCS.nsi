Unicode True

!ifndef StageWindows
  !error "StageWindows must point to the validated UE staged Windows directory."
!endif
!ifndef AppVersion
  !define AppVersion "1.0.0"
!endif
!ifndef VersionInfoVersion
  !define VersionInfoVersion "1.0.0.0"
!endif
!ifndef OutputDir
  !define OutputDir ".\Output"
!endif

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

Name "DroneGCS Ground Control Station"
OutFile "${OutputDir}\DroneGCS-Setup-${AppVersion}-x64.exe"
InstallDir "$PROGRAMFILES64\DroneGCS"
InstallDirRegKey HKLM "Software\DroneGCS" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetCompressorDictSize 64
CRCCheck on
XPStyle on
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VersionInfoVersion}"
VIAddVersionKey /LANG=1033 "ProductName" "DroneGCS Ground Control Station"
VIAddVersionKey /LANG=1033 "ProductVersion" "${AppVersion}"
VIAddVersionKey /LANG=1033 "FileDescription" "DroneGCS UE frontend and Console backend installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${VersionInfoVersion}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (c) DroneGCS"

!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Section "DroneGCS" SEC_MAIN
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "DroneGCS requires 64-bit Windows."
    Abort
  ${EndIf}

  SetShellVarContext all
  SetRegView 64
  SetOutPath "$INSTDIR\Windows"
  File /r /x "*.pdb" /x "*.ilk" /x "Manifest_*_Win64.txt" /x "Saved" /x "Logs" "${StageWindows}\*.*"

  SetOutPath "$INSTDIR\Windows\DroneShowPlatform1"
  File "${__FILEDIR__}\installed-layout.marker"

  WriteRegStr HKLM "Software\DroneGCS" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroneGCS" "DisplayName" "DroneGCS Ground Control Station"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroneGCS" "DisplayVersion" "${AppVersion}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroneGCS" "Publisher" "DroneGCS"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroneGCS" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroneGCS" "DisplayIcon" "$INSTDIR\Windows\DroneShowPlatform1\DroneGCSLauncher.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroneGCS" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroneGCS" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroneGCS" "NoRepair" 1
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortcut "$DESKTOP\DroneGCS Ground Control Station.lnk" "$INSTDIR\Windows\DroneShowPlatform1\DroneGCSLauncher.exe" "" "$INSTDIR\Windows\DroneShowPlatform1\DroneGCSLauncher.exe" 0 SW_SHOWNORMAL "" "Start DroneGCS frontend and visible backend"
  CreateShortcut "$SMPROGRAMS\DroneGCS Ground Control Station.lnk" "$INSTDIR\Windows\DroneShowPlatform1\DroneGCSLauncher.exe" "" "$INSTDIR\Windows\DroneShowPlatform1\DroneGCSLauncher.exe" 0 SW_SHOWNORMAL "" "Start DroneGCS frontend and visible backend"

  IfFileExists "$INSTDIR\Windows\Engine\Extras\Redist\en-us\UEPrereqSetup_x64.exe" 0 prereq_done
    DetailPrint "Installing Unreal Engine prerequisites..."
    ExecWait '"$INSTDIR\Windows\Engine\Extras\Redist\en-us\UEPrereqSetup_x64.exe" /quiet /norestart' $0
    ${If} $0 != 0
      MessageBox MB_OK|MB_ICONSTOP "Unreal Engine prerequisites failed with exit code $0."
      Abort
    ${EndIf}
  prereq_done:

  DetailPrint "Configuring the DroneGCS MAVLink firewall rule..."
  nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="DroneGCS MAVLink UDP 10001"'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="DroneGCS MAVLink UDP 10001" dir=in action=allow program="$INSTDIR\Windows\DroneShowPlatform1\Binaries\Win64\Backend\DroneGCS.exe" protocol=UDP localip=192.168.1.254 localport=10001 profile=any enable=yes'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONSTOP "Windows Firewall rule creation failed with exit code $0. Installation cannot continue safely."
    Abort
  ${EndIf}
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64
  nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="DroneGCS MAVLink UDP 10001"'
  Pop $0
  Delete "$DESKTOP\DroneGCS Ground Control Station.lnk"
  Delete "$SMPROGRAMS\DroneGCS Ground Control Station.lnk"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroneGCS"
  DeleteRegKey HKLM "Software\DroneGCS"
  RMDir /r "$INSTDIR"
  ; Per-user Config, Logs, UE Saved data, and ArtifactWorkspace are deliberately preserved.
SectionEnd
