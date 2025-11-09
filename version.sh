#!/bin/sh
set -eu
VERSION='$Format:%d$'

if echo "$VERSION" | grep tag >/dev/null 2>&1; then
    VERSION=$(printf '%s' "$VERSION" | sed -n 's/.*tag: \([^,)]*\).*/\1/p')
else
    VERSION=$(git describe --tags --always --dirty 2>/dev/null || echo '')
fi

if [ ! -z "$VERSION" ]; then
	printf '%s\n' "$VERSION"
else
	printf '%s\n' "v0.0-unknown"
fi
