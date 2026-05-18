#!/usr/bin/env bash
# Flash ClauLi firmware to esp32c3. Usage: flash-esp32c3.sh [serial-port]
exec "$(dirname "$0")/flash.sh" esp32c3 "$@"
