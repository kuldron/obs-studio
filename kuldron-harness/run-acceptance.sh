#!/bin/bash
# Acceptance harness for the Enhanced RTMP v2 reconnect work.
# Runs inside the obs-build container. Every case starts its own servers and
# its own OBS, and asserts on the servers' JSONL transcripts.
set -u

HARNESS=/out/harness
RUNDIR=/out/build/rundir/RelWithDebInfo
RESULTS=$HARNESS/results
rm -rf "$RESULTS"; mkdir -p "$RESULTS"

# A filtered plugin set: the Qt-dependent frontend plugins abort in a headless
# process, and none of them are on the path under test.
export OBS_PLUGIN_BIN=$HARNESS/plugins
export OBS_PLUGIN_DATA="$RUNDIR/share/obs/obs-plugins/%module%"
export LD_LIBRARY_PATH=$RUNDIR/lib64
export LIBGL_ALWAYS_SOFTWARE=1

Xvfb :99 -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
export DISPLAY=:99
sleep 2

# A CA the client will trust, and a server certificate valid for 127.0.0.1, so
# the RTMPS cases exercise the real TLS path rather than a bypass.
CERTS=$RESULTS/certs
mkdir -p "$CERTS"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
  -subj "/CN=Kuldron RTMP Harness CA" \
  -keyout "$CERTS/ca.key" -out "$CERTS/ca.pem" >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes \
  -subj "/CN=127.0.0.1" \
  -keyout "$CERTS/server.key" -out "$CERTS/server.csr" >/dev/null 2>&1
printf "subjectAltName=IP:127.0.0.1,DNS:localhost\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n" > "$CERTS/ext.cnf"
openssl x509 -req -in "$CERTS/server.csr" -CA "$CERTS/ca.pem" -CAkey "$CERTS/ca.key" \
  -CAcreateserial -days 2 -extfile "$CERTS/ext.cnf" -out "$CERTS/server.pem" >/dev/null 2>&1
cp "$CERTS/ca.pem" /etc/pki/ca-trust/source/anchors/kuldron-rtmp-harness-ca.crt
update-ca-trust extract >/dev/null 2>&1

pass=0; fail=0
declare -a FAILED

ok()   { echo "PASS  $1"; pass=$((pass+1)); }
bad()  { echo "FAIL  $1"; echo "      $2"; fail=$((fail+1)); FAILED+=("$1"); }

drive() { # name url key seconds enable limit
  local name=$1 url=$2 key=$3 secs=$4 enable=$5 limit=$6
  ( cd "$RUNDIR" && "$HARNESS/obs-rtmp-driver" "$url" "$key" "$secs" "$enable" "$limit" ) \
    > "$RESULTS/$name.driver.log" 2>&1
  echo $?
}

start_server() { # logname args...
  local logname=$1; shift
  node "$HARNESS/rtmp-server.js" "$@" > "$RESULTS/$logname.jsonl" 2>&1 &
  echo $!
}

# grep -c always prints a count; it exits 1 on no match, which must not be
# turned into a second line of output by an `|| echo 0`.
count() { # pattern file
  local n
  n=$(grep -c -- "$1" "$2" 2>/dev/null)
  echo "${n:-0}"
}

kill_quiet() { for p in "$@"; do kill "$p" 2>/dev/null; done; wait "$@" 2>/dev/null; }

echo "=============================================================="
echo " 1. Regression: ordinary, unmodified RTMP servers"
echo "=============================================================="
# Two third-party servers, neither of which knows this feature exists. Both send
# NetStream.Publish.Start on publish -- the message a client that gates on
# `level` alone mistakes for a reconnect request, once per publish and then
# again on every keyframe as it loops. The feature is switched ON for both runs;
# a client that is correct only because it is disabled has proved nothing.
regress_check() { # label logname
  local label=$1 log=$2 n
  n=$(count "Reconnect requested" "$RESULTS/$log.driver.log")
  if [ "$n" = "0" ]; then
    ok "$label: no reconnect was triggered"
  else
    bad "$label: no reconnect was triggered" "$n triggered"
  fi
  n=$(count "Connecting to RTMP URL" "$RESULTS/$log.driver.log")
  if [ "$n" = "1" ]; then
    ok "$label: exactly one connection for the whole run"
  else
    bad "$label: exactly one connection for the whole run" "saw $n connects"
  fi
  n=$(count "Refused a reconnect request" "$RESULTS/$log.driver.log")
  if [ "$n" = "0" ]; then
    ok "$label: nothing was even parsed as a reconnect request"
  else
    bad "$label: nothing was even parsed as a reconnect request" "$n refusals"
  fi
}

