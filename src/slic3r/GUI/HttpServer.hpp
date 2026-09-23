#ifndef slic3r_Http_App_hpp_
#define slic3r_Http_App_hpp_

#include <atomic>
#include <iostream>
#include <mutex>
#include <stack>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <string>
#include <set>
#include <memory>
#include <vector>

#define LOCALHOST_PORT      13618
#define PAGE_HTTP_PORT      13619
#define LOCALHOST_URL       "http://127.0.0.1:"
// Ultra: the base URL advertised to bambulab.com/sign-in via get_localhost_url. Bambu
// Studio reports "http://localhost:" (LOCALHOST_URL upstream); bambulab validates the
// redirect target against its registered callback and rejects the 127.0.0.1 spelling, so
// this must stay "localhost" even though our other loopback URLs use 127.0.0.1.
#define BBL_LOGIN_LOCALHOST_URL "http://localhost:"
#define WCP_DOWNLOAD_PREFIX "/wcp_download/"

namespace Slic3r { namespace GUI {

class session;

class http_headers
{
    std::string method;
    std::string url;
    std::string version;

    std::map<std::string, std::string> headers;

    friend class session;
public:
    std::string get_url() { return url; }

    // Case-insensitive lookup with surrounding whitespace trimmed.
    std::string get_header(const std::string& name) const;

    int content_length()
    {
        auto request = headers.find("content-length");
        if (request != headers.end()) {
            std::stringstream ssLength(request->second);
            int               content_length;
            ssLength >> content_length;
            return content_length;
        }
        return 0;
    }

    void on_read_header(std::string line)
    {
        // std::cout << "header: " << line << std::endl;

        std::stringstream ssHeader(line);
        std::string       headerName;
        std::getline(ssHeader, headerName, ':');

        std::string value;
        std::getline(ssHeader, value);
        headers[headerName] = value;
    }

    void on_read_request_line(std::string line)
    {
        std::stringstream ssRequestLine(line);
        ssRequestLine >> method;
        ssRequestLine >> url;
        ssRequestLine >> version;
    }
};

class HttpServer
{
    // Written by start_locked() (io/health-check threads) and read by the GUI thread.
    std::atomic<boost::asio::ip::port_type> port;

public:
    class Response
    {
    public:
        virtual ~Response()                                   = default;
        virtual void write_response(std::stringstream& ssOut) = 0;

        // Forwarded conditional request headers, used by ResponseFile to implement
        // 304 Not Modified revalidation (If-Modified-Since / If-None-Match).
        void set_conditional_headers(const std::string& if_modified_since, const std::string& if_none_match)
        {
            m_if_modified_since = if_modified_since;
            m_if_none_match     = if_none_match;
        }

        // "Access-Control-Allow-Origin: *" and friends. Only the login-callback servers send
        // them; the page server answers our own web views only (see page_server::authorize).
        void set_allow_any_origin(bool on) { m_allow_any_origin = on; }
        // Extra "Name: value" header lines (e.g. Set-Cookie), written after the status line.
        void add_header(const std::string& line) { m_extra_headers.push_back(line); }

    protected:
        // Status line + CORS (when allowed) + extra headers + Connection: close.
        void write_head(std::stringstream& out, int status_code, const std::string& reason_phrase) const;
        // Copies the header policy onto a response written on this one's behalf.
        void copy_head_policy_to(Response& other) const
        {
            other.m_allow_any_origin = m_allow_any_origin;
            other.m_extra_headers    = m_extra_headers;
        }

        std::string              m_if_modified_since;
        std::string              m_if_none_match;
        bool                     m_allow_any_origin = false;
        std::vector<std::string> m_extra_headers;
    };

    // Refused request (page server authorisation). Plain-text body naming the reason.
    class ResponseForbidden : public Response
    {
        int         m_status;
        std::string m_reason;

