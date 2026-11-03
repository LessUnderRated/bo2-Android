#!/bin/zsh
# native_impl.sh sps|mps|zms : relève à part les corps Steam que le runtime remplace par du natif et les renomme
# _sub_<va>_impl (leur repli : mode 0/2 et hors voie rapide), joints à lifted.o.
# À relancer après toute reconstruction de lifted.o.
set -e
cd "$(dirname $0)/../.."
T=$1; W=boot_cache/$T/natif; L=/opt/homebrew/opt/llvm@17/bin; mkdir -p $W; O=boot_cache/$T/lifted.o
case $T in
  sps) TOUS=(47b080 47b4e0 47ba60 47c110) ;;
  mps) TOUS=(59a120 459800 459ce0 45a150 47ab10 47af70 47b4f0 47bba0) ;;
  zms) TOUS=(59b6c0 4584c0 47acf0 47b150 47b6d0 47bd80) ;;
  *) echo "usage: native_impl.sh sps|mps|zms"; exit 1 ;;
esac
VAS=()
for va in $TOUS; do $L/llvm-nm --defined-only -j $O | grep -qx _sub_${va}_impl || VAS+=($va); done
[ ${#VAS} -eq 0 ] && { echo "deja dans lifted.o"; exit 0; }
BO2_TARGET=$T BOOT_WORKDIR=boot_cache/$T BO2_INLINE_FLAGS=1 BO2_INLINE_TRIV=1 BO2_INLINE_READS=1 BO2_INLINE_WRITES=1 python3 -c "
import sys; sys.path.insert(0,'tools'); import lift_extra
lift_extra.WORKDIR='$W'; v='${VAS[*]}'.split(); assert len(lift_extra.lift(v))==len(v)"
for va in $VAS; do
  $L/clang -arch arm64 -c -O2 -x ir -o $W/$va.o $W/func_$va.bc.fixed
  $L/llvm-objcopy --redefine-sym _sub_$va=_sub_${va}_impl $W/$va.o $W/${va}_impl.o
done
$L/clang -arch arm64 -r -nostdlib -o $O.new $O $(for va in $VAS; do echo $W/${va}_impl.o; done)
mv $O.new $O
echo "impl joints a $O : $VAS"
