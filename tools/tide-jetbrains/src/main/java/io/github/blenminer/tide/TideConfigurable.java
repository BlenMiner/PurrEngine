package io.github.blenminer.tide;

import com.intellij.openapi.fileChooser.FileChooserDescriptorFactory;
import com.intellij.openapi.options.Configurable;
import com.intellij.openapi.ui.TextBrowseFolderListener;
import com.intellij.openapi.ui.TextFieldWithBrowseButton;
import com.intellij.util.ui.FormBuilder;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.JComponent;
import javax.swing.JPanel;

// Settings > Languages & Frameworks > Tide: the language server to run.
public final class TideConfigurable implements Configurable {
    private @Nullable TextFieldWithBrowseButton tidels;

    @Override
    public @NotNull String getDisplayName() {
        return "Tide";
    }

    @Override
    public @NotNull JComponent createComponent() {
        final TextFieldWithBrowseButton field = new TextFieldWithBrowseButton();
        field.addBrowseFolderListener(new TextBrowseFolderListener(FileChooserDescriptorFactory.createSingleFileNoJarsDescriptor()));
        tidels = field;
        return FormBuilder.createFormBuilder()
            .addLabeledComponent("Language server (tidels):", field)
            .addTooltip("Empty: the project's build/tools/tidels in Tide's own repo, once you trust the project; "
                        + "else tidels from PATH, or where tide is installed")
            .addComponentFillVertically(new JPanel(), 0)
            .getPanel();
    }

    @Override
    public boolean isModified() {
        return tidels != null && !tidels.getText().trim().equals(TideSettings.get().tidels());
    }

    @Override
    public void apply() {
        if (tidels == null) return;
        TideSettings.get().setTidels(tidels.getText());
        TidelsFactory.restartAll();
    }

    @Override
    public void reset() {
        if (tidels != null) tidels.setText(TideSettings.get().tidels());
    }

    @Override
    public void disposeUIResources() {
        tidels = null;
    }
}
