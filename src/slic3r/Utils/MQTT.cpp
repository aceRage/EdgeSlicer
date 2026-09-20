#include "MQTT.hpp"
#include "MqttReconnectPolicy.hpp"
#include <thread>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <future>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <vector>

namespace {

// Paho throws when disconnect() is called while already disconnected / handle gone.
bool is_soft_disconnect_error(const mqtt::exception& e)
{
    const int rc = e.get_return_code();
    return rc == MQTTASYNC_DISCONNECTED || rc == MQTTASYNC_FAILURE;
}

// Every client create() built and ~MqttClient has not yet pruned. Leaked on
// purpose: Moonraker_Mqtt keeps its clients in statics, and the order in which
// static objects die at exit is not one to lean on.
struct LiveRegistry {
    std::mutex                             mtx;
    std::vector<std::weak_ptr<MqttClient>> clients;
};
LiveRegistry& live_registry()
{
    static LiveRegistry* r = new LiveRegistry;
    return *r;
}

long long steady_ms_since(const std::chrono::steady_clock::time_point& t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

void MqttClient::register_live(const std::shared_ptr<MqttClient>& client)
{
    auto& reg = live_registry();
    std::lock_guard<std::mutex> lock(reg.mtx);
    reg.clients.erase(std::remove_if(reg.clients.begin(), reg.clients.end(),
                                     [](const std::weak_ptr<MqttClient>& w) { return w.expired(); }),
                      reg.clients.end());
    reg.clients.push_back(client);
}

void MqttClient::unregister_live(const MqttClient* client)
{
    // Called from ~MqttClient, where our own entry is already expired; every
    // expired entry goes, whichever client it belonged to.
    (void)client;
    auto& reg = live_registry();
    std::lock_guard<std::mutex> lock(reg.mtx);
    reg.clients.erase(std::remove_if(reg.clients.begin(), reg.clients.end(),
                                     [](const std::weak_ptr<MqttClient>& w) { return w.expired(); }),
                      reg.clients.end());
}

size_t MqttClient::live_client_count()
{
    auto& reg = live_registry();
    std::lock_guard<std::mutex> lock(reg.mtx);
    return size_t(std::count_if(reg.clients.begin(), reg.clients.end(),
                                [](const std::weak_ptr<MqttClient>& w) { return !w.expired(); }));
}

size_t MqttClient::reconnect_all_live(const std::string& reason)
{
    // Pin the live ones under the lock, act outside it: reconnect_now() spawns
    // a thread and logs, neither of which belongs under the registry mutex.
    std::vector<std::shared_ptr<MqttClient>> live;
    {
        auto& reg = live_registry();
        std::lock_guard<std::mutex> lock(reg.mtx);
        for (const auto& w : reg.clients)
            if (auto p = w.lock())
                live.push_back(std::move(p));
    }
    BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] " << reason << ": asking " << live.size() << " live MQTT client(s) to reconnect";
    for (const auto& p : live)
        p->reconnect_now(reason);
    return live.size();
}

// Constructor: Initialize MQTT client with server address and client ID
// @param server_address: Address of the MQTT broker
// @param client_id: Unique identifier for this client
// @param clean_session: Whether to start with a clean session
MqttClient::MqttClient(const std::string& server_address, const std::string& client_id, const std::string& username, const std::string& password,  bool clean_session)
    : server_address_(server_address)
    , client_id_(client_id)
    , client_(std::make_unique<mqtt::async_client>(server_address_, client_id_))
    , connOpts_()
    , subListener_("Subscription")
    , connected_(false)            
    , is_reconnecting(false)
    , connection_failure_callback_(nullptr)
    , watch_(std::make_shared<ReconnectWatch>())
    , ever_connected_(false)
{
    BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] initializing MQTT connection, server_address: " << server_address << ", client_id: " << client_id;

    // Configure connection options    
    connOpts_.set_clean_session(false);
    connOpts_.set_keep_alive_interval(30);
    connOpts_.set_connect_timeout(10);
    // auto-reconnect enabled only after first successful connection
    connOpts_.set_automatic_reconnect(std::chrono::seconds(0), std::chrono::seconds(0));
    client_->set_callback(*this);

