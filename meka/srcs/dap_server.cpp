//-----------------------------------------------------------------------------
// MEKA - dap_server.cpp
// DAP (Debug Adapter Protocol) TCP server implementation
//-----------------------------------------------------------------------------

#include "shared.h"
#include "dap_server.h"
#include "dap_json.h"
#include <cstring>

#ifdef _WIN32
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET socket_t;
    #define INVALID_SOCK INVALID_SOCKET
    #define SOCKERR SOCKET_ERROR
    #define sockclose closesocket
#else
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    typedef int socket_t;
    #define INVALID_SOCK (-1)
    #define SOCKERR (-1)
    #define sockclose close
#endif

//-----------------------------------------------------------------------------
// Static helpers
//-----------------------------------------------------------------------------

static void set_nonblocking(socket_t s, bool enable)
{
#ifdef _WIN32
    u_long mode = enable ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (enable)
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
    else
        fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

static int sock_last_error()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static bool sock_would_block()
{
#ifdef _WIN32
    int e = WSAGetLastError();
    return (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS);
#else
    return (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS);
#endif
}

//-----------------------------------------------------------------------------
// Singleton
//-----------------------------------------------------------------------------

DAP_Server& DAP_Server::Instance()
{
    static DAP_Server instance;
    return instance;
}

DAP_Server::DAP_Server()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

DAP_Server::~DAP_Server()
{
    Stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

//-----------------------------------------------------------------------------
// Start / Stop
//-----------------------------------------------------------------------------

bool DAP_Server::Start(int port)
{
    if (running_) return false;

    port_ = port;

    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCK)
        return false;

    // Allow reusing the address
    int optval = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
    addr.sin_port = htons((u_short)port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKERR)
    {
        sockclose(s);
        return false;
    }

    if (listen(s, 1) == SOCKERR)
    {
        sockclose(s);
        return false;
    }

    listen_socket_ = (void*)(intptr_t)s;
    running_ = true;

    accept_thread_ = std::thread(&DAP_Server::AcceptLoop, this);

    ConsolePrintf("DAP server listening on port %d\n", port);
    return true;
}

void DAP_Server::Stop()
{
    if (!running_) return;

    running_ = false;

    // Close listen socket to unblock accept()
    if (listen_socket_)
    {
        sockclose((socket_t)(intptr_t)listen_socket_);
        listen_socket_ = nullptr;
    }

    // Close client socket
    if (client_socket_)
    {
        sockclose((socket_t)(intptr_t)client_socket_);
        client_socket_ = nullptr;
    }

    // Signal stop condition to unblock any waiters
    {
        std::lock_guard<std::mutex> lock(mutex_);
        machine_halted_ = true;
    }
    cv_stopped_.notify_all();

    if (accept_thread_.joinable())
        accept_thread_.join();

    ConsolePrintf("DAP server stopped.\n");
}

//-----------------------------------------------------------------------------
// Update (called from main loop)
//-----------------------------------------------------------------------------

void DAP_Server::Update()
{
    // Nothing needed per-frame currently.
    // DAP communication happens in its own thread.
}

//-----------------------------------------------------------------------------
// Machine halt notification (called from debugger hook)
//-----------------------------------------------------------------------------

void DAP_Server::OnMachineHalted()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        machine_halted_ = true;
    }
    cv_stopped_.notify_all();
}

bool DAP_Server::WaitForStop(int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex_);
    machine_halted_ = false; // Reset before waiting
    return cv_stopped_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this] { return machine_halted_ || !running_; });
}

bool DAP_Server::IsMachineHalted() const
{
    return machine_halted_;
}

bool DAP_Server::IsClientConnected() const
{
    return client_connected_;
}

void DAP_Server::SetRequestHandler(RequestHandler handler)
{
    request_handler_ = std::move(handler);
}

//-----------------------------------------------------------------------------
// Send event (from DAP handler, thread-safe)
//-----------------------------------------------------------------------------

void DAP_Server::SendEvent(const std::string& json_body)
{
    if (!client_socket_) return;

    // DAP wire format: Content-Length header + JSON body
    std::string msg = "Content-Length: " + std::to_string(json_body.size()) + "\r\n\r\n" + json_body;

    std::lock_guard<std::mutex> lock(mutex_);
    SendRaw(msg.c_str(), msg.size());
}

