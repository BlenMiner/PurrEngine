package io.github.blenminer.tide;

import com.intellij.execution.actions.ConfigurationContext;
import com.intellij.execution.actions.LazyRunConfigurationProducer;
import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.openapi.util.Ref;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.nio.file.Path;

// Runs the game of the .tide file at hand (Run in its context menu, or
// Ctrl+Shift+F10): in a window, or on the web. For a file tide run can't play
// (one of a game CMake builds from a list of files), running says why.
public abstract class TideRunProducer extends LazyRunConfigurationProducer<TideRunConfiguration> {
    public static final class Desktop extends TideRunProducer {
        public Desktop() {
            super(false);
        }
    }

    public static final class Web extends TideRunProducer {
        public Web() {
            super(true);
        }
    }

    private final boolean web;

    private TideRunProducer(boolean web) {
        this.web = web;
    }

    @Override
    public @NotNull ConfigurationFactory getConfigurationFactory() {
        return TideRunConfigurationType.factory();
    }

    @Override
    protected boolean setupConfigurationFromContext(@NotNull TideRunConfiguration configuration, @NotNull ConfigurationContext context,
                                                    @NotNull Ref<PsiElement> sourceElement) {
        final VirtualFile file = tideFile(context);
        if (file == null) return false;
        final TideGames.Game game = game(context, file);
        final TideRunConfiguration.Options options = configuration.getOptions();
        options.setWeb(web);
        if (game.folder() != null) {
            options.setFolder(game.folder().toString());
            configuration.setGeneratedName();
        } else {
            options.setProblem(game.error()); // checkConfiguration says it when it runs
            configuration.setName(file.getName() + (web ? " (web)" : ""));
        }
        return true;
    }

    @Override
    public boolean isConfigurationFromContext(@NotNull TideRunConfiguration configuration, @NotNull ConfigurationContext context) {
        final VirtualFile file = tideFile(context);
        final TideRunConfiguration.Options options = configuration.getOptions();
        if (file == null || options.isWeb() != web) return false;
        final TideGames.Game game = game(context, file);
        return game.folder() != null ? game.folder().toString().equals(options.getFolder())
                                     : game.error() != null && game.error().equals(options.getProblem());
    }

    private static @Nullable VirtualFile tideFile(@NotNull ConfigurationContext context) {
        final PsiElement element = context.getPsiLocation();
        final PsiFile psi = element != null ? element.getContainingFile() : null;
        final VirtualFile file = psi != null ? psi.getVirtualFile() : null;
        if (file == null || !file.isInLocalFileSystem() || !"tide".equals(file.getExtension())) return null;
        return file;
    }

    private static @NotNull TideGames.Game game(@NotNull ConfigurationContext context, @NotNull VirtualFile file) {
        final String base = context.getProject().getBasePath();
        return TideGames.of(file.toNioPath(), base != null ? Path.of(base) : null);
    }
}