    // set authentication info
    if (!username.empty()) {
        connOpts_.set_user_name(username);
        if (!password.empty()) {
            connOpts_.set_password(password);
        }
    }
}

// SSL/TLS
MqttClient::MqttClient(const std::string& server_address, 
                      const std::string& client_id,
                      const std::string& ca_content,        
                      const std::string& cert_content,
                      const std::string& key_content,
                      const std::string& username,
                      const std::string& password,
                      bool clean_session)
    : MqttClient(server_address, client_id, username, password, clean_session)
{
    BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] initializing MQTT SSL connection, server_address: " << server_address << ", client_id: " << client_id
                            << ", ca_content: " << ca_content << ", cert_content: " << cert_content << ", username: " << username
                            << ", password: " << password;
    
    try {
        boost::filesystem::path temp_dir = boost::filesystem::temp_directory_path();

        boost::filesystem::path ca_path = temp_dir / ("ca_" + client_id + std::to_string(int64_t(this)) + ".pem");
        if (!ca_content.empty()) {
            boost::filesystem::ofstream ca_file(ca_path);
            ca_file << ca_content;
            ca_file.close();
            if (!ca_file) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] failed to save CA certificate: " << ca_path;
                throw std::runtime_error("Failed to write CA certificate temporary file");
            }
            BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] CA certificate saved to: " << ca_path;
        }
        
        boost::filesystem::path cert_path = temp_dir / ("cert_" + client_id + std::to_string(int64_t(this)) + ".pem");
        if (!cert_content.empty()) {
            boost::filesystem::ofstream cert_file(cert_path);
            cert_file << cert_content;
            cert_file.close();
            if (!cert_file) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] failed to save client certificate: " << cert_path;
                throw std::runtime_error("Failed to write client certificate temporary file");
            }
            BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] client certificate saved to: " << cert_path;
        }
        
        boost::filesystem::path key_path = temp_dir / ("key_" + client_id + std::to_string(int64_t(this)) + ".pem");
        if (!key_content.empty()) {
            boost::filesystem::ofstream key_file(key_path);
            key_file << key_content;
            key_file.close();
            if (!key_file) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] failed to save private key: " << key_path;
                throw std::runtime_error("Failed to write private key temporary file");
            }
            BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] private key saved to: " << key_path;
        }
        
        mqtt::ssl_options ssl_opts;                
        ssl_opts.set_verify(false);
        ssl_opts.set_enable_server_cert_auth(true);
                
        if (!ca_content.empty()) {
            ssl_opts.set_trust_store(ca_path.string());
        }
        if (!cert_content.empty() && !key_content.empty()) {
            ssl_opts.set_key_store(cert_path.string());
            ssl_opts.set_private_key(key_path.string());
        }

        ssl_opts.set_ssl_version(MQTT_SSL_VERSION_TLS_1_2);                
        connOpts_.set_ssl(ssl_opts);             
        temp_ca_path_ = ca_path;
        temp_cert_path_ = cert_path;
        temp_key_path_ = key_path;
    } catch (const std::exception& e) {
        cleanup_temp_files();
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT SSL initialization failed: " << e.what();
        throw;
    }
}

