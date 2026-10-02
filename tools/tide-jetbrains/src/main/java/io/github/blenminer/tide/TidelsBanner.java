package io.github.blenminer.tide;

import com.intellij.ide.BrowserUtil;
import com.intellij.openapi.fileEditor.FileEditor;
import com.intellij.openapi.options.ShowSettingsUtil;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.ui.EditorNotificationPanel;
import com.intellij.ui.EditorNotificationProvider;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.JComponent;
import java.util.function.Function;

// Above a Tide file while there's no tidels to run: why, and the ways to fix
// it, which start the server without reopening anything.
public final class TidelsBanner implements EditorNotificationProvider, DumbAware {
    @Override
    public @Nullable Function<? super FileEditor, ? extends JComponent> collectNotificationData(@NotNull Project project,
                                                                                              @NotNull VirtualFile file) {
        if (!"tide".equals(file.getExtension())) return null;
        final String problem = TidelsFactory.find(project).problem();
        if (problem == null) return null;
        return editor -> {
            final EditorNotificationPanel panel = new EditorNotificationPanel(editor, EditorNotificationPanel.Status.Warning);
            panel.setText(problem + " Without it, Tide files have no completion, errors or navigation.");
            panel.createActionLabel("Install tide", () -> BrowserUtil.browse(Tide.INSTALL));
            // Applying the settings starts it (TideConfigurable.apply).
            panel.createActionLabel("Choose tidels", () -> ShowSettingsUtil.getInstance().showSettingsDialog(project, TideConfigurable.class));
            panel.createActionLabel("Try again", () -> TidelsFactory.restart(project));
            return panel;
        };
    }
}
