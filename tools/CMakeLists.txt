# Editor support in the package's editors/ folder. The TextMate grammar is for
# editors that read TextMate bundles; JetBrains IDEs get the Marketplace plugin
# instead (tools/purrlang-jetbrains).
install(DIRECTORY purrlang-syntax DESTINATION editors COMPONENT purr)

# The VS Code extension, for VS Code, Cursor, VSCodium and Windsurf. It needs
# Node, so only package builds make it.
option(PURR_EDITOR_EXTENSIONS "Build the editor extensions (needs Node's npm)" OFF)
if(PURR_EDITOR_EXTENSIONS)
    # Extensions take plain x.y.z versions.
    string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" extension_version "${PURR_VERSION}")
    set(vsix "${CMAKE_BINARY_DIR}/editors/purrlang.vsix")
    add_custom_command(OUTPUT "${vsix}"
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/purrlang-vscode"
            "-DSYNTAX=${CMAKE_CURRENT_SOURCE_DIR}/purrlang-syntax"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/purrlang-vscode"
            "-DVERSION=${extension_version}"
            "-DOUT=${vsix}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/purrlang-vscode/vsix.cmake"
        DEPENDS
            purrlang-vscode/vsix.cmake
            purrlang-vscode/package.json
            purrlang-vscode/package-lock.json
            purrlang-vscode/README.md
            purrlang-vscode/src/extension.js
            purrlang-syntax/language-configuration.json
            purrlang-syntax/syntaxes/purrlang.tmLanguage.json
        COMMENT "Building the VS Code extension"
        VERBATIM)
    add_custom_target(purrlang_vscode ALL DEPENDS "${vsix}")
    install(FILES "${vsix}" DESTINATION editors COMPONENT purr)
endif()
