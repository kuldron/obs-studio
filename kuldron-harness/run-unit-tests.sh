#!/bin/bash
# Unit tests for the reconnect target resolver.
#
# The resolver calls astrcmpi_n, so this links against libobs the same way
# OBS's own test/cmocka tests link OBS::libobs. NO_CRYPTO keeps librtmp's
# header from pulling in TLS context types that nothing under test touches.
#
# Defaults to the container layout. To run against any other libobs:
#   OBS_SRC=<obs checkout> OBS_LIB=<dir with libobs.so> \
#   OBS_CONFIG=<dir with obsconfig.h> ./run-unit-tests.sh
#
# Set SANITIZE=1 for the AddressSanitizer/UBSan build.
set -eu

HARNESS=$(cd "$(dirname "$0")" && pwd)
OBS_SRC=${OBS_SRC:-/src}
OBS_LIB=${OBS_LIB:-/out/build/rundir/RelWithDebInfo/lib64}
OBS_CONFIG=${OBS_CONFIG:-/out/build/rundir/RelWithDebInfo/config}
SRC=$OBS_SRC/plugins/obs-outputs
BIN=${TMPDIR:-/tmp}/rtmp-reconnect-test

# The build tree ships libobs.so.30 with no unversioned symlink, so resolve the
# library file rather than relying on -lobs.
LIBOBS=$(ls "$OBS_LIB"/libobs.so "$OBS_LIB"/libobs.so.* 2>/dev/null | head -1)
if [[ -z ${LIBOBS:-} ]]; then
  echo "no libobs found in $OBS_LIB (set OBS_LIB)" >&2
  exit 1
fi

declare -a SAN=()
if [[ ${SANITIZE:-0} != 0 ]]; then
  SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

cc -g -DNO_CRYPTO "${SAN[@]}" \
  -I"$SRC" -I"$OBS_SRC/libobs" -I"$OBS_CONFIG" \
  -o "$BIN" \
  "$HARNESS/rtmp-reconnect-test.c" \
  "$SRC/rtmp-reconnect.c" \
  "$SRC/librtmp/parseurl.c" \
  "$SRC/librtmp/log.c" \
  "$LIBOBS" -Wl,-rpath,"$OBS_LIB"

"$BIN"
