#!/bin/bash -e
cd "$APPDIR"
export SSL_CERT_FILE="$APPDIR/etc/ssl/certs/ca-certificates.crt"
export SSL_CERT_DIR="$APPDIR/etc/ssl/certs"
exec "$APPDIR/usr/bin/glissando" "$@"
