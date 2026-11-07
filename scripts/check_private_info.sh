#!/usr/bin/env bash
# Refuse to commit personal infrastructure.
#
# This tree is public. Relay hosts, the speech endpoint and its token, Wi-Fi
# credentials and a pinned address belong in esp32/devices/sdkconfig.local, which
# is git-ignored. Run this before a commit, or wire it as a pre-commit hook:
#
#     ln -sf ../../scripts/check_private_info.sh .git/hooks/pre-commit
#
# Exits 1 and prints the offending file:line when a tracked or staged file
# matches any of the markers below. Personal markers are named by pattern, not
# by value, so this script does not itself leak them.

set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)" || exit 1

# Patterns: personal domain, relay/worker suffixes, the VPS IP, the speech token
# prefix used in this project, the house SSID and the LAN prefix.
# Personal markers, by shape rather than by value: a personal domain, a relay
# suffix seen in this project, the VPS address, a 32-hex shared secret in a
# query string (`k=...`), the house SSID prefix and the LAN prefix.
PATTERNS='(blacksamurai\.(top|cn|xyz)|239818\.xyz|124\.220\.19\.238|k=[0-9a-f]{32}|Xiaomi_[0-9]+|10\.168\.[0-9]+\.)'

files=$( { git diff --cached --name-only --diff-filter=ACM; git ls-files; } | sort -u )
hits=0
for f in $files; do
    [ -f "$f" ] || continue
    case "$f" in
        esp32/devices/sdkconfig.local|*.pem|*.p12|*/.env|*/.env.*) continue ;;
        esp32/devices/sdkconfig.local.example) continue ;;
    esac
    out=$(grep -nEI "$PATTERNS" "$f" 2>/dev/null | head -3)
    if [ -n "$out" ]; then
        printf '%s\n' "private marker in $f:" "$out"
        hits=$((hits + 1))
    fi
done

if [ "$hits" -gt 0 ]; then
    printf '\n%d file(s) carry personal infrastructure. Move the value into\n' "$hits"
    printf 'esp32/devices/sdkconfig.local (git-ignored) and leave a placeholder.\n'
    exit 1
fi
exit 0
