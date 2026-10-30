# Windows installer (NSIS, through CPack).
#
# Installs for all the users in "Program Files\QuickEdit", with a Start menu
# shortcut, an optional desktop shortcut, the .qep projects opened by
# QuickEdit, and an uninstaller listed in the Windows settings. The settings
# and caches of each user go to %APPDATA%\QuickEdit, the user's own files
# (recordings, shaders, speech models) to Documents\QuickEdit: the
# uninstaller leaves them.

set(CPACK_NSIS_PACKAGE_NAME "QuickEdit")
set(CPACK_NSIS_DISPLAY_NAME "QuickEdit")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall QuickEdit")

set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/src/resources/icon.ico")
set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/src/resources/icon.ico")
set(CPACK_NSIS_INSTALLED_ICON_NAME "QuickEdit.exe")

# The executable is at the top of the installation, not in bin/.
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")

set(CPACK_NSIS_MUI_FINISHPAGE_RUN "QuickEdit.exe")
set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/Zovelik/QuickEdit")

# The desktop shortcut is offered on the shortcuts page.
set(CPACK_CREATE_DESKTOP_LINKS "QuickEdit")

# .qep projects: opened by QuickEdit, with its icon; removed on uninstall.
# The NSIS commands are in install.nsh and uninstall.nsh: CPack writes the
# values into CPackConfig.cmake, read again by CMake, hence the escapes.
function(quickedit_nsis_commands variable file)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/${file}" commands)
    string(REGEX REPLACE ";[^\n]*\n" "" commands "${commands}")
    string(REPLACE "\\" "\\\\" commands "${commands}")
    string(REPLACE "\"" "\\\"" commands "${commands}")
    string(REPLACE "$" "\\$" commands "${commands}")
    set(${variable} "${commands}" PARENT_SCOPE)
endfunction()

quickedit_nsis_commands(CPACK_NSIS_EXTRA_INSTALL_COMMANDS install.nsh)
quickedit_nsis_commands(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS uninstall.nsh)
