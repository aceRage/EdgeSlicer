// DNS-SD meta-discovery over mjansson/mdns.
//
// Origin: CrealityOfficial/CrealityPrint v7.1.1 src/slic3r/GUI/print_manage/utils/cxmdns.cpp
// (AGPL-3.0), as vendored by OrcaSlicer #13752 (d6cffbba26 / d643701529). Adapted for EdgeSlicer
// (AGPL-3.0); what changed and why is listed in cxmdns.h and NOTICE.md.
#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#endif

#include "cxmdns.h"
#include "mdns.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#ifndef _WIN32
#include <sys/select.h>
#endif

namespace cxnet
{
namespace
{
	// _services._dns-sd._udp.local. as a DNS name, the name the answers must carry.
	const uint8_t kServicesName[] = {
		0x09, '_', 's', 'e', 'r', 'v', 'i', 'c', 'e', 's',
		0x07, '_', 'd', 'n', 's', '-', 's', 'd',
		0x04, '_', 'u', 'd', 'p',
		0x05, 'l', 'o', 'c', 'a', 'l', 0x00 };

	// mdns.c keeps interface state in globals (service_address_ipv4, ...): one scan at a time.
	std::mutex& scan_mutex()
	{
		static std::mutex m;
		return m;
	}

	std::string dotted(const uint8_t* b)
	{
		return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." + std::to_string(b[3]);
	}

	// The sender as a dotted quad; "" for a native IPv6 sender (a print_host wants the IPv4 one,
	// which the same printer also answers from).
	std::string ipv4_of(const struct sockaddr_storage& from)
	{
		if (from.ss_family == AF_INET) {
			const auto* a = reinterpret_cast<const struct sockaddr_in*>(&from);
			return dotted(reinterpret_cast<const uint8_t*>(&a->sin_addr));
		}
		if (from.ss_family == AF_INET6) {
			// An IPv4-mapped IPv6 sender (::ffff:a.b.c.d) is still an IPv4 printer.
			const auto*    a = reinterpret_cast<const struct sockaddr_in6*>(&from);
			const uint8_t* b = reinterpret_cast<const uint8_t*>(&a->sin6_addr);
			static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
			if (std::memcmp(b, mapped, sizeof(mapped)) == 0)
				return dotted(b + 12);
		}
		return {};
	}
} // namespace

	void parse_discovery_reply(const void* buffer, size_t size, const std::string& from_ip,
	                           const std::vector<std::string>& prefix, std::vector<machine_info>& out)
	{
		if (buffer == nullptr || size < 12 || from_ip.empty())
			return;
		const uint8_t* bytes = static_cast<const uint8_t*>(buffer);

		const uint16_t query_id    = mdns_ntohs(bytes + 0);
		const uint16_t flags       = mdns_ntohs(bytes + 2);
		const uint16_t questions   = mdns_ntohs(bytes + 4);
		const uint16_t answer_rrs  = mdns_ntohs(bytes + 6);
		// RFC 6762: the reply to our query carries query id 0 and the QR (response) bit. Upstream
		// also demanded the AA bit (flags == 0x8400); only QR is required here.
		if (query_id != 0 || (flags & 0x8000) == 0)
			return;

		size_t offset = 12;
		// Questions, if echoed (legacy-unicast replies echo ours; some responders send none).
		for (uint16_t i = 0; i < questions; ++i) {
			if (!mdns_string_skip(buffer, size, &offset))
				return;
			if (offset + 4 > size)
				return;
			offset += 4; // type, class
		}

		for (uint16_t i = 0; i < answer_rrs; ++i) {
			if (offset >= size)
				return;
			size_t       name_offset   = offset;
			size_t       verify_offset = 0;
			const int    is_answer     = mdns_string_equal(buffer, size, &name_offset, kServicesName,
			                                               sizeof(kServicesName), &verify_offset);
			if (is_answer)
				offset = name_offset;
			else if (!mdns_string_skip(buffer, size, &offset))
				return;
			if (offset + 10 > size)
				return;
			const uint16_t rtype  = mdns_ntohs(bytes + offset);
			const uint16_t rclass = mdns_ntohs(bytes + offset + 2);
			const uint16_t length = mdns_ntohs(bytes + offset + 8);
			offset += 10;
			if (length > size - offset)
				return;
			if (is_answer && rtype == MDNS_RECORDTYPE_PTR && (rclass & 0x7FFF) == MDNS_CLASS_IN) {
				char          namebuf[256];
				mdns_string_t name = mdns_record_parse_ptr(buffer, size, offset, length, namebuf, sizeof(namebuf));
				if (name.str != nullptr && name.length > 0) {
					const std::string answer_name(name.str, name.length);
					bool match = false;
					for (const std::string& p : prefix)
						if (!p.empty() && answer_name.find(p) != std::string::npos)
							match = true;
					if (match)
						out.push_back({ from_ip, answer_name });
				}
			}
			// Upstream never stepped over the record data, so a reply with more than one answer
			// was read from the wrong place after the first.
			offset += length;
		}
	}

