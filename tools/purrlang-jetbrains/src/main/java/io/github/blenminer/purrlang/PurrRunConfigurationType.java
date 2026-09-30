package io.github.blenminer.purrlang;

import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.execution.configurations.ConfigurationTypeBase;
import com.intellij.execution.configurations.ConfigurationTypeUtil;
import com.intellij.execution.configurations.RunConfiguration;
import com.intellij.icons.AllIcons;
import com.intellij.openapi.components.BaseState;
import com.intellij.openapi.project.Project;
import org.jetbrains.annotations.NotNull;

// PurrLang run configurations: purr runs a game, in a window of its own or on
// the web, in the PurrLang Game tool window.
public final class PurrRunConfigurationType extends ConfigurationTypeBase {
    public PurrRunConfigurationType() {
        super("PurrLang", "PurrLang", "Runs a PurrLang game with purr", AllIcons.Actions.Execute);
        addFactory(new ConfigurationFactory(this) {
            @Override
            public @NotNull String getId() {
                return "PurrLang";
            }

            @Override
            public @NotNull RunConfiguration createTemplateConfiguration(@NotNull Project project) {
                return new PurrRunConfiguration(project, this);
            }

            @Override
            public Class<? extends BaseState> getOptionsClass() {
                return PurrRunConfiguration.Options.class;
            }
        });
    }

    static @NotNull ConfigurationFactory factory() {
        return ConfigurationTypeUtil.findConfigurationType(PurrRunConfigurationType.class).getConfigurationFactories()[0];
    }
}
