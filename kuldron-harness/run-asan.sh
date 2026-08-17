#!/bin/bash
# The memory-safety half of the acceptance run: the same cases that put hostile
# and malformed data on the receive path, against an AddressSanitizer build.
#
# Build that tree first. -fPIC is required, not optional: ASan emits module
# constructors that need PIC relocations, and libobs-version's object is not
# position-independent by default, so the libobs link fails without it (as
# "failed to set dynamic section sizes" from ld.bfd, or an explicit
# R_X86_64_32 relocation error from lld).
#
#   SAN="-fsanitize=address -fno-omit-frame-pointer -fPIC"
#   cmake -S /src -B /out/asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#     -DENABLE_FRONTEND=OFF -DENABLE_SCRIPTING=OFF \
#     -DENABLE_QSV11=OFF -DENABLE_AJA=OFF -DENABLE_DECKLINK=OFF \
#     -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
#     -DCMAKE_C_FLAGS="$SAN" -DCMAKE_CXX_FLAGS="$SAN" \
#     -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
#     -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address"
#   cmake --build /out/asan --parallel
set -u

HARNESS=/out/harness
RUNDIR=/out/asan/rundir/RelWithDebInfo
RESULTS=$HARNESS/asan-results
rm -rf "$RESULTS"; mkdir -p "$RESULTS" "$HARNESS/asan-plugins"

for p in obs-outputs obs-x264 obs-ffmpeg image-source rtmp-services obs-filters; do
  ln -sf "$RUNDIR/lib64/obs-plugins/$p.so" "$HARNESS/asan-plugins/$p.so"
done
ln -sf libobs-opengl.so.30 "$RUNDIR/lib64/libobs-opengl.so" 2>/dev/null

gcc -std=gnu11 -Wall -Wextra -g -fsanitize=address -o "$HARNESS/obs-rtmp-driver-asan" \
  "$HARNESS/obs-rtmp-driver.c" -I/src/libobs -I/out/asan/config \
  "$RUNDIR/lib64/libobs.so.30" -Wl,-rpath,"$RUNDIR/lib64" || exit 1

export OBS_PLUGIN_BIN=$HARNESS/asan-plugins
export OBS_PLUGIN_DATA="$RUNDIR/share/obs/obs-plugins/%module%"
export LD_LIBRARY_PATH=$RUNDIR/lib64
export LIBGL_ALWAYS_SOFTWARE=1

# Mesa's software rasteriser and the encoder libraries leak on this path in ways
# that have nothing to do with the code under test, so leak detection is off and
# the run is judged on ASan's error detector: overflows, use-after-free, bad
# frees. Those are what the wire-parsing changes could plausibly introduce.
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:halt_on_error=0:log_path=$RESULTS/asan

Xvfb :98 -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
export DISPLAY=:98
sleep 2

run_case() { # name seconds policy servers...
  local name=$1 secs=$2 policy=$3; shift 3
  local pids=()
  for spec in "$@"; do
    # shellcheck disable=SC2086
    node "$HARNESS/rtmp-server.js" $spec > "$RESULTS/$name.$(echo "$spec" | grep -o 'port [0-9]*' | tr ' ' '-').jsonl" 2>&1 &
    pids+=($!)
  done
  sleep 1
  ( cd "$RUNDIR" && "$HARNESS/obs-rtmp-driver-asan" "rtmp://127.0.0.1:$4" x "$secs" "$policy" 5 ) \
    > "$RESULTS/$name.driver.log" 2>&1
  local rc=$?
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
  wait "${pids[@]}" 2>/dev/null
  echo "$name driver_rc=$rc"
}

echo "--- handoff ---"
node "$HARNESS/rtmp-server.js" --name A --port 12043 --reconnect-after 6 \
  --reconnect-url rtmp://127.0.0.1:12044/live > "$RESULTS/handoff-a.jsonl" 2>&1 & A=$!
node "$HARNESS/rtmp-server.js" --name B --port 12044 > "$RESULTS/handoff-b.jsonl" 2>&1 & B=$!
sleep 1
( cd "$RUNDIR" && "$HARNESS/obs-rtmp-driver-asan" rtmp://127.0.0.1:12043/live hkey 30 enable 5 ) \
  > "$RESULTS/handoff.driver.log" 2>&1
echo "handoff driver_rc=$?"
kill $A $B 2>/dev/null; wait $A $B 2>/dev/null

echo "--- malformed command messages ---"
node "$HARNESS/rtmp-server.js" --name A --port 12045 --garbage-after 2 \
  > "$RESULTS/garbage.jsonl" 2>&1 & A=$!
sleep 1
( cd "$RUNDIR" && "$HARNESS/obs-rtmp-driver-asan" rtmp://127.0.0.1:12045/live gkey 30 enable 5 ) \
  > "$RESULTS/garbage.driver.log" 2>&1
echo "garbage driver_rc=$?"
kill $A 2>/dev/null; wait $A 2>/dev/null

echo "--- over-long tcUrl, repeated ---"
node "$HARNESS/rtmp-server.js" --name A --port 12046 --reconnect-after 2 --reconnect-repeat \
  --reconnect-tcurl-len 60000 > "$RESULTS/long.jsonl" 2>&1 & A=$!
sleep 1
( cd "$RUNDIR" && "$HARNESS/obs-rtmp-driver-asan" rtmp://127.0.0.1:12046/live lkey 25 enable 5 ) \
  > "$RESULTS/long.driver.log" 2>&1
echo "long-tcurl driver_rc=$?"
kill $A 2>/dev/null; wait $A 2>/dev/null

echo "--- repeated same-server reconnects ---"
node "$HARNESS/rtmp-server.js" --name A --port 12047 --reconnect-after 3 --reconnect-repeat \
  > "$RESULTS/churn.jsonl" 2>&1 & A=$!
sleep 1
( cd "$RUNDIR" && "$HARNESS/obs-rtmp-driver-asan" rtmp://127.0.0.1:12047/live ckey 40 enable 5 ) \
  > "$RESULTS/churn.driver.log" 2>&1
echo "churn driver_rc=$?"
kill $A 2>/dev/null; wait $A 2>/dev/null

kill $XVFB 2>/dev/null

echo
echo "=== AddressSanitizer reports ==="
found=0
for f in "$RESULTS"/asan.*; do
  [ -e "$f" ] || continue
  found=1
  echo "--- $f"
  head -40 "$f"
done
if grep -lq "ERROR: AddressSanitizer" "$RESULTS"/*.driver.log 2>/dev/null; then
  found=1
  grep -h -A 25 "ERROR: AddressSanitizer" "$RESULTS"/*.driver.log | head -60
fi
if [ "$found" = "0" ]; then
  echo "none"
fi
[ "$found" = "0" ]
