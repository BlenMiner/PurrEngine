#!/bin/sh
# Installs purr for this user, on Linux or macOS (Apple Silicon):
#
#     curl -fsSL https://raw.githubusercontent.com/BlenMiner/PurrEngine/release/install.sh | sh
#
# Nightly versions instead: ... | PURR_CHANNEL=nightly sh
# purr goes in ~/.purr, and its bin folder on PATH through the shell's profile.
# No root needed. Once installed, `purr upgrade` keeps it up to date.
set -eu

repo="BlenMiner/PurrEngine"
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) package="purr-linux-x64.tar.gz" ;;
    Darwin-arm64) package="purr-macos-arm64.tar.gz" ;;
    *) echo "There's no purr package for $(uname -s) on $(uname -m) yet." >&2; exit 1 ;;
esac
channel="stable"
[ "${PURR_CHANNEL:-}" = "nightly" ] && channel="nightly"
root="$HOME/.purr"
bin="$root/bin"

if [ -x "$bin/purr" ]; then
    echo "purr is already installed in $root; upgrading it."
    exec "$bin/purr" upgrade "--$channel"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The channel's highest version, not the last one published (as purr upgrade
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
    echo "There's no $channel release of purr yet." >&2
    exit 1
fi

echo "Downloading purr ${tag#v}..."
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

# purr's bin folder on PATH, for new terminals.
line="export PATH=\"\$HOME/.purr/bin:\$PATH\""
for profile in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
    if [ -f "$profile" ] && ! grep -qs '.purr/bin' "$profile"; then
        printf '\n# purr\n%s\n' "$line" >> "$profile"
    fi
done

echo "Installed purr in $root."
# PurrLang in VS Code and the editors like it.
"$bin/purr" editors || true
if [ "$(uname -s)" = "Darwin" ]; then
    echo "Native games need Apple's command-line tools: xcode-select --install"
else
    echo "Native games need your distribution's C development files, which come with gcc (build-essential on Debian and Ubuntu)."
fi
echo "Open a new terminal, go to a folder with .purr files and run: purr run"
