//-----------------------------------------------------------------------------
// MEKA - dap_server.h
// DAP (Debug Adapter Protocol) TCP server
//-----------------------------------------------------------------------------

#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>

//-----------------------------------------------------------------------------
// DAP_Server - Singleton TCP server for DAP protocol
//-----------------------------------------------------------------------------
class DAP_Server
{
public:
    static DAP_Server& Instance();

    DAP_Server(const DAP_Server&) = delete;
    DAP_Server& operator=(const DAP_Server&) = delete;

    bool Start(int port);
    void Stop();
    bool IsRunning() const { return running_; }

    // Called from main/gui thread each frame
    void Update();

    // Called from debugger hook when machine halts at breakpoint/step
    void OnMachineHalted();

    // Called from DAP handler to emit events (stopped, output, etc.)
    void SendEvent(const std::string& json_body);

    // Wait for machine to halt (used after continue/step commands)
    bool WaitForStop(int timeout_ms);

    // Check if machine is currently halted
    bool IsMachineHalted() const;

    // Check if a DAP client is connected
    bool IsClientConnected() const;

    // Set a callback for handling incoming DAP requests
    using RequestHandler = std::function<std::string(const std::string& command, int seq, const std::string& arguments_json)>;
    void SetRequestHandler(RequestHandler handler);

    int GetPort() const { return port_; }

private:
    DAP_Server();
    ~DAP_Server();

    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> client_connected_{false};

    // Threading
    std::thread accept_thread_;
    std::mutex mutex_;
    std::condition_variable cv_stopped_;
    bool machine_halted_ = false;

    RequestHandler request_handler_;

    // Socket handles (platform-specific internals in .cpp)
    void* listen_socket_ = nullptr;
    void* client_socket_ = nullptr;

    void AcceptLoop();
    void HandleClient();
    bool SendRaw(const char* data, size_t len);
    bool ReadMessage(std::string& out_json);
};

// Global accessor
inline DAP_Server& g_dap_server() { return DAP_Server::Instance(); }