// Establish connection to the MQTT broker
// @return: true if connection successful, false otherwise
bool MqttClient::Connect(std::string& msg)
{
    // The owner wants this session up: from here a system resume may bounce
    // it, and a fresh outage may be reported once more.
    wants_connection_.store(true, std::memory_order_release);
    failure_reported_.store(false, std::memory_order_release);

    if (connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] " << client_id_ 
                                   << " Already connected to MQTT server " << server_address_;
        msg = "success";
        return true;
    }

    {                        
         {
            auto ssl_opts = connOpts_.get_ssl_options();
            BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] SSL config info:"
                << "\n - server address: " << server_address_
                << "\n - client id: " << client_id_
                << "\n - CA length: " << (ssl_opts.get_trust_store().empty() ? 0 : ssl_opts.get_trust_store().length())
                << "\n - client cert length: " << (ssl_opts.get_key_store().empty() ? 0 : ssl_opts.get_key_store().length())
                << "\n - private key length: " << (ssl_opts.get_private_key().empty() ? 0 : ssl_opts.get_private_key().length());
        }

        const char* context = "connection";
        mqtt::token_ptr conntok = client_->connect(connOpts_, (void*)(context), *this);
        
        if (!conntok->wait_for(std::chrono::seconds(20))) {
            auto rc = conntok->get_return_code();
            auto reason = conntok->get_reason_code();
            
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection timeout. Return code: " << rc 
                                    << ", Reason code: " << static_cast<int>(reason)
                                    << ", Server: " << server_address_;

            if (rc == MQTTASYNC_FAILURE) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection failed - MQTTASYNC_FAILURE";
                msg = "MQTTASYNC_FAILURE";
            } else if (rc == MQTTASYNC_DISCONNECTED) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection failed - MQTTASYNC_DISCONNECTED";
                msg = "MQTTASYNC_DISCONNECTED";
            }
            
            connected_.store(false, std::memory_order_release);
            return false;
        }

        if (!client_->is_connected()) {
            auto rc = conntok->get_return_code();
            auto reason = conntok->get_reason_code();
            
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection failed. Return code: " << rc 
                                    << ", Reason code: " << static_cast<int>(reason)
                                    << ", Server: " << server_address_;

            msg = "connetion failed, Return code: " + std::to_string(rc) + " Reason code: " + std::to_string(reason);
            
            connected_.store(false, std::memory_order_release);
            return false;
        }

        connected_.store(true, std::memory_order_release);
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Successfully connected to MQTT server";
        msg = "success";
        return true;
    }
}

// Disconnect from the MQTT broker
// @return: true if disconnection successful, false otherwise
bool MqttClient::Disconnect(std::string& msg)
{
    // The owner (or a give-up) wants this session gone: no resume bounce, and
    // any lost-session watcher stands down.
    wants_connection_.store(false, std::memory_order_release);
    is_reconnecting.store(false, std::memory_order_release);
    return do_disconnect(msg);
}

bool MqttClient::do_disconnect(std::string& msg)
{
    connected_.store(false, std::memory_order_release);

    if (!client_) {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Disconnect completed (no client)";
        msg = "success";
        return true;
    }

    // Always call disconnect(): even if already down, C lib clears shouldBeConnected
    // (stops auto-reconnect) then returns MQTTASYNC_DISCONNECTED which C++ throws.
    try {
        auto disctok = client_->disconnect();
        if (!disctok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT disconnect timeout";
        }
    } catch (const mqtt::exception& e) {
        if (is_soft_disconnect_error(e)) {
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] MQTT disconnect soft error (already disconnected), rc="
                                      << e.get_return_code();
            msg = "success";
            return true;
        }
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT disconnect failed: " << e.what()
                                 << ", rc=" << e.get_return_code();
        msg = e.what();
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Disconnect completed";
    msg = "success";
    return true;
}

// Subscribe to a specific MQTT topic
// @param topic: The topic to subscribe to
// @param qos: Quality of Service level (0, 1, or 2)
// @return: true if subscription successful, false otherwise
bool MqttClient::Subscribe(const std::string& topic, int qos, std::string& msg)
{
    if (!CheckConnected()) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Cannot subscribe: client not connected";

        msg = "client not connected";
        return false;
    }

    {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Subscribing to MQTT topic '" << topic << "' with QoS " << qos;
        mqtt::token_ptr subtok = client_->subscribe(topic, qos, nullptr, subListener_);
        if (!subtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Subscribe timeout for topic: " << topic;
            msg = "subscribe timeout";
            return false;
        }
        add_topic_to_resubscribe(topic, qos);
        msg = "success";
        return true;
    }
}