ffmpeg -hide_banner -loglevel info -f flv -listen 1 -i rtmp://127.0.0.1:11940/live/regress \
  -c copy -f null - > "$RESULTS/ffmpeg-regression.log" 2>&1 &
FF=$!
sleep 1
rc=$(drive regression-ffmpeg rtmp://127.0.0.1:11940/live regress 60 enable 5)
kill_quiet $FF
if [ "$rc" = "0" ]; then
  ok "ffmpeg: 60s of streaming, the output never stopped"
else
  bad "ffmpeg: 60s of streaming, the output never stopped" "driver rc=$rc (2 = stopped early)"
fi
regress_check "ffmpeg" regression-ffmpeg

cat > "$RESULTS/mediamtx.yml" <<'YML'
logLevel: info
rtmpAddress: :11960
rtsp: no
hls: no
webrtc: no
srt: no
api: no
paths:
  all_others:
YML
( cd "$RESULTS" && mediamtx "$RESULTS/mediamtx.yml" ) > "$RESULTS/mediamtx.log" 2>&1 &
MM=$!
sleep 2
rc=$(drive regression-mediamtx rtmp://127.0.0.1:11960/live regress 60 enable 5)
kill_quiet $MM
if [ "$rc" = "0" ]; then
  ok "mediamtx: 60s of streaming, the output never stopped"
else
  bad "mediamtx: 60s of streaming, the output never stopped" "driver rc=$rc (2 = stopped early)"
fi
regress_check "mediamtx" regression-mediamtx

echo
echo "=============================================================="
echo " 2. capsEx advertisement"
echo "=============================================================="
S=$(start_server caps-off --name CAPSOFF --port 11941)
sleep 1
drive caps-off rtmp://127.0.0.1:11941/live k1 8 disabled 5 >/dev/null
kill_quiet $S
if grep -q '"event":"connect".*"capsExPresent":false' "$RESULTS/caps-off.jsonl"; then
  ok "capsEx is absent from connect when the feature is off"
else
  bad "capsEx is absent from connect when the feature is off" "$(grep '"event":"connect"' "$RESULTS/caps-off.jsonl")"
fi

S=$(start_server caps-on --name CAPSON --port 11942)
sleep 1
drive caps-on rtmp://127.0.0.1:11942/live k1 8 enable 5 >/dev/null
kill_quiet $S
if grep -q '"event":"connect".*"capsEx":1,' "$RESULTS/caps-on.jsonl"; then
  ok "capsEx = 1 (Reconnect, and only Reconnect) when the feature is on"
else
  bad "capsEx = 1 when the feature is on" "$(grep '"event":"connect"' "$RESULTS/caps-on.jsonl")"
fi

echo
echo "=============================================================="
echo " 3. The feature: a spec-correct ReconnectRequest moves the client"
echo "=============================================================="
# A redirects to B. The client follows it because the feature is on; it does not
# police the target host, and a loopback target (as here) is allowed like any other.
A=$(start_server handoff-a --name A --port 11943 --reconnect-after 6 --reconnect-url rtmp://127.0.0.1:11944/live)
B=$(start_server handoff-b --name B --port 11944)
sleep 1
rc=$(drive handoff rtmp://127.0.0.1:11943/live hkey 30 enable 5)
kill_quiet $A $B

if [ "$rc" = "0" ]; then
  ok "the output survived the handoff (never signalled a stop)"
else
  bad "the output survived the handoff" "driver rc=$rc"
fi
if grep -q '"event":"reconnect_request_sent".*"spec_shaped":true' "$RESULTS/handoff-a.jsonl"; then
  ok "server A emitted a spec-shaped ReconnectRequest"
else
  bad "server A emitted a spec-shaped ReconnectRequest" "not found"
fi
if grep -q '"event":"publish"' "$RESULTS/handoff-b.jsonl"; then
  ok "the client published to server B"
else
  bad "the client published to server B" "no publish on B"
fi
bkf=$(count '"event":"keyframe"' "$RESULTS/handoff-b.jsonl")
if [ "$bkf" -ge 1 ]; then
  ok "server B received video keyframes after the handoff ($bkf logged)"
else
  bad "server B received video keyframes after the handoff" "none"
fi
if grep -q "Reconnect complete: now publishing to rtmp://127.0.0.1:11944/live" "$RESULTS/handoff.driver.log"; then
  ok "the client logged the cut-over old -> new"
else
  bad "the client logged the cut-over" "not found"
fi

# The new server must receive its own sequence headers before any keyframe,
# otherwise what it received after the handoff is not decodable.
if grep -q '"event":"video_sequence_header".*"before_first_keyframe":true' "$RESULTS/handoff-b.jsonl"; then
  ok "server B got a video sequence header before its first keyframe"
else
  bad "server B got a video sequence header before its first keyframe" "$(grep 'sequence_header' "$RESULTS/handoff-b.jsonl" | head -3)"
fi
if grep -q '"event":"audio_sequence_header"' "$RESULTS/handoff-b.jsonl"; then
  ok "server B got an audio sequence header too"
else
  bad "server B got an audio sequence header" "none"
fi

# Overlap: B's publish must precede A's close.
b_pub=$(grep '"event":"publish"' "$RESULTS/handoff-b.jsonl" | head -1 | sed 's/.*"t":\([0-9]*\).*/\1/')
a_close=$(grep '"event":"connection_close"' "$RESULTS/handoff-a.jsonl" | head -1 | sed 's/.*"t":\([0-9]*\).*/\1/')
if [ -n "$b_pub" ] && [ -n "$a_close" ] && [ "$b_pub" -lt "$a_close" ]; then
  ok "overlap held: B was publishing $((a_close - b_pub)) ms before A was closed"
else
  bad "overlap held (B publishing before A closed)" "b_publish=$b_pub a_close=$a_close"
fi

echo
echo "=============================================================="
echo " 4. Refusals"
echo "=============================================================="

# 4a. A cross-domain redirect is followed. The client does not police which
# host a server may name -- the spec places no such restriction and the
# safeguard is that the feature is off by default -- so an unrelated target is
# accepted, not refused. (This deliberately does NOT reconnect anywhere real:
# the target is dead, so the move fails and the stream carries on, which is what
# lets the driver still exit 0.)
A=$(start_server crossdomain --name A --port 11945 --reconnect-after 5 --reconnect-url rtmp://elsewhere.example.net/live)
sleep 1
rc=$(drive crossdomain rtmp://127.0.0.1:11945/live rkey 14 enable 5)
kill_quiet $A
if grep -q "Reconnect requested: moving from" "$RESULTS/crossdomain.driver.log"; then
  ok "a cross-domain redirect is accepted (the host is not policed)"
else
  bad "a cross-domain redirect is accepted" "not accepted"
fi
if [ "$rc" = "0" ]; then
  ok "the stream carried on after the redirect target proved dead"
else
  bad "the stream carried on after the redirect target proved dead" "driver rc=$rc"
fi

# 4c. TLS downgrade. The origin is a real RTMPS connection, certificate-verified
# against the CA installed above; the target is the same host over plain RTMP.
# Same host, loopback origin, so the only rule that can refuse it is the
# no-downgrade rule.
A=$(start_server refuse-downgrade --name A --port 11947 \
      --tls-key "$CERTS/server.key" --tls-cert "$CERTS/server.pem" \
      --reconnect-after 5 --reconnect-url rtmp://127.0.0.1:11948/live)
B=$(start_server downgrade-plain --name B --port 11948)
sleep 1
rc=$(drive refuse-downgrade rtmps://127.0.0.1:11947/live dkey 16 enable 5)
kill_quiet $A $B

if grep -q '"event":"publish"' "$RESULTS/refuse-downgrade.jsonl"; then
  ok "the RTMPS origin connected and published (TLS path is real)"
else
  bad "the RTMPS origin connected and published" "no publish; TLS may have failed"
fi
if grep -q "would drop TLS" "$RESULTS/refuse-downgrade.driver.log"; then
  ok "a redirect from rtmps to rtmp is refused as a downgrade"
else
  bad "a redirect from rtmps to rtmp is refused as a downgrade" "not logged"
fi
if grep -q '"event":"connection_open"' "$RESULTS/downgrade-plain.jsonl"; then
  bad "the client never contacted the plaintext target" "it connected"
else
  ok "the client never contacted the plaintext target at all"
fi
if [ "$rc" = "0" ]; then
  ok "the RTMPS stream carried on after refusing the downgrade"
else
  bad "the RTMPS stream carried on after refusing the downgrade" "driver rc=$rc"
fi

# 4c-control: the same origin redirected to RTMPS is accepted, so 4c failed on
# the downgrade and not on anything else about the redirect.
A=$(start_server upgrade-a --name A --port 11957 \
      --tls-key "$CERTS/server.key" --tls-cert "$CERTS/server.pem" \
      --reconnect-after 5 --reconnect-url rtmps://127.0.0.1:11958/live)
B=$(start_server upgrade-b --name B --port 11958 \
      --tls-key "$CERTS/server.key" --tls-cert "$CERTS/server.pem")
sleep 1
rc=$(drive accept-tls-to-tls rtmps://127.0.0.1:11957/live ukey 25 enable 5)
kill_quiet $A $B
if grep -q '"event":"publish"' "$RESULTS/upgrade-b.jsonl"; then
  ok "control: an rtmps to rtmps redirect on the same host is followed"
else
  bad "control: an rtmps to rtmps redirect on the same host is followed" "no publish on B"
fi


# 4d. Redirect cap.
A=$(start_server refuse-cap --name A --port 11949 --reconnect-after 3 --reconnect-repeat --reconnect-url rtmp://127.0.0.1:11949/live)
sleep 1
rc=$(drive refuse-cap rtmp://127.0.0.1:11949/live ckey 40 enable 2)
kill_quiet $A
sent=$(count '"event":"reconnect_request_sent"' "$RESULTS/refuse-cap.jsonl")
followed=$(count "Reconnect requested: moving from" "$RESULTS/refuse-cap.driver.log")
if [ "$sent" -gt 2 ] && [ "$followed" = "2" ]; then
  ok "the cap held: $sent requests sent, exactly 2 followed"
else
  bad "the cap held at 2" "sent=$sent followed=$followed"
fi
if grep -q "Refusing further reconnect requests" "$RESULTS/refuse-cap.driver.log"; then
  ok "the client said why it stopped following them"
else
  bad "the client said why it stopped following them" "not logged"
fi
if [ "$rc" = "0" ]; then
  ok "the stream survived a server that asked to be redirected repeatedly"
else
  bad "the stream survived repeated redirect requests" "driver rc=$rc"
fi

echo
echo "=============================================================="
echo " 5. The defect that breaks the naive implementation"
echo "=============================================================="
# level "status" with a code that is not the reconnect code. This is the shape
# of NetStream.Publish.Start, and a client that checks only `level` reconnects
# on it.
A=$(start_server wrong-code --name A --port 11950 --reconnect-after 4 --reconnect-repeat \
      --reconnect-code NetStream.Publish.Start --reconnect-url rtmp://127.0.0.1:11951/live)
sleep 1
rc=$(drive wrong-code rtmp://127.0.0.1:11950/live wkey 25 enable 5)
kill_quiet $A
followed=$(count "Reconnect requested: moving from" "$RESULTS/wrong-code.driver.log")
if [ "$followed" = "0" ]; then
  ok "a status-level onStatus with another code does not trigger a reconnect"
else
  bad "a status-level onStatus with another code is ignored" "$followed reconnect(s) triggered"
fi

# The right code with the wrong level is refused too, and says so.
A=$(start_server wrong-level --name A --port 11952 --reconnect-after 4 --reconnect-level error \
      --reconnect-url rtmp://127.0.0.1:11953/live)
sleep 1
drive wrong-level rtmp://127.0.0.1:11952/live lkey 14 enable 5 >/dev/null
kill_quiet $A
if grep -q "Refused a reconnect request: level is 'error' and the specification requires" "$RESULTS/wrong-level.driver.log"; then
  ok "the reconnect code at level 'error' is ignored, and logged as such"
else
  bad "the reconnect code at level 'error' is ignored" "not logged"
fi

echo
echo "=============================================================="
echo " 6. A target that will not accept the connection"
echo "=============================================================="
# Nothing is listening on 11955. The handoff must fail without taking the
# stream down with it.
A=$(start_server dead-target --name A --port 11954 --reconnect-after 5 --reconnect-url rtmp://127.0.0.1:11955/live)
sleep 1
rc=$(drive dead-target rtmp://127.0.0.1:11954/live tkey 25 enable 5)
kill_quiet $A
if [ "$rc" = "0" ]; then
  ok "a handoff to a dead target leaves the stream running"
else
  bad "a handoff to a dead target leaves the stream running" "driver rc=$rc"
fi
if grep -q "failed; staying on rtmp://127.0.0.1:11954/live" "$RESULTS/dead-target.driver.log"; then
  ok "the client logged that it stayed where it was"
else
  bad "the client logged that it stayed where it was" "not logged"
fi
akf=$(count '"event":"keyframe"' "$RESULTS/dead-target.jsonl")
if [ "$akf" -ge 3 ]; then
  ok "the original server kept receiving keyframes throughout ($akf logged)"
else
  bad "the original server kept receiving keyframes" "$akf"
fi

echo
echo "=============================================================="
echo " 7. No tcUrl means 'reconnect where you are'"
echo "=============================================================="
A=$(start_server no-tcurl --name A --port 11956 --reconnect-after 6)
sleep 1
rc=$(drive no-tcurl rtmp://127.0.0.1:11956/live nkey 25 enable 5)
kill_quiet $A
pubs=$(count '"event":"publish"' "$RESULTS/no-tcurl.jsonl")
followed=$(count "Reconnect requested: moving from" "$RESULTS/no-tcurl.driver.log")
if [ "$pubs" -ge 2 ] && [ "$pubs" = "$((followed + 1))" ]; then
  ok "an absent tcUrl reconnected to the same server ($followed reconnects, $pubs publishes)"
else
  bad "an absent tcUrl reconnected to the same server" "publishes=$pubs followed=$followed"
fi
if [ "$followed" -le 5 ]; then
  ok "the same-server reconnects stayed within the limit of 5"
else
  bad "the same-server reconnects stayed within the limit" "followed=$followed"
fi
if [ "$rc" = "0" ]; then
  ok "the output survived a same-server reconnect"
else
  bad "the output survived a same-server reconnect" "driver rc=$rc"
fi

echo
echo "=============================================================="
echo " 8. Hostile and malformed input on the receive path"
echo "=============================================================="
# Malformed command messages every 2s alongside real traffic: truncated AMF, a
# string whose declared length runs past the message, random bytes, an info
# object in the wrong argument slot, and an empty message. None may do anything
# except be ignored.
A=$(start_server garbage --name A --port 11961 --garbage-after 2)
sleep 1
rc=$(drive garbage rtmp://127.0.0.1:11961/live gkey 30 enable 5)
kill_quiet $A
sent=$(count '"event":"garbage_sent"' "$RESULTS/garbage.jsonl")
followed=$(count "Reconnect requested: moving from" "$RESULTS/garbage.driver.log")
crashed=$(count "Refused a reconnect request" "$RESULTS/garbage.driver.log")
if [ "$rc" = "0" ]; then
  ok "the client survived $sent malformed command messages"
else
  bad "the client survived malformed command messages" "driver rc=$rc after $sent"
fi
if [ "$followed" = "0" ] && [ "$crashed" = "0" ]; then
  ok "none of them were treated as a reconnect request"
else
  bad "none of them were treated as a reconnect request" "followed=$followed refused=$crashed"
fi
if [ "$sent" -ge 10 ]; then
  ok "the run actually exercised the malformed cases ($sent sent)"
else
  bad "the run actually exercised the malformed cases" "only $sent sent"
fi

# An over-long tcUrl: the client must bound it against its own buffer rather
# than trusting the length on the wire.
A=$(start_server long-tcurl --name A --port 11962 --reconnect-after 2 --reconnect-repeat --reconnect-tcurl-len 40000)
sleep 1
rc=$(drive long-tcurl rtmp://127.0.0.1:11962/live lkey 25 enable 5)
kill_quiet $A
if grep -q "Refused a reconnect request: the tcUrl is 40[0-9][0-9][0-9] bytes, longer than the 1023" "$RESULTS/long-tcurl.driver.log"; then
  ok "a 40 kB tcUrl is refused on length, with the length named"
else
  bad "a 40 kB tcUrl is refused on length" "$(grep -i tcurl "$RESULTS/long-tcurl.driver.log" | head -2)"
fi
if [ "$rc" = "0" ]; then
  ok "the stream survived repeated over-long tcUrls"
else
  bad "the stream survived repeated over-long tcUrls" "driver rc=$rc"
fi

# Refusal logging is capped, so a server cannot fill the user's log file.
refusals=$(count "Refused a reconnect request" "$RESULTS/long-tcurl.driver.log")
requests=$(count '"event":"reconnect_request_sent"' "$RESULTS/long-tcurl.jsonl")
if [ "$requests" -gt 5 ] && [ "$refusals" -le 5 ]; then
  ok "refusal logging is capped: $requests requests produced $refusals log lines"
else
  bad "refusal logging is capped" "requests=$requests refusals=$refusals"
fi

kill_quiet $XVFB

echo
echo "=============================================================="
echo "  $pass passed, $fail failed"
for f in "${FAILED[@]:-}"; do [ -n "$f" ] && echo "  failed: $f"; done
echo "=============================================================="
[ "$fail" = "0" ]
