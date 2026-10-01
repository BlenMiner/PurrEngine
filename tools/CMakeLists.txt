# Editor support in the package's editors/ folder. The TextMate grammar is for
# editors that read TextMate bundles; JetBrains IDEs get the Marketplace plugin
# instead (tools/tide-jetbrains).
install(DIRECTORY tide-syntax DESTINATION editors COMPONENT tide)

# The VS Code extension, for VS Code, Cursor, VSCodium and Windsurf. It needs
# Node, so only package builds make it.
option(TIDE_EDITOR_EXTENSIONS "Build the editor extensions (needs Node's npm)" OFF)
if(TIDE_EDITOR_EXTENSIONS)
    # Extensions take plain x.y.z versions.
    string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" extension_version "${TIDE_VERSION}")
    set(vsix "${CMAKE_BINARY_DIR}/editors/tide.vsix")
    add_custom_command(OUTPUT "${vsix}"
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tide-vscode"
            "-DSYNTAX=${CMAKE_CURRENT_SOURCE_DIR}/tide-syntax"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/tide-vscode"
            "-DVERSION=${extension_version}"
            "-DOUT=${vsix}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tide-vscode/vsix.cmake"
        DEPENDS
            tide-vscode/vsix.cmake
            tide-vscode/package.json
            tide-vscode/package-lock.json
            tide-vscode/README.md
            tide-vscode/src/extension.js
            tide-syntax/language-configuration.json
            tide-syntax/syntaxes/tide.tmLanguage.json
        COMMENT "Building the VS Code extension"
        VERBATIM)
    add_custom_target(tide_vscode ALL DEPENDS "${vsix}")
    install(FILES "${vsix}" DESTINATION editors COMPONENT tide)
endif()