// Unsubscribe from a specific MQTT topic
// @param topic: The topic to unsubscribe from
// @return: true if unsubscription successful, false otherwise
bool MqttClient::Unsubscribe(const std::string& topic, std::string& msg)
{
    if (!CheckConnected()) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Cannot unsubscribe: client not connected";
        msg = "client not connect"; 
        return false;
    }

    {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Unsubscribing from MQTT topic '" << topic << "'";
        mqtt::token_ptr unsubtok = client_->unsubscribe(topic);
        if (!unsubtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Unsubscribe timeout for topic: " << topic;
            msg = "Unsubscribe timeout for topic";
            return false;
        }
        remove_topic_from_resubscribe(topic);
        msg = "success";
        return true;
    }
}

// Publish a message to a specific MQTT topic
// @param topic: The topic to publish to
// @param payload: The message content
// @param qos: Quality of Service level (0, 1, or 2)
// @return: true if publish successful, false otherwise
bool MqttClient::Publish(const std::string& topic, const std::string& payload, int qos, std::string& msg)
{
    if (!CheckConnected()) {
        msg = "client not connect";
        return false;
    }

    mqtt::message_ptr pubmsg = mqtt::make_message(topic, payload);
    pubmsg->set_qos(qos);

    {
        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Publishing message to topic '" << topic << "' with QoS " << qos;
        mqtt::token_ptr pubtok = client_->publish(pubmsg);
        /*if (!pubtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "Publish timeout for topic: " << topic;
            return false;
        }*/
        msg = "success";
        return true;
    } 
}

// Set callback function for handling incoming messages
// @param callback: Function to be called when a message arrives
void MqttClient::SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload)> callback)
{
    std::lock_guard<std::mutex> lock(cb_mtx_);
    message_callback1_ = nullptr;
    message_callback_ = callback;
}

void MqttClient::SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload, void* this_)> callback)
{
    std::lock_guard<std::mutex> lock(cb_mtx_);
    message_callback_  = nullptr;
    message_callback1_ = callback;
}

