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

    // The game `file` is in, found as tidels finds it (game_folder in
    // compiler/lsp/server.c): the folder the project's build/tools/games.txt
    // lists it in; else the innermost folder with a tide.packages it's in, up
    // to the project's folder, and for a package, the project's folder when
    // its game lists it; else the project's folder, unless that file lists
    // games in it, whose other files stand alone; else the file's own folder.
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
        final boolean inProject = root != null && path.startsWith(root);
        for (Path dir = path.getParent(); dir != null; dir = dir.getParent()) {
            final List<String> own = packagesLines(dir);
            if (own != null) {
                final String name = packageName(own);
                if (name == null) return new Game(dir, null);
                final List<String> game = root != null ? packagesLines(root) : null;
                if (game != null && packageName(game) == null && listsPackage(root, game, dir)) return new Game(root, null);
                return new Game(null, file.getFileName() + " is in package " + name
                                      + ", and tide run plays a game: run one whose tide.packages lists it.");
            }
            if (inProject && dir.equals(root)) break;
        }
        if (root == null || !inProject) return new Game(path.getParent(), null);
        for (final Line line : lines) {
            if (Path.of(line.path).toAbsolutePath().normalize().startsWith(root)) {
                return new Game(null, file.getFileName() + " is in none of the games build/tools/games.txt lists.");
            }
        }
        return new Game(root, null);
    }

    // A folder's tide.packages (see docs/guide/packages.md), as lines without
    // their comments, or null when it has none.
    private static @Nullable List<String> packagesLines(@NotNull Path folder) {
        final Path file = folder.resolve("tide.packages");
        if (!Files.isRegularFile(file)) return null;
        final List<String> lines = new ArrayList<>();
        try {
            for (final String text : Files.readAllLines(file, StandardCharsets.UTF_8)) {
                final String line = text.replaceAll("(^|\\s)#.*$", "").trim();
                if (!line.isEmpty()) lines.add(line);
            }
        } catch (IOException e) {
            return null;
        }
        return lines;
    }

    // The package a folder's lines make it, by its `package` line, or null for a game.
    private static @Nullable String packageName(@NotNull List<String> lines) {
        for (final String line : lines) {
            final String[] words = line.split("\\s+");
            if (words.length == 2 && words[0].equals("package")) return words[1];
        }
        return null;
    }

    // Where tide keeps packages from git, as compiler/lsp/packages.c says.
    private static @NotNull Path packagesCache() {
        final String custom = System.getenv("TIDE_PACKAGES");
        if (custom != null && !custom.isEmpty()) return Path.of(custom);
        final String local = System.getenv("LOCALAPPDATA");
        if (System.getProperty("os.name").startsWith("Windows")) return Path.of(local != null ? local : ".", "Tide", "packages");
        return Path.of(System.getProperty("user.home"), ".tide", "packages");
    }

    // Whether the game in `game`, with these lines, lists the package in
    // `folder`: a folder, or a repository's commit (with //sub for a folder in
    // it) in tide's cache.
    private static boolean listsPackage(@NotNull Path game, @NotNull List<String> lines, @NotNull Path folder) {
        for (final String line : lines) {
            final String[] words = line.split("\\s+");
            final String source = words[0];
            if (source.equals("package") || source.equals("tide")) continue;
            try {
                Path dir;
                if (source.matches("^(\\.|/|\\\\|[A-Za-z]:).*")) {
                    dir = game.resolve(source);
                } else if (words.length == 2) {
                    final int slash = source.indexOf('/');
                    final int at = source.lastIndexOf('@');
                    final String plain = at > slash ? source.substring(0, at) : source;
                    final int sub = plain.indexOf("//");
                    dir = packagesCache().resolve(sub < 0 ? plain : plain.substring(0, sub)).resolve(words[1]);
                    if (sub >= 0) dir = dir.resolve(plain.substring(sub + 2));
                } else {
                    continue;
                }
                if (dir.toAbsolutePath().normalize().equals(folder)) return true;
            } catch (InvalidPathException e) {
                // Not a path on this machine
            }
        }
        return false;
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
