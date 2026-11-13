; Installer for one CJ Community application (CJ Radio, SkillSwap, ...).
; Built by tools/make-installers.sh, which passes:
;   APP_ID, APP_NAME, APP_EXE, APP_VERSION, SRC_DIR (the verified package folder), ICON, ART, OUT_FILE
;
; Installs for the current Windows user only (no administrator prompt). Default
; folder: %LOCALAPPDATA%\NorthRay\Apps\<id>; NorthRay Launcher passes /D= to use
; the folder chosen in its settings. The uninstall entry (NorthRay.<id>) records
; the folder and version, which is how the launcher finds the app.

Target amd64-unicode   ; the apps are 64-bit, so the installers are too
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"

Name "${APP_NAME}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\NorthRay\Apps\${APP_ID}"
BrandingText "${APP_NAME} ${APP_VERSION}"
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "FileDescription" "${APP_NAME} setup"
VIAddVersionKey "LegalCopyright" "${DEVELOPER} project"
VIAddVersionKey "CompanyName" "${DEVELOPER}"

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\NorthRay.${APP_ID}"
!define LEGACY_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\CJCommunity.${APP_ID}"
!define MUTEX "Local\CJCommunity.App.${APP_ID}"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${ART}"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${ART}"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "Install ${APP_NAME}"
!define MUI_WELCOMEPAGE_TEXT "This installs ${APP_NAME} ${APP_VERSION} for your Windows account. No administrator permission is needed.$\r$\n$\r$\n${APP_NAME} runs on its own. NorthRay Launcher finds it and keeps it up to date, if you use it.$\r$\n$\r$\nThis is a preview release from ${DEVELOPER}."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "${APP_NAME} is installed"
!define MUI_FINISHPAGE_TEXT "You'll find it in the Start menu."
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Open ${APP_NAME} now"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; Refuse to replace files while the app is running.
!macro CheckNotRunning
  System::Call 'kernel32::OpenMutexW(i 0x00100000, i 0, w "${MUTEX}") p .r0'
  ${If} $0 P<> 0
    System::Call 'kernel32::CloseHandle(p r0)'
    MessageBox MB_OK|MB_ICONEXCLAMATION "${APP_NAME} is open. Close it, then try again."
    Abort
  ${EndIf}
!macroend

Function .onInit
  !insertmacro CheckNotRunning
FunctionEnd

Function un.onInit
  !insertmacro CheckNotRunning
FunctionEnd

Section "Install"
  ; A clean copy each time, so nothing from an older version is left behind.
  ; Settings live elsewhere (%LOCALAPPDATA%\CJ Community) and are kept.
  ; A copy installed by the older CJ Community setup is removed first (its settings are kept).
  ReadRegStr $1 HKCU "${LEGACY_KEY}" "InstallLocation"
  StrCmp $1 "" legacyFolder
  IfFileExists "$1\Uninstall.exe" 0 +2
    ExecWait '"$1\Uninstall.exe" /S _?=$1'
  RMDir /r "$1"
  DeleteRegKey HKCU "${LEGACY_KEY}"
  legacyFolder:
  IfFileExists "$LOCALAPPDATA\CJ Community\Apps\${APP_ID}\${APP_EXE}" 0 +2
    RMDir /r "$LOCALAPPDATA\CJ Community\Apps\${APP_ID}"
  Delete "$SMPROGRAMS\CJ Community\${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\CJ Community"

  RMDir /r "$INSTDIR"
  SetOutPath "$INSTDIR"
  File /r "${SRC_DIR}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

!ifdef NO_SHORTCUT
  ; Opened from another app (CJ Radio opens from CJ Philip Community): no Start menu entry of its own.
  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
!else
  CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
!endif

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
  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  DeleteRegKey HKCU "${UNINST_KEY}"
  RMDir /r "$INSTDIR"
SectionEnd