// Check if the client is currently connected
// @return: true if connected, false otherwise
bool MqttClient::CheckConnected()
{
    if (!connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT client is not connected to server";
        return false;
    }

    
    if (!client_) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT client pointer is null";
        connected_.store(false, std::memory_order_release);
        return false;
    }
    
    auto check_future = std::async(std::launch::async, [this]() {
        
        return client_->is_connected();      
    });
    
    if (check_future.wait_for(std::chrono::seconds(3)) == std::future_status::timeout) {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Connection status check timeout";
        connected_.store(false, std::memory_order_release);
        return false;
    }
    
    if (!check_future.get()) {
        connected_.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

// Callback when connection is lost
// Implements automatic reconnection with retry logic
// @param cause: Reason for connection loss
void MqttClient::connection_lost(const std::string& cause)
{
    BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] MQTT connection lost, address: " << this->server_address_;
    if (!cause.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Cause: " << cause;
    }

    connected_.store(false, std::memory_order_release);

    // ~MqttClient nulls the callbacks and disconnects; a connection_lost that
    // races destruction must not spawn the reconnect checker below (its
    // shared_from_this() would throw bad_weak_ptr once the last owner is gone,
    // and the exception escapes through Paho's C callback into terminate()).
    if (tearing_down_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] MQTT client is being destroyed, ignoring connection_lost";
        return;
    }

    if (!ever_connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(error) << "The first connection failed. Since no successful connection has been made before, automatic reconnection remains disabled";
        report_connection_failure();
        return;
    }

    if (manual_reconnect_active_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] a reconnect_now() worker owns this outage; no watcher started";
        return;
    }

    if (is_reconnecting.exchange(true, std::memory_order_acq_rel)) {
        // A watcher is already supervising this outage; it keeps looking at
        // connected_ and needs no second thread.
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] lost-session watcher already running";
        return;
    }

    // self_ is cached by create() while the client is owned, so reading it
    // can never throw (unlike shared_from_this(), which throws
    // bad_weak_ptr once the last owner has started destruction). An
    // expired/empty weak_ptr means no owner: auto-reconnect is impossible
    // for this client.
    std::weak_ptr<MqttClient> weak_self = self_;
    if (weak_self.expired()) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT client has no shared_ptr owner (or is being destroyed); automatic reconnection skipped";
        is_reconnecting.store(false, std::memory_order_release);
        return;
    }

    // Paho's own automatic reconnect (armed by connected()) keeps retrying the
    // socket at 2..30 s. This watcher only decides how long to let it: it
    // looks every CHECK_INTERVAL_MS and gives the session up - Disconnect()
    // (which stops Paho's retrying) plus the failure callback - once
    // GIVE_UP_AFTER_MS have passed without connected() firing. A PC waking
    // from sleep needs well over the old single 20 s look for its network to
    // come back, and until now that one look inside the outage was final.
    std::shared_ptr<ReconnectWatch> watch = watch_;
    watch->pending_reconnect_checks.fetch_add(1, std::memory_order_acq_rel);
    try {
    std::thread([weak_self, watch]() {
        using namespace Slic3r::MqttReconnectPolicy;
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            if (!sleep_unless_stopped(watch, next_wait_ms(steady_ms_since(t0))))
                break;

            auto self = weak_self.lock();
            if (!self || self->tearing_down_.load(std::memory_order_acquire) || watch->stop.load(std::memory_order_acquire)) {
                BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] MQTT client is gone; lost-session watcher leaving";
                break;
            }
            if (!self->is_reconnecting.load(std::memory_order_acquire)) {
                // connected() fired, the owner disconnected, or reconnect_now()
                // took the outage over: nothing left to supervise.
                BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] lost-session watcher stood down for " << self->client_id_;
                break;
            }

            const long long elapsed = steady_ms_since(t0);
            const Verdict   verdict = decide(elapsed, self->connected_.load(std::memory_order_acquire));
            if (verdict == Verdict::Restored) {
                BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] MQTT connection restored after " << elapsed / 1000 << " s, address: " << self->server_address_;
                self->is_reconnecting.store(false, std::memory_order_release);
                break;
            }
            if (verdict == Verdict::KeepWaiting) {
                BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] MQTT connection still down after " << elapsed / 1000
                                        << " s; waiting up to " << GIVE_UP_AFTER_MS / 1000 << " s, address: " << self->server_address_;
                continue;
            }

            if (stopping(*self, *watch))
                break;
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT connection not restored after " << elapsed / 1000
                                     << " s, giving up, address: " << self->server_address_;
            std::string dc_msg;
            self->Disconnect(dc_msg); // also clears is_reconnecting
            if (stopping(*self, *watch))
                break;
            self->report_connection_failure();
            break;
        }
        watch->pending_reconnect_checks.fetch_sub(1, std::memory_order_acq_rel);
    }).detach();
    } catch (const std::exception& e) {
        // No thread, no watcher: leave nothing counted or claimed.
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] could not start the lost-session watcher: " << e.what();
        watch->pending_reconnect_checks.fetch_sub(1, std::memory_order_acq_rel);
        is_reconnecting.store(false, std::memory_order_release);
    }
}

bool MqttClient::stopping(const MqttClient& self, const ReconnectWatch& watch)
{
    return self.tearing_down_.load(std::memory_order_acquire) || watch.stop.load(std::memory_order_acquire);
}

bool MqttClient::sleep_unless_stopped(const std::shared_ptr<ReconnectWatch>& watch, long long ms)
{
    // 250 ms slices: ~MqttClient waits at most 2 s for the checkers, and a
    // checker asleep for a whole interval would blow through that.
    constexpr long long SLICE_MS = 250;
    while (ms > 0) {
        if (watch->stop.load(std::memory_order_acquire))
            return false;
        const long long step = ms < SLICE_MS ? ms : SLICE_MS;
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        ms -= step;
    }
    return !watch->stop.load(std::memory_order_acquire);
}

void MqttClient::report_connection_failure()
{
    if (failure_reported_.exchange(true, std::memory_order_acq_rel))
        return;
    std::function<void()> failure_cb;
    {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        failure_cb = connection_failure_callback_;
    }
    if (failure_cb) {
        failure_cb();
    }
}

