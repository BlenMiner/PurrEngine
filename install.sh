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

# The newest release of the channel (the list is newest first). Nightly takes
# stable releases too, when they're newer.
curl -fsSL "https://api.github.com/repos/$repo/releases?per_page=30" -o "$work/releases.json"
tag="$(python3 - "$work/releases.json" "$channel" <<'EOF'
import json, sys
releases = json.load(open(sys.argv[1]))
for r in releases:
    if r["draft"] or (sys.argv[2] == "stable" and r["prerelease"]):
        continue
    print(r["tag_name"])
    break
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
