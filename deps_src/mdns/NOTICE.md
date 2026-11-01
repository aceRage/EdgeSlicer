# mDNS / DNS-SD library

The four files in this directory implement mDNS / DNS-SD lookup and are
vendored from third-party sources:

## mdns.h, mdns.c

mDNS / DNS-SD lookup library by Mattias Jansson. Originally released to
the public domain at https://github.com/mjansson/mdns.

The exact files here were taken from CrealityOfficial/CrealityPrint
v7.1.1, which split the upstream header-only library into separate
declaration (mdns.h) and implementation (mdns.c) files.

- Source: https://github.com/mjansson/mdns
- License: Public domain (no restrictions on use)

## cxmdns.h, cxmdns.cpp

Thin C++ wrapper over mdns.{h,c} that exposes a single function:

  std::vector<machine_info> syncDiscoveryService(
      const std::vector<std::string>& prefix);

It sends a DNS-SD meta-discovery query (`_services._dns-sd._udp.local.`),
listens for ~5 seconds, and returns `{ip, service_name}` for every
service announcement whose name contains any of the given prefixes.

OrcaSlicer uses this to find Creality K-series printers on the LAN
(service-name prefix "Creality"), since K-series firmware announces
each printer under a per-device-unique service type
`_Creality-<MAC-derived-hex>._udp.local.` that no fixed-name query can
target.

- Source: CrealityOfficial/CrealityPrint v7.1.1
  `src/slic3r/GUI/print_manage/utils/cxmdns.{h,cpp}`
- License: GNU AGPL-3.0 (compatible with OrcaSlicer's AGPL-3.0; see
  top-level LICENSE.txt)
- Imported: 2026-05-19

## EdgeSlicer

Ported into EdgeSlicer (AGPL-3.0) from OrcaSlicer #13752 (merge fcfadf0e8c;
vendoring commits d6cffbba26 and d643701529) on 2026-10-29.

- mdns.h, mdns.c: unchanged from OrcaSlicer (public domain, mjansson/mdns
  via CrealityPrint v7.1.1).
- cxmdns.h, cxmdns.cpp: adapted (AGPL-3.0). A hard overall deadline replaces
  "listen until 5 s pass with no reply" (which never ends on a chatty
  network); the query is re-sent every second; a cancel flag; only IPv4
  sender addresses are reported; no console-control/signal handlers, no
  printf, no function-static buffers; each answer's record data is stepped
  over (upstream mis-read every answer after the first); and the datagram
  parser is a separate function (`parse_discovery_reply`) so it is unit-tested
  with canned bytes (tests/slic3rutils/creality_discovery_tests.cpp).
