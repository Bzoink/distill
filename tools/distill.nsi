Unicode true
RequestExecutionLevel user

!include "MUI2.nsh"
!include "x64.nsh"
!include "payload.nsh"

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Distill"
!define READER "$INSTDIR\bin\distill-fb.exe"

Name "Distill ${DISTILL_VERSION}"
OutFile "Distill-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\Distill"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"

VIProductVersion "${DISTILL_FILE_VERSION}"
VIAddVersionKey "ProductName" "Distill"
VIAddVersionKey "ProductVersion" "${DISTILL_VERSION}"
VIAddVersionKey "FileDescription" "Distill Setup (Windows x64)"
VIAddVersionKey "FileVersion" "${DISTILL_FILE_VERSION}"
VIAddVersionKey "LegalCopyright" ""

SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

!define MUI_ABORTWARNING
!define MUI_ICON "${__FILEDIR__}/../share/branding/distill.ico"
!define MUI_UNICON "${MUI_ICON}"
!define MUI_FINISHPAGE_RUN "${READER}"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
    SetShellVarContext current
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "Distill requires 64-bit Windows." /SD IDOK
        SetErrorLevel 1
        Abort
    ${EndIf}
FunctionEnd

Function un.onInit
    SetShellVarContext current
FunctionEnd

Section "Distill"
    ClearErrors
    SetOutPath "$INSTDIR"
    File /r "Distill/*"
    IfErrors install_failed
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    SetOutPath "$INSTDIR\bin"
    CreateShortCut "$SMPROGRAMS\Distill.lnk" "${READER}" "" "${READER}" 0
    IfErrors install_failed

    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "Distill"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${DISTILL_VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "${READER}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
    IfErrors 0 installed
    install_failed:
        MessageBox MB_OK|MB_ICONSTOP "Setup could not complete. Close Distill and try again." /SD IDOK
        SetErrorLevel 1
        Abort
    installed:
SectionEnd

Section "Uninstall"
    SetOutPath "$TEMP"
    ClearErrors
    !insertmacro DeletePayload
    IfErrors 0 payload_removed
        MessageBox MB_OK|MB_ICONSTOP "Some files could not be removed. Close Distill and try again." /SD IDOK
        SetErrorLevel 1
        Abort
    payload_removed:
    ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "InstallLocation"
    ${If} $0 == $INSTDIR
        Delete "$SMPROGRAMS\Distill.lnk"
        DeleteRegKey HKCU "${UNINSTALL_KEY}"
    ${EndIf}
    Delete "$INSTDIR\Uninstall.exe"
    !insertmacro PrunePayloadDirectories
    RMDir "$INSTDIR"
SectionEnd
