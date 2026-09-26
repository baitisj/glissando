# freedv-backend (vendored, without RADE)

A copy of [tmiw/freedv-backend](https://github.com/tmiw/freedv-backend) at
commit f02e7e94da8d35e839d186a7a0c92c9e1b28277d (2026-09-23), carried in this
tree so the Glissando app no longer fetches the backend's `main` branch at
configure time. RADE has been removed: its build (`cmake/BuildRADE.cmake`,
which fetched rade_c and Opus/FARGAN), the RADE transmit and receive steps,
RADE text (`rade_text`, the LDPC code it used), `MinimalTxRxThread`,
`BandwidthExpandStep` (Opus OSCE) and their tests. The FreeDV Reporter
client, the socket.io/websocketpp client it used and the TLS (OpenSSL/LibreSSL)
support that existed for it have been removed as well. What is left is audio
processing (resampling, AGC, RNNoise) and PSK Reporter, UDP and CSV reporting
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

## Getting Support

Please create a GitHub issue if you find a problem with this repository.
