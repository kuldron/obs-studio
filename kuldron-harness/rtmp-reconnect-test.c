/******************************************************************************
    Copyright (C) 2026 by Kuldron

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/*
 * Standalone tests for Enhanced RTMP v2 reconnect target resolution.
 *
 * Deliberately free of libobs and of CMake: this decides where a live
 * broadcast and its stream key are allowed to go, so it has to be cheap enough
 * to run on every change. Build and run it with:
 *
 *   ./run-unit-tests.sh
 *
 * which compiles it against the policy module and the two librtmp files it
 * needs, with no libobs and no CMake in the way.
 */

#include "rtmp-reconnect.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(const char *what, const char *current, const char *requested,
		  enum rtmp_reconnect_verdict want_verdict, const char *want_url)
{
	char out[RTMP_RECONNECT_MAX_URL];
	memset(out, 0xaa, sizeof(out));

	checks++;

	enum rtmp_reconnect_verdict got = rtmp_reconnect_resolve(current, requested, out, sizeof(out));

	if (got != want_verdict) {
		printf("FAIL %s\n  requested %s\n  verdict %d (%s), wanted %d (%s)\n", what,
		       requested ? requested : "(null)", (int)got, rtmp_reconnect_verdict_str(got), (int)want_verdict,
		       rtmp_reconnect_verdict_str(want_verdict));
		failures++;
		return;
	}

	if (got == RTMP_RECONNECT_ACCEPT && want_url && strcmp(out, want_url) != 0) {
		printf("FAIL %s\n  resolved to '%s', wanted '%s'\n", what, out, want_url);
		failures++;
	}
}

int main(void)
{
	const char *cur = "rtmp://ingest.example.com/live";
	const char *cur_tls = "rtmps://ingest.example.com/live";

	/* --- an ordinary move ----------------------------------------------- */
	check("a sibling host", cur, "rtmp://edge1.example.com/live", RTMP_RECONNECT_ACCEPT,
	      "rtmp://edge1.example.com/live");
	check("an unrelated host", cur, "rtmp://elsewhere.test/live", RTMP_RECONNECT_ACCEPT,
	      "rtmp://elsewhere.test/live");
	check("a new application", cur, "rtmp://ingest.example.com/live2", RTMP_RECONNECT_ACCEPT,
	      "rtmp://ingest.example.com/live2");
	check("a new port", cur, "rtmp://ingest.example.com:1936/live", RTMP_RECONNECT_ACCEPT,
	      "rtmp://ingest.example.com:1936/live");
	/* The spec's own second example for tcUrl is a loopback address. */
	check("loopback, as the spec itself exemplifies", cur, "rtmp://127.0.0.1/realtimeapp", RTMP_RECONNECT_ACCEPT,
	      "rtmp://127.0.0.1/realtimeapp");

	/* --- TLS is a ratchet ------------------------------------------------ */
	check("rtmps must not become rtmp", cur_tls, "rtmp://ingest.example.com/live",
	      RTMP_RECONNECT_REJECT_DOWNGRADE, NULL);
	check("nor for a different host", cur_tls, "rtmp://edge1.example.com/live", RTMP_RECONNECT_REJECT_DOWNGRADE,
	      NULL);
	check("rtmp may become rtmps", cur, "rtmps://ingest.example.com/live", RTMP_RECONNECT_ACCEPT,
	      "rtmps://ingest.example.com/live");
	check("rtmps stays rtmps", cur_tls, "rtmps://edge1.example.com/live", RTMP_RECONNECT_ACCEPT,
	      "rtmps://edge1.example.com/live");

	/* --- only RTMP and RTMPS --------------------------------------------- */
	check("rtmpt is refused", cur, "rtmpt://ingest.example.com/live", RTMP_RECONNECT_REJECT_SCHEME, NULL);
	check("rtmpe is refused", cur, "rtmpe://ingest.example.com/live", RTMP_RECONNECT_REJECT_SCHEME, NULL);
	/* RTMP_ParseURL warns about an unknown scheme and then parses the host
	 * anyway, leaving the protocol at its RTMP default, so "http://" comes
	 * back looking like plain RTMP. The scheme is decided independently. */
	check("http is refused despite ParseURL's fallthrough", cur, "http://ingest.example.com/live",
	      RTMP_RECONNECT_REJECT_SCHEME, NULL);
	check("https is refused", cur, "https://ingest.example.com/live", RTMP_RECONNECT_REJECT_SCHEME, NULL);
	check("file is refused", cur, "file:///etc/passwd", RTMP_RECONNECT_REJECT_SCHEME, NULL);

	/* --- relative references from the spec's own table -------------------- */
	check("//host/app takes the current scheme", cur, "//edge1.example.com/live2", RTMP_RECONNECT_ACCEPT,
	      "rtmp://edge1.example.com/live2");
	check("//host/app keeps TLS", cur_tls, "//edge1.example.com/live2", RTMP_RECONNECT_ACCEPT,
	      "rtmps://edge1.example.com/live2");
	check("/app keeps scheme and authority", "rtmp://ingest.example.com:1936/live", "/live2",
	      RTMP_RECONNECT_ACCEPT, "rtmp://ingest.example.com:1936/live2");
	check("a bare relative path is not resolvable", cur, "live2", RTMP_RECONNECT_REJECT_UNRESOLVABLE, NULL);

	/* --- absent tcUrl means "the current one" ----------------------------- */
	check("no tcUrl reconnects where we are", "rtmp://edge1.example.com/live", "", RTMP_RECONNECT_ACCEPT,
	      "rtmp://edge1.example.com/live");
	check("a null tcUrl behaves the same", "rtmp://edge1.example.com/live", NULL, RTMP_RECONNECT_ACCEPT,
	      "rtmp://edge1.example.com/live");

	/* --- refused up front -------------------------------------------------- */
	/* Only what the resolver still owns: control/space/non-ASCII bytes it will
	 * not hand to the URL parser, an over-long URL, and a current URL that is
	 * not itself a usable rtmp URL. Host reachability and host shape are the
	 * connect's job now -- an unreachable or ill-formed host is resolved,
	 * followed, and fails the handoff, leaving the current connection up. */
	check("a space in the URL", cur, "rtmp://edge1.example.com/li ve", RTMP_RECONNECT_REJECT_MALFORMED, NULL);
	check("a newline in the URL", cur, "rtmp://edge1.example.com/live\nx", RTMP_RECONNECT_REJECT_MALFORMED, NULL);
	check("a non-ASCII byte", cur, "rtmp://ex\xc3\xa4mple.com/live", RTMP_RECONNECT_REJECT_MALFORMED, NULL);
	check("no scheme separator", cur, "rtmp:/live", RTMP_RECONNECT_REJECT_UNRESOLVABLE, NULL);
	check("a broken current URL", "not a url", "rtmp://edge1.example.com/live", RTMP_RECONNECT_REJECT_MALFORMED,
	      NULL);
	check("a null current URL", NULL, "rtmp://edge1.example.com/live", RTMP_RECONNECT_REJECT_MALFORMED, NULL);

	{
		char huge[RTMP_RECONNECT_MAX_URL + 64];
		memset(huge, 'a', sizeof(huge) - 1);
		huge[sizeof(huge) - 1] = '\0';
		memcpy(huge, "rtmp://", 7);
		check("an over-long URL", cur, huge, RTMP_RECONNECT_REJECT_MALFORMED, NULL);
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
