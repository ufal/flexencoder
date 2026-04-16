#!/bin/bash
# Start flexicorp-pando-server on the Unix socket TEITOK flexicorp.php expects by default
# ($PROJECT/tmp/flexicorp-pando.sock). Run in the background under tmux/screen or systemd.
#
# Usage: ./start-daemon.sh /path/to/teitok/project
#    or: FLEXICORP_PANDO_SERVER=/path/to/flexicorp-pando-server ./start-daemon.sh /path/to/project
#
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="${1:?Usage: $0 /path/to/teitok-or-corpus-project-root}"
PROJECT="$(cd "$PROJECT" && pwd)"
SOCKET="${FLEXICORP_PANDO_SOCKET:-$PROJECT/tmp/flexicorp-pando.sock}"
mkdir -p "$(dirname "$SOCKET")"

SERVER="${FLEXICORP_PANDO_SERVER:-}"
if [[ -z "$SERVER" || ! -x "$SERVER" ]]; then
	if [[ -x "$SCRIPT_DIR/build/flexicorp-pando-server" ]]; then
		SERVER="$SCRIPT_DIR/build/flexicorp-pando-server"
	elif command -v flexicorp-pando-server >/dev/null 2>&1; then
		SERVER="$(command -v flexicorp-pando-server)"
	else
		echo "flexicorp-pando-server not found. Build with cmake in flexicorp_pando/build or set FLEXICORP_PANDO_SERVER." >&2
		exit 1
	fi
fi

echo "Socket: $SOCKET"
echo "Corpus roots: unrestricted (omit --corpus-root; TEITOK sends full path to pando/)"
echo "Starting: $SERVER --socket $SOCKET"
exec "$SERVER" --socket "$SOCKET"