void MqttClient::reconnect_now(const std::string& reason)
{
    if (tearing_down_.load(std::memory_order_acquire))
        return;
    if (!wants_connection_.load(std::memory_order_acquire) || !ever_connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] " << reason << ": " << client_id_ << " has no session to restore; skipped";
        return;
    }
    // The gate: exactly one caller flips false -> true and owns the bounce
    // until its worker clears the flag; every other caller (a second resume,
    // the same resume seen twice) is turned away here.
    bool not_active = false;
    if (!manual_reconnect_active_.compare_exchange_strong(not_active, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] " << reason << ": " << client_id_ << " is already reconnecting; skipped";
        return;
    }
    std::weak_ptr<MqttClient> weak_self = self_;
    if (weak_self.expired()) {
        manual_reconnect_active_.store(false, std::memory_order_release);
        return;
    }

    // From here this worker owns the outage: the lost-session watcher (if one
    // is running) stands down, and Paho's own retrying is cancelled by the
    // bounce below.
    is_reconnecting.store(false, std::memory_order_release);
    BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] " << reason << ": bouncing the session of " << client_id_ << " to " << server_address_;

    std::shared_ptr<ReconnectWatch> watch = watch_;
    watch->pending_reconnect_checks.fetch_add(1, std::memory_order_acq_rel);
    try {
    std::thread([weak_self, watch, reason]() {
        using namespace Slic3r::MqttReconnectPolicy;
        const auto t0      = std::chrono::steady_clock::now();
        bool       bounced = false;
        for (;;) {
            auto self = weak_self.lock();
            if (!self || self->tearing_down_.load(std::memory_order_acquire) || watch->stop.load(std::memory_order_acquire))
                break;
            if (!self->wants_connection_.load(std::memory_order_acquire)) {
                // The owner closed the session while we were retrying.
                self->manual_reconnect_active_.store(false, std::memory_order_release);
                break;
            }

            if (!bounced) {
                // Always: after a sleep Paho may still hold a socket that is
                // dead on the wire, and it reconnects nothing while it
                // believes the socket is up.
                std::string dc_msg;
                self->do_disconnect(dc_msg);
                bounced = true;
            }

            std::string msg;
            bool        up = false;
            try {
                up = self->Connect(msg);
            } catch (const std::exception& e) {
                msg = e.what();
            }
            if (up) {
                BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] " << reason << ": session of " << self->client_id_ << " restored after "
                                        << steady_ms_since(t0) / 1000 << " s";
                // Sessions are persistent (clean_session=false) so the broker
                // usually still holds the subscriptions; this covers the one
                // that does not. Still under the gate: a second bounce must
                // not tear the socket down under these subscribes.
                self->resubscribe_topics();
                self->manual_reconnect_active_.store(false, std::memory_order_release);
                // A drop that arrived while the gate was held started no
                // watcher (connection_lost() defers to the bounce). If the new
                // session is already gone, hand the outage to the ordinary
                // watcher now; Paho's own retrying is armed either way.
                if (!self->connected_.load(std::memory_order_acquire) && !stopping(*self, *watch)) {
                    BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] " << reason << ": session of " << self->client_id_
                                               << " dropped again during the bounce; watching it";
                    self->connection_lost("dropped during the resume bounce");
                }
                break;
            }

            const long long elapsed = steady_ms_since(t0);
            if (decide(elapsed, false) == Verdict::GiveUp) {
                if (stopping(*self, *watch))
                    break;
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] " << reason << ": session of " << self->client_id_ << " not restored after "
                                         << elapsed / 1000 << " s (" << msg << "), giving up";
                std::string dc_msg;
                self->Disconnect(dc_msg);
                self->manual_reconnect_active_.store(false, std::memory_order_release);
                if (stopping(*self, *watch))
                    break;
                self->report_connection_failure();
                break;
            }
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] " << reason << ": connect to " << self->server_address_ << " failed (" << msg
                                       << ") after " << elapsed / 1000 << " s; retrying";
            const long long wait = next_wait_ms(elapsed, RESUME_RETRY_MS);
            self.reset(); // never pin the client while asleep
            if (!sleep_unless_stopped(watch, wait))
                break;
        }
        watch->pending_reconnect_checks.fetch_sub(1, std::memory_order_acq_rel);
    }).detach();
    } catch (const std::exception& e) {
        // No thread, no bounce: release the gate and the count, or the next
        // resume would be turned away for ever.
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] " << reason << ": could not start the reconnect worker: " << e.what();
        watch->pending_reconnect_checks.fetch_sub(1, std::memory_order_acq_rel);
        manual_reconnect_active_.store(false, std::memory_order_release);
    }
}

