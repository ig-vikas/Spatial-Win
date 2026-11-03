Unicode True

!include "MUI2.nsh"
!include "FileFunc.nsh"

Name "Spatial"
OutFile "${SETUP_PATH}"
InstallDir "$LOCALAPPDATA\Programs\Spatial"
InstallDirRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Spatial" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "Spatial ${SPATIAL_VERSION}"

!define MUI_ICON "spatial.ico"
!define MUI_UNICON "spatial.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\Spatial.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open Spatial"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var Portable

Function .onInit
    SetShellVarContext current
    ${GetParameters} $0
    ${GetOptions} $0 "/PORTABLE" $Portable
    ${If} ${Errors}
        StrCpy $Portable "0"
    ${Else}
        StrCpy $Portable "1"
    ${EndIf}
    ${If} $Portable == "0"
        checkRunning:
        FindWindow $0 "SpatialDesktop"
        ${If} $0 != 0
            MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Quit all running copies of Spatial from the notification area, then click Retry." /SD IDCANCEL IDRETRY checkRunning
            Abort
        ${EndIf}
    ${EndIf}
FunctionEnd

Section "Spatial" SpatialFiles
    SetOutPath "$INSTDIR"
    File "${PAYLOAD_DIR}\Spatial.exe"
    File "${PAYLOAD_DIR}\phonon.dll"
    File "${PAYLOAD_DIR}\README.md"
    File "${PAYLOAD_DIR}\STEAM-AUDIO-LICENSE.txt"
    File "${PAYLOAD_DIR}\STEAM-AUDIO-THIRDPARTY.txt"

    ${If} $Portable == "0"
        WriteUninstaller "$INSTDIR\Uninstall.exe"
        CreateShortcut "$SMPROGRAMS\Spatial.lnk" "$INSTDIR\Spatial.exe"
        !define UninstallKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\Spatial"
        WriteRegStr HKCU "${UninstallKey}" "DisplayName" "Spatial"
        WriteRegStr HKCU "${UninstallKey}" "DisplayVersion" "${SPATIAL_VERSION}"
        WriteRegStr HKCU "${UninstallKey}" "InstallLocation" "$INSTDIR"
        WriteRegStr HKCU "${UninstallKey}" "DisplayIcon" "$INSTDIR\Spatial.exe"
        WriteRegStr HKCU "${UninstallKey}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
        WriteRegDWORD HKCU "${UninstallKey}" "NoModify" 1
        WriteRegDWORD HKCU "${UninstallKey}" "NoRepair" 1
    ${EndIf}
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    Delete "$SMPROGRAMS\Spatial.lnk"
    DeleteRegKey HKCU "${UninstallKey}"
    Delete "$INSTDIR\Spatial.exe"
    Delete "$INSTDIR\phonon.dll"
    Delete "$INSTDIR\README.md"
    Delete "$INSTDIR\STEAM-AUDIO-LICENSE.txt"
    Delete "$INSTDIR\STEAM-AUDIO-THIRDPARTY.txt"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
