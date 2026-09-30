package io.github.blenminer.purrlang;

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

// Starts purrls, the language server that comes with purr, for LSP4IJ.
public final class PurrlsFactory implements LanguageServerFactory {
    @Override
    public @NotNull StreamConnectionProvider createConnectionProvider(@NotNull Project project) {
        final Path server = find(project);
        if (server == null) {
            NotificationGroupManager.getInstance().getNotificationGroup("PurrLang")
                .createNotification("PurrLang: couldn't find purrls, the language server that comes with purr. Install purr, then reopen the file.",
                                    NotificationType.ERROR)
                .addAction(NotificationAction.createSimpleExpiring("Install purr", () -> BrowserUtil.browse(Purr.INSTALL)))
                .notify(project);
        }
        final GeneralCommandLine command = new GeneralCommandLine(server != null ? server.toString() : Purr.exe("purrls"));
        if (project.getBasePath() != null) command.setWorkDirectory(project.getBasePath());
        return new OSProcessStreamConnectionProvider(command);
    }

    // The project's own build/tools/purrls, when the project is PurrEngine
    // itself; else the one that comes with purr.
    private static @Nullable Path find(@NotNull Project project) {
        final String base = project.getBasePath();
        final String exe = Purr.exe("purrls");
        if (base != null && Purr.isFile(base, "build", "tools", exe)) return Path.of(base, "build", "tools", exe);
        return Purr.installed("purrls");
    }
}
