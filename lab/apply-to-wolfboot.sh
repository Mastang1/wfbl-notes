#!/usr/bin/env bash
# Usage: bash lab/apply-to-wolfboot.sh /path/to/wolfBoot
set -euo pipefail
if [ "$#" -ne 1 ]; then echo "Usage: $0 /path/to/wolfBoot" >&2; exit 2; fi
base="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$1" && pwd)"
tag="$(git -C "$repo" describe --tags --exact-match 2>/dev/null || true)"
if [ "$tag" != 'v2.10.0' ]; then echo "Requires checkout at exact v2.10.0 tag; got '$tag'" >&2; exit 1; fi
if [ -f "$repo/.config" ] && [ ! -f "$repo/.config.before-explorer" ]; then
  cp "$repo/.config" "$repo/.config.before-explorer"
fi
if [ ! -f "$repo/test-app/app_stm32f4.c.upstream" ]; then
  cp "$repo/test-app/app_stm32f4.c" "$repo/test-app/app_stm32f4.c.upstream"
fi
cp "$base/stm32f407-explorer.config" "$repo/.config"
cp "$base/app_stm32f4.c" "$repo/test-app/app_stm32f4.c"
python3 - "$repo/hal/stm32f4.c" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text()
if 'flash_waitstates = 3;' in s:
    assert s.count('flash_waitstates = 3;') == 1
    s=s.replace('flash_waitstates = 3;', 'flash_waitstates = 5;', 1)
    p.write_text(s)
elif 'flash_waitstates = 5;' not in s:
    raise SystemExit('Unexpected upstream HAL source; do not patch blindly')
PY
echo 'Applied lab files and STM32F407 168 MHz flash latency fix.'
echo 'Inspect: git -C '"$repo"' diff -- hal/stm32f4.c test-app/app_stm32f4.c'
