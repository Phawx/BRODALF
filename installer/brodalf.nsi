; BRODALF installer: a per-user install that needs no administrator rights.
;
; Build from the repository root after the Windows build, for example:
;   makensis -DVERSION=0.4.0 installer\brodalf.nsi
; VERSION is required, as x.y.z (CI can read it from project() in
; CMakeLists.txt). BINDIR is the folder holding brodalf.exe and
; brodalf-cli.exe, absolute or relative to this script's folder; it defaults
; to ..\build\Release, where the MSVC build puts them. OUTFILE names the
; installer (default: next to this script).
;
; It installs brodalf.exe, brodalf-cli.exe and THIRD-PARTY-NOTICES.txt into
; %LOCALAPPDATA%\Programs\BRODALF, adds a Start menu shortcut, an uninstaller
; and an entry in Installed apps (HKCU). Catalogs, the app log and the
; settings under HKCU\Software\BRODALF belong to the user and are kept on
; uninstall, as are the copies on the backup drives.

Unicode true

!ifndef VERSION
  !error "Pass the version: makensis -DVERSION=x.y.z installer\brodalf.nsi"
!endif
!ifndef BINDIR
  !define BINDIR "..\build\Release"
!endif
!ifndef OUTFILE
  !define OUTFILE "brodalf-v${VERSION}-windows-x64-setup.exe"
!endif

!define APP "BRODALF"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\BRODALF"

Name "${APP}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\BRODALF"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ManifestDPIAware true
BrandingText "${APP} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "See THIRD-PARTY-NOTICES.txt for the libraries BRODALF uses"

!include "MUI2.nsh"

!define MUI_FINISHPAGE_RUN "$INSTDIR\brodalf.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open BRODALF now"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetShellVarContext current
FunctionEnd

Function un.onInit
  SetShellVarContext current
FunctionEnd

Section "BRODALF" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${BINDIR}\brodalf.exe"
  File "${BINDIR}\brodalf-cli.exe"
  File "..\THIRD-PARTY-NOTICES.txt"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateShortCut "$SMPROGRAMS\BRODALF.lnk" "$INSTDIR\brodalf.exe" "" "$INSTDIR\brodalf.exe" 0

  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\brodalf.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ; Installed size in KB, for Installed apps.
  SectionGetSize ${SecMain} $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" $0
SectionEnd

Section "Uninstall"
  ; Only what the installer put there: never the user's catalogs or backups.
  Delete "$INSTDIR\brodalf.exe"
  Delete "$INSTDIR\brodalf-cli.exe"
  Delete "$INSTDIR\THIRD-PARTY-NOTICES.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\BRODALF.lnk"
  DeleteRegKey HKCU "${UNINST_KEY}"
SectionEnd
