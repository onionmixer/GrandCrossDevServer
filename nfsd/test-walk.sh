#!/bin/bash
# test-walk.sh - gnfsd keeps answering while the restart walk runs, and
# requests only the walk can resolve are answered on retransmit
# (PLAN_handle_rebuild_fix.md stage 2). Own port; kills only its PIDs.
HERE="$(cd "$(dirname "$0")" && pwd)"
python3 -I "$HERE/test-walk.py" "$HERE/gnfsd" "${1:-12097}"
