#!/bin/bash
# Install a long-lived Claude Code OAuth token (from `claude setup-token`) into
# YT-Streamer/.env as CLAUDE_CODE_OAUTH_TOKEN, so the moderation poller's SDK
# calls stop depending on the login-Keychain credential (which goes stale when
# SSH Claude sessions rotate the file-based grant instead — the Aug 2026 401s).
#
# Paste is hidden (read -s); the token is never echoed and never appears in
# argv or shell history. The .env rewrite is atomic (temp file + mv).
set -euo pipefail

ENV_FILE="$(cd "$(dirname "$0")" && pwd)/.env"
KEY="CLAUDE_CODE_OAUTH_TOKEN"

[ -f "$ENV_FILE" ] || { echo "ERROR: $ENV_FILE not found" >&2; exit 1; }

printf 'Paste the token from `claude setup-token` (input hidden): '
IFS= read -rs TOKEN
echo

# Strip surrounding whitespace/newlines a terminal paste can add.
TOKEN="$(printf '%s' "$TOKEN" | tr -d '[:space:]')"

if [ -z "$TOKEN" ]; then
    echo "ERROR: empty input, .env unchanged" >&2
    exit 1
fi
case "$TOKEN" in
    sk-ant-oat*) ;;
    *)  echo "ERROR: that doesn't look like a setup-token (expected sk-ant-oat...); .env unchanged" >&2
        exit 1 ;;
esac

TMP="$(mktemp "${ENV_FILE}.XXXXXX")"
trap 'trash "$TMP" 2>/dev/null || true' EXIT
chmod 600 "$TMP"

# Copy everything except a previous marker-delimited token block (and any bare
# CLAUDE_CODE_OAUTH_TOKEN line added by hand), then append the new block
# (matches the file's `KEY = value` style).
MARK_BEGIN="# --- set_claude_token.sh begin ---"
MARK_END="# --- set_claude_token.sh end ---"
ACTION="added"
if grep -qE "^[[:space:]]*${KEY}[[:space:]]*=" "$ENV_FILE"; then
    ACTION="replaced"
fi
awk -v b="$MARK_BEGIN" -v e="$MARK_END" -v key="$KEY" '
    $0 == b { inblk = 1; next }
    $0 == e { inblk = 0; next }
    inblk   { next }
    $0 ~ "^[[:space:]]*" key "[[:space:]]*=" { next }
    { print }
' "$ENV_FILE" > "$TMP"
{
    echo ""
    echo "$MARK_BEGIN"
    echo "# Long-lived Claude Code OAuth token for the moderation poller's SDK calls."
    echo "# Takes priority over Keychain/.credentials.json in the CLI the SDK spawns."
    echo "# Mint a new one with: claude setup-token   then re-run ./set_claude_token.sh"
    echo "${KEY} = ${TOKEN}"
    echo "$MARK_END"
} >> "$TMP"
mv "$TMP" "$ENV_FILE"
trap - EXIT
chmod 600 "$ENV_FILE"

echo "OK: ${ACTION} ${KEY} in $ENV_FILE (now chmod 600)"
echo
echo "The daemon reads .env only at startup — restart it to pick this up:"
printf 'Restart the daemon now via ./launchagent.sh restart? [y/N] '
read -r REPLY
if [ "$REPLY" = "y" ] || [ "$REPLY" = "Y" ]; then
    "$(dirname "$ENV_FILE")/launchagent.sh" restart
else
    echo "Skipped. Run: cd $(dirname "$ENV_FILE") && ./launchagent.sh restart"
fi
