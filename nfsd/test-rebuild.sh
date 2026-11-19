#!/bin/bash
# test-rebuild.sh - restart walk must stay linear in the export size
# (PLAN_handle_rebuild_fix.md). Own high port; kills only its own PIDs.
HERE="$(cd "$(dirname "$0")" && pwd)"
python3 -I "$HERE/test-rebuild.py" "$HERE/gnfsd" "${1:-12095}"
