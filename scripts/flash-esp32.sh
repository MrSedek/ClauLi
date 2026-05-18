#!/usr/bin/env bash
# Flash ClauLi firmware to esp32. Usage: flash-esp32.sh [serial-port]
exec "$(dirname "$0")/flash.sh" esp32 "$@"
