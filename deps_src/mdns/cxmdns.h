#ifndef _CX_MDNS_H
#define _CX_MDNS_H
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

// DNS-SD meta-discovery (_services._dns-sd._udp.local.) over mjansson/mdns.
//
// Origin: CrealityOfficial/CrealityPrint v7.1.1 src/slic3r/GUI/print_manage/utils/cxmdns.h
// (AGPL-3.0), as vendored by OrcaSlicer #13752 (d6cffbba26, moved to deps_src/ in d643701529).
// EdgeSlicer changes (AGPL-3.0): a hard overall deadline instead of "5 s after the last reply",
// periodic re-sends, a cancel flag, IPv4-only sender addresses, no console/signal handlers, no
// printf, no function-static buffers, and the packet parser split out so it can be unit-tested
// with canned bytes. See NOTICE.md.
namespace cxnet
{
	struct machine_info
	{
		std::string machineIp; // dotted-quad IPv4 of the responder
		std::string answer;    // the PTR target, e.g. "_Creality-543324280CDB19._udp.local."
	};

	// Parse one DNS-SD reply datagram. Appends {from_ip, name} for every PTR answer to the
	// _services._dns-sd._udp.local. question whose name contains any of `prefix`. Never reads
	// outside [buffer, buffer + size); malformed or foreign packets add nothing.
	void parse_discovery_reply(const void* buffer, size_t size, const std::string& from_ip,
	                           const std::vector<std::string>& prefix, std::vector<machine_info>& out);

	// Send the meta-query on every usable interface (one socket per interface address, so several
	// NICs are covered), re-send it every second, and collect matching answers until `timeout_ms`
	// has passed or *cancel turns true. Blocks the calling thread: call it off the UI thread.
	// Concurrent calls are serialised.
	std::vector<machine_info> syncDiscoveryService(const std::vector<std::string>& prefix,
	                                               int timeout_ms = 4000,
	                                               const std::atomic<bool>* cancel = nullptr);
}
#endif
