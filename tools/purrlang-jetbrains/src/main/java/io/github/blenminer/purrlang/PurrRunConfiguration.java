package io.github.blenminer.purrlang;

import com.intellij.execution.ExecutionException;
import com.intellij.execution.Executor;
import com.intellij.execution.configurations.CommandLineState;
import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.execution.configurations.LocatableConfigurationBase;
import com.intellij.execution.configurations.LocatableRunConfigurationOptions;
import com.intellij.execution.configurations.RunConfiguration;
import com.intellij.execution.configurations.RunProfileState;
import com.intellij.execution.configurations.RuntimeConfigurationError;
import com.intellij.execution.process.KillableColoredProcessHandler;
import com.intellij.execution.process.ProcessEvent;
import com.intellij.execution.process.ProcessHandler;
import com.intellij.execution.process.ProcessListener;
import com.intellij.execution.process.ProcessOutputTypes;
import com.intellij.execution.process.ProcessTerminatedListener;
import com.intellij.execution.runners.ExecutionEnvironment;
import com.intellij.openapi.components.StoredProperty;
import com.intellij.openapi.fileChooser.FileChooserDescriptorFactory;
import com.intellij.openapi.options.SettingsEditor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.ui.TextBrowseFolderListener;
import com.intellij.openapi.ui.TextFieldWithBrowseButton;
import com.intellij.openapi.util.Key;
import com.intellij.ui.components.JBCheckBox;
import com.intellij.util.ui.FormBuilder;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.JComponent;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

// `purr run` on a game's folder, in the Run tool window: purr's output and the
// game's, and what's typed there goes to purr (r and Enter starts the game
// over). On the web, purr serves the page without opening a browser, and the
// PurrLang Game tool window shows it.
public final class PurrRunConfiguration extends LocatableConfigurationBase<PurrRunConfiguration.Options> {
    // Where purr says it serves the page (see purr_run_web in compiler/cli/build.c).
    private static final Pattern PAGE = Pattern.compile("purr: the game is at (http://\\S+)");

    public static final class Options extends LocatableRunConfigurationOptions {
        private final StoredProperty<String> folder = string("").provideDelegate(this, "folder");
        private final StoredProperty<Boolean> web = property(false).provideDelegate(this, "web");
        private final StoredProperty<String> purr = string("").provideDelegate(this, "purr"); // Empty: the installed one

        public String getFolder() {
            return folder.getValue(this);
        }

        public void setFolder(String value) {
            folder.setValue(this, value);
        }

        public boolean isWeb() {
            return web.getValue(this);
        }

        public void setWeb(boolean value) {
            web.setValue(this, value);
        }

        public String getPurr() {
            return purr.getValue(this);
        }

        public void setPurr(String value) {
            purr.setValue(this, value);
        }
    }

    PurrRunConfiguration(@NotNull Project project, @NotNull ConfigurationFactory factory) {
        super(project, factory);
    }

    @Override
    protected @NotNull Options getOptions() {
        return (Options) super.getOptions();
    }

    @Override
    public @Nullable String suggestedName() {
        final String folder = getOptions().getFolder();
        if (folder == null || folder.isEmpty()) return null;
        final Path name = Path.of(folder).getFileName();
        return (name != null ? name.toString() : folder) + (getOptions().isWeb() ? " (web)" : "");
    }

    @Override
    public void checkConfiguration() throws RuntimeConfigurationError {
        final String folder = getOptions().getFolder();
        if (folder == null || folder.isEmpty()) throw new RuntimeConfigurationError("Choose the game's folder");
        if (!Files.isDirectory(Path.of(folder))) throw new RuntimeConfigurationError("There's no folder " + folder);
        final String purr = getOptions().getPurr();
        if (purr != null && !purr.isEmpty() && !Files.isRegularFile(Path.of(purr))) {
            throw new RuntimeConfigurationError("There's no purr at " + purr);
        }
    }

    @Override
    public @NotNull SettingsEditor<? extends RunConfiguration> getConfigurationEditor() {
        return new Editor(getProject());
    }

    @Override
    public @NotNull RunProfileState getState(@NotNull Executor executor, @NotNull ExecutionEnvironment environment) {
        return new CommandLineState(environment) {
            @Override
            protected @NotNull ProcessHandler startProcess() throws ExecutionException {
                return start();
            }
        };
    }

    private @NotNull ProcessHandler start() throws ExecutionException {
        final Options options = getOptions();
        final String setting = options.getPurr();
        final Path purr = setting != null && !setting.isEmpty() ? Path.of(setting) : Purr.installed("purr");
        if (purr == null) {
            throw new ExecutionException("Couldn't find purr. Install it (" + Purr.INSTALL + "), or choose it in the run configuration.");
        }
        final GeneralCommandLine command = new GeneralCommandLine(purr.toString(), "run")
            .withWorkDirectory(options.getFolder())
            .withCharset(StandardCharsets.UTF_8);
        if (options.isWeb()) command.addParameters("--web", "--no-open");

        final KillableColoredProcessHandler handler = new KillableColoredProcessHandler(command);
        handler.setShouldKillProcessSoftly(false); // Stop ends purr and the game it started at once
        ProcessTerminatedListener.attach(handler);
        if (options.isWeb()) {
            handler.addProcessListener(new ProcessListener() {
                private final StringBuilder seen = new StringBuilder(); // purr's output until it says where the page is
                private boolean shown;

                @Override
                public void onTextAvailable(@NotNull ProcessEvent event, @NotNull Key outputType) {
                    if (shown || outputType != ProcessOutputTypes.STDOUT) return;
                    seen.append(event.getText());
                    final Matcher page = PAGE.matcher(seen);
                    if (!page.find()) return;
                    shown = true;
                    PurrGamePage.of(getProject()).show(page.group(1));
                }
            });
        }
        return handler;
    }

    private static final class Editor extends SettingsEditor<PurrRunConfiguration> {
        private final TextFieldWithBrowseButton folder = new TextFieldWithBrowseButton();
        private final JBCheckBox web = new JBCheckBox("On the web, in the PurrLang Game tool window");
        private final TextFieldWithBrowseButton purr = new TextFieldWithBrowseButton();

        Editor(@NotNull Project project) {
            folder.addBrowseFolderListener(new TextBrowseFolderListener(FileChooserDescriptorFactory.createSingleFolderDescriptor(), project));
            purr.addBrowseFolderListener(new TextBrowseFolderListener(FileChooserDescriptorFactory.createSingleFileNoJarsDescriptor(), project));
        }

        @Override
        protected void resetEditorFrom(@NotNull PurrRunConfiguration configuration) {
            final Options options = configuration.getOptions();
            folder.setText(options.getFolder());
            web.setSelected(options.isWeb());
            purr.setText(options.getPurr());
        }

        @Override
        protected void applyEditorTo(@NotNull PurrRunConfiguration configuration) {
            final Options options = configuration.getOptions();
            options.setFolder(folder.getText().trim());
            options.setWeb(web.isSelected());
            options.setPurr(purr.getText().trim());
        }

        @Override
        protected @NotNull JComponent createEditor() {
            return FormBuilder.createFormBuilder()
                .addLabeledComponent("Game folder:", folder)
                .addComponent(web)
                .addLabeledComponent("purr:", purr)
                .addTooltip("Empty: purr from PATH, or where it's installed")
                .getPanel();
        }
    }
}
