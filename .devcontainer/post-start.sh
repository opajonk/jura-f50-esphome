#!/usr/bin/env bash
# Reports what is still missing for the live loop; never fails the container start.
cd "$(dirname "$0")/.."
token="$HOME/.config/jura-loop/home-assistant-token"

echo "jura-f50-esphome dev container"
if [ ! -s "$token" ]; then
  echo "  - No Home Assistant token: save a long-lived token on the host in"
  echo "    ~/.config/jura-loop/home-assistant-token (mode 600) and rebuild or reopen."
elif [ -n "$(find "$token" -perm /077)" ]; then
  echo "  - $token is readable by group/others; run: chmod 600 on the host file."
fi
if [ -z "${JURA_HOME_ASSISTANT:-}" ]; then
  echo "  - JURA_HOME_ASSISTANT is unset: export it on the host before opening the"
  echo "    container, or pass --home-assistant URL to tools/jura_loop.py."
fi
if ! gh auth status >/dev/null 2>&1; then
  echo "  - GitHub CLI is not logged in: run 'gh auth login && gh auth setup-git' to push."
fi
echo "  Local compile check: .venv/bin/esphome compile tests/esphome/jura-test.yaml"
echo "  Live loop: see README.md, 'Local edit, deploy, and test loop'."
exit 0
