//-----------------------------------------------------------------------------
// MEKA - dap_handler.h
// DAP command handler - bridges DAP protocol to MEKA debugger
//-----------------------------------------------------------------------------

#pragma once

#include <string>

namespace DAP_Handler {

// Process a DAP request. Returns full DAP response JSON string.
// Called from the DAP server thread.
std::string ProcessRequest(const std::string& command, int seq, const std::string& arguments_json);

// Convenience: build a DAP response JSON envelope
std::string BuildResponse(int seq, const std::string& command, bool success, const std::string& body_json);
std::string BuildErrorResponse(int seq, const std::string& command, const std::string& message);
std::string BuildEvent(int seq, const std::string& event_type, const std::string& body_json);

} // namespace DAP_Handler
