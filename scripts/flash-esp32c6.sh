#!/usr/bin/env bash
# Flash ClauLi firmware to esp32c6. Usage: flash-esp32c6.sh [serial-port]
exec "$(dirname "$0")/flash.sh" esp32c6 "$@"
