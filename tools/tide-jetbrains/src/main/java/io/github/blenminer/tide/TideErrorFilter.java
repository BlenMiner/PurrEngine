package io.github.blenminer.tide;

import com.intellij.execution.filters.Filter;
import com.intellij.execution.filters.OpenFileHyperlinkInfo;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.LocalFileSystem;
import com.intellij.openapi.vfs.VirtualFile;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

// Links the places errors and warnings name in the Run console to the file:
// tidec's `path:line:column: error: ...` (see report in compiler/src/common.c)
// and clang's, for the game's C. A relative path is in the game's folder,
// where tide runs.
final class TideErrorFilter implements Filter, DumbAware {
    // A path (with a drive on Windows) and where in it, at the start of a line.
    private static final Pattern PLACE = Pattern.compile("^((?:[A-Za-z]:)?[^:\\r\\n]+):(\\d{1,9}):(\\d{1,9}):");

    private final Project project;
    private final Path folder;

    TideErrorFilter(@NotNull Project project, @NotNull Path folder) {
        this.project = project;
        this.folder = folder;
    }

    @Override
    public @Nullable Result applyFilter(@NotNull String line, int entireLength) {
        final Matcher place = PLACE.matcher(line);
        if (!place.find()) return null;
        final Path path;
        try {
            final Path named = Path.of(place.group(1).trim());
            path = (named.isAbsolute() ? named : folder.resolve(named)).normalize();
        } catch (InvalidPathException e) {
            return null; // Not a path after all
        }
        final VirtualFile file = LocalFileSystem.getInstance().findFileByNioFile(path);
        if (file == null || file.isDirectory()) return null;
        final int lineNumber = Math.max(Integer.parseInt(place.group(2)) - 1, 0);
        final int column = Math.max(Integer.parseInt(place.group(3)) - 1, 0);
        final int start = entireLength - line.length();
        return new Result(start + place.start(1), start + place.end(3), new OpenFileHyperlinkInfo(project, file, lineNumber, column));
    }
}
