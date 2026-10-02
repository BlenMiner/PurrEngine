#!/bin/sh
# Installs tide for this user, on Linux or macOS (Apple Silicon):
#
#     curl -fsSL https://raw.githubusercontent.com/BlenMiner/tide-engine/release/install.sh | sh
#
# Nightly versions instead: ... | TIDE_CHANNEL=nightly sh
# tide goes in ~/.tide, and its bin folder on PATH through the shell's profile.
# No root needed. Once installed, `tide upgrade` keeps it up to date.
set -eu

repo="BlenMiner/tide-engine"
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) package="tide-linux-x64.tar.gz" ;;
    Darwin-arm64) package="tide-macos-arm64.tar.gz" ;;
    *) echo "There's no tide package for $(uname -s) on $(uname -m) yet." >&2; exit 1 ;;
esac
channel="stable"
[ "${TIDE_CHANNEL:-}" = "nightly" ] && channel="nightly"
root="$HOME/.tide"
bin="$root/bin"

if [ -x "$bin/tide" ]; then
    echo "tide is already installed in $root; upgrading it."
    # It keeps its channel, unless TIDE_CHANNEL says which.
    if [ -n "${TIDE_CHANNEL:-}" ]; then
        "$bin/tide" upgrade "--$channel"
    else
        "$bin/tide" upgrade
    fi
    # Tide in the editors installed since.
    "$bin/tide" editors || true
    exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The channel's highest version, not the last one published (as tide upgrade
# picks, compiler/cli/release.c). Nightly takes stable releases too, when
# they're newer. Stable also asks for GitHub's latest release, since nightly
# ones can push it out of the list.
curl -fsSL "https://api.github.com/repos/$repo/releases?per_page=100" -o "$work/releases.json"
echo '{}' > "$work/latest.json"
if [ "$channel" = "stable" ]; then
    curl -fsSL "https://api.github.com/repos/$repo/releases/latest" -o "$work/latest.json" ||
        echo '{}' > "$work/latest.json"
fi
tag="$(python3 - "$work/releases.json" "$work/latest.json" "$channel" <<'EOF'
import json, re, sys
releases = json.load(open(sys.argv[1])) + [json.load(open(sys.argv[2]))]

# Semantic versioning's order: a pre-release comes before its release, and
# its parts compare as numbers when they are, before words.
def order(tag):
    m = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+.*)?", tag or "")
    if not m:
        return None
    pre = m.group(4)
    parts = [(0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split(".")] if pre else []
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), pre is None, parts)

best = None
for r in releases:
    key = order(r.get("tag_name"))
    if key is None or r.get("draft") or (sys.argv[3] == "stable" and r.get("prerelease")):
        continue
    if best is None or key > best[0]:
        best = (key, r["tag_name"])
print(best[1] if best else "")
EOF
)"
if [ -z "$tag" ]; then
    echo "There's no $channel release of tide yet." >&2
    exit 1
fi

echo "Downloading tide ${tag#v}..."
base="https://github.com/$repo/releases/download/$tag"
curl -fsSL "$base/$package" -o "$work/$package"
curl -fsSL "$base/SHA256SUMS" -o "$work/SHA256SUMS"

# The download must match the checksum published with it.
expected="$(grep " $package\$" "$work/SHA256SUMS" | cut -d' ' -f1)"
if command -v sha256sum >/dev/null; then
    actual="$(sha256sum "$work/$package" | cut -d' ' -f1)"
else
    actual="$(shasum -a 256 "$work/$package" | cut -d' ' -f1)" # macOS
fi
if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
    echo "The download is damaged (its checksum doesn't match). Nothing was installed." >&2
    exit 1
fi

mkdir -p "$root"
tar -xzf "$work/$package" -C "$root"
printf '%s' "$channel" > "$root/channel"

# tide's bin folder on PATH, for new terminals: in each shell profile there is,
# and in the login shell's own, made when it has none (a new Mac's zsh has no
# ~/.zshrc). macOS's bash reads ~/.bash_profile or ~/.profile, and one made
# would hide the other, so it gets ~/.profile only when it has neither.
line="export PATH=\"\$HOME/.tide/bin:\$PATH\""
case "$(basename "${SHELL:-sh}")" in
    zsh) touch "$HOME/.zshrc" ;;
    bash)
        if [ "$(uname -s)" != "Darwin" ]; then
            touch "$HOME/.bashrc"
        elif [ ! -f "$HOME/.bash_profile" ] && [ ! -f "$HOME/.bash_login" ] && [ ! -f "$HOME/.profile" ]; then
            touch "$HOME/.profile"
        fi
        ;;
esac
if [ "$(basename "${SHELL:-sh}")" = "fish" ] || [ -d "$HOME/.config/fish" ]; then
    mkdir -p "$HOME/.config/fish/conf.d"
    printf '# tide\ncontains -- "$HOME/.tide/bin" $PATH; or set -gx PATH "$HOME/.tide/bin" $PATH\n' \
        > "$HOME/.config/fish/conf.d/tide.fish"
fi
for profile in "$HOME/.bashrc" "$HOME/.bash_profile" "$HOME/.bash_login" "$HOME/.zshrc" "$HOME/.profile"; do
    if [ -f "$profile" ] && ! grep -qs '.tide/bin' "$profile"; then
        printf '\n# tide\n%s\n' "$line" >> "$profile"
    fi
done

echo "Installed tide in $root."
# Tide in VS Code and the editors like it.
"$bin/tide" editors || true
if [ "$(uname -s)" = "Darwin" ]; then
    echo "Native games need Apple's command-line tools: xcode-select --install"
else
    echo "Native games need your distribution's C development files, which come with gcc (build-essential on Debian and Ubuntu)."
fi
echo "Open a new terminal, go to a folder with .tide files and run: tide run"
