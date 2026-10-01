package io.github.blenminer.tide;

import com.intellij.openapi.application.PathManager;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.plugins.textmate.api.TextMateBundleProvider;

import java.nio.file.Path;
import java.util.List;

// The TextMate grammar (tools/tide-syntax), which the build puts in the
// plugin's folder: it colors keywords, numbers and comments, and tidels's
// semantic highlighting adds what names mean.
public final class TideTextMateBundle implements TextMateBundleProvider {
    @Override
    public @NotNull List<PluginBundle> getBundles() {
        // This class is in <plugin>/lib/tide.jar.
        final String jar = PathManager.getJarPathForClass(TideTextMateBundle.class);
        if (jar == null) return List.of();
        final Path plugin = Path.of(jar).getParent().getParent();
        return List.of(new PluginBundle("Tide", plugin.resolve("textmate")));
    }
}