	std::vector<machine_info> syncDiscoveryService(const std::vector<std::string>& prefix, int timeout_ms,
	                                               const std::atomic<bool>* cancel)
	{
		std::vector<machine_info> found;
		// Nothing is opened (no socket, no multicast join) for a scan that cannot run.
		if (timeout_ms <= 0 || (cancel != nullptr && cancel->load()))
			return found;
		std::lock_guard<std::mutex> lock(scan_mutex());

#ifdef _WIN32
		WSADATA wsa_data;
		if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
			return found;
#endif
		int sockets[32];
		const int num_sockets = open_client_sockets(sockets, int(sizeof(sockets) / sizeof(sockets[0])), 0);
		if (num_sockets <= 0) {
#ifdef _WIN32
			WSACleanup();
#endif
			return found;
		}

		using clock              = std::chrono::steady_clock;
		const auto start         = clock::now();
		const auto deadline      = start + std::chrono::milliseconds(timeout_ms);
		// Windows Firewall lets a unicast reply to a multicast query in for a few seconds after the
		// query left; re-sending every second keeps that window open for the whole scan, and gives
		// a printer that missed the first packet another chance.
		const auto resend_period = std::chrono::milliseconds(1000);
		auto       next_send     = start;

		std::vector<uint8_t> buffer(9000); // largest mDNS datagram (RFC 6762 section 17)
		while (true) {
			if (cancel != nullptr && cancel->load())
				break;
			auto now = clock::now();
			if (now >= deadline)
				break;
			if (now >= next_send) {
				for (int i = 0; i < num_sockets; ++i)
					mdns_discovery_send(sockets[i]);
				next_send = now + resend_period;
			}
			// Wake at least every 200 ms so a cancel is honoured promptly.
			auto wake = (std::min)({ deadline, next_send, now + std::chrono::milliseconds(200) });
			auto wait = std::chrono::duration_cast<std::chrono::microseconds>(wake - now).count();
			if (wait < 0)
				wait = 0;

			fd_set readfs;
			FD_ZERO(&readfs);
			int nfds = 0;
			for (int i = 0; i < num_sockets; ++i) {
				if (sockets[i] >= nfds)
					nfds = sockets[i] + 1;
				FD_SET(sockets[i], &readfs);
			}
			struct timeval tv;
			tv.tv_sec  = long(wait / 1000000);
			tv.tv_usec = long(wait % 1000000);
			const int res = select(nfds, &readfs, nullptr, nullptr, &tv);
			if (res < 0)
				break;
			if (res == 0)
				continue;
			for (int i = 0; i < num_sockets; ++i) {
				if (!FD_ISSET(sockets[i], &readfs))
					continue;
				struct sockaddr_storage from;
				std::memset(&from, 0, sizeof(from));
				socklen_t addrlen = sizeof(from);
				const mdns_ssize_t got = recvfrom(sockets[i], reinterpret_cast<char*>(buffer.data()),
				                                  (mdns_size_t) buffer.size(), 0,
				                                  reinterpret_cast<struct sockaddr*>(&from), &addrlen);
				if (got <= 0)
					continue;
				parse_discovery_reply(buffer.data(), size_t(got), ipv4_of(from), prefix, found);
			}
		}

		for (int i = 0; i < num_sockets; ++i)
			mdns_socket_close(sockets[i]);
#ifdef _WIN32
		WSACleanup();
#endif
		return found;
	}
}
