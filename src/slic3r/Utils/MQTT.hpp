#ifndef MQTT_H
#define MQTT_H

#include <functional>
#include <string>
#include <map>
#include <mqtt/async_client.h>
#include <boost/log/trivial.hpp>
#include <memory>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>
#include <boost/filesystem.hpp>

// Number of retries for connection and subscription attempts
#define CONNECT_RETRY_TIME 3
#define SUBSCRIBE_RETRY_TIME 3

// Action listener class to handle MQTT operation callbacks
class action_listener : public virtual mqtt::iaction_listener
{
private:
    std::string name_;  // Name identifier for the listener

public:
    // Constructor with a name for identification
    action_listener(const std::string& name) : name_(name) {}

    // Called when an MQTT operation fails
    void on_failure(const mqtt::token& tok) override
    {
        BOOST_LOG_TRIVIAL(error) << name_ << " operation failed";
        if (tok.get_message_id() != 0) {
            BOOST_LOG_TRIVIAL(error) << "Token: [" << tok.get_message_id() << "]";
        }
    }

    // Called when an MQTT operation succeeds
    void on_success(const mqtt::token& tok) override
    {
        BOOST_LOG_TRIVIAL(debug) << name_ << " operation successful";
        if (tok.get_message_id() != 0) {
            BOOST_LOG_TRIVIAL(debug) << "Token: [" << tok.get_message_id() << "]";
        }
        auto top = tok.get_topics();
        if (top && !top->empty()) {
            BOOST_LOG_TRIVIAL(debug) << "Token topic: '" << (*top)[0] << "'";
        }
    }
};

// Main MQTT client class implementing both callback and action listener interfaces
class MqttClient : public mqtt::callback, 
                  public virtual mqtt::iaction_listener,
                  public std::enable_shared_from_this<MqttClient>
{
private:
    // The constructors are private on purpose: MqttClient::create() below is the
    // only way to build a client, so every instance has its self_ populated.
    // connection_lost() arms auto-reconnect from self_; a client built with a
    // raw `new MqttClient(...)` (or make_shared) would silently never reconnect.
    // normal MQTT connect 
    MqttClient(const std::string& server_address,
               const std::string& client_id,
               const std::string& username = "",
               const std::string& password = "",
               bool               clean_session = false);
               
    // SSL/TLS - and use CA connect
    MqttClient(const std::string& server_address, 
               const std::string& client_id,
               const std::string& ca_content,
               const std::string& cert_content = "",
               const std::string& key_content = "",
               const std::string& username = "",
               const std::string& password = "",
               bool clean_session = false);

public:
    // Factory: the supported way to create a MqttClient. It caches the
    // client's own weak reference (self_) exactly when shared ownership is
    // established, so Paho callbacks can arm the reconnect checker from the
    // cached weak_ptr instead of calling shared_from_this() — which throws
    // bad_weak_ptr once the last owner has started destruction (and always
    // threw for raw `new`-ed clients).
    template<typename... Args>
    static std::shared_ptr<MqttClient> create(Args&&... args) {
        std::shared_ptr<MqttClient> p(new MqttClient(std::forward<Args>(args)...));
        p->self_ = p;
        register_live(p);
        return p;
    }

    // Destructor
    ~MqttClient();

    // Connect to the MQTT broker
    bool Connect(std::string& msg);

    // Disconnect from the MQTT broker
    bool Disconnect(std::string& msg);

    // Subscribe to a specific topic with given QoS
    bool Subscribe(const std::string& topic, int qos, std::string& msg);

    // Unsubscribe from a specific topic
    bool Unsubscribe(const std::string& topic, std::string& msg);

    // Publish a message to a specific topic with given QoS
    bool Publish(const std::string& topic, const std::string& payload, int qos, std::string& msg);

    // Set callback for handling incoming messages
    void SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload)> callback);
    void SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload, void* this_)> callback);
    // Resolves ambiguity of SetMessageCallback(nullptr) between the two overloads above
    void SetMessageCallback(std::nullptr_t) {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        message_callback_  = nullptr;
        message_callback1_ = nullptr;
    }

    //  add set connect callback
    void SetConnectionFailureCallback(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        connection_failure_callback_ = callback;
    }

    // Callback interface implementations
    void connection_lost(const std::string& cause) override;
    void message_arrived(mqtt::const_message_ptr msg) override;
    void delivery_complete(mqtt::delivery_token_ptr token) override;
    void connected(const std::string& cause) override;

    // Action listener interface implementations
    void on_failure(const mqtt::token& tok) override;
    void on_success(const mqtt::token& tok) override;

    // Check if client is currently connected
    bool CheckConnected();

    // Re-establish the session on a worker thread, e.g. after the PC woke from
    // sleep. A no-op for a client the owner never connected or has since
    // disconnected, and while an earlier reconnect_now() is still at work.
    // Otherwise the session is always bounced (disconnect + connect): after a
    // sleep Paho can still believe the socket is up when it is dead, and its
    // own reconnect() does nothing while it believes that. Connect attempts
    // repeat at MqttReconnectPolicy::RESUME_RETRY_MS for GIVE_UP_AFTER_MS; only
    // when that window closes does the connection-failure callback fire.
    void reconnect_now(const std::string& reason);

    // reconnect_now() on every client built by create() that is still owned.
    // Returns how many were asked. Called by the main frame on system resume.
    static size_t reconnect_all_live(const std::string& reason);
    static size_t live_client_count();

    std::string get_client_id() {return client_id_;}
