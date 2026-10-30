; Extra commands of the uninstaller (see Installer.cmake): the .qep file
; type goes, the extension only when it still points to QuickEdit.
ReadRegStr $0 HKLM "Software\Classes\.qep" ""
StrCmp $0 "QuickEdit.Project" 0 +2
DeleteRegKey HKLM "Software\Classes\.qep"
DeleteRegKey HKLM "Software\Classes\QuickEdit.Project"
System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
