package io.github.blenminer.tide;

import com.intellij.ide.FileIconProvider;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.IconLoader;
import com.intellij.openapi.vfs.VirtualFile;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.Icon;

// .tide files show Tide's logo, where TextMate's files would show its own.
public final class TideFileIcon implements FileIconProvider, DumbAware {
    private static final Icon ICON = IconLoader.getIcon("/icons/tide.svg", TideFileIcon.class);

    @Override
    public @Nullable Icon getIcon(@NotNull VirtualFile file, int flags, @Nullable Project project) {
        return "tide".equals(file.getExtension()) ? ICON : null;
    }
}
