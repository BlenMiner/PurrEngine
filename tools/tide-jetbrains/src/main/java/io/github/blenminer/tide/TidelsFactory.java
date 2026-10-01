package io.github.blenminer.tide;

import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.ide.BrowserUtil;
import com.intellij.notification.NotificationAction;
import com.intellij.notification.NotificationGroupManager;
import com.intellij.notification.NotificationType;
import com.intellij.openapi.project.Project;
import com.redhat.devtools.lsp4ij.LanguageServerFactory;
import com.redhat.devtools.lsp4ij.server.OSProcessStreamConnectionProvider;
import com.redhat.devtools.lsp4ij.server.StreamConnectionProvider;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.nio.file.Path;

// Starts tidels, the language server that comes with tide, for LSP4IJ.
public final class TidelsFactory implements LanguageServerFactory {
    @Override
    public @NotNull StreamConnectionProvider createConnectionProvider(@NotNull Project project) {
        final Path server = find(project);
        if (server == null) {
            NotificationGroupManager.getInstance().getNotificationGroup("Tide")
                .createNotification("Tide: couldn't find tidels, the language server that comes with tide. Install tide, then reopen the file.",
                                    NotificationType.ERROR)
                .addAction(NotificationAction.createSimpleExpiring("Install tide", () -> BrowserUtil.browse(Tide.INSTALL)))
                .notify(project);
        }
        final GeneralCommandLine command = new GeneralCommandLine(server != null ? server.toString() : Tide.exe("tidels"));
        if (project.getBasePath() != null) command.setWorkDirectory(project.getBasePath());
        return new OSProcessStreamConnectionProvider(command);
    }

    // The project's own build/tools/tidels, when the project is Tide
    // itself; else the one that comes with tide.
    private static @Nullable Path find(@NotNull Project project) {
        final String base = project.getBasePath();
        final String exe = Tide.exe("tidels");
        if (base != null && Tide.isFile(base, "build", "tools", exe)) return Path.of(base, "build", "tools", exe);
        return Tide.installed("tidels");
    }
}
