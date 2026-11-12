#!/bin/bash
# Print the PowerPC instructions of a recompiled function from generated/.
# usage: tools/disasm.sh <hex address without 0x> [max lines]
cd "$(dirname "$0")/../generated" || exit 1
f=$(grep -l "^DEFINE_REX_FUNC(sub_$1)" Fable2_recomp.*.cpp | head -1)
[ -z "$f" ] && { echo "sub_$1 not found"; exit 1; }
awk "/^DEFINE_REX_FUNC\(sub_$1\)/,/^}/" "$f" | grep -E "^[[:space:]]*//|^loc_" |
  sed -E 's#^[[:space:]]*// ##' | head -${2:-60} | tr '\n' ';' | fold -w 230
echo
