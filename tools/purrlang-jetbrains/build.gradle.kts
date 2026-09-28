// The PurrLang plugin for JetBrains IDEs: the grammar from ../purrlang-syntax
// for highlighting, and purrls through LSP4IJ for everything else.
//
//   ./gradlew buildPlugin   the plugin, in build/distributions
//   ./gradlew runIde        an IDE with the plugin, to try it
//
// -PpluginVersion=<version> sets its version (CI passes purr's), and
// -PpluginChannel=nightly publishes to Marketplace's nightly channel.

import org.jetbrains.intellij.platform.gradle.IntelliJPlatformType

plugins {
    id("java")
    id("org.jetbrains.intellij.platform") version "2.19.0"
}

version = providers.gradleProperty("pluginVersion").getOrElse("0.0.0-dev")

repositories {
    mavenCentral()
    intellijPlatform {
        defaultRepositories()
    }
}

dependencies {
    intellijPlatform {
        // The oldest platform LSP4IJ supports, so the plugin runs from there on.
        intellijIdeaCommunity("2024.2.6")
        bundledPlugin("org.jetbrains.plugins.textmate")
        plugin("com.redhat.devtools.lsp4ij", "0.21.0")
    }
}

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(21) // Platform 2024.2's
    }
}

intellijPlatform {
    pluginConfiguration {
        ideaVersion {
            sinceBuild = "242"
            untilBuild = provider { null }
        }
    }
    // verifyPlugin checks it against the oldest IDE it supports and the newest
    // CLion, or with -PverifyIde=<folder>, against an installed IDE.
    pluginVerification {
        ides {
            val installed = providers.gradleProperty("verifyIde").orNull
            if (installed != null) {
                local(installed)
            } else {
                current()
                latest { types = listOf(IntelliJPlatformType.CLion) }
            }
        }
    }
    publishing {
        token = providers.environmentVariable("JETBRAINS_MARKETPLACE_TOKEN")
        channels = providers.gradleProperty("pluginChannel").map { listOf(it) }.orElse(listOf("default"))
    }
    buildSearchableOptions = false
    instrumentCode = false
}

tasks {
    // The test IDE opens projects without asking.
    runIde {
        jvmArgs("-Didea.trust.all.projects=true", "-Djb.consents.confirmation.enabled=false")
    }
    // The grammar goes next to the plugin's jar, where the IDE reads it (see PurrTextMateBundle).
    prepareSandbox {
        from(layout.projectDirectory.dir("../purrlang-syntax")) {
            into("purrlang/textmate")
        }
    }
}
