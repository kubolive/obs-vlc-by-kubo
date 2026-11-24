!include "MUI2.nsh"
!include "FileFunc.nsh"

Name "obs-vlc-by-kubo"
OutFile "..\..\obs-vlc-by-kubo-windows-x64-installer.exe"
InstallDir "$PROGRAMFILES64\obs-studio"
InstallDirRegKey HKLM "Software\OBS Studio" ""
RequestExecutionLevel admin

!define MUI_ABORTWARNING

; Welcome page
!insertmacro MUI_PAGE_WELCOME

; Warning License Page
!define MUI_TEXT_LICENSE_TITLE "Important Overwrite Warning"
!define MUI_TEXT_LICENSE_SUBTITLE "Please read this important notice before proceeding with installation."
!define MUI_LICENSEPAGE_TEXT_TOP "WARNING: This plugin will overwrite OBS Studio's original vlc-video files."
!define MUI_LICENSEPAGE_TEXT_BOTTOM "By clicking Next, you confirm that existing plugin files will be overwritten."
!define MUI_LICENSEPAGE_BUTTON "Next"
!insertmacro MUI_PAGE_LICENSE "warning.txt"

; Directory Selection
!insertmacro MUI_PAGE_DIRECTORY

; Installation
!insertmacro MUI_PAGE_INSTFILES

; Finish page
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Section "Install"
  ; Backup existing vlc-video.dll if present
  IfFileExists "$INSTDIR\obs-plugins\64bit\vlc-video.dll" 0 +2
    CopyFiles /SILENT "$INSTDIR\obs-plugins\64bit\vlc-video.dll" "$INSTDIR\obs-plugins\64bit\vlc-video.dll.bak"

  ; Install binary
  SetOutPath "$INSTDIR\obs-plugins\64bit"
  File "..\..\build\vlc-video.dll"

  ; Install locale files
  SetOutPath "$INSTDIR\data\obs-plugins\vlc-video\locale"
  File /r "..\..\data\locale\*.*"
SectionEnd
