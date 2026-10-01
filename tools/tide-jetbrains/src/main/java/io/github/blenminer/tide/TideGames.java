package io.github.blenminer.tide;

import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

// The game a .tide file is in, as `tide run` plays it: a folder.
final class TideGames {
    // A folder to run, or why there's none.
    record Game(@Nullable Path folder, @Nullable String error) {}

    // A game CMake builds (tide_add_game) and one of its files, or a folder
    // (ending in '/') for every .tide file in it and its subfolders.
    private record Line(String game, String path) {}

    private TideGames() {}

    // The game `file` is in, found as tidels finds it (game_of in
    // compiler/lsp/server.c): the folder the project's build/tools/games.txt
    // lists it in; else the project's folder, unless that file lists games in
    // it, whose other files stand alone; else the file's own folder.
    static @NotNull Game of(@NotNull Path file, @Nullable Path project) {
        final Path path = file.toAbsolutePath().normalize();
        final List<Line> lines = project != null ? manifest(project) : List.of();
        for (final Line line : lines) {
            final Path listed = Path.of(line.path).toAbsolutePath().normalize();
            if (line.path.endsWith("/") ? path.startsWith(listed) : path.equals(listed)) {
                if (line.path.endsWith("/")) return new Game(listed, null);
                return new Game(null, file.getFileName() + " is in " + line.game
                                      + ", which CMake builds from a list of files, and tide run plays a whole folder.");
            }
        }
        final Path root = project != null ? project.toAbsolutePath().normalize() : null;
        if (root == null || !path.startsWith(root)) return new Game(path.getParent(), null);
        for (final Line line : lines) {
            if (Path.of(line.path).toAbsolutePath().normalize().startsWith(root)) {
                return new Game(null, file.getFileName() + " is in none of the games build/tools/games.txt lists.");
            }
        }
        return new Game(root, null);
    }

    private static @NotNull List<Line> manifest(@NotNull Path project) {
        final List<Line> lines = new ArrayList<>();
        try {
            for (final String text : Files.readAllLines(project.resolve("build/tools/games.txt"), StandardCharsets.UTF_8)) {
                final int tab = text.indexOf('\t');
                if (tab <= 0) continue;
                final String path = text.substring(tab + 1);
                try {
                    Path.of(path);
                } catch (InvalidPathException e) {
                    continue;
                }
                lines.add(new Line(text.substring(0, tab), path));
            }
        } catch (IOException e) {
            // No games built with CMake there
        }
        return lines;
    }
}
