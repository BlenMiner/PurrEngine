package io.github.blenminer.tide;

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
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

// `tide run` on a game's folder, in the Run tool window: tide's output and the
// game's, where errors link to their place, and what's typed there goes to
// tide (r and Enter starts the game over). On the web, tide serves the page
// without opening a browser, and the Tide Game tool window shows it.
public final class TideRunConfiguration extends LocatableConfigurationBase<TideRunConfiguration.Options> {
    // Where tide says it serves the page (see tide_run_web in compiler/cli/build.c).
    private static final Pattern PAGE = Pattern.compile("tide: the game is at (http://\\S+)");

    public static final class Options extends LocatableRunConfigurationOptions {
        private final StoredProperty<String> folder = string("").provideDelegate(this, "folder");
        private final StoredProperty<Boolean> web = property(false).provideDelegate(this, "web");
        private final StoredProperty<String> tide = string("").provideDelegate(this, "tide"); // Empty: the installed one
        // Why tide run can't play the file it was made for (see TideRunProducer). Empty: it can.
        private final StoredProperty<String> problem = string("").provideDelegate(this, "problem");

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

        public String getTide() {
            return tide.getValue(this);
        }

        public void setTide(String value) {
            tide.setValue(this, value);
        }

        public String getProblem() {
            return problem.getValue(this);
        }

        public void setProblem(String value) {
            problem.setValue(this, value);
        }
    }

    TideRunConfiguration(@NotNull Project project, @NotNull ConfigurationFactory factory) {
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
        final String problem = getOptions().getProblem();
        if (problem != null && !problem.isEmpty()) throw new RuntimeConfigurationError(problem);
        final String folder = getOptions().getFolder();
        if (folder == null || folder.isEmpty()) throw new RuntimeConfigurationError("Choose the game's folder");
        if (!Files.isDirectory(Path.of(folder))) throw new RuntimeConfigurationError("There's no folder " + folder);
        final String tide = getOptions().getTide();
        if (tide != null && !tide.isEmpty() && !Files.isRegularFile(Path.of(tide))) {
            throw new RuntimeConfigurationError("There's no tide at " + tide);
        }
    }

    @Override
    public @NotNull SettingsEditor<? extends RunConfiguration> getConfigurationEditor() {
        return new Editor(getProject());
    }

    @Override
    public @NotNull RunProfileState getState(@NotNull Executor executor, @NotNull ExecutionEnvironment environment) {
        final CommandLineState state = new CommandLineState(environment) {
            @Override
            protected @NotNull ProcessHandler startProcess() throws ExecutionException {
                return start();
            }
        };
        // Errors' places link to the file.
        final String folder = getOptions().getFolder();
        try {
            if (folder != null && !folder.isEmpty()) state.addConsoleFilters(new TideErrorFilter(getProject(), Path.of(folder)));
        } catch (InvalidPathException e) {
            // checkConfiguration says so
        }
        return state;
    }

    private @NotNull ProcessHandler start() throws ExecutionException {
        final Options options = getOptions();
        final String problem = options.getProblem();
        if (problem != null && !problem.isEmpty()) throw new ExecutionException(problem);
        final String setting = options.getTide();
        final Path tide = setting != null && !setting.isEmpty() ? Path.of(setting) : Tide.installed("tide");
        if (tide == null) {
            throw new ExecutionException("Couldn't find tide. Install it (" + Tide.INSTALL + "), or choose it in the run configuration.");
        }
        final GeneralCommandLine command = new GeneralCommandLine(tide.toString(), "run")
            .withWorkDirectory(options.getFolder())
            .withCharset(StandardCharsets.UTF_8);
        if (options.isWeb()) command.addParameters("--web", "--no-open");

        final KillableColoredProcessHandler handler = new KillableColoredProcessHandler(command);
        handler.setShouldKillProcessSoftly(false); // Stop ends tide and the game it started at once
        ProcessTerminatedListener.attach(handler);
        if (options.isWeb()) {
            handler.addProcessListener(new ProcessListener() {
                private final StringBuilder seen = new StringBuilder(); // tide's output until it says where the page is
                private boolean shown;

                @Override
                public void onTextAvailable(@NotNull ProcessEvent event, @NotNull Key outputType) {
                    if (shown || outputType != ProcessOutputTypes.STDOUT) return;
                    seen.append(event.getText());
                    final Matcher page = PAGE.matcher(seen);
                    if (!page.find()) return;
                    shown = true;
                    TideGamePage.of(getProject()).show(page.group(1));
                }
            });
        }
        return handler;
    }

    private static final class Editor extends SettingsEditor<TideRunConfiguration> {
        private final TextFieldWithBrowseButton folder = new TextFieldWithBrowseButton();
        private final JBCheckBox web = new JBCheckBox("On the web, in the Tide Game tool window");
        private final TextFieldWithBrowseButton tide = new TextFieldWithBrowseButton();

        Editor(@NotNull Project project) {
            folder.addBrowseFolderListener(new TextBrowseFolderListener(FileChooserDescriptorFactory.createSingleFolderDescriptor(), project));
            tide.addBrowseFolderListener(new TextBrowseFolderListener(FileChooserDescriptorFactory.createSingleFileNoJarsDescriptor(), project));
        }

        @Override
        protected void resetEditorFrom(@NotNull TideRunConfiguration configuration) {
            final Options options = configuration.getOptions();
            folder.setText(options.getFolder());
            web.setSelected(options.isWeb());
            tide.setText(options.getTide());
        }

        @Override
        protected void applyEditorTo(@NotNull TideRunConfiguration configuration) {
            final Options options = configuration.getOptions();
            // Another folder is another game, which tide run may play.
            if (!folder.getText().trim().equals(options.getFolder())) options.setProblem("");
            options.setFolder(folder.getText().trim());
            options.setWeb(web.isSelected());
            options.setTide(tide.getText().trim());
        }

        @Override
        protected @NotNull JComponent createEditor() {
            return FormBuilder.createFormBuilder()
                .addLabeledComponent("Game folder:", folder)
                .addComponent(web)
                .addLabeledComponent("tide:", tide)
                .addTooltip("Empty: tide from PATH, or where it's installed")
                .getPanel();
        }
    }
}