    public:
        ResponseForbidden(int status, const std::string& reason) : m_status(status), m_reason(reason) {}
        ~ResponseForbidden() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    class ResponseNotFound : public Response
    {
    public:
        ~ResponseNotFound() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    // Ultra: a well-formed third-party login callback that could not be completed. A 200
    // page, never a 404 - a 404 in the system browser is exactly what users report as
    // "signing in with Google 404s".
    class ResponseLoginFailed : public Response
    {
    public:
        ~ResponseLoginFailed() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    class ResponseRedirect : public Response
    {
        const std::string location_str;

    public:
        ResponseRedirect(const std::string& location) : location_str(location) {}
        ~ResponseRedirect() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    class ResponseFile : public Response
    {
        std::string file_path;
        bool        m_native_path = false;  // true if path is already in system encoding, skip UTF-8 conversion

    public:
        ResponseFile(const std::string& path, bool native_path = false) : file_path(path), m_native_path(native_path) {}
        ~ResponseFile() override = default;

        void write_response(std::stringstream& ssOut) override;

        bool ends_with(const std::string& str, const std::string& suffix)
        {
            if (str.length() >= suffix.length()) {
                return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
            }
            return false;
        };
    };

    HttpServer(boost::asio::ip::port_type port = LOCALHOST_PORT);
    ~HttpServer();  // 添加析构函数

    boost::thread    m_http_server_thread;
    // Written by the io thread's exception handler without holding m_server_mtx
    // and read unlocked by is_started()/setPort() and the health-check loop,
    // so it must be atomic.
    std::atomic<bool> start_http_server = false;
    
    // 添加自动健康检查相关成员
    boost::thread m_health_check_thread;
    bool          m_health_check_enabled = false;
    int           m_health_check_interval = 5000; // 5秒检查一次
    mutable std::mutex m_health_check_mutex;  // 保护健康检查相关变量
    std::condition_variable m_health_check_cv;  // 条件变量，用于精确控制间隔
    bool          m_restart_requested = false;  // 添加重启请求标志
    
    // 添加重启检查线程
    boost::thread m_restart_check_thread;
    bool          m_restart_check_enabled = false;

    bool is_started() { return start_http_server; }
    void start();
    void stop();
    void restart();  // 添加重启方法
    bool is_healthy();  // 添加健康检查方法
    void start_health_check();  // 启动健康检查
    void stop_health_check();   // 停止健康检查
    void set_health_check_interval(int interval_ms);  // 设置健康检查间隔
    int get_health_check_interval() const;  // 获取健康检查间隔
    bool is_health_check_enabled() const;   // 检查健康检查是否启用
    bool is_restart_requested() const;      // 检查是否有重启请求
    void start_restart_check();  // 启动重启检查
    void stop_restart_check();   // 停止重启检查
    void simulate_crash();       // 模拟服务器崩溃，用于测试重启机制
    void set_request_handler(const std::function<std::shared_ptr<Response>(const std::string&)>& m_request_handler);
    void setPort(boost::asio::ip::port_type new_port) { 
        if (!start_http_server) {  // 只有在服务器未启动时才允许修改端口
            port = new_port; 
        }
    }

    boost::asio::ip::port_type get_port() const { return port; }

    // ---- page server lock-down (see PageServerSecurity.hpp) ----
    // When on, every request must come from one of our web views: Host 127.0.0.1/localhost:<port>,
    // no foreign Origin, and this process's secret as ?edge_page_token=, X-Edge-Page-Token or the
    // cookie set on the first tokened page load. No CORS headers are sent. Off for the login
    // callback servers, which a system browser reaches by redirect.
    void enable_page_security(bool on) { m_page_security = on; }
    bool page_security() const { return m_page_security; }
    std::string page_secret() const;
    // "http://127.0.0.1:<port><path_and_query>" with the secret appended: the URL to load into a
    // web view. Use it for every top-level page load; requests the page then makes itself carry
    // the cookie and need no token.
    std::string page_url(const std::string& path_and_query) const;
    // Adds the secret to url if it points at this server (for URLs a page asks us to open).
    std::string add_token_if_ours(const std::string& url) const;
    // Hands a file to the pages: registers it with page_server::file_grants() and returns
    // "http://127.0.0.1:<port>/localfile/cap/<random id>/<name>". Only granted files (and the
    // installed resources) can be read through /localfile/ and /wcp_download/.
    std::string localfile_url(const std::string& utf8_path) const;

    static std::string map_url_to_file_path(const std::string& url);

    static std::shared_ptr<Response> bbl_auth_handle_request(const std::string& url);

    static std::shared_ptr<Response> web_server_handle_request(const std::string& url);

private:
    std::atomic<bool>  m_page_security{false};
    mutable std::mutex m_secret_mtx;
    std::string        m_secret; // per process; replaced if a restart has to move to another port
    bool               m_bound_once = false;

    class IOServer
    {
    public:
        HttpServer&                        server;
        boost::asio::io_service           io_service;
        boost::asio::ip::tcp::acceptor    acceptor;
        std::set<std::shared_ptr<session>> sessions;

        // 只声明构造函数，不在头文件中定义
        IOServer(HttpServer& server);

        // Binds 127.0.0.1:<port> and listens. Exclusive on Windows (SO_EXCLUSIVEADDRUSE): nobody
        // else can bind the port while we hold it, and a port somebody else holds - even with
        // SO_REUSEADDR, as older builds did - fails instead of being silently shared.
        bool bind_loopback(boost::asio::ip::port_type port, std::string* error);

        void do_accept();
        void start(std::shared_ptr<session> session);
        void stop(std::shared_ptr<session> session);
        void stop_all();
    };
    friend class session;

    // Serializes server_ / m_http_server_thread lifecycle between stop(),
    // restart() and is_healthy() (the latter runs on the health-check thread).
    // The io thread never takes this lock; it only touches the IOServer, whose
    // sessions set is joined before teardown (see HttpServer::stop).
    std::mutex m_server_mtx;

    // Body of start() that runs under m_server_mtx. Deliberately does NOT
    // start the health check: start_health_check() may join a retired
    // health-check thread that is itself blocked in is_healthy() waiting for
    // m_server_mtx, so it must only be called after the lock is released.
    void start_locked();

    std::unique_ptr<IOServer> server_{nullptr};

    std::function<std::shared_ptr<Response>(const std::string&)> m_request_handler{&HttpServer::bbl_auth_handle_request};
};

class session : public std::enable_shared_from_this<session>
{
    HttpServer::IOServer& server;
    boost::asio::ip::tcp::socket socket;

    // Bounded: a client that never ends its header line must not grow the buffer without limit.
    boost::asio::streambuf buff{64 * 1024};
    http_headers           headers;
    int                    header_lines = 0;

    void read_first_line();
    void read_next_line();
    void read_body();

public:
    session(HttpServer::IOServer& server, boost::asio::ip::tcp::socket socket) : server(server), socket(std::move(socket)) {}

    void start();
    void stop();
};

std::string url_get_param(const std::string& url, const std::string& key);

}};

#endif
