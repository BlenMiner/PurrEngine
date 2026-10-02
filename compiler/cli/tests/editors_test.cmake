# tide editors --update finds editors where their installers put them, and
# updates those that have Tide, never one that only has the grammar
# (tools/tide-syntax). The editors are fakes that write down how they ran.
# Only --update: it never runs an editor whose extensions folder (in a fake
# home) lacks Tide, and the fakes are found before the places tide looks
# that can't be faked (Program Files, which Windows sets for each process,
# /Applications, the system's Flatpaks), so this machine's own editors are
# left alone.
#
# cmake -DTIDE=<the tide program> -DWORK=<a scratch folder> -P editors_test.cmake

cmake_minimum_required(VERSION 3.25)
foreach(var TIDE WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "Pass -D${var}=...")
    endif()
endforeach()

# An installation of tide (tide is in <root>/bin) with an extension.
file(REMOVE_RECURSE "${WORK}")
set(install "${WORK}/install")
set(home "${WORK}/home")
set(path "${WORK}/path")
file(COPY "${TIDE}" DESTINATION "${install}/bin")
get_filename_component(tide_name "${TIDE}" NAME)
set(tide "${install}/bin/${tide_name}")
file(WRITE "${install}/editors/tide.vsix" "not a real extension")
file(MAKE_DIRECTORY "${path}")

# An editor's command that writes its arguments to called-<name>.txt.
function(fake_editor file name)
    get_filename_component(dir "${file}" DIRECTORY)
    file(MAKE_DIRECTORY "${dir}")
    if(WIN32)
        file(TO_NATIVE_PATH "${WORK}/called-${name}.txt" log)
        file(WRITE "${file}" "@echo %*> \"${log}\"\r\n")
    else()
        file(WRITE "${file}" "#!/bin/sh\necho \"$@\" > '${WORK}/called-${name}.txt'\n")
        file(CHMOD "${file}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endif()
endfunction()

# Cursor on PATH; VS Code and VSCodium where their installers put them for
# this user: in %LOCALAPPDATA%\Programs on Windows, in ~/Applications on
# macOS, and as Flatpaks on Linux.
if(WIN32)
    fake_editor("${path}/cursor.cmd" cursor)
    fake_editor("${WORK}/local/Programs/Microsoft VS Code/bin/code.cmd" code)
    fake_editor("${WORK}/local/Programs/VSCodium/bin/codium.cmd" codium)
    set(ENV{LOCALAPPDATA} "${WORK}/local")
    set(ENV{USERPROFILE} "${home}")
elseif(APPLE)
    fake_editor("${path}/cursor" cursor)
    fake_editor("${home}/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code" code)
    fake_editor("${home}/Applications/VSCodium.app/Contents/Resources/app/bin/codium" codium)
else()
    fake_editor("${path}/cursor" cursor)
    fake_editor("${home}/.local/share/flatpak/exports/bin/com.visualstudio.code" code)
    fake_editor("${home}/.local/share/flatpak/exports/bin/com.vscodium.codium" codium)
endif()
set(ENV{HOME} "${home}")
set(ENV{PATH} "${path}")

# VS Code and Cursor have Tide; VSCodium only has the grammar.
file(MAKE_DIRECTORY "${home}/.vscode/extensions/tide-engine.tide-0.1.0")
file(MAKE_DIRECTORY "${home}/.cursor/extensions/tide-engine.tide-0.1.0")
file(MAKE_DIRECTORY "${home}/.vscode-oss/extensions/tide-engine.tide-syntax-0.1.0")

function(check_ran name)
    if(NOT EXISTS "${WORK}/called-${name}.txt")
        message(FATAL_ERROR "tide didn't run ${name}")
    endif()
    file(READ "${WORK}/called-${name}.txt" args)
    if(NOT args MATCHES "--install-extension .*tide\\.vsix.* --force")
        message(FATAL_ERROR "tide ran ${name} with ${args}")
    endif()
    file(REMOVE "${WORK}/called-${name}.txt")
endfunction()

function(check_output output)
    foreach(wanted ${ARGN})
        string(FIND "${output}" "${wanted}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "tide didn't say \"${wanted}\":\n${output}")
        endif()
    endforeach()
endfunction()

execute_process(COMMAND "${tide}" editors --update RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "tide editors --update failed (${result}):\n${output}")
endif()
check_ran(code)
check_ran(cursor)
if(EXISTS "${WORK}/called-codium.txt")
    message(FATAL_ERROR "tide updated VSCodium, which only has the grammar:\n${output}")
endif()
check_output("${output}" "Updated Tide in VS Code." "Updated Tide in Cursor." "Developer: Reload Window")
