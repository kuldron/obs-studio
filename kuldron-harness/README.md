# Enhanced RTMP v2 reconnect acceptance harness

The rig used to verify `kuldron/ertmp-reconnect`. It is deliberately **not** on
that branch: it is a Kuldron acceptance rig, not something that belongs in the
OBS tree, and the feature branch is kept in a shape a human could offer upstream
without editing.

The unit tests for target resolution live here too. They were on the feature
branch, which put them in a directory (`plugins/obs-outputs/test/`) that exists
nowhere else in the OBS tree and that nothing in the build system compiled — an
orphan in the one branch that has to stay offerable. OBS's only first-party test
convention is `test/cmocka/` at the repo root, and these are standalone C rather
than cmocka, so upstream has no home for them today. They sit beside the rig
that runs them instead.

## What is here

| file | what it is |
| --- | --- |
| `Containerfile` | Fedora 44 image with OBS build dependencies, x264 from RPM Fusion, Xvfb, Node, ffmpeg, mediamtx, and the sanitiser runtimes. Nothing is installed on the host. |
| `obs-rtmp-driver.c` | Headless libobs driver. Brings up libobs with a synthetic video source and silent audio, points `rtmp_output` at a server, streams for N seconds, and reports how the output ended. Exit 0 ran to completion, 2 stopped early. |
| `rtmp-server.js` | Hand-written RTMP publish-receiving server. Logs the `capsEx` a client declares, emits spec-shaped (and deliberately malformed) `ReconnectRequest`s, optionally over TLS, and prints a JSONL transcript to assert on. |
| `run-acceptance.sh` | The 42-check acceptance suite. |
| `rtmp-reconnect-test.c` | Standalone unit tests for target resolution: the spec's relative reference forms, an absent tcUrl, scheme refusal including ParseURL's unknown-scheme fallthrough, the TLS-downgrade ratchet, and malformed and over-long input. |
| `run-unit-tests.sh` | Builds and runs the above. Links libobs for `astrcmpi_n`, the way OBS's own `test/cmocka` tests do; no CMake. Defaults to the container layout, overridable with `OBS_SRC`, `OBS_LIB` and `OBS_CONFIG` to run against any other libobs. `SANITIZE=1` for the ASan/UBSan build. |
| `run-asan.sh` | The same hostile-input cases against an AddressSanitizer build. |

The RTMP server is hand-written rather than borrowed because the tests need to
assert on the exact `capsEx` on the wire, to emit a ReconnectRequest whose
`level`, `code` and `tcUrl` can each be wrong independently, and to observe from
the outside that the client's new connection overlaps its old one.

## Running it

```sh
podman build -t obs-build:f44 -f Containerfile .

# Build OBS. /src is the checkout, /out is scratch that persists between runs.
podman run --rm -v <checkout>:/src:z -v <scratch>:/out:z -w /src localhost/obs-build:f44 sh -c '
  cmake -S . -B /out/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_FRONTEND=OFF -DENABLE_SCRIPTING=OFF \
    -DENABLE_QSV11=OFF -DENABLE_AJA=OFF -DENABLE_DECKLINK=OFF \
    -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF &&
  cmake --build /out/build --parallel'

podman run --rm --network=host -v <checkout>:/src:z -v <scratch>:/out:z \
  localhost/obs-build:f44 bash /out/harness/run-acceptance.sh
```

`plugins/obs-browser` and `plugins/obs-websocket` are required submodules that
the top-level CMake refuses to configure without. Neither is on any path under
test; stub each with a `CMakeLists.txt` containing `add_custom_target(<name>)`
and `target_disable(<name>)`, and keep them out of the index.

## What the suite covers

1. **Regression against unmodified servers.** ffmpeg's RTMP listener and
   mediamtx, 60 seconds each, with the feature *enabled*. Asserts no reconnect
   triggered, nothing even parsed as a request, and exactly one connection for
   the whole run. This is the check that catches gating on `level` alone: both
   servers send `NetStream.Publish.Start` at level `status`, and a client that
   reads that as a reconnect request loops once per GOP.
2. **capsEx.** Absent from `connect` when the feature is off; exactly `1` when on.
3. **The handoff.** A spec-shaped request moves the client, the new server gets
   its own sequence headers before its first keyframe, and the new connection is
   publishing before the old one is closed.
4. **Refusals.** Unrelated domain, TLS downgrade over a real certificate-verified
   RTMPS connection, and the per-session cap. Each has a control that accepts
   the same target under a wider policy, so a refusal cannot pass for the wrong
   reason.
5. **Wrong code, wrong level.** Both ignored, and the run proves the server was
   actually sending them.
6. **A dead target.** The handoff fails and the stream stays where it is.
7. **An absent tcUrl.** Reconnects to the same server, bounded by the limit.
8. **Hostile input.** Truncated AMF, a declared string length running past the
   message, random bytes, an info object in the wrong argument slot, an empty
   message, and a 40 kB `tcUrl`. Nothing is acted on, nothing crashes, and the
   refusal logging stays capped so a server cannot fill the user's log file.

`run-asan.sh` repeats the handoff, malformed-input, over-long-tcUrl and
reconnect-churn cases against an ASan build. Leak detection is off — mesa's
software rasteriser and the encoder libraries leak on this path regardless — so
the run is judged on ASan's error detector. Confirm the detector actually works
in the image before believing a clean run; a deliberate heap overflow under the
same `ASAN_OPTIONS` should produce a report under `log_path`.
