#!/usr/bin/env bash
# Flash ClauLi firmware to esp32s3. Usage: flash-esp32s3.sh [serial-port]
exec "$(dirname "$0")/flash.sh" esp32s3 "$@"
