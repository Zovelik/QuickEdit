; Extra commands of the uninstaller (see Installer.cmake): the .qep file
; type goes, the extension only when it still points to QuickEdit.
ReadRegStr $0 HKLM "Software\Classes\.qep" ""
StrCmp $0 "QuickEdit.Project" 0 +2
DeleteRegKey HKLM "Software\Classes\.qep"
DeleteRegKey HKLM "Software\Classes\QuickEdit.Project"
System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

; The settings and caches of the user who uninstalls, on request (kept by
; default, and when the uninstaller runs silently, as before an upgrade).
; The user's own files in Documents\QuickEdit always stay.
SetShellVarContext current
MessageBox MB_YESNO|MB_ICONQUESTION "Also delete your QuickEdit settings (window layout, recording scenes, plug-in list, caches)?$\r$\n$\r$\nYour recordings, shaders and speech models in Documents\QuickEdit are kept." /SD IDNO IDNO +3
RMDir /r "$APPDATA\QuickEdit"
RMDir /r "$LOCALAPPDATA\QuickEdit"
SetShellVarContext all