//-----------------------------------------------------------------------------
// Accept loop (runs in separate thread)
//-----------------------------------------------------------------------------

void DAP_Server::AcceptLoop()
{
    while (running_)
    {
        socket_t listen_s = (socket_t)(intptr_t)listen_socket_;
        if (listen_s == INVALID_SOCK)
            break;

        // Accept with timeout so we can check running_ periodically
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_s, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select((int)listen_s + 1, &readfds, nullptr, nullptr, &tv);
        if (sel < 0 || !running_) break;
        if (sel == 0) continue; // timeout, loop back

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        socket_t client = accept(listen_s, (struct sockaddr*)&client_addr, &addr_len);
        if (client == INVALID_SOCK)
        {
            if (!running_) break;
            continue;
        }

        // Disable Nagle's algorithm for lower latency
        int nodelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

        client_socket_ = (void*)(intptr_t)client;
        client_connected_ = true;

        ConsolePrintf("DAP client connected.\n");

        HandleClient();

        client_connected_ = false;
        sockclose(client);
        client_socket_ = nullptr;

        ConsolePrintf("DAP client disconnected.\n");
    }
}

//-----------------------------------------------------------------------------
// Handle client messages
//-----------------------------------------------------------------------------

void DAP_Server::HandleClient()
{
    while (running_)
    {
        std::string json_body;
        if (!ReadMessage(json_body))
        {
            // Connection closed or error
            break;
        }

        if (!request_handler_)
            continue;

        // Parse the DAP request JSON
        DapJson::Value req = DapJson::parse(json_body);
        if (req.is_null() || !req.is_object())
        {
            ConsolePrintf("DAP: invalid JSON request\n");
            continue;
        }

        std::string type = req["type"].as_string();
        std::string command = req["command"].as_string();
        int seq = req["seq"].as_int();

        if (type != "request")
            continue;

        // Extract arguments as JSON string
        std::string args_json;
        if (req.has("arguments"))
        {
            args_json = DapJson::to_string(req["arguments"]);
        }
        else
        {
            args_json = "{}";
        }

        ConsolePrintf("DAP request: %s (seq=%d)\n", command.c_str(), seq);

        // Process via handler (blocking call)
        std::string response_json = request_handler_(command, seq, args_json);

        // Prepend DAP response envelope
        // response_json should be just the "body" contents, or include additional fields
        // The handler returns response_json which is the full DAP response JSON string
        // (including seq, type, request_seq, success, command, body)

        // Send response
        std::string wire = "Content-Length: " + std::to_string(response_json.size()) + "\r\n\r\n" + response_json;
        if (!SendRaw(wire.c_str(), wire.size()))
            break;
    }
}

//-----------------------------------------------------------------------------
// Raw I/O
//-----------------------------------------------------------------------------

bool DAP_Server::SendRaw(const char* data, size_t len)
{
    socket_t s = (socket_t)(intptr_t)client_socket_;
    if (s == INVALID_SOCK) return false;

    size_t sent = 0;
    while (sent < len)
    {
        int n = send(s, data + sent, (int)(len - sent), 0);
        if (n == SOCKERR)
            return false;
        sent += n;
    }
    return true;
}

bool DAP_Server::ReadMessage(std::string& out_json)
{
    socket_t s = (socket_t)(intptr_t)client_socket_;
    if (s == INVALID_SOCK) return false;

    // Read headers until \r\n\r\n
    std::string header;
    while (true)
    {
        char c;
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return false;
        header += c;
        if (header.size() >= 4 &&
            header[header.size()-4] == '\r' &&
            header[header.size()-3] == '\n' &&
            header[header.size()-2] == '\r' &&
            header[header.size()-1] == '\n')
            break;
    }

    // Parse Content-Length
    int content_length = 0;
    // Simple header parsing
    const char* cl = strstr(header.c_str(), "Content-Length:");
    if (cl)
    {
        cl += 15; // skip "Content-Length:"
        while (*cl == ' ') cl++;
        content_length = atoi(cl);
    }

    if (content_length <= 0 || content_length > 1024 * 1024) // max 1MB
        return false;

    // Read body
    std::vector<char> body(content_length);
    size_t received = 0;
    while (received < (size_t)content_length)
    {
        int n = recv(s, body.data() + received, (int)(content_length - received), 0);
        if (n <= 0) return false;
        received += n;
    }

    out_json.assign(body.data(), content_length);
    return true;
}
