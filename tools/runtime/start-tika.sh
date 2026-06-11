#!/usr/bin/env sh
set -eu

HOST_NAME="${HOST_NAME:-127.0.0.1}"
PORT="${PORT:-9998}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

if [ "${TIKA_JAR:-}" = "" ]; then
  TIKA_JAR="$(find "$SCRIPT_DIR" -maxdepth 1 -name 'tika-server*.jar' | head -n 1)"
fi

if [ "${TIKA_JAR:-}" = "" ] || [ ! -f "$TIKA_JAR" ]; then
  echo "Tika jar was not found next to this script. Set TIKA_JAR explicitly." >&2
  exit 1
fi

if [ "${JAVA_PATH:-}" = "" ]; then
  if [ -x "$SCRIPT_DIR/../jre/bin/java" ]; then
    JAVA_PATH="$SCRIPT_DIR/../jre/bin/java"
  else
    JAVA_PATH="java"
  fi
fi

exec "$JAVA_PATH" -jar "$TIKA_JAR" --host "$HOST_NAME" --port "$PORT"
