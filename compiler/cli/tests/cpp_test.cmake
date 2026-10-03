# tide building a game's C++ files, which have no C++ runtime behind them (see
# AGENTS.md, Language): that they build, natively and for the web, and what
# tide says after a build that fails for want of the runtime. On the web, a
# function nothing defines fails the build too.
#
# It needs a tide that can build games, with the engine's files next to it and
# a compiler: an installation, or this repo's package (cmake/package.cmake). So
# it's not one of a build tree's tests: CI runs it on the package it made.
#
# cmake -DTIDE=<the tide program> -DWORK=<a scratch folder> [-DWEB=OFF] [-DANDROID=ON] -P cpp_test.cmake

cmake_minimum_required(VERSION 3.25)
foreach(var TIDE WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "Pass -D${var}=...")
    endif()
endforeach()
if(NOT DEFINED WEB)
    set(WEB ON)
endif()
# tide runs in each game's folder, so paths given from this one are made whole.
get_filename_component(TIDE "${TIDE}" ABSOLUTE)
get_filename_component(WORK "${WORK}" ABSOLUTE)
if(NOT EXISTS "${TIDE}" AND EXISTS "${TIDE}.exe")
    string(APPEND TIDE ".exe")
endif()

file(REMOVE_RECURSE "${WORK}")
set(ENV{TIDE_NO_UPDATE_CHECK} 1)
set(e2e "${CMAKE_CURRENT_LIST_DIR}/../../tests/e2e")

