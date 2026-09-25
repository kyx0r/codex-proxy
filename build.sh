#!/bin/sh
set -eu

cd "$(dirname "$0")"

CC=${CC:-cc}
CFLAGS=${CFLAGS-'-O2 -Wall -Wextra'}

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcurl; then
    curl_cflags=$(pkg-config --cflags libcurl)
    curl_libs=$(pkg-config --libs libcurl)
else
    curl_cflags=
    curl_libs=-lcurl
fi

# Split compiler and flag variables into arguments without expanding wildcards.
set -f
$CC -std=c99 ${CPPFLAGS-} $CFLAGS $curl_cflags \
    codex-proxy.c cJSON.c ${LDFLAGS-} $curl_libs ${LDLIBS-} \
    -o codex-proxy
