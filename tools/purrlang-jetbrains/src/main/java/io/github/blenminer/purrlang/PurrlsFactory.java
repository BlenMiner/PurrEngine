package io.github.blenminer.purrlang;

import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.ide.BrowserUtil;
import com.intellij.notification.NotificationAction;
import com.intellij.notification.NotificationGroupManager;
import com.intellij.notification.NotificationType;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.SystemInfo;
import com.intellij.util.EnvironmentUtil;
import com.redhat.devtools.lsp4ij.LanguageServerFactory;
import com.redhat.devtools.lsp4ij.server.OSProcessStreamConnectionProvider;
import com.redhat.devtools.lsp4ij.server.StreamConnectionProvider;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;

// Starts purrls, the language server that comes with purr, for LSP4IJ.
public final class PurrlsFactory implements LanguageServerFactory {
    private static final String EXE = SystemInfo.isWindows ? "purrls.exe" : "purrls";
    private static final String INSTALL = "https://github.com/BlenMiner/PurrEngine#install";

    @Override
    public @NotNull StreamConnectionProvider createConnectionProvider(@NotNull Project project) {
        final Path server = find(project);
        if (server == null) {
            NotificationGroupManager.getInstance().getNotificationGroup("PurrLang")
                .createNotification("PurrLang: couldn't find purrls, the language server that comes with purr. Install purr, then reopen the file.",
                                    NotificationType.ERROR)
                .addAction(NotificationAction.createSimpleExpiring("Install purr", () -> BrowserUtil.browse(INSTALL)))
                .notify(project);
        }
        final GeneralCommandLine command = new GeneralCommandLine(server != null ? server.toString() : EXE);
        if (project.getBasePath() != null) command.setWorkDirectory(project.getBasePath());
        return new OSProcessStreamConnectionProvider(command);
    }

    // The project's own build/tools/purrls, when the project is PurrEngine
    // itself; else purrls on PATH; else where purr's installers put it, since
    // an IDE started before purr was installed doesn't have it on its PATH yet.
    private static @Nullable Path find(@NotNull Project project) {
        final String base = project.getBasePath();
        if (base != null && isFile(base, "build", "tools", EXE)) return Path.of(base, "build", "tools", EXE);
        final String path = EnvironmentUtil.getValue("PATH");
        if (path != null) {
            for (final String dir : path.split(File.pathSeparator)) {
                if (!dir.isEmpty() && isFile(dir, EXE)) return Path.of(dir, EXE);
            }
        }
        final String installed = SystemInfo.isWindows ? System.getenv("LOCALAPPDATA") : System.getProperty("user.home");
        final String folder = SystemInfo.isWindows ? "Purr" : ".purr";
        if (installed != null && isFile(installed, folder, "bin", EXE)) return Path.of(installed, folder, "bin", EXE);
        return null;
    }

    private static boolean isFile(@NotNull String first, @NotNull String... more) {
        try {
            return Files.isRegularFile(Path.of(first, more));
        } catch (InvalidPathException e) {
            return false; // A PATH entry that isn't a path
        }
    }
}