# Runs tide in `dir`, and checks how it ended and that what it said has each
# of the texts after EXPECT.
function(tide dir)
    cmake_parse_arguments(ARG "FAILS" "" "ARGS;EXPECT" ${ARGN})
    execute_process(COMMAND "${TIDE}" ${ARG_ARGS} WORKING_DIRECTORY "${dir}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(said "${out}${err}")
    if(ARG_FAILS AND code EQUAL 0)
        message(FATAL_ERROR "tide ${ARG_ARGS} should have failed, and said:\n${said}")
    elseif(NOT ARG_FAILS AND NOT code EQUAL 0)
        message(FATAL_ERROR "tide ${ARG_ARGS} failed (${code}):\n${said}")
    endif()
    foreach(text IN LISTS ARG_EXPECT)
        string(FIND "${said}" "${text}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "tide ${ARG_ARGS} should have said '${text}', and said:\n${said}")
        endif()
    endforeach()
endfunction()

# The targets to build for: the machine's own, and the web.
set(targets native)
if(WEB)
    list(APPEND targets web)
endif()
set(native_args "")
set(web_args --web)

# ---------------------------------------------------------------------------
# C++ that needs no runtime builds: the compiler's own test of it (classes,
# virtual functions, templates, new and delete it defines, constructors of
# globals), in a subfolder as a library would be.

set(game "${WORK}/game")
file(COPY "${e2e}/cpp.tide" DESTINATION "${game}")
file(COPY "${e2e}/cpp_code.cpp" DESTINATION "${game}/lib")
foreach(target IN LISTS targets)
    tide("${game}" ARGS build . ${${target}_args} EXPECT "Built ")
endforeach()
if(ANDROID)
    tide("${game}" ARGS build . --android EXPECT "Built ")
endif()

# ---------------------------------------------------------------------------
# What the compiler refuses: the standard library's headers, exceptions, RTTI.

file(WRITE "${game}/lib/more.cpp" "#include <cstdint>\nint32_t more;\n")
tide("${game}" ARGS build . FAILS
    EXPECT "'cstdint' file not found" "<cstdint> is the C++ standard library's, which tide doesn't have"
           "include <stdint.h>, C's header for the same")

file(WRITE "${game}/lib/more.cpp" "#include <vector>\nstd::vector<int> more;\n")
tide("${game}" ARGS build . FAILS
    EXPECT "<vector> is the C++ standard library's, which tide doesn't have" "builds with no C++ runtime")

# A header of the game's own that isn't there is only the compiler's to report.
file(WRITE "${game}/lib/more.cpp" "#include <shapes>\nint more;\n")
tide("${game}" ARGS build . FAILS EXPECT "<shapes> is the C++ standard library's")
file(WRITE "${game}/lib/more.cpp" "#include \"shapes.hpp\"\nint more;\n")
execute_process(COMMAND "${TIDE}" build . WORKING_DIRECTORY "${game}" RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(code EQUAL 0 OR "${out}${err}" MATCHES "standard library")
    message(FATAL_ERROR "a missing header of the game's own isn't the standard library's:\n${out}${err}")
endif()

file(WRITE "${game}/lib/more.cpp" "struct Base { virtual void F() {} };
struct Derived : Base {};

int Check(Base *base)
{
    if (!base) throw 1;
    return dynamic_cast<Derived *>(base) != nullptr;
}
")
tide("${game}" ARGS build . FAILS
    EXPECT "a game's C++ builds without exceptions" "a game's C++ builds without RTTI" "builds with no C++ runtime")

# ---------------------------------------------------------------------------
# What the linker misses: new and delete, and a pure virtual function's slot,
# which code defines itself. A virtual destructor uses delete too.

set(bare "${WORK}/bare")
file(WRITE "${bare}/game.tide" "extern int Made();

component Result
{
    int made;
}

scene Main { }

event(Spawned) Setup(with Main)
{
    Spawn(Result);
}

system Run(mut Result r)
{
    r.made = Made();
}
")
file(WRITE "${bare}/made.cpp" "struct Thing {
    int value = 3;
    virtual ~Thing() {}
};

extern \"C\" int Made(void)
{
    Thing *thing = new Thing;
    const int value = thing->value;
    delete thing;
    return value;
}
")
foreach(target IN LISTS targets)
    tide("${bare}" ARGS build . ${${target}_args} FAILS
        EXPECT "operator new" "the game's C++ uses new or delete, which tide has no C++ runtime to define"
               "void *operator new(size_t size) { return malloc(size); }")
endforeach()

file(WRITE "${bare}/made.cpp" "struct Shape {
    Shape();
    virtual int Sides() const = 0;
};

Shape::Shape() {}

struct Square : Shape {
    int Sides() const override { return 4; }
};

extern \"C\" int Made(void)
{
    static const Square square;
    return square.Sides();
}
")
foreach(target IN LISTS targets)
    tide("${bare}" ARGS build . ${${target}_args} FAILS
        EXPECT "a class with a pure virtual function needs __cxa_pure_virtual"
               "extern \"C\" void __cxa_pure_virtual() { abort(); }")
endforeach()

# ...and with them defined, it builds.
file(APPEND "${bare}/made.cpp" "
#include <stdlib.h>

extern \"C\" void __cxa_pure_virtual() { abort(); }
")
foreach(target IN LISTS targets)
    tide("${bare}" ARGS build . ${${target}_args} EXPECT "Built ")
endforeach()

# ---------------------------------------------------------------------------
# On the web, a function nothing defines fails the build, where it would
# otherwise fail when the page calls it: an extern function of the game's, and
# one its C calls.

if(WEB)
    file(WRITE "${bare}/made.cpp" "#ifndef __wasm__
extern \"C\" int Made(void)
{
    return 1;
}
#endif
")
    tide("${bare}" ARGS build . EXPECT "Built ")
    tide("${bare}" ARGS build . --web FAILS
        EXPECT "nothing defines the C function Made for the web" "write a stand-in inside '#ifdef __wasm__'")

    file(WRITE "${bare}/made.cpp" "extern \"C\" int only_native(void);

extern \"C\" int Made(void)
{
#ifdef __wasm__
    return only_native();
#else
    return 1;
#endif
}
")
    tide("${bare}" ARGS build . EXPECT "Built ")
    tide("${bare}" ARGS build . --web FAILS EXPECT "undefined symbol: only_native")
endif()
