# toolchain/env.sh - put the win32/dos cross toolchains on the shell.
#
#   source toolchain/env.sh
#   make -f make/Makefile.mgw          # win32 (llvm-mingw)
#   make -f make/Makefile.dos          # dos serial (Open Watcom)
#   make -f make/Makefile.dtcp         # dos TCP (Open Watcom + Watt-32)
#
# Sets WATCOM / WATT (the Makefiles read these) and prepends the
# llvm-mingw and Open Watcom bin dirs to PATH. Payload lives under
# toolchain/ (gitignored); run toolchain/setup.sh once to populate it.
# See doc/toolchain.md.

_tc="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

export WATCOM="$_tc/ow"
export WATT="$_tc/watt32"
export INCLUDE="$WATCOM/h"
export PATH="$_tc/llvm-mingw/bin:$WATCOM/binl64:$PATH"

unset _tc
