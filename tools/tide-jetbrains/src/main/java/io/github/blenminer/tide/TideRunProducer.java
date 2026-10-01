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
// Ctrl+Shift+F10): in a window, or on the web.
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
        final Path folder = gameFolder(context);
        if (folder == null) return false;
        final TideRunConfiguration.Options options = configuration.getOptions();
        options.setFolder(folder.toString());
        options.setWeb(web);
        configuration.setGeneratedName();
        return true;
    }

    @Override
    public boolean isConfigurationFromContext(@NotNull TideRunConfiguration configuration, @NotNull ConfigurationContext context) {
        final Path folder = gameFolder(context);
        final TideRunConfiguration.Options options = configuration.getOptions();
        return folder != null && options.isWeb() == web && folder.toString().equals(options.getFolder());
    }

    private static @Nullable Path gameFolder(@NotNull ConfigurationContext context) {
        final PsiElement element = context.getPsiLocation();
        final PsiFile psi = element != null ? element.getContainingFile() : null;
        final VirtualFile file = psi != null ? psi.getVirtualFile() : null;
        if (file == null || !file.isInLocalFileSystem() || !"tide".equals(file.getExtension())) return null;
        final String base = context.getProject().getBasePath();
        return TideGames.of(file.toNioPath(), base != null ? Path.of(base) : null).folder();
    }
}