// Callback when a message arrives
// @param msg: Pointer to the received message
void MqttClient::message_arrived(mqtt::const_message_ptr msg)
{
    // Copy the callbacks under the lock and invoke the copies outside it:
    // the setters (and ~MqttClient) may run concurrently on another thread.
    std::function<void(const std::string&, const std::string&)> cb;
    std::function<void(const std::string&, const std::string&, void*)> cb1;
    {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        cb  = message_callback_;
        cb1 = message_callback1_;
    }

    if (cb) {
        cb(msg->get_topic(), msg->to_string());
    }

    if (cb1) {
        cb1(msg->get_topic(), msg->to_string(), this);
    }
}

// Callback when message delivery is complete
// @param token: Delivery token containing message details
void MqttClient::delivery_complete(mqtt::delivery_token_ptr token)
{
    BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Message delivery complete for token: " << (token ? token->get_message_id() : -1);
}

// Callback for operation failure
// @param tok: Token containing operation details
void MqttClient::on_failure(const mqtt::token& tok)
{ 
    if (tok.get_user_context() && 
        std::string(static_cast<const char*>(tok.get_user_context())) == "connection") {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT connection attempt failed";
        if (tok.get_reason_code() != 0) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Reason code: " << tok.get_reason_code();
        }
 
        connected_.store(false, std::memory_order_release);

        if (manual_reconnect_active_.load(std::memory_order_acquire)) {
            // One attempt of a reconnect_now() loop failed; the loop retries
            // and reports only when its whole window has closed.
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] connect attempt failed during a reconnect_now() window; not reported";
            return;
        }

        BOOST_LOG_TRIVIAL(error) << "The first connection failed. Since no successful connection has been made before, automatic reconnection remains disabled";
        report_connection_failure();
    } else {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Operation failed for token: " << tok.get_message_id();
        if (tok.get_reason_code() != 0) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Reason code: " << tok.get_reason_code();
        }
    }
}

// Callback for operation success
// @param tok: Token containing operation details
void MqttClient::on_success(const mqtt::token& tok)
{
    BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Operation successful for token: " << tok.get_message_id();
    auto top = tok.get_topics();
    if (top && !top->empty()) {
        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Token topic: '" << (*top)[0] << "'";
    }
}

void MqttClient::connected(const std::string& cause)
{

    connected_.store(true, std::memory_order_release);
    is_reconnecting.store(false, std::memory_order_release);
    failure_reported_.store(false, std::memory_order_release);

    ever_connected_.store(true, std::memory_order_release);

    connOpts_.set_automatic_reconnect(std::chrono::seconds(2), std::chrono::seconds(30));
    BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] auto-reconnect enabled after successful connection";
}

void MqttClient::resubscribe_topics() {
    if (!client_) {
        return;
    }

    // Snapshot the map: subscribing can block for seconds per topic, so the
    // lock must not be held while waiting on the broker.
    std::map<std::string, int> topics;
    {
        std::lock_guard<std::mutex> lock(topics_mtx_);
        topics = topics_to_resubscribe_;
    }
    if (topics.empty()) {
        return;
    }

    for (const auto& topic_pair : topics) {
        // subscribe() throws (MQTTASYNC_DISCONNECTED) if the session dropped
        // again between the connect and this call; on the detached reconnect
        // worker an escaping exception would be terminate(), so it is a log
        // line here and the next topic is tried (it will throw too, cheaply).
        try {
            auto tok = client_->subscribe(topic_pair.first, topic_pair.second, nullptr,  subListener_);
            if (!tok->wait_for(std::chrono::seconds(5))) {
                BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Subscribe timeout for topic: " << topic_pair.first;
                continue;
            }
            if (!tok->is_complete() || tok->get_return_code() != 0) {
                BOOST_LOG_TRIVIAL(info) << "[MQTT_INFO] Failed to resubscribe to topic: " << topic_pair.first;
            }
        } catch (const mqtt::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Could not resubscribe to topic " << topic_pair.first << ": " << e.what()
                                       << ", rc=" << e.get_return_code();
        }
    }
}

