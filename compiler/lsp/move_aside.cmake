# Clears the way for a new EXE while an editor may still run the old one.
# Windows locks a running program against writes and deletes, but not renames:
# an EXE that can't be deleted is renamed instead, and deleted by a later build
# once nothing runs it.
#
# cmake -DEXE=<path> -P move_aside.cmake

file(GLOB old_copies "${EXE}.old-*")
foreach(path IN LISTS old_copies ITEMS "${EXE}")
    # Fails quietly for a file that's still running.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${path}" RESULT_VARIABLE ignored ERROR_QUIET)
endforeach()

if(EXISTS "${EXE}")
    string(RANDOM LENGTH 8 suffix)
    file(RENAME "${EXE}" "${EXE}.old-${suffix}" RESULT result)
endif()
