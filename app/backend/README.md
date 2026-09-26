# freedv-backend (vendored, without RADE)

A copy of [tmiw/freedv-backend](https://github.com/tmiw/freedv-backend) at
commit f02e7e94da8d35e839d186a7a0c92c9e1b28277d (2026-09-23), carried in this
tree so the Glissando app no longer fetches the backend's `main` branch at
configure time. RADE has been removed: its build (`cmake/BuildRADE.cmake`,
which fetched rade_c and Opus/FARGAN), the RADE transmit and receive steps,
RADE text (`rade_text`, the LDPC code it used), `MinimalTxRxThread`,
`BandwidthExpandStep` (Opus OSCE) and their tests. What is left is audio
processing (resampling, AGC, RNNoise) and FreeDV Reporter / PSK Reporter
support. Everything else is unchanged from upstream and keeps its BSD
2-Clause licence (`LICENSE`).

## Compiling standalone

This project can be compiled standalone by running `cmake`:

```sh
mkdir build
cd build
cmake ..
make
```

To enable unit tests, you can pass `-DBUILD_BACKEND_UNITTESTS=1` to `cmake`:

```sh
cmake -DBUILD_BACKEND_UNITTESTS=1 ..
make
ctest -V
```

TLS support (mainly for FreeDV Reporter) is enabled by default, but can be disabled (i.e. for platforms that can't do TLS) by passing `-DDISABLE_TLS_SUPPORT=1` to `cmake`.
Additionally, LibreSSL can be statically linked instead of the system's copy of OpenSSL by passing in `-DUSE_STATIC_LIBRESSL=1`.

## Getting Support

Please create a GitHub issue if you find a problem with this repository.