void MqttClient::add_topic_to_resubscribe(const std::string& topic, int qos) {
    std::lock_guard<std::mutex> lock(topics_mtx_);
    topics_to_resubscribe_[topic] = qos;
    BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Added topic to resubscribe list: " << topic;
}

void MqttClient::remove_topic_from_resubscribe(const std::string& topic) {
    std::lock_guard<std::mutex> lock(topics_mtx_);
    auto it = topics_to_resubscribe_.find(topic);
    if (it != topics_to_resubscribe_.end()) {
        topics_to_resubscribe_.erase(it);
        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Removed topic from resubscribe list: " << topic;
    }
}

MqttClient::~MqttClient()
{
    // 0. Mark teardown before anything else: connection_lost() checks this
    //    before calling shared_from_this(), which would otherwise throw
    //    bad_weak_ptr (the last owner is the one running this destructor) and
    //    kill the process from Paho's C callback thread.
    tearing_down_.store(true, std::memory_order_release);

    // 1. Null the callbacks FIRST so any Paho callback that is still in flight
    //    (or fires during teardown below) becomes a no-op instead of touching
    //    members being destroyed. Without this, message_arrived() could run on
    //    the Paho receive thread while these std::function members are being
    //    torn apart — one of the sources of STATUS_HEAP_CORRUPTION crashes.
    {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        message_callback_            = nullptr;
        message_callback1_           = nullptr;
        connection_failure_callback_ = nullptr;
    }

    connected_.store(false, std::memory_order_release);
    is_reconnecting.store(false, std::memory_order_release);
    wants_connection_.store(false, std::memory_order_release);
    unregister_live(this);

    // 2. Wait briefly for the detached reconnect-check threads to finish so
    //    they do not call Disconnect() on a client being destroyed. They hold
    //    watch_ by shared_ptr (not the client), poll its stop flag every
    //    250 ms and count themselves out of pending_reconnect_checks on the
    //    way out, so this normally returns within one slice.
    watch_->stop.store(true, std::memory_order_release);
    int timeout_count = 0;
    const int max_timeout = 20; // max 2s (20 * 100ms)
    while (watch_->pending_reconnect_checks.load(std::memory_order_acquire) > 0 && timeout_count < max_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        timeout_count++;
    }

    if (timeout_count >= max_timeout) {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] timeout waiting for reconnect checks, forcing destruction";
    }

    // 3. Disconnect: clears shouldBeConnected in the C lib, stopping the
    //    auto-reconnect cycle before the client is destroyed.
    if (client_) {
        // Reuse Disconnect: soft-catches already-disconnected and clears shouldBeConnected.
        std::string dc_msg;
        Disconnect(dc_msg);
    }

    // 4. Destroy the async client. ~async_client calls MQTTAsync_destroy(),
    //    which stops the Paho send/receive threads; doing this BEFORE our own
    //    members are destroyed (end of destructor) closes the window in which
    //    a Paho thread could call back into this object after teardown.
    client_.reset();

    {
        std::lock_guard<std::mutex> lock(topics_mtx_);
        topics_to_resubscribe_.clear();
    }

    cleanup_temp_files();
}

void MqttClient::cleanup_temp_files()
{
    {
        if (!temp_ca_path_.empty() && boost::filesystem::exists(temp_ca_path_)) {
            boost::filesystem::remove(temp_ca_path_);
        }
        if (!temp_cert_path_.empty() && boost::filesystem::exists(temp_cert_path_)) {
            boost::filesystem::remove(temp_cert_path_);
        }
        if (!temp_key_path_.empty() && boost::filesystem::exists(temp_key_path_)) {
            boost::filesystem::remove(temp_key_path_);
        }
    }
}
