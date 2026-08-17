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

#include "rtmp-reconnect.h"

#include "librtmp/rtmp.h"

#include <util/dstr.h>

#include <stdio.h>
#include <string.h>

/* RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) */
static inline bool is_scheme_char(char c, bool first)
{
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
		return true;
	if (first)
		return false;
	return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/* Rejects anything that is not printable ASCII. A tcUrl containing a control
 * character, a space, or a non-ASCII byte is not something we are prepared to
 * hand to librtmp's URL parser, so it is refused outright. */
static bool is_printable_ascii(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x21 || c > 0x7e)
			return false;
	}
	return true;
}

/* Length of the scheme in `url` if it begins with "<scheme>://", else 0. */
static size_t scheme_length(const char *url)
{
	size_t i = 0;
	while (url[i] && is_scheme_char(url[i], i == 0))
		i++;
	if (i == 0)
		return 0;
	if (url[i] != ':' || url[i + 1] != '/' || url[i + 2] != '/')
		return 0;
	return i;
}

/* The "//host:port" span of an absolute URL, excluding the leading "//" and
 * stopping at the first '/'. Returns false when the URL is not absolute. */
static bool split_absolute(const char *url, size_t *scheme_len, const char **authority, size_t *authority_len)
{
	size_t slen = scheme_length(url);
	if (slen == 0)
		return false;

	const char *auth = url + slen + 3;
	const char *slash = strchr(auth, '/');
	*scheme_len = slen;
	*authority = auth;
	*authority_len = slash ? (size_t)(slash - auth) : strlen(auth);
	return true;
}

/* ------------------------------------------------------------------------- */
/* verdicts                                                                  */

const char *rtmp_reconnect_verdict_str(enum rtmp_reconnect_verdict verdict)
{
	switch (verdict) {
	case RTMP_RECONNECT_ACCEPT:
		return "accepted";
	case RTMP_RECONNECT_REJECT_UNRESOLVABLE:
		return "the requested URL is a relative reference that cannot be resolved";
	case RTMP_RECONNECT_REJECT_MALFORMED:
		return "the requested URL is malformed or too long";
	case RTMP_RECONNECT_REJECT_SCHEME:
		return "the requested URL is not an RTMP or RTMPS URL";
	case RTMP_RECONNECT_REJECT_DOWNGRADE:
		return "the requested URL would drop TLS";
	}
	return "rejected";
}

/* ------------------------------------------------------------------------- */
/* parsing and resolution                                                    */

/*
 * Returns false if `url` is not a printable-ASCII "<scheme>://..." within the
 * length bound; otherwise sets *protocol to RTMP, RTMPS, or UNDEFINED.
 *
 * The scheme is decided here rather than left to RTMP_SetupURL, which treats an
 * unrecognized scheme as plain RTMP: "http://host" would otherwise be set up
 * and connected to as RTMP.
 */
static bool parse_url(const char *url, int *protocol)
{
	size_t len = strlen(url);
	if (len == 0 || len >= RTMP_RECONNECT_MAX_URL)
		return false;
	if (!is_printable_ascii(url, len))
		return false;

	size_t slen = scheme_length(url);
	if (slen == 0)
		return false;

	if (slen == 5 && astrcmpi_n(url, "rtmps", 5) == 0)
		*protocol = RTMP_PROTOCOL_RTMPS;
	else if (slen == 4 && astrcmpi_n(url, "rtmp", 4) == 0)
		*protocol = RTMP_PROTOCOL_RTMP;
	else
		*protocol = RTMP_PROTOCOL_UNDEFINED;
	return true;
}

static bool protocol_supported(int protocol)
{
	return protocol == RTMP_PROTOCOL_RTMP || protocol == RTMP_PROTOCOL_RTMPS;
}

static bool protocol_is_tls(int protocol)
{
	return (protocol & RTMP_FEATURE_SSL) != 0;
}

/* Builds the absolute form of `requested` per the relative-reference forms the
 * Enhanced RTMP v2 Reconnect Request table gives: an absolute URL, an
 * authority-relative "//host/app", or a path-relative "/app". Anything else is
 * ambiguous and is refused rather than guessed at. */
static enum rtmp_reconnect_verdict resolve_reference(const char *current_url, const char *requested, char *out,
						     size_t out_size)
{
	size_t requested_len = strlen(requested);
	if (requested_len == 0 || requested_len >= out_size)
		return RTMP_RECONNECT_REJECT_MALFORMED;
	if (!is_printable_ascii(requested, requested_len))
		return RTMP_RECONNECT_REJECT_MALFORMED;

	if (scheme_length(requested) > 0) {
		memcpy(out, requested, requested_len + 1);
		return RTMP_RECONNECT_ACCEPT;
	}

	size_t current_scheme_len;
	const char *current_authority;
	size_t current_authority_len;
	if (!split_absolute(current_url, &current_scheme_len, &current_authority, &current_authority_len))
		return RTMP_RECONNECT_REJECT_MALFORMED;

	int written;
	if (requested[0] == '/' && requested[1] == '/') {
		written = snprintf(out, out_size, "%.*s:%s", (int)current_scheme_len, current_url, requested);
	} else if (requested[0] == '/') {
		written = snprintf(out, out_size, "%.*s://%.*s%s", (int)current_scheme_len, current_url,
				   (int)current_authority_len, current_authority, requested);
	} else {
		return RTMP_RECONNECT_REJECT_UNRESOLVABLE;
	}

	if (written < 0 || (size_t)written >= out_size)
		return RTMP_RECONNECT_REJECT_MALFORMED;

	return RTMP_RECONNECT_ACCEPT;
}

enum rtmp_reconnect_verdict rtmp_reconnect_resolve(const char *current_url, const char *requested, char *out,
						   size_t out_size)
{
	char resolved[RTMP_RECONNECT_MAX_URL];
	int current_protocol;
	int target_protocol;

	if (!out || out_size == 0 || out_size > RTMP_RECONNECT_MAX_URL)
		return RTMP_RECONNECT_REJECT_MALFORMED;

	if (!current_url || !parse_url(current_url, &current_protocol) || !protocol_supported(current_protocol))
		return RTMP_RECONNECT_REJECT_MALFORMED;

	/* No tcUrl means "reconnect to the current one" (spec, Info Object table). */
	if (!requested || requested[0] == '\0') {
		size_t len = strlen(current_url);
		if (len >= out_size)
			return RTMP_RECONNECT_REJECT_MALFORMED;
		memcpy(out, current_url, len + 1);
		return RTMP_RECONNECT_ACCEPT;
	}

	enum rtmp_reconnect_verdict verdict = resolve_reference(current_url, requested, resolved, sizeof(resolved));
	if (verdict != RTMP_RECONNECT_ACCEPT)
		return verdict;

	if (strlen(resolved) >= out_size)
		return RTMP_RECONNECT_REJECT_MALFORMED;

	if (!parse_url(resolved, &target_protocol))
		return RTMP_RECONNECT_REJECT_MALFORMED;

	if (!protocol_supported(target_protocol))
		return RTMP_RECONNECT_REJECT_SCHEME;

	/* Once a session is authenticated by a certificate, nothing the server
	 * says can talk it back down to plaintext. Upgrading is always allowed. */
	if (!protocol_is_tls(target_protocol) && protocol_is_tls(current_protocol))
		return RTMP_RECONNECT_REJECT_DOWNGRADE;

	size_t resolved_len = strlen(resolved);
	memcpy(out, resolved, resolved_len + 1);
	return RTMP_RECONNECT_ACCEPT;
}
