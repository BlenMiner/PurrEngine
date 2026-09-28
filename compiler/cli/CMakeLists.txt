# purr: the command users run to build, run and upgrade games. It runs purrc's
# front end in-process and calls clang (see AGENTS.md, Packaging and releases).
add_executable(purr_cli main.c build.c editors.c sha256.c sys.c toolchain.c upgrade.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../lsp/folders.c" "${CMAKE_CURRENT_SOURCE_DIR}/../lsp/json.c")
target_include_directories(purr_cli PRIVATE . ../lsp)
target_link_libraries(purr_cli PRIVATE purrc_core)
# The engine library is already the `purr` target; the program is still called purr.
set_target_properties(purr_cli PROPERTIES OUTPUT_NAME purr)
target_compile_definitions(purr_cli PRIVATE
    "PURR_VERSION=\"${PURR_VERSION}\""
    "PURR_CHANNEL=\"${PURR_CHANNEL}\""
    "PURR_REPOSITORY=\"${PURR_REPOSITORY}\"")
install(TARGETS purr_cli RUNTIME DESTINATION bin COMPONENT purr)

# Packages build clang and lld into purr, so users need no compiler of their
# own (see cmake/LLVM.cmake).
option(PURR_EMBED_LLVM "Build clang and lld into purr, from LLVM's static libraries" OFF)
if(PURR_EMBED_LLVM)
    include("${PROJECT_SOURCE_DIR}/cmake/LLVM.cmake")
    target_link_libraries(purr_cli PRIVATE purr_cc)
    target_compile_definitions(purr_cli PRIVATE PURR_EMBEDDED_CLANG "PURR_LLVM_VERSION=\"${PURR_LLVM_VERSION}\"")
endif()
