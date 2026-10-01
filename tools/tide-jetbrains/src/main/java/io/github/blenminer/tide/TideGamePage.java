package io.github.blenminer.tide;

import com.intellij.icons.AllIcons;
import com.intellij.ide.BrowserUtil;
import com.intellij.openapi.Disposable;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.components.Service;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.project.DumbAwareAction;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.Disposer;
import com.intellij.openapi.wm.ToolWindow;
import com.intellij.openapi.wm.ToolWindowFactory;
import com.intellij.openapi.wm.ToolWindowManager;
import com.intellij.ui.content.Content;
import com.intellij.ui.content.ContentFactory;
import com.intellij.ui.jcef.JBCefApp;
import com.intellij.ui.jcef.JBCefBrowser;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.util.List;

// The page of a game run on the web, in the Tide Game tool window, which
// shows up with the first one. In an IDE without JCEF, it opens in the
// system's browser.
@Service(Service.Level.PROJECT)
public final class TideGamePage implements Disposable {
    static final String WINDOW = "Tide Game";

    private final Project project;
    private @Nullable JBCefBrowser browser;
    private @Nullable String pending; // To load once the browser is there

    public TideGamePage(@NotNull Project project) {
        this.project = project;
    }

    static @NotNull TideGamePage of(@NotNull Project project) {
        return project.getService(TideGamePage.class);
    }

    // Shows the page at `url`, in the last one's place.
    void show(@NotNull String url) {
        ApplicationManager.getApplication().invokeLater(() -> {
            if (project.isDisposed()) return;
            final ToolWindow window = ToolWindowManager.getInstance(project).getToolWindow(WINDOW);
            if (window == null || !JBCefApp.isSupported()) {
                BrowserUtil.browse(url);
                return;
            }
            pending = url;
            window.setAvailable(true);
            window.activate(this::load); // The game gets the keys
        });
    }

    private void attach(@NotNull ToolWindow window) {
        if (browser != null || !JBCefApp.isSupported()) return;
        final JBCefBrowser page = JBCefBrowser.createBuilder().build();
        Disposer.register(this, page);
        browser = page;
        final Content content = ContentFactory.getInstance().createContent(page.getComponent(), null, false);
        window.getContentManager().addContent(content);
        // What each reload did shows in the page's console.
        window.setTitleActions(List.of(DumbAwareAction.create("Open DevTools", AllIcons.Actions.StartDebugger, e -> page.openDevtools())));
        load();
    }

    private void load() {
        if (browser == null || pending == null) return;
        browser.loadURL(pending);
        pending = null;
    }

    @Override
    public void dispose() {}

    public static final class Window implements ToolWindowFactory, DumbAware {
        @Override
        public void createToolWindowContent(@NotNull Project project, @NotNull ToolWindow window) {
            of(project).attach(window);
        }

        @Override
        public boolean shouldBeAvailable(@NotNull Project project) {
            return false; // Until a game runs on the web
        }
    }
}
