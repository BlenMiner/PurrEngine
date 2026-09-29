# PurrLang for VS Code

Completion, errors as you type with quick fixes, hover, go to definition, find
usages, rename, formatting, parameter hints, inlay hints, the outline, symbols
across the game, folding, semantic highlighting, moving a declaration to a file
of its own, and each system's schedule above it, for `.purr` files. It works in
VS Code, Cursor, VSCodium and Windsurf.

purr's installer adds this extension to the editors it finds, and `purr
editors` adds it again, for example after installing a new editor. The
language server is `purrls`, which comes with purr. The extension runs it from
`PATH`, or from where purr is installed. In PurrEngine's own repo, it runs the
build's `build/tools/purrls`.

Open the folder of your game, the one you'd run `purr run` in: every `.purr`
file in it and its subfolders is one game.

## Settings

- `purrlang.server.path`: another language server to run.
  `${workspaceFolder}` is the open folder.

After a server update, run **PurrLang: Restart Language Server**.
