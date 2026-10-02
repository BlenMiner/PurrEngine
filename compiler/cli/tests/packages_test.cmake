# tide with packages, without the network: packages in folders, and packages
# from git through TIDE_GIT_MIRROR, a folder that stands in for every git host
# (compiler/cli/fetch.c), with each repository's list of branches and tags and
# an archive of each commit. `tide schedule` runs the compiler's front end on
# a game, packages and all, which needs no C compiler.
#
# cmake -DTIDE=<the tide program> -DWORK=<a scratch folder> -P packages_test.cmake

cmake_minimum_required(VERSION 3.25)
foreach(var TIDE WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "Pass -D${var}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
set(mirror "${WORK}/mirror")
set(ENV{TIDE_PACKAGES} "${WORK}/cache")
set(ENV{TIDE_GIT_MIRROR} "${mirror}")
set(ENV{TIDE_NO_UPDATE_CHECK} 1)

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

function(expect_file path)
    file(READ "${path}" text)
    string(REPLACE "\r\n" "\n" text "${text}") # file(WRITE) writes "\r\n" on Windows
    foreach(want IN LISTS ARGN)
        string(FIND "${text}" "${want}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "${path} should have '${want}', and has:\n${text}")
        endif()
    endforeach()
endfunction()

# ---------------------------------------------------------------------------
# Packages in folders

set(games "${WORK}/games")
file(WRITE "${games}/maths/tide.packages" "package Maths\n")
file(WRITE "${games}/maths/step.tide" "namespace Maths;\n\nfloat Step()\n{\n    return 0.5;\n}\n")
file(WRITE "${games}/physics/tide.packages" "package Physics\n../maths\n")
file(WRITE "${games}/physics/body.tide" "namespace Physics;

component Body
{
    float3 position;
    float3 velocity;
}

system Integrate(mut Body body)
{
    body.position += body.velocity * Maths.Step();
}
")
# A game in the package's folder, to try it: not one of the package's files.
file(WRITE "${games}/physics/example/tide.packages" "..\n../../maths\n")
file(WRITE "${games}/physics/example/main.tide" "using Physics;\n\nscene Main { }\n\nevent(Spawned) Setup(with Main)\n{\n    Spawn(Body { });\n}\n")
file(WRITE "${games}/game/main.tide" "using Physics;

scene Main { }

event(Spawned) Setup(with Main)
{
    Spawn(Body { velocity = float3(0, 1, 0) });
}

system Fall(mut Body body)
{
    body.velocity.y -= 1;
}
")

# tide add puts the package in, and what it needs.
tide("${games}/game" ARGS add ../physics EXPECT "added Physics, from ../physics" "added ../maths, which Physics needs")
expect_file("${games}/game/tide.packages" "../physics\n../maths\n")
tide("${games}/game" ARGS add ../physics EXPECT "Physics is in tide.packages already")
# Packages' files come first, each after those it needs.
tide("${games}/game" ARGS schedule . EXPECT "stage 1  Physics.Integrate" "stage 2  Fall")
tide("${games}/physics/example" ARGS schedule . EXPECT "example: 1 system")
tide("${games}/physics" ARGS schedule . FAILS EXPECT "is package Physics, not a game")

# What's wrong with a game's packages.
file(WRITE "${games}/game/tide.packages" "../physics\n")
tide("${games}/game" ARGS schedule . FAILS
    EXPECT "tide.packages:1: error: Physics needs ../maths, which this file doesn't list" "add this line: ../maths")
file(WRITE "${games}/game/tide.packages" "../physics\n../maths\n../nothing\n")
file(MAKE_DIRECTORY "${games}/nothing")
tide("${games}/game" ARGS schedule . FAILS EXPECT "tide.packages:3: error: ../nothing isn't a package: it has no tide.packages")
file(WRITE "${games}/game/tide.packages" "../physics\n../maths\ntide 99.1\n")
tide("${games}/game" ARGS schedule . FAILS EXPECT "error: this needs tide 99.1 or newer")
file(WRITE "${games}/game/tide.packages" "../physics\n../maths\n")
file(WRITE "${games}/maths/main.tide" "namespace Maths;\n\nscene Main { }\n")
tide("${games}/game" ARGS schedule . FAILS EXPECT "package Maths can't declare Main: the game that uses it does")
file(WRITE "${games}/maths/main.tide" "struct Pair { float a; }\n")
tide("${games}/game" ARGS schedule . FAILS EXPECT "what package Maths declares goes in its namespace, Maths")
file(REMOVE "${games}/maths/main.tide")

# ---------------------------------------------------------------------------
# Packages from git

# One of git's packet lines: its length in four hex digits, then the text.
function(packet out text)
    string(LENGTH "${text}" n)
    math(EXPR n "${n} + 4" OUTPUT_FORMAT HEXADECIMAL)
    string(SUBSTRING "${n}" 2 -1 hex)
    string(LENGTH "${hex}" digits)
    while(digits LESS 4)
        string(PREPEND hex "0")
        math(EXPR digits "${digits} + 1")
    endwhile()
    set(${out} "${${out}}${hex}${text}" PARENT_SCOPE)
endfunction()

# A repository in the mirror: its default branch is the first ref.
function(repository repo)
    set(refs "")
    packet(refs "# service=git-upload-pack\n")
    string(APPEND refs "0000")
    list(GET ARGN 1 head)
    packet(refs "${head} HEAD\n")
    list(LENGTH ARGN n)
    math(EXPR last "${n} - 1")
    foreach(i RANGE 0 ${last} 2)
        math(EXPR j "${i} + 1")
        list(GET ARGN ${i} name)
        list(GET ARGN ${j} commit)
        packet(refs "${commit} ${name}\n")
    endforeach()
    string(APPEND refs "0000")
    # Lengths count "\n" as one byte, which file(WRITE) would make "\r\n" on Windows.
    file(CONFIGURE OUTPUT "${mirror}/${repo}/refs" CONTENT "${refs}" @ONLY NEWLINE_STYLE LF)
endfunction()

# An archive of `repo` at `commit`, of the files in `folder`, in one folder
# of its own as hosts make them.
function(commit repo commit folder)
    get_filename_component(name "${repo}" NAME)
    set(top "${WORK}/archives/${name}-${commit}")
    file(COPY "${folder}/" DESTINATION "${top}")
    file(MAKE_DIRECTORY "${mirror}/${repo}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${mirror}/${repo}/${commit}.tar.gz" "${name}-${commit}"
        WORKING_DIRECTORY "${WORK}/archives" RESULT_VARIABLE code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "couldn't make the archive of ${repo} at ${commit}")
    endif()
endfunction()

set(M1 "1111111111111111111111111111111111111111")
set(P1 "2222222222222222222222222222222222222222")
set(P2 "3333333333333333333333333333333333333333")
set(P3 "4444444444444444444444444444444444444444")
set(N1 "5555555555555555555555555555555555555555")

set(src "${WORK}/sources")
file(WRITE "${src}/maths/tide.packages" "package Maths\n")
file(COPY "${games}/maths/step.tide" DESTINATION "${src}/maths")
commit(github.com/test/maths ${M1} "${src}/maths")
repository(github.com/test/maths refs/heads/main ${M1})

# Physics in a folder of its repository, needing Maths from git.
file(WRITE "${src}/physics/packages/physics/tide.packages" "package Physics\ngithub.com/test/maths ${M1}\n")
file(COPY "${games}/physics/body.tide" DESTINATION "${src}/physics/packages/physics")
commit(github.com/test/physics ${P1} "${src}/physics")
file(WRITE "${src}/physics/packages/physics/later.tide" "namespace Physics;\n\nsystem Settle(mut Body body)\n{\n    body.velocity *= 0.5;\n}\n")
commit(github.com/test/physics ${P2} "${src}/physics")
file(WRITE "${src}/physics/packages/physics/tide.packages" "package Physics\ntide 99.0\ngithub.com/test/maths ${M1}\n")
commit(github.com/test/physics ${P3} "${src}/physics")
repository(github.com/test/physics refs/heads/main ${P2} refs/heads/dev ${P3} refs/tags/v1.0.0 ${P1} refs/tags/v1.1.0 ${P2})

file(WRITE "${src}/nothing/README.md" "Not a package\n")
commit(github.com/test/nothing ${N1} "${src}/nothing")
repository(github.com/test/nothing refs/heads/main ${N1})

set(game "${WORK}/git-game")
file(COPY "${games}/game/main.tide" DESTINATION "${game}")
tide("${game}" ARGS add https://github.com/test/nothing.git FAILS EXPECT "github.com/test/nothing isn't a package")
tide("${game}" ARGS add github.com/test/physics//packages/physics@v1
    EXPECT "downloading github.com/test/physics at 3333333333" "added Physics, at 3333333 (v1.1.0)"
           "added github.com/test/maths ${M1}, which Physics needs")
expect_file("${game}/tide.packages" "github.com/test/physics//packages/physics@v1 ${P2}\n" "github.com/test/maths ${M1}\n")
tide("${game}" ARGS schedule . EXPECT "Physics.Integrate" "Physics.Settle")

# tide update moves a line to the newest commit of what it follows, keeping
# what's around it.
file(WRITE "${game}/tide.packages" "# Packages\ngithub.com/test/physics//packages/physics@v1 ${P1}  # Physics\ngithub.com/test/maths ${M1}\n")
tide("${game}" ARGS update physics EXPECT "Physics 2222222 -> 3333333 (v1.1.0)")
expect_file("${game}/tide.packages" "# Packages\ngithub.com/test/physics//packages/physics@v1 ${P2}  # Physics\n")
tide("${game}" ARGS update EXPECT "Physics is up to date, at 3333333 (v1.1.0)" "Maths is up to date, at 1111111 (main)")
tide("${game}" ARGS update nope FAILS EXPECT "tide.packages has no package called nope" "it has Physics and Maths")

# A line with no commit gets one from tide update, and a package for a newer
# tide says so.
file(WRITE "${game}/tide.packages" "github.com/test/physics//packages/physics@dev\ngithub.com/test/maths ${M1}\n")
tide("${game}" ARGS schedule . FAILS EXPECT "has no commit to build with" "`tide update github.com/test/physics//packages/physics`")
tide("${game}" ARGS update EXPECT "is at 4444444 (dev)")
tide("${game}" ARGS schedule . FAILS EXPECT "Physics needs tide 99.0 or newer")

# A package from git that's gone from this machine comes back by itself.
file(WRITE "${game}/tide.packages" "github.com/test/physics//packages/physics ${P1}\ngithub.com/test/maths ${M1}\n")
file(REMOVE_RECURSE "${WORK}/cache")
tide("${game}" ARGS schedule . EXPECT "downloading github.com/test/physics at 2222222222" "Physics.Integrate")
