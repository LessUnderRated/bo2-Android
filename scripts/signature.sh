#!/bin/bash
# signature.sh lift|link <mode> — fingerprint of the sources one step of a mode's conversion is made from; build_steam.sh
# keeps it in boot_cache/<mode>/.sig_lift and .sig_link. lift: what lifted.o is made of (lift, fold, the native bodies
# native_impl.sh joins to it), a change redoes the mode. link: what the link adds (runtime, shims), a change relinks
# (~1.5 min a mode).
# signature.sh check — status 1 if a mode up to boot_cache/conversion_target was made from other sources, or was begun
# with signatures and not finished (an old boot may still be there), else 0.
# Left out: Remill and the exes. build_remill.sh only builds a missing Remill and install_steam.sh accepts one exact
# Steam build, so neither changes under an existing conversion.
cd "$(dirname "$0")/.."
sig() {
  if [ "$1" = lift ]; then
    set -- tools/boot.py tools/lift_roots.py tools/lift_extra.py tools/pe_parser.py tools/ghidra_csv.py "tools/ghidra-$2" \
      "tools/lift_roots_$2.txt" scripts/fold_lift.sh scripts/fold tools/steam/native_impl.sh
  else
    set -- runtime tools/steam tools/patch_redirect.py scripts/boot_info.plist third_party/HLSLcc/include
  fi
  find "$@" -type f ! -name .DS_Store ! -path '*/__pycache__/*' -print0 2>/dev/null | sort -z | xargs -0 shasum -a 256 \
    | shasum -a 256 | cut -c1-16
}
case "${1:-}" in
  lift|link) sig "$1" "$2" ;;
  check)  # tools/signature_test.sh
    FIN=$(cat boot_cache/conversion_target 2>/dev/null) || true; case "$FIN" in sps|zms) ;; *) FIN=mps ;; esac
    for t in sps zms mps; do
      [ ! -f boot_cache/$t/.sig_lift ] || [ -f boot_cache/$t/.fini ] || exit 1
      for s in lift link; do
        [ ! -f boot_cache/$t/.sig_$s ] || [ "$(cat boot_cache/$t/.sig_$s)" = "$(sig $s $t)" ] || exit 1
      done
      [ $t = "$FIN" ] && exit 0
    done ;;
  *) echo "usage: signature.sh lift|link <mode> | check"; exit 2 ;;
esac
