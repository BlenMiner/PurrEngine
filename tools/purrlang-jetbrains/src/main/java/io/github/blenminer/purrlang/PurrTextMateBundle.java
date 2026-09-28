package io.github.blenminer.purrlang;

import com.intellij.openapi.application.PathManager;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.plugins.textmate.api.TextMateBundleProvider;

import java.nio.file.Path;
import java.util.List;

// The TextMate grammar (tools/purrlang-syntax), which the build puts in the
// plugin's folder: it colors keywords, numbers and comments, and purrls's
// semantic highlighting adds what names mean.
public final class PurrTextMateBundle implements TextMateBundleProvider {
    @Override
    public @NotNull List<PluginBundle> getBundles() {
        // This class is in <plugin>/lib/purrlang.jar.
        final String jar = PathManager.getJarPathForClass(PurrTextMateBundle.class);
        if (jar == null) return List.of();
        final Path plugin = Path.of(jar).getParent().getParent();
        return List.of(new PluginBundle("PurrLang", plugin.resolve("textmate")));
    }
}
