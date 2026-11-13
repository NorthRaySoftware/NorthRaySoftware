; Installer for NorthRay Launcher on its own. The apps (Writing Studio, CJ Radio,
; CJ Philip Community) have their own installers; the launcher can also download them.
; Built by tools/make-installers.sh, which passes APP_VERSION, SRC_DIR, ICON, ART, OUT_FILE.

Target amd64-unicode   ; the apps are 64-bit, so the installers are too
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"

!define APP_NAME "NorthRay Launcher"
!define APP_EXE "NorthRayLauncher.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\NorthRay.launcher"
!define LEGACY_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\CJCommunity.launcher"

Name "${APP_NAME}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\NorthRay Launcher"
BrandingText "${APP_NAME} ${APP_VERSION}"
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "FileDescription" "${APP_NAME} setup"
VIAddVersionKey "LegalCopyright" "${DEVELOPER} project"
VIAddVersionKey "CompanyName" "${DEVELOPER}"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${ART}"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${ART}"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "Install NorthRay Launcher"
!define MUI_WELCOMEPAGE_TEXT "This installs NorthRay Launcher ${APP_VERSION} for your Windows account. No administrator permission is needed.$\r$\n$\r$\nIt finds, installs and updates software from ${DEVELOPER}. Each app also installs and runs on its own.$\r$\n$\r$\nThis is a preview release."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "NorthRay Launcher is installed"
!define MUI_FINISHPAGE_TEXT "You'll find it in the Start menu."
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Open NorthRay Launcher now"
!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Add a shortcut to the desktop"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION DesktopShortcut
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function DesktopShortcut
  CreateShortcut "$DESKTOP\NorthRay Launcher.lnk" "$INSTDIR\${APP_EXE}"
FunctionEnd

Function .onInit
  FindWindow $0 "NorthRayLauncher"
  ${If} $0 <> 0
    ; Updating from inside the launcher: give it a moment to close itself.
    Sleep 2000
    FindWindow $0 "NorthRayLauncher"
  ${EndIf}
  ${If} $0 <> 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "NorthRay Launcher is open. Close it, then try again."
    Abort
  ${EndIf}
FunctionEnd

Section "Install"
  ; The old CJ Community launcher is replaced (CJ Philip Community is now a separate app).
  ReadRegStr $1 HKCU "${LEGACY_KEY}" "InstallLocation"
  StrCmp $1 "" noLegacy
  IfFileExists "$1\Uninstall.exe" 0 +2
    ExecWait '"$1\Uninstall.exe" /S _?=$1'
  RMDir /r "$1"
  DeleteRegKey HKCU "${LEGACY_KEY}"
  noLegacy:
  Delete "$SMPROGRAMS\CJ Community\CJ Community.lnk"
  RMDir "$SMPROGRAMS\CJ Community"
  Delete "$DESKTOP\CJ Community.lnk"

  RMDir /r "$INSTDIR"
  SetOutPath "$INSTDIR"
  File /r "${SRC_DIR}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortcut "$SMPROGRAMS\NorthRay Launcher.lnk" "$INSTDIR\${APP_EXE}"

  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${DEVELOPER}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  ; The apps are separate and stay installed.
  Delete "$SMPROGRAMS\NorthRay Launcher.lnk"
  Delete "$DESKTOP\NorthRay Launcher.lnk"
  DeleteRegKey HKCU "${UNINST_KEY}"
  RMDir /r "$INSTDIR"
SectionEnd