private:
    // The registry of live clients: weak_ptrs populated by create(), pruned by
    // ~MqttClient, so a system-resume trigger can reach every open session.
    static void register_live(const std::shared_ptr<MqttClient>& client);
    static void unregister_live(const MqttClient* client);

    // The bounce half of reconnect_now(): Disconnect() minus the "the owner
    // wants this session gone" bookkeeping, so a resume never revives a client
    // its owner had closed on purpose.
    bool do_disconnect(std::string& msg);

    // Fires connection_failure_callback_ (copied under cb_mtx_, invoked outside
    // it) at most once per outage; connected() and Connect() re-arm it.
    void report_connection_failure();

    // Sleeps `ms` in short slices, returning early (false) once the watch says
    // stop, so a checker thread never outlives the destructor's wait.
    struct ReconnectWatch;
    static bool sleep_unless_stopped(const std::shared_ptr<ReconnectWatch>& watch, long long ms);
    std::string server_address_;     // MQTT broker address
    std::string client_id_;          // Unique client identifier
    std::unique_ptr<mqtt::async_client> client_;      // Async MQTT client instance
    // Guards message_callback_ / message_callback1_ / connection_failure_callback_,
    // which are read on the Paho callback threads and written from owner threads
    // (including being nulled at the start of ~MqttClient).
    mutable std::mutex cb_mtx_;
    std::function<void(const std::string& topic, const std::string& payload)> message_callback_;  // Message handler
    std::function<void(const std::string& topic, const std::string& payload, void* this_)> message_callback1_;  // Message handler

    mqtt::connect_options connOpts_; // Connection options
    std::atomic<bool> connected_;    // Connection status flag
    mutable std::mutex topics_mtx_;  // Guards topics_to_resubscribe_
    std::map<std::string, int> topics_to_resubscribe_;  // Topics to resubscribe after reconnection
    action_listener subListener_;    // Subscription listener
    int connect_retry_time_;         // Connection retry counter
    int subscribe_retry_time_;       // Subscription retry counter
    std::function<void()> connection_failure_callback_; 

    // True while a lost-session watcher thread is supervising an outage.
    // Cleared by connected(), by Disconnect() and by reconnect_now() (which
    // takes the outage over); the watcher stands down when it sees it clear.
    std::atomic<bool> is_reconnecting;
    // The detached checker threads' bookkeeping, shared with them by
    // shared_ptr so it outlives the client: pending_reconnect_checks counts
    // the threads still alive (the destructor waits for it to reach zero) and
    // stop tells them to leave without touching the client again.
    struct ReconnectWatch {
        std::atomic<int>  pending_reconnect_checks{0};
        std::atomic<bool> stop{false};
    };
    std::shared_ptr<ReconnectWatch> watch_;
    std::atomic<bool> ever_connected_;
    // The owner's intent: set by Connect(), cleared by Disconnect(). A resume
    // only bounces clients whose owner still wants them up.
    std::atomic<bool> wants_connection_{false};
    // True while a reconnect_now() worker owns the outage: connection_lost()
    // starts no watcher and a failed connect attempt fires no failure callback.
    std::atomic<bool> manual_reconnect_active_{false};
    // The failure callback has been fired for the current outage.
    std::atomic<bool> failure_reported_{false};
    // Set as the very first step of ~MqttClient. Paho callbacks (esp.
    // connection_lost) check it before touching any other member.
    std::atomic<bool> tearing_down_{false};
    // Cached by create() while the client is owned; connection_lost() reads
    // this instead of calling shared_from_this() (which throws bad_weak_ptr
    // once the last owner has started destruction).
    std::weak_ptr<MqttClient> self_;

    // tmp path
    boost::filesystem::path temp_ca_path_;
    boost::filesystem::path temp_cert_path_;
    boost::filesystem::path temp_key_path_;
    
    // clean tmp files
    void cleanup_temp_files();

    //add new fucntion to resubscribe
    void resubscribe_topics();
    void add_topic_to_resubscribe(const std::string& topic, int qos);
    void remove_topic_from_resubscribe(const std::string& topic);
};

#endif // MQTT_H
