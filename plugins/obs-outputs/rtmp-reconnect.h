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

/* Target resolution for Enhanced RTMP v2 Reconnect Request. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest redirect target accepted, including the terminating NUL. Anything
 * longer is refused rather than truncated. */
#define RTMP_RECONNECT_MAX_URL 1024

enum rtmp_reconnect_verdict {
	RTMP_RECONNECT_ACCEPT = 0,
	/* A relative reference that is not "//authority/..." or "/...". */
	RTMP_RECONNECT_REJECT_UNRESOLVABLE,
	/* Unparseable, over-long, or containing characters that do not belong. */
	RTMP_RECONNECT_REJECT_MALFORMED,
	/* Not an RTMP or RTMPS URL. */
	RTMP_RECONNECT_REJECT_SCHEME,
	/* RTMPS to RTMP. */
	RTMP_RECONNECT_REJECT_DOWNGRADE,
};

/* Stable, log-safe description of a verdict. Never NULL. */
const char *rtmp_reconnect_verdict_str(enum rtmp_reconnect_verdict verdict);

/*
 * Resolves `requested`, the tcUrl from the server's Info Object, against
 * `current_url`, the connection the request arrived on. `requested` may be
 * absolute, "//host/app", "/app", or NULL/empty for "the current tcUrl".
 *
 * Writes the resolved absolute URL to `out` on ACCEPT, and leaves `out`
 * untouched on every other verdict. The host is not checked; only the scheme
 * and a drop from RTMPS to RTMP are refused.
 */
enum rtmp_reconnect_verdict rtmp_reconnect_resolve(const char *current_url, const char *requested, char *out,
						   size_t out_size);

#ifdef __cplusplus
}
#endif
