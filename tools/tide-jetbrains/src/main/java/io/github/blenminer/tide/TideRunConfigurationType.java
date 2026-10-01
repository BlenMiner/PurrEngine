package io.github.blenminer.tide;

import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.execution.configurations.ConfigurationTypeBase;
import com.intellij.execution.configurations.ConfigurationTypeUtil;
import com.intellij.execution.configurations.RunConfiguration;
import com.intellij.icons.AllIcons;
import com.intellij.openapi.components.BaseState;
import com.intellij.openapi.project.Project;
import org.jetbrains.annotations.NotNull;

// Tide run configurations: tide runs a game, in a window of its own or on
// the web, in the Tide Game tool window.
public final class TideRunConfigurationType extends ConfigurationTypeBase {
    public TideRunConfigurationType() {
        super("Tide", "Tide", "Runs a Tide game with tide", AllIcons.Actions.Execute);
        addFactory(new ConfigurationFactory(this) {
            @Override
            public @NotNull String getId() {
                return "Tide";
            }

            @Override
            public @NotNull RunConfiguration createTemplateConfiguration(@NotNull Project project) {
                return new TideRunConfiguration(project, this);
            }

            @Override
            public Class<? extends BaseState> getOptionsClass() {
                return TideRunConfiguration.Options.class;
            }
        });
    }

    static @NotNull ConfigurationFactory factory() {
        return ConfigurationTypeUtil.findConfigurationType(TideRunConfigurationType.class).getConfigurationFactories()[0];
    }
}
