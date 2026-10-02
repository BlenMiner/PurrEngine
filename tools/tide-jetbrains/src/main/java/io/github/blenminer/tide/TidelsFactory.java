package io.github.blenminer.tide;

import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.project.ProjectManager;
import com.intellij.openapi.util.Key;
import com.intellij.openapi.util.SystemInfo;
import com.intellij.ui.EditorNotifications;
import com.redhat.devtools.lsp4ij.LanguageServerEnablementSupport;
import com.redhat.devtools.lsp4ij.LanguageServerFactory;
import com.redhat.devtools.lsp4ij.LanguageServerManager;
import com.redhat.devtools.lsp4ij.server.OSProcessStreamConnectionProvider;
import com.redhat.devtools.lsp4ij.server.StreamConnectionProvider;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;

// Starts tidels, the language server that comes with tide, for LSP4IJ. While
// there's none to start, LSP4IJ leaves it off, and Tide files say why
// (TidelsBanner).
public final class TidelsFactory implements LanguageServerFactory, LanguageServerEnablementSupport {
    static final String SERVER = "tidels"; // Its id in plugin.xml

    // Turned off in LSP4IJ's Language Servers view, until turned on again.
    private static final Key<Boolean> DISABLED = Key.create("tide.tidels.disabled");

    // The tidels to run, or why there's none.
    record Found(@Nullable Path server, @Nullable String problem) {}

    @Override
    public @NotNull StreamConnectionProvider createConnectionProvider(@NotNull Project project) {
        final Path server = find(project).server();
        // isEnabled keeps LSP4IJ from getting here without one.
        final GeneralCommandLine command = new GeneralCommandLine(server != null ? server.toString() : Tide.exe("tidels"));
        if (project.getBasePath() != null) command.setWorkDirectory(project.getBasePath());
        return new OSProcessStreamConnectionProvider(command);
    }

    @Override
    public boolean isEnabled(@NotNull Project project) {
        return !Boolean.TRUE.equals(project.getUserData(DISABLED)) && find(project).server() != null;
    }

    @Override
    public void setEnabled(boolean enabled, @NotNull Project project) {
        project.putUserData(DISABLED, enabled ? null : Boolean.TRUE);
    }

    // The tidels the settings name; else the project's own build/tools/tidels,
    // when it's Tide itself and the project is trusted, as it runs what's in
    // the project; else the one that comes with tide.
    static @NotNull Found find(@NotNull Project project) {
        final String setting = TideSettings.get().tidels();
        if (!setting.isEmpty()) {
            final Path named = program(setting);
            if (named != null) return new Found(named, null);
            return new Found(null, "There's no tidels at " + setting + ", where Settings | Languages & Frameworks | Tide says it is.");
        }
        final String base = project.getBasePath();
        final String exe = Tide.exe("tidels");
        if (base != null && Tide.isFile(base, "build", "tools", exe) && isTrusted(project)) {
            return new Found(Path.of(base, "build", "tools", exe), null);
        }
        final Path installed = Tide.installed("tidels");
        if (installed != null) return new Found(installed, null);
        return new Found(null, "Couldn't find tidels, the language server that comes with tide.");
    }

    // Whether the user trusts the project, so that its programs can run:
    // TrustedProjects.isProjectTrusted, where the platform has it, else the
    // older TrustedProjects.isTrusted, which it replaces. Looked up by name:
    // 2024.2 lacks the first, and later platforms deprecate the second. Not
    // trusted when neither answers.
    private static boolean isTrusted(@NotNull Project project) {
        final String[][] apis = {
            {"com.intellij.ide.trustedProjects.TrustedProjects", "isProjectTrusted"},
            {"com.intellij.ide.impl.TrustedProjects", "isTrusted"},
        };
        for (final String[] api : apis) {
            try {
                final Method method = Class.forName(api[0]).getMethod(api[1], Project.class);
                if (Modifier.isStatic(method.getModifiers()) && method.invoke(null, project) instanceof Boolean trusted) return trusted;
            } catch (ReflectiveOperationException e) {
                // Not on this platform: the next one
            }
        }
        return false;
    }

    // The program at `path` (~ is the home folder), which on Windows can leave out .exe.
    private static @Nullable Path program(@NotNull String setting) {
        final String home = System.getProperty("user.home");
        final String path = home != null && (setting.equals("~") || setting.startsWith("~/") || setting.startsWith("~\\"))
                            ? home + setting.substring(1) : setting;
        try {
            final Path file = Path.of(path);
            if (Files.isRegularFile(file)) return file;
            final Path exe = Path.of(path + ".exe");
            return SystemInfo.isWindows && Files.isRegularFile(exe) ? exe : null;
        } catch (InvalidPathException e) {
            return null;
        }
    }

    // Starts tidels again in `project`, as where it is may have changed: with
    // none, stops the one that runs, and Tide files say why.
    static void restart(@NotNull Project project) {
        if (project.isDisposed()) return;
        EditorNotifications.getInstance(project).updateAllNotifications();
        final LanguageServerManager servers = LanguageServerManager.getInstance(project);
        if (find(project).server() == null) servers.stop(SERVER);
        else servers.start(SERVER, new LanguageServerManager.StartOptions().setForceRestart(true));
    }

    static void restartAll() {
        for (final Project project : ProjectManager.getInstance().getOpenProjects()) restart(project);
    }
}
