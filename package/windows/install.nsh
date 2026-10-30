; Extra commands of the installer (see Installer.cmake): the .qep projects
; are opened by QuickEdit, with its icon.
WriteRegStr HKLM "Software\Classes\.qep" "" "QuickEdit.Project"
WriteRegStr HKLM "Software\Classes\QuickEdit.Project" "" "QuickEdit Project"
WriteRegStr HKLM "Software\Classes\QuickEdit.Project\DefaultIcon" "" "$INSTDIR\QuickEdit.exe,0"
WriteRegStr HKLM "Software\Classes\QuickEdit.Project\shell\open\command" "" '"$INSTDIR\QuickEdit.exe" "%1"'
System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
