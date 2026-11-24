# CLAP headers

This directory contains the public CLAP API headers needed by the host from the official
[`free-audio/clap`](https://github.com/free-audio/clap) repository.

- Upstream revision: `a47f6badb49d948fd009998f28309cdab78979c9`
- API version: 1.2.10
- License: MIT; see [LICENSE](LICENSE)

Awesome Studio uses these headers to host Linux CLAP audio effects and instruments. The headers
are included in the source tree so a separate CLAP SDK installation is not
needed to build the application.

The bundled `clap/ext/latency.h` comes from the same upstream revision and
provides activation-time plug-in latency reporting for mixer compensation.
