package io.github.blenminer.tide;

import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.components.PersistentStateComponent;
import com.intellij.openapi.components.RoamingType;
import com.intellij.openapi.components.Service;
import com.intellij.openapi.components.State;
import com.intellij.openapi.components.Storage;
import org.jetbrains.annotations.NotNull;

// Tide's settings, for every project (Settings > Languages & Frameworks >
// Tide). They name programs on this machine, so they don't roam with the
// IDE's settings.
@Service(Service.Level.APP)
@State(name = "TideSettings", storages = @Storage(value = "tide.xml", roamingType = RoamingType.DISABLED))
public final class TideSettings implements PersistentStateComponent<TideSettings.Values> {
    public static final class Values {
        public String tidels = ""; // Empty: found as TidelsFactory.find says
    }

    private Values values = new Values();

    static @NotNull TideSettings get() {
        return ApplicationManager.getApplication().getService(TideSettings.class);
    }

    @Override
    public @NotNull Values getState() {
        return values;
    }

    @Override
    public void loadState(@NotNull Values loaded) {
        values = loaded;
    }

    @NotNull String tidels() {
        return values.tidels != null ? values.tidels.trim() : "";
    }

    void setTidels(@NotNull String path) {
        values.tidels = path.trim();
    }
}
