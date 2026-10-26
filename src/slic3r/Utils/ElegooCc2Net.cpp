// The networking half of ElegooCc2.hpp: GET /system/info over Http, and one short-lived paho MQTT
// session per CANVAS read or print start. Nothing here is kept open between calls - the printer
// limits its clients ("too many clients"), and the CC1 path behaves the same way.

#include "ElegooCc2.hpp"
#include "Http.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

#include <boost/log/trivial.hpp>
#include <curl/curl.h>
#include <mqtt/async_client.h>

namespace Slic3r {
namespace ElegooCc2 {

namespace {

std::string url_escape(const std::string& s)
{
    std::string out;
    if (CURL* curl = curl_easy_init()) {
        if (char* e = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()))) {
            out = e;
            curl_free(e);
        }
        curl_easy_cleanup(curl);
    }
    return out;
}

class PahoTransport : public MqttTransport
{
public:
    PahoTransport(std::string host, std::string token, int connect_timeout_s)
        : m_host(std::move(host)), m_token(std::move(token)), m_connect_timeout_s(connect_timeout_s)
    {}
    ~PahoTransport() override { close(); }

    bool connect(const std::string& client_id, std::string& error) override
    {
        try {
            m_client = std::make_unique<mqtt::async_client>("tcp://" + m_host + ":" + std::to_string(MQTT_PORT), client_id);
            mqtt::connect_options opts;
            opts.set_clean_session(true);
            opts.set_keep_alive_interval(30);
            opts.set_connect_timeout(m_connect_timeout_s);
            opts.set_automatic_reconnect(false);
            opts.set_user_name(MQTT_USER);
            opts.set_password(m_token);
            m_client->start_consuming();
            auto tok = m_client->connect(opts);
            if (!tok->wait_for(std::chrono::seconds(m_connect_timeout_s + 2)) || !m_client->is_connected()) {
                error = "could not connect to the printer's MQTT port";
                close();
                return false;
            }
            return true;
        } catch (const mqtt::exception& e) {
            // 4 and 5 are MQTT's "bad user name or password" and "not authorised".
            const int rc = e.get_return_code();
            error = (rc == 4 || rc == 5) ? std::string("the printer refused the access code") :
                                           std::string("could not connect to the printer's MQTT port: ") + e.what();
        } catch (const std::exception& e) {
            error = std::string("could not connect to the printer's MQTT port: ") + e.what();
        }
        close();
        return false;
    }

    bool subscribe(const std::string& topic, std::string& error) override
    {
        try {
            if (m_client && m_client->subscribe(topic, 1)->wait_for(std::chrono::seconds(5)))
                return true;
        } catch (const std::exception& e) {
            error = std::string("subscribe failed: ") + e.what();
            return false;
        }
        error = "subscribe failed";
        return false;
    }

    bool publish(const std::string& topic, const std::string& payload, std::string& error) override
    {
        try {
            if (m_client && m_client->publish(topic, payload, 1, false)->wait_for(std::chrono::seconds(5)))
                return true;
        } catch (const std::exception& e) {
            error = std::string("publish failed: ") + e.what();
            return false;
        }
        error = "publish failed";
        return false;
    }

    bool receive(std::string& topic, std::string& payload, int timeout_ms) override
    {
        if (!m_client)
            return false;
        mqtt::const_message_ptr msg;
        try {
            if (!m_client->try_consume_message_for(&msg, std::chrono::milliseconds(timeout_ms)) || !msg)
                return false;
        } catch (const std::exception&) {
            return false;
        }
        topic   = msg->get_topic();
        payload = msg->to_string();
        return true;
    }

    void close() override
    {
        if (!m_client)
            return;
        try {
            if (m_client->is_connected())
                m_client->disconnect()->wait_for(std::chrono::seconds(2));
        } catch (...) {}
        try {
            m_client->stop_consuming();
        } catch (...) {}
        m_client.reset();
    }

private:
    std::string                         m_host;
    std::string                         m_token;
    int                                 m_connect_timeout_s;
    std::unique_ptr<mqtt::async_client> m_client;
};

} // namespace

bool fetch_serial(const std::string& address, const std::string& token, std::string& serial, std::string& error, int timeout_s)
{
    serial.clear();
    const std::string host = host_of(address);
    if (host.empty()) {
        error = "this printer has no address";
        return false;
    }
    bool ok = false;
    auto http = Http::get("http://" + host + "/system/info?X-Token=" + url_escape(token));
    http.timeout_connect(std::max(1, timeout_s)).timeout_max(std::max(2, 2 * timeout_s));
    http.header("X-Token", token);
    http.header("Accept", "application/json");
    http.on_complete([&](std::string body, unsigned) { ok = parse_system_info(body, serial, error); })
        .on_error([&](std::string body, std::string err, unsigned status) {
            if (status == 401 || status == 403)
                error = "the printer refused the access code";
            else if (!body.empty() && parse_system_info(body, serial, error))
                ok = true;
            else
                error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
        })
        .perform_sync();
    return ok;
}

CanvasQueryResult canvas_query(const std::string& address, const std::string& token_in, const CanvasQueryOptions& opts)
{
    CanvasQueryResult out;
    const std::string token = token_or_default(token_in);
    const int         timeout_s = std::max(1, opts.frame_timeout_ms / 1000);
    std::string       sn;
    if (!fetch_serial(address, token, sn, out.error, timeout_s)) {
        BOOST_LOG_TRIVIAL(info) << "CANVAS (CC2): no serial from the printer: " << out.error;
        return out;
    }
    PahoTransport  transport(host_of(address), token, timeout_s);
    SessionOptions so;
    so.reply_timeout_ms = std::max(1000, opts.step_timeout_ms);
    Session session(transport, sn, so);
    if (!session.open(out.error)) {
        out.reached = true; // HTTP answered; only the MQTT side did not
        BOOST_LOG_TRIVIAL(info) << "CANVAS (CC2): MQTT session failed: " << out.error;
        return out;
    }
    out = canvas_query_over(session);
    session.close();
    BOOST_LOG_TRIVIAL(info) << "CANVAS (CC2): " << (out.ok ? "read " + std::to_string(out.info.trays.size()) + " trays" :
                                                              (out.no_canvas ? std::string("no CANVAS attached") : out.error))
                            << (out.print_state.empty() ? std::string() : ", printer " + out.print_state);
    return out;
}

StartResult start_print(const std::string&                        address,
                        const std::string&                        token_in,
                        const std::map<std::string, std::string>& extended_info,
                        int                                       pc_bed_type,
                        const std::string&                        filename)
{
    StartResult       out;
    const std::string token = token_or_default(token_in);
    std::string       sn;
    if (!fetch_serial(address, token, sn, out.error)) {
        out.error = "could not read the printer's serial number (" + out.error + ")";
        return out;
    }
    PahoTransport transport(host_of(address), token, 5);
    Session       session(transport, sn);
    if (!session.open(out.error))
        return out;
    out = start_print_over(session, extended_info, pc_bed_type, filename);
    session.close();
    return out;
}

} // namespace ElegooCc2
} // namespace Slic3r
