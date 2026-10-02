package io.github.blenminer.tide;

import com.intellij.openapi.util.SystemInfo;
import com.intellij.util.EnvironmentUtil;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;

// The programs that come with tide.
final class Tide {
    static final String INSTALL = "https://github.com/BlenMiner/tide-engine#install";

    private Tide() {}

    static @NotNull String exe(@NotNull String name) {
        return SystemInfo.isWindows ? name + ".exe" : name;
    }

    // `name` on PATH; else where tide's installers put it, since an IDE started
    // before tide was installed doesn't have it on its PATH yet. Only PATH's
    // absolute folders: a relative one would be wherever the IDE runs.
    static @Nullable Path installed(@NotNull String name) {
        final String exe = exe(name);
        final String path = EnvironmentUtil.getValue("PATH");
        if (path != null) {
            for (final String dir : path.split(File.pathSeparator)) {
                if (!dir.isEmpty() && isAbsolute(dir) && isFile(dir, exe)) return Path.of(dir, exe);
            }
        }
        final String installed = SystemInfo.isWindows ? System.getenv("LOCALAPPDATA") : System.getProperty("user.home");
        final String folder = SystemInfo.isWindows ? "Tide" : ".tide";
        if (installed != null && isFile(installed, folder, "bin", exe)) return Path.of(installed, folder, "bin", exe);
        return null;
    }

    private static boolean isAbsolute(@NotNull String path) {
        try {
            return Path.of(path).isAbsolute();
        } catch (InvalidPathException e) {
            return false;
        }
    }

    static boolean isFile(@NotNull String first, @NotNull String... more) {
        try {
            return Files.isRegularFile(Path.of(first, more));
        } catch (InvalidPathException e) {
            return false; // A PATH entry that isn't a path
        }
    }
}
