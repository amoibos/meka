//-----------------------------------------------------------------------------
// MEKA - dap_handler.cpp
// DAP command handler implementation
//-----------------------------------------------------------------------------

#include "shared.h"
#include "dap_handler.h"
#include "dap_server.h"
#include "dap_json.h"
#include "debugger.h"
#include "file.h"
#include "vmachine.h"
#include "inputs_inject.h"
#include "keyinfo.h"
#include "sk1100.h"
#include <cstring>
#include <cctype>

// Z80 disassembler - declared in debugger.cpp
extern int Z80_Disassemble(char *dst, word addr, bool display_symbols, bool display_symbols_for_current_index_registers, bool resolve_indirect_offsets);

// Forward declarations for debugger functions we need (will be made public in debugger.h)
extern t_debugger_breakpoint* Debugger_BreakPoint_Add(int type, int location, int access_flags, int address_start, int address_end, int auto_delete, const char *desc);
extern void                   Debugger_BreakPoint_Remove(t_debugger_breakpoint *breakpoint);
extern void                   Debugger_BreakPoints_Clear(bool disabled_only);
extern int                    Debugger_Eval_ParseExpression(char **expr, t_debugger_value *result);
extern int                    Debugger_Eval_GetValue(char **src, t_debugger_value *result);

// Z80 register names (for variables responses)
static const char* z80_reg_names[] = {
    "PC", "SP", "AF", "A", "BC", "B", "C", "DE", "D", "E", "HL", "H", "L",
    "IX", "IY", "AF'", "BC'", "DE'", "HL'", "I", "R", "IFF"
};

static u16 get_reg_value(const char* name)
{
    Z80* cpu = &sms.R;
    if (!strcmp(name, "PC"))  return cpu->PC.W;
    if (!strcmp(name, "SP"))  return cpu->SP.W;
    if (!strcmp(name, "AF"))  return cpu->AF.W;
    if (!strcmp(name, "A"))   return cpu->AF.B.h;
    if (!strcmp(name, "BC"))  return cpu->BC.W;
    if (!strcmp(name, "B"))   return cpu->BC.B.h;
    if (!strcmp(name, "C"))   return cpu->BC.B.l;
    if (!strcmp(name, "DE"))  return cpu->DE.W;
    if (!strcmp(name, "D"))   return cpu->DE.B.h;
    if (!strcmp(name, "E"))   return cpu->DE.B.l;
    if (!strcmp(name, "HL"))  return cpu->HL.W;
    if (!strcmp(name, "H"))   return cpu->HL.B.h;
    if (!strcmp(name, "L"))   return cpu->HL.B.l;
    if (!strcmp(name, "IX"))  return cpu->IX.W;
    if (!strcmp(name, "IY"))  return cpu->IY.W;
    if (!strcmp(name, "AF'")) return cpu->AF1.W;
    if (!strcmp(name, "BC'")) return cpu->BC1.W;
    if (!strcmp(name, "DE'")) return cpu->DE1.W;
    if (!strcmp(name, "HL'")) return cpu->HL1.W;
    if (!strcmp(name, "I"))   return cpu->I;
    if (!strcmp(name, "R"))   return cpu->R;
    if (!strcmp(name, "IFF")) return cpu->IFF;
    return 0;
}

static bool set_reg_value(const char* name, u16 value)
{
    Z80* cpu = &sms.R;
    if (!strcmp(name, "PC"))  { cpu->PC.W = value; return true; }
    if (!strcmp(name, "SP"))  { cpu->SP.W = value; return true; }
    if (!strcmp(name, "AF"))  { cpu->AF.W = value; return true; }
    if (!strcmp(name, "A"))   { cpu->AF.B.h = (u8)value; return true; }
    if (!strcmp(name, "BC"))  { cpu->BC.W = value; return true; }
    if (!strcmp(name, "B"))   { cpu->BC.B.h = (u8)value; return true; }
    if (!strcmp(name, "C"))   { cpu->BC.B.l = (u8)value; return true; }
    if (!strcmp(name, "DE"))  { cpu->DE.W = value; return true; }
    if (!strcmp(name, "D"))   { cpu->DE.B.h = (u8)value; return true; }
    if (!strcmp(name, "E"))   { cpu->DE.B.l = (u8)value; return true; }
    if (!strcmp(name, "HL"))  { cpu->HL.W = value; return true; }
    if (!strcmp(name, "H"))   { cpu->HL.B.h = (u8)value; return true; }
    if (!strcmp(name, "L"))   { cpu->HL.B.l = (u8)value; return true; }
    if (!strcmp(name, "IX"))  { cpu->IX.W = value; return true; }
    if (!strcmp(name, "IY"))  { cpu->IY.W = value; return true; }
    if (!strcmp(name, "AF'")) { cpu->AF1.W = value; return true; }
    if (!strcmp(name, "BC'")) { cpu->BC1.W = value; return true; }
    if (!strcmp(name, "DE'")) { cpu->DE1.W = value; return true; }
    if (!strcmp(name, "HL'")) { cpu->HL1.W = value; return true; }
    if (!strcmp(name, "I"))   { cpu->I = (u8)value; return true; }
    if (!strcmp(name, "R"))   { cpu->R = (u8)value; return true; }
    if (!strcmp(name, "IFF")) { cpu->IFF = (u8)value; return true; }
    return false;
}

static char hex_digit(u8 v) {
    v &= 0xF;
    return (v < 10) ? ('0' + v) : ('A' + v - 10);
}

static std::string format_hex(u16 v, int digits) {
    char buf[8];
    if (digits == 2) {
        buf[0] = hex_digit((u8)(v >> 4));
        buf[1] = hex_digit((u8)v);
        buf[2] = 0;
    } else {
        buf[0] = hex_digit((u8)(v >> 12));
        buf[1] = hex_digit((u8)(v >> 8));
        buf[2] = hex_digit((u8)(v >> 4));
        buf[3] = hex_digit((u8)v);
        buf[4] = 0;
    }
    return std::string(buf);
}

//-----------------------------------------------------------------------------
// Response builders
//-----------------------------------------------------------------------------

namespace DAP_Handler {

std::string BuildResponse(int seq, const std::string& command, bool success, const std::string& body_json)
{
    DapJson::Value resp = DapJson::obj();
    DapJson::set(resp, "type", "response");
    DapJson::set(resp, "seq", seq + 1);
    DapJson::set(resp, "request_seq", seq);
    DapJson::set(resp, "success", success);
    DapJson::set(resp, "command", command);
    // Body as raw JSON string - we need to embed it
    // Since body_json is already a JSON string, we build the response manually
    std::string s = "{\"type\":\"response\",\"seq\":" + std::to_string(seq + 1) +
        ",\"request_seq\":" + std::to_string(seq) +
        ",\"success\":" + std::string(success ? "true" : "false") +
        ",\"command\":\"" + DapJson::escape(command) + "\"";
    if (!body_json.empty() && body_json != "{}") {
        s += ",\"body\":" + body_json;
    }
    s += "}";
    return s;
}

std::string BuildErrorResponse(int seq, const std::string& command, const std::string& message)
{
    std::string body = "{\"error\":{\"id\":1,\"format\":\"" + DapJson::escape(message) + "\",\"showUser\":true}}";
    return BuildResponse(seq, command, false, body);
}

std::string BuildEvent(int seq, const std::string& event_type, const std::string& body_json)
{
    std::string s = "{\"type\":\"event\",\"seq\":" + std::to_string(seq) +
        ",\"event\":\"" + DapJson::escape(event_type) + "\"";
    if (!body_json.empty() && body_json != "{}") {
        s += ",\"body\":" + body_json;
    }
    s += "}";
    return s;
}

//-----------------------------------------------------------------------------
// Command handlers
//-----------------------------------------------------------------------------

static int event_seq = 0;
static int next_event_seq() { return ++event_seq; }

static std::string handle_initialize(int seq, const std::string& args_json)
{
    // Build capabilities
    std::string caps =
        "\"supportsConfigurationDoneRequest\":false,"
        "\"supportsFunctionBreakpoints\":true,"
        "\"supportsConditionalBreakpoints\":false,"
        "\"supportsHitConditionalBreakpoints\":false,"
        "\"supportsEvaluateForHovers\":true,"
        "\"supportsStepBack\":false,"
        "\"supportsSetVariable\":true,"
        "\"supportsRestartFrame\":false,"
        "\"supportsGotoTargetsRequest\":false,"
        "\"supportsStepInTargetsRequest\":false,"
        "\"supportsCompletionsRequest\":false,"
        "\"supportsModulesRequest\":false,"
        "\"supportsRestartRequest\":false,"
        "\"supportsExceptionOptions\":false,"
        "\"supportsValueFormattingOptions\":false,"
        "\"supportsExceptionInfoRequest\":false,"
        "\"supportTerminateDebuggee\":false,"
        "\"supportsDelayedStackTraceLoading\":false,"
        "\"supportsLoadedSourcesRequest\":false,"
        "\"supportsLogPoints\":false,"
        "\"supportsTerminateThreadsRequest\":false,"
        "\"supportsSetExpression\":true,"
        "\"supportsTerminateRequest\":false,"
        "\"supportsDataBreakpoints\":false,"
        "\"supportsReadMemoryRequest\":true,"
        "\"supportsDisassembleRequest\":true,"
        "\"supportsCancelRequest\":false,"
        "\"supportsBreakpointLocationsRequest\":false,"
        "\"supportsClipboardContext\":false,"
        "\"supportsSteppingGranularity\":true,"
        "\"supportsInstructionBreakpoints\":true,"
        "\"supportsExceptionFilterOptions\":false,"
        "\"supportsSingleThreadExecutionRequests\":false";

    // Send initialized event
    g_dap_server().SendEvent(BuildEvent(next_event_seq(), "initialized", "{}"));

    return BuildResponse(seq, "initialize", true, "{" + caps + "}");
}

static std::string handle_launch(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);
    std::string program = args["program"].as_string();
    bool stop_on_entry = args["stopOnEntry"].as_bool();

    if (program.empty())
        return BuildErrorResponse(seq, "launch", "No program specified");

    // Set the media image path for Load_ROM
    strncpy(g_env.Paths.MediaImageFile, program.c_str(), FILENAME_LEN - 1);
    g_env.Paths.MediaImageFile[FILENAME_LEN - 1] = '\0';

    // Load the ROM
    bool loaded = Load_ROM(LOAD_MODE_GUI, true); // FIXME: LOAD_MODE_COMMANDLINE?
    if (!loaded)
        return BuildErrorResponse(seq, "launch", "Failed to load ROM: " + program);

    // Enable debugger if not already active
#ifdef MEKA_Z80_DEBUGGER
    if (!Debugger.active)
        Debugger_Switch();
#endif

    // Send initialized event
    g_dap_server().SendEvent(BuildEvent(next_event_seq(), "initialized", "{}"));

    if (stop_on_entry) {
        // Pause immediately
        Machine_Debug_Start();
        g_dap_server().OnMachineHalted();
        g_dap_server().SendEvent(BuildEvent(next_event_seq(), "stopped",
            "{\"reason\":\"entry\",\"threadId\":1}"));
    }

    return BuildResponse(seq, "launch", true, "");
}

static std::string handle_attach(int seq, const std::string& args_json)
{
    (void)args_json;

    // Ensure debugger is active
#ifdef MEKA_Z80_DEBUGGER
    if (!Debugger.active)
        Debugger_Switch();
#endif

    // Send initialized event
    g_dap_server().SendEvent(BuildEvent(next_event_seq(), "initialized", "{}"));

    return BuildResponse(seq, "attach", true, "");
}

static std::string handle_setBreakpoints(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);

    // Get existing breakpoints for clean replacement
    // For simplicity, clear all CPU breakpoints and re-add
    Debugger_BreakPoints_Clear(false);

    DapJson::Value src = args["source"];
    DapJson::Value requested_bps = args["breakpoints"];
    DapJson::Value lines = args["lines"];

    DapJson::Value result_bps = DapJson::arr();

    int src_ref = src["sourceReference"].as_int();
    // If sourceReference == 1, it's disassembly mode -> lines are addresses

    for (size_t i = 0; i < requested_bps.size(); i++)
    {
        int line = requested_bps[i]["line"].as_int();
        int addr;
        if (src_ref == 1) {
            // Disassembly mode: line = address + 1 (1-based)
            addr = line - 1;
        } else {
            // Source file mode: need source map, not supported yet
            // Use line as address for now
            addr = line;
        }

        // Add breakpoint at address
        t_debugger_breakpoint* bp = Debugger_BreakPoint_Add(
            BREAKPOINT_TYPE_BREAK,
            BREAKPOINT_LOCATION_CPU,
            BREAKPOINT_ACCESS_X,
            addr, addr,
            -1,
            nullptr
        );

        DapJson::Value result = DapJson::obj();
        DapJson::set(result, "id", bp ? bp->id : 0);
        DapJson::set(result, "verified", bp != nullptr);
        if (bp) {
            DapJson::set(result, "line", line);
            char addr_str[8];
            snprintf(addr_str, sizeof(addr_str), "%04X", addr);
            DapJson::set(result, "instructionReference", std::string("0x") + addr_str);
        }
        DapJson::push(result_bps, result);
    }

    std::string body = "{\"breakpoints\":" + DapJson::to_string(result_bps) + "}";
    return BuildResponse(seq, "setBreakpoints", true, body);
}

static std::string handle_setFunctionBreakpoints(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);

    // Clear existing breakpoints
    Debugger_BreakPoints_Clear(false);

    DapJson::Value bps = args["breakpoints"];
    DapJson::Value result_bps = DapJson::arr();

    for (size_t i = 0; i < bps.size(); i++)
    {
        std::string name = bps[i]["name"].as_string();
        int addr = -1;

        // Search symbols for this name
        for (t_list* syms = Debugger.symbols; syms != nullptr; syms = syms->next)
        {
            t_debugger_symbol* sym = (t_debugger_symbol*)syms->elem;
            if (sym->name && !strcmp(sym->name, name.c_str()))
            {
                addr = sym->cpu_addr;
                break;
            }
        }

        DapJson::Value result = DapJson::obj();
        DapJson::set(result, "name", name);

        if (addr >= 0)
        {
            t_debugger_breakpoint* bp = Debugger_BreakPoint_Add(
                BREAKPOINT_TYPE_BREAK,
                BREAKPOINT_LOCATION_CPU,
                BREAKPOINT_ACCESS_X,
                addr, addr,
                -1,
                name.c_str()
            );
            DapJson::set(result, "verified", bp != nullptr);
            if (bp) DapJson::set(result, "id", bp->id);
        }
        else
        {
            DapJson::set(result, "verified", false);
            DapJson::set(result, "message", "Symbol not found: " + name);
        }

        DapJson::push(result_bps, result);
    }

    std::string body = "{\"breakpoints\":" + DapJson::to_string(result_bps) + "}";
    return BuildResponse(seq, "setFunctionBreakpoints", true, body);
}

static std::string handle_setInstructionBreakpoints(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);

    Debugger_BreakPoints_Clear(false);

    DapJson::Value bps = args["breakpoints"];
    DapJson::Value result_bps = DapJson::arr();

    for (size_t i = 0; i < bps.size(); i++)
    {
        std::string ref = bps[i]["instructionReference"].as_string();
        int addr = 0;
        if (!ref.empty()) {
            if (ref.compare(0, 2, "0x") == 0 || ref.compare(0, 2, "0X") == 0)
                addr = (int)strtol(ref.c_str() + 2, nullptr, 16);
            else
                addr = (int)strtol(ref.c_str(), nullptr, 16);
        }

        t_debugger_breakpoint* bp = Debugger_BreakPoint_Add(
            BREAKPOINT_TYPE_BREAK,
            BREAKPOINT_LOCATION_CPU,
            BREAKPOINT_ACCESS_X,
            addr, addr,
            -1,
            nullptr
        );

        DapJson::Value result = DapJson::obj();
        DapJson::set(result, "id", bp ? bp->id : 0);
        DapJson::set(result, "verified", bp != nullptr);
        DapJson::push(result_bps, result);
    }

    std::string body = "{\"breakpoints\":" + DapJson::to_string(result_bps) + "}";
    return BuildResponse(seq, "setInstructionBreakpoints", true, body);
}

static std::string handle_continue(int seq, const std::string& args_json)
{
    (void)args_json;
    if (!(g_machine_flags & MACHINE_POWER_ON))
        return BuildErrorResponse(seq, "continue", "Machine not powered on");

    // Disable trap and stepping
    Debugger_SetTrap(-1);
    sms.R.Trace = 0;
    Debugger.stepping = 0;
    Debugger.stepping_trace_after = 0;
    Debugger.stepping_out_enable = false;

    Machine_Debug_Stop();

    // Resume CPU immediately (debugger "C" / Continue). Do not block here:
    // breakpoints report "stopped" asynchronously via Debugger_DAP_HaltCallback.
    g_dap_server().SendEvent(BuildEvent(next_event_seq(), "continued",
        "{\"threadId\":1,\"allThreadsContinued\":true}"));

    return BuildResponse(seq, "continue", true, "{\"allThreadsContinued\":true}");
}

static std::string handle_pause(int seq, const std::string& args_json)
{
    (void)args_json;
    if (!(g_machine_flags & MACHINE_POWER_ON))
        return BuildErrorResponse(seq, "pause", "Machine not powered on");

    Machine_Debug_Start();
    g_dap_server().OnMachineHalted();

    Z80* cpu = &sms.R;
    char pc_str[8];
    snprintf(pc_str, sizeof(pc_str), "0x%04X", cpu->PC.W);
    g_dap_server().SendEvent(BuildEvent(next_event_seq(), "stopped",
        "{\"reason\":\"pause\",\"threadId\":1,\"text\":\"Paused at $" + std::string(pc_str) + "\"}"));

    return BuildResponse(seq, "pause", true, "");
}

static std::string handle_next(int seq, const std::string& args_json)
{
    (void)args_json;
    if (!(g_machine_flags & MACHINE_POWER_ON))
        return BuildErrorResponse(seq, "next", "Machine not powered on");

    // Get address after current instruction
    u16 pc = sms.R.PC.W;
    u16 next_pc = pc + Z80_Disassemble(NULL, pc, false, false, false);

    Debugger_SetTrap(next_pc);
    sms.R.Trace = 0;
    Debugger.stepping = 1;
    Debugger.stepping_trace_after = 0;

    Machine_Debug_Stop();

    bool stopped = g_dap_server().WaitForStop(5000);
    if (stopped)
    {
        Z80* cpu = &sms.R;
        char pc_str[8];
        snprintf(pc_str, sizeof(pc_str), "0x%04X", cpu->PC.W);
        g_dap_server().SendEvent(BuildEvent(next_event_seq(), "stopped",
            "{\"reason\":\"step\",\"threadId\":1,\"text\":\"Stepped to $" + std::string(pc_str) + "\"}"));
    }

    return BuildResponse(seq, "next", true, "");
}

static std::string handle_stepIn(int seq, const std::string& args_json)
{
    (void)args_json;
    if (!(g_machine_flags & MACHINE_POWER_ON))
        return BuildErrorResponse(seq, "stepIn", "Machine not powered on");

    Debugger_SetTrap(-1);
    sms.R.Trace = 1;
    Debugger.stepping = 1;
    Debugger.stepping_trace_after = 1;

    Machine_Debug_Stop();

    bool stopped = g_dap_server().WaitForStop(5000);
    if (stopped)
    {
        Z80* cpu = &sms.R;
        char pc_str[8];
        snprintf(pc_str, sizeof(pc_str), "0x%04X", cpu->PC.W);
        g_dap_server().SendEvent(BuildEvent(next_event_seq(), "stopped",
            "{\"reason\":\"step\",\"threadId\":1,\"text\":\"Step in at $" + std::string(pc_str) + "\"}"));
    }

    return BuildResponse(seq, "stepIn", true, "");
}

static std::string handle_stepOut(int seq, const std::string& args_json)
{
    (void)args_json;
    if (!(g_machine_flags & MACHINE_POWER_ON))
        return BuildErrorResponse(seq, "stepOut", "Machine not powered on");

    Debugger_SetTrap(-1);
    sms.R.Trace = 1;
    Debugger.stepping_out_enable = true;
    Debugger.stepping_out_stack_ref = sms.R.SP.W;
    Debugger.stepping = 1;
    Debugger.stepping_trace_after = 1;

    Machine_Debug_Stop();

    bool stopped = g_dap_server().WaitForStop(5000);
    if (stopped)
    {
        Z80* cpu = &sms.R;
        char pc_str[8];
        snprintf(pc_str, sizeof(pc_str), "0x%04X", cpu->PC.W);
        g_dap_server().SendEvent(BuildEvent(next_event_seq(), "stopped",
            "{\"reason\":\"step\",\"threadId\":1,\"text\":\"Step out at $" + std::string(pc_str) + "\"}"));
    }

    return BuildResponse(seq, "stepOut", true, "");
}

static std::string handle_threads(int seq, const std::string& args_json)
{
    (void)args_json;
    return BuildResponse(seq, "threads", true,
        "{\"threads\":[{\"id\":1,\"name\":\"z80\"}]}");
}

static std::string handle_stackTrace(int seq, const std::string& args_json)
{
    (void)args_json;
    // NOTE: Reading registers works even while machine is running
    // (race condition is acceptable for debugging)

    Z80* cpu = &sms.R;
    u16 pc = cpu->PC.W;
    u16 sp = cpu->SP.W;

    DapJson::Value frames = DapJson::arr();

    // First frame: current PC
    {
        DapJson::Value f = DapJson::obj();
        DapJson::set(f, "id", 0);
        DapJson::set(f, "name", "PC");
        char addr_str[16];
        snprintf(addr_str, sizeof(addr_str), "0x%04X", pc);
        DapJson::set(f, "instructionPointerReference", std::string(addr_str));
        DapJson::set(f, "line", 0);
        DapJson::set(f, "column", 0);
        DapJson::push(frames, f);
    }

    // Walk stack for return addresses
    // Read up to 16 return addresses from the stack
    int frame_id = 1;
    u16 stack_ptr = sp;
    for (int i = 0; i < 16 && stack_ptr <= 0xFFFE; i++, stack_ptr += 2, frame_id++)
    {
        u8 lo = RdZ80_NoHook(stack_ptr);
        u8 hi = RdZ80_NoHook(stack_ptr + 1);
        u16 ret_addr = lo | (hi << 8);

        if (ret_addr == 0x0000)
            break; // likely end of meaningful stack

        DapJson::Value f = DapJson::obj();
        DapJson::set(f, "id", frame_id);
        DapJson::set(f, "name", "RET");
        char addr_str[16];
        snprintf(addr_str, sizeof(addr_str), "0x%04X", ret_addr);
        DapJson::set(f, "instructionPointerReference", std::string(addr_str));
        DapJson::set(f, "line", 0);
        DapJson::set(f, "column", 0);
        DapJson::push(frames, f);
    }

    std::string body = "{\"stackFrames\":" + DapJson::to_string(frames) + ",\"totalFrames\":" + std::to_string(frame_id) + "}";
    return BuildResponse(seq, "stackTrace", true, body);
}

static std::string handle_scopes(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);
    int frame_id = args["frameId"].as_int();

    DapJson::Value scopes = DapJson::arr();

    if (frame_id == 0) {
        // Registers scope
        DapJson::Value scope = DapJson::obj();
        DapJson::set(scope, "name", "CPU Registers");
        DapJson::set(scope, "variablesReference", 1); // Ref ID for registers
        DapJson::set(scope, "expensive", false);
        DapJson::push(scopes, scope);

        // VDP scope
        DapJson::Value vdp_scope = DapJson::obj();
        DapJson::set(vdp_scope, "name", "VDP Registers");
        DapJson::set(vdp_scope, "variablesReference", 2);
        DapJson::set(vdp_scope, "expensive", false);
        DapJson::push(scopes, vdp_scope);
    }

    std::string body = "{\"scopes\":" + DapJson::to_string(scopes) + "}";
    return BuildResponse(seq, "scopes", true, body);
}

static std::string handle_variables(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);
    int var_ref = args["variablesReference"].as_int();

    DapJson::Value vars = DapJson::arr();

    if (var_ref == 1) {
        // CPU Registers
        for (const char* name : z80_reg_names)
        {
            u16 val = get_reg_value(name);
            DapJson::Value v = DapJson::obj();
            DapJson::set(v, "name", name);
            // Determine hex width: names with 2+ chars (or with ') are 16-bit values (or wider)
            int width = (strlen(name) == 1) ? 2 : 4;
            if (strcmp(name, "IFF") == 0) width = 4;
            DapJson::set(v, "value", std::string("0x") + format_hex(val, width));
            DapJson::set(v, "variablesReference", 0);
            DapJson::push(vars, v);
        }
    } else if (var_ref == 2) {
        // VDP Registers
        for (int i = 0; i < 16; i++)
        {
            DapJson::Value v = DapJson::obj();
            char name[8];
            snprintf(name, sizeof(name), "VDP[%d]", i);
            DapJson::set(v, "name", name);
            DapJson::set(v, "value", std::string("0x") + format_hex(sms.VDP[i], 2));
            DapJson::set(v, "variablesReference", 0);
            DapJson::push(vars, v);
        }
    }

    std::string body = "{\"variables\":" + DapJson::to_string(vars) + "}";
    return BuildResponse(seq, "variables", true, body);
}

static bool parse_mem_addr_token(const std::string& token, int& addr_out)
{
    std::string addr_part = token;
    if (addr_part.empty() || addr_part[0] != '@')
        return false;
    addr_part = addr_part.substr(1);
    if (!addr_part.empty() && addr_part[0] == '@')
        addr_part = addr_part.substr(1);
    if (!addr_part.empty() && addr_part[0] == '(')
        addr_part = addr_part.substr(1);
    if (!addr_part.empty() && addr_part.back() == ')')
        addr_part.pop_back();

    if (addr_part.compare(0, 2, "0x") == 0 || addr_part.compare(0, 2, "0X") == 0)
        addr_out = (int)strtol(addr_part.c_str() + 2, nullptr, 16);
    else if (!addr_part.empty() && addr_part[0] == '$')
        addr_out = (int)strtol(addr_part.c_str() + 1, nullptr, 16);
    else
        addr_out = (int)strtol(addr_part.c_str(), nullptr, 16);
    return addr_out >= 0 && addr_out <= 0xFFFF;
}

static std::string handle_evaluate(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);
    std::string expr = args["expression"].as_string();

    // Poke: @($addr)=value  (DAP extension for automation / tests)
    const size_t eq = expr.find('=');
    if (!expr.empty() && expr[0] == '@' && eq != std::string::npos && eq > 0)
    {
        int addr = 0;
        if (parse_mem_addr_token(expr.substr(0, eq), addr))
        {
            std::string val_part = expr.substr(eq + 1);
            int value = 0;
            if (val_part.compare(0, 2, "0x") == 0 || val_part.compare(0, 2, "0X") == 0)
                value = (int)strtol(val_part.c_str() + 2, nullptr, 16);
            else if (!val_part.empty() && val_part[0] == '$')
                value = (int)strtol(val_part.c_str() + 1, nullptr, 16);
            else if (val_part.size() >= 3 && val_part[0] == '\'' && val_part[2] == '\'')
                value = (unsigned char)val_part[1];
            else
                value = (int)strtol(val_part.c_str(), nullptr, 0);

            const u8 old = RdZ80_NoHook((u16)addr);
            WrZ80_NoHook((u16)addr, (u8)(value & 0xFF));
            std::string result = "0x" + format_hex(old, 2);
            return BuildResponse(seq, "evaluate", true,
                "{\"result\":\"" + DapJson::escape(result) + "\",\"variablesReference\":0}");
        }
    }

    // Try to evaluate as a register first
    u16 reg_val;
    bool is_reg = true;
    reg_val = get_reg_value(expr.c_str());
    // If it returned 0 for an unknown name, check explicitly
    if (reg_val == 0 && strcmp(expr.c_str(), "PC") != 0 &&
        strcmp(expr.c_str(), "A") != 0 && strcmp(expr.c_str(), "B") != 0 &&
        strcmp(expr.c_str(), "C") != 0 && strcmp(expr.c_str(), "D") != 0 &&
        strcmp(expr.c_str(), "E") != 0 && strcmp(expr.c_str(), "H") != 0 &&
        strcmp(expr.c_str(), "L") != 0 && strcmp(expr.c_str(), "I") != 0 &&
        strcmp(expr.c_str(), "R") != 0)
    {
        is_reg = false;
    }

    if (is_reg)
    {
        std::string result = "0x" + format_hex(reg_val, 4);
        return BuildResponse(seq, "evaluate", true,
            "{\"result\":\"" + DapJson::escape(result) + "\",\"variablesReference\":0}");
    }

    // Try word memory peek: word @(addr) or word @@(addr)
    if (expr.compare(0, 4, "word") == 0 && (expr[4] == ' ' || expr[4] == '\t'))
    {
        std::string rest = expr.substr(5);
        if (!rest.empty() && rest[0] == '@')
        {
            rest = rest.substr(1);
            if (!rest.empty() && rest[0] == '@')
                rest = rest.substr(1);
            if (!rest.empty() && rest[0] == '(')
                rest = rest.substr(1);
            if (!rest.empty() && rest.back() == ')')
                rest.pop_back();

            int addr = 0;
            if (rest.compare(0, 2, "0x") == 0 || rest.compare(0, 2, "0X") == 0)
                addr = (int)strtol(rest.c_str() + 2, nullptr, 16);
            else if (!rest.empty() && rest[0] == '$')
                addr = (int)strtol(rest.c_str() + 1, nullptr, 16);
            else
                addr = (int)strtol(rest.c_str(), nullptr, 16);

            if (addr >= 0 && addr <= 0xFFFF)
            {
                u16 val = (u16)RdZ80_NoHook((u16)addr) | ((u16)RdZ80_NoHook((u16)(addr + 1)) << 8);
                std::string result = "0x" + format_hex(val, 4);
                return BuildResponse(seq, "evaluate", true,
                    "{\"result\":\"" + DapJson::escape(result) + "\",\"variablesReference\":0}");
            }
        }
    }

    // Try memory peek: @(addr)
    if (!expr.empty() && expr[0] == '@')
    {
        // Could be @(addr) or @addr
        std::string addr_part = expr.substr(1);
        // Handle @@(addr) -> treat as @(addr) single byte
        if (!addr_part.empty() && addr_part[0] == '@')
            addr_part = addr_part.substr(1);
        if (!addr_part.empty() && addr_part[0] == '(')
            addr_part = addr_part.substr(1);
        if (!addr_part.empty() && addr_part.back() == ')')
            addr_part.pop_back();

        int addr = 0;
        if (addr_part.compare(0, 2, "0x") == 0 || addr_part.compare(0, 2, "0X") == 0)
            addr = (int)strtol(addr_part.c_str() + 2, nullptr, 16);
        else if (!addr_part.empty() && addr_part[0] == '$')
            addr = (int)strtol(addr_part.c_str() + 1, nullptr, 16);
        else
            addr = (int)strtol(addr_part.c_str(), nullptr, 16);

        if (addr >= 0 && addr <= 0xFFFF)
        {
            u8 val = RdZ80_NoHook((u16)addr);
            std::string result = "0x" + format_hex(val, 2);
            return BuildResponse(seq, "evaluate", true,
                "{\"result\":\"" + DapJson::escape(result) + "\",\"variablesReference\":0}");
        }
    }

    // Fallback: try to read as hex number
    const char* s = expr.c_str();
    u16 eval_addr;
    if (sscanf(s, "$%hx", &eval_addr) == 1 || sscanf(s, "0x%hx", &eval_addr) == 1)
    {
        u8 val = RdZ80_NoHook(eval_addr);
        std::string result = "0x" + format_hex(val, 2);
        return BuildResponse(seq, "evaluate", true,
            "{\"result\":\"" + DapJson::escape(result) + "\",\"variablesReference\":0}");
    }

    // Try the expression evaluator (may produce console output for syntax errors)
    char* expr_buf = strdup(expr.c_str());
    t_debugger_value value;
    memset(&value, 0, sizeof(value));
    char* p = expr_buf;
    int ret = Debugger_Eval_ParseExpression(&p, &value);
    free(expr_buf);

    if (ret > 0)
    {
        std::string result = "0x" + format_hex((u16)value.data, 4);
        return BuildResponse(seq, "evaluate", true,
            "{\"result\":\"" + DapJson::escape(result) + "\",\"variablesReference\":0}");
    }

    return BuildErrorResponse(seq, "evaluate", "Cannot evaluate: " + expr);
}

static std::string handle_readMemory(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);
    std::string ref = args["memoryReference"].as_string();
    int count = args["count"].as_int();
    if (count <= 0) count = 16;
    if (count > 4096) count = 4096;

    int addr = 0;
    if (ref.compare(0, 2, "0x") == 0 || ref.compare(0, 2, "0X") == 0)
        addr = (int)strtol(ref.c_str() + 2, nullptr, 16);
    else
        addr = (int)strtol(ref.c_str(), nullptr, 16);

    // Read memory using non-hooking accessor
    std::string hex_data;
    hex_data.reserve(count * 3);
    for (int i = 0; i < count; i++)
    {
        u8 val = RdZ80_NoHook((u16)(addr + i));
        if (i > 0) hex_data += ' ';
        hex_data += hex_digit(val >> 4);
        hex_data += hex_digit(val & 0xF);
    }

    // Base64 encode the raw bytes
    std::string raw_bytes;
    raw_bytes.resize(count);
    for (int i = 0; i < count; i++)
        raw_bytes[i] = (char)RdZ80_NoHook((u16)(addr + i));

    // Simple base64 encoding
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64_data;
    for (size_t i = 0; i < raw_bytes.size(); i += 3)
    {
        u32 n = (u8)raw_bytes[i] << 16;
        if (i + 1 < raw_bytes.size()) n |= (u8)raw_bytes[i + 1] << 8;
        if (i + 2 < raw_bytes.size()) n |= (u8)raw_bytes[i + 2];

        b64_data += b64[(n >> 18) & 0x3F];
        b64_data += b64[(n >> 12) & 0x3F];
        b64_data += (i + 1 < raw_bytes.size()) ? b64[(n >> 6) & 0x3F] : '=';
        b64_data += (i + 2 < raw_bytes.size()) ? b64[n & 0x3F] : '=';
    }

    return BuildResponse(seq, "readMemory", true,
        "{\"address\":\"" + DapJson::escape(ref) + "\","
        "\"data\":\"" + b64_data + "\","
        "\"unreadableBytes\":0}");
}

static std::string handle_setVariable(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);
    std::string name = args["name"].as_string();
    std::string value = args["value"].as_string();

    // Parse value (expects hex like 0x1234)
    u16 val = 0;
    if (value.compare(0, 2, "0x") == 0 || value.compare(0, 2, "0X") == 0)
        val = (u16)strtol(value.c_str() + 2, nullptr, 16);
    else if (!value.empty() && value[0] == '$')
        val = (u16)strtol(value.c_str() + 1, nullptr, 16);
    else
        val = (u16)strtol(value.c_str(), nullptr, 16);

    if (set_reg_value(name.c_str(), val))
    {
        std::string new_val = "0x" + format_hex(get_reg_value(name.c_str()),
            (strlen(name.c_str()) <= 2) ? 2 : 4);
        return BuildResponse(seq, "setVariable", true,
            "{\"value\":\"" + DapJson::escape(new_val) + "\"}");
    }

    return BuildErrorResponse(seq, "setVariable", "Unknown register: " + name);
}

static std::string handle_disconnect(int seq, const std::string& args_json)
{
    (void)args_json;
    return BuildResponse(seq, "disconnect", true, "");
}

//-----------------------------------------------------------------------------
// keyboardEvent - inject key into emulation (DAP extension)
//-----------------------------------------------------------------------------

static int resolve_key_from_json(const DapJson::Value& key_val)
{
    if (key_val.is_number())
        return key_val.as_int();

    if (!key_val.is_string())
        return -1;

    const std::string& key = key_val.as_string();
    if (key.empty())
        return -1;

    return Inputs_InjectResolveKey(key.c_str());
}

static std::string handle_keyboardEvent(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);

    int scancode = resolve_key_from_json(args["key"]);
    if (scancode < 0)
        return BuildErrorResponse(seq, "keyboardEvent", "Unknown key");

    std::string action = "press";
    if (args["action"].is_string())
        action = args["action"].as_string();
    else if (args["type"].is_string())
        action = args["type"].as_string();

    int duration_ms = 60;
    if (args["durationMs"].is_number())
        duration_ms = args["durationMs"].as_int();
    else if (args["duration"].is_number())
        duration_ms = args["duration"].as_int();
    if (duration_ms < 1)
        duration_ms = 1;

    for (char& c : action)
        c = (char)tolower((unsigned char)c);

    std::string target = "ppi";
    if (args["target"].is_string())
        target = args["target"].as_string();
    for (char& c : target)
        c = (char)tolower((unsigned char)c);

    const bool use_ppi = (target == "ppi" || target == "matrix" || target == "sc3000")
        && Inputs.SK1100_Enabled;

    if (use_ppi)
    {
        if (action == "down")
            SK1100_InjectAllegroKey(scancode, true);
        else if (action == "up")
            SK1100_InjectAllegroKey(scancode, false);
        else if (action == "press" || action == "tap")
            SK1100_InjectAllegroKeyPress(scancode, duration_ms);
        else
            return BuildErrorResponse(seq, "keyboardEvent", "Unknown action: " + action);
    }
    else if (action == "down")
        Inputs_InjectKey(scancode, true);
    else if (action == "up")
        Inputs_InjectKey(scancode, false);
    else if (action == "press" || action == "tap")
        Inputs_InjectKeyPress(scancode, duration_ms);
    else
        return BuildErrorResponse(seq, "keyboardEvent", "Unknown action: " + action);

    const t_key_info* ki = KeyInfo_FindByScancode(scancode);
    const char* key_name = ki ? ki->name : "?";

    DapJson::Value body = DapJson::obj();
    DapJson::set(body, "key", key_name);
    DapJson::set(body, "scancode", scancode);
    DapJson::set(body, "action", action);
    DapJson::set(body, "durationMs", duration_ms);
    DapJson::set(body, "target", use_ppi ? "ppi" : target);

    return BuildResponse(seq, "keyboardEvent", true, DapJson::to_string(body));
}

static std::string handle_setDebuggerKeycodes(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);

    bool has_enabled = args["enabled"].is_bool() || args["enabled"].is_number()
        || args["enable"].is_bool() || args["enable"].is_number();

    if (has_enabled)
    {
        bool enabled = Debugger_Keycodes_IsEnabled();
        if (args["enabled"].is_bool())
            enabled = args["enabled"].as_bool();
        else if (args["enabled"].is_number())
            enabled = args["enabled"].as_int() != 0;
        else if (args["enable"].is_bool())
            enabled = args["enable"].as_bool();
        else if (args["enable"].is_number())
            enabled = args["enable"].as_int() != 0;

        Debugger_Keycodes_SetEnabled(enabled);
    }

    DapJson::Value body = DapJson::obj();
    DapJson::set(body, "enabled", Debugger_Keycodes_IsEnabled());
    return BuildResponse(seq, "setDebuggerKeycodes", true, DapJson::to_string(body));
}

static std::string handle_resetEmulation(int seq, const std::string& args_json)
{
    (void)args_json;

    if (!(g_machine_flags & MACHINE_POWER_ON))
        return BuildErrorResponse(seq, "resetEmulation", "Machine not powered on");

    Machine_Reset();

    return BuildResponse(seq, "resetEmulation", true, "{\"ok\":true}");
}

static std::string handle_disassemble(int seq, const std::string& args_json)
{
    DapJson::Value args = DapJson::parse(args_json);
    std::string ref = args["memoryReference"].as_string();
    int count = args["instructionCount"].as_int();
    if (count <= 0) count = 8;
    if (count > 64) count = 64;

    int addr = 0;
    if (ref.compare(0, 2, "0x") == 0 || ref.compare(0, 2, "0X") == 0)
        addr = (int)strtol(ref.c_str() + 2, nullptr, 16);
    else
        addr = (int)strtol(ref.c_str(), nullptr, 16);

    DapJson::Value instructions = DapJson::arr();
    int current = addr;

    for (int i = 0; i < count && current <= 0xFFFF; i++)
    {
        char disasm_buf[128];
        int len = Z80_Disassemble(disasm_buf, (word)current, true, false, false);

        DapJson::Value instr = DapJson::obj();
        char addr_str[16];
        snprintf(addr_str, sizeof(addr_str), "0x%04X", current);
        DapJson::set(instr, "address", std::string(addr_str));
        DapJson::set(instr, "instruction", std::string(disasm_buf));

        // Read instruction bytes as base64
        std::string bytes_str;
        for (int b = 0; b < len && current + b <= 0xFFFF; b++) {
            bytes_str += (char)RdZ80_NoHook((u16)(current + b));
        }
        // base64 encode
        static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        for (size_t bi = 0; bi < bytes_str.size(); bi += 3) {
            u32 n = (u8)bytes_str[bi] << 16;
            if (bi + 1 < bytes_str.size()) n |= (u8)bytes_str[bi + 1] << 8;
            if (bi + 2 < bytes_str.size()) n |= (u8)bytes_str[bi + 2];
            b64 += b64c[(n >> 18) & 0x3F];
            b64 += b64c[(n >> 12) & 0x3F];
            b64 += (bi + 1 < bytes_str.size()) ? b64c[(n >> 6) & 0x3F] : '=';
            b64 += (bi + 2 < bytes_str.size()) ? b64c[n & 0x3F] : '=';
        }
        DapJson::set(instr, "instructionBytes", b64);

        DapJson::push(instructions, instr);

        if (len <= 0) break;
        current += len;
    }

    std::string body = "{\"instructions\":" + DapJson::to_string(instructions) + "}";
    return BuildResponse(seq, "disassemble", true, body);
}

//-----------------------------------------------------------------------------
// Main dispatcher
//-----------------------------------------------------------------------------

std::string ProcessRequest(const std::string& command, int seq, const std::string& arguments_json)
{
    if (command == "initialize")
        return handle_initialize(seq, arguments_json);
    if (command == "launch")
        return handle_launch(seq, arguments_json);
    if (command == "attach")
        return handle_attach(seq, arguments_json);
    if (command == "setBreakpoints")
        return handle_setBreakpoints(seq, arguments_json);
    if (command == "setFunctionBreakpoints")
        return handle_setFunctionBreakpoints(seq, arguments_json);
    if (command == "setInstructionBreakpoints")
        return handle_setInstructionBreakpoints(seq, arguments_json);
    if (command == "continue")
        return handle_continue(seq, arguments_json);
    if (command == "pause")
        return handle_pause(seq, arguments_json);
    if (command == "next")
        return handle_next(seq, arguments_json);
    if (command == "stepIn")
        return handle_stepIn(seq, arguments_json);
    if (command == "stepOut")
        return handle_stepOut(seq, arguments_json);
    if (command == "threads")
        return handle_threads(seq, arguments_json);
    if (command == "stackTrace")
        return handle_stackTrace(seq, arguments_json);
    if (command == "scopes")
        return handle_scopes(seq, arguments_json);
    if (command == "variables")
        return handle_variables(seq, arguments_json);
    if (command == "evaluate")
        return handle_evaluate(seq, arguments_json);
    if (command == "readMemory")
        return handle_readMemory(seq, arguments_json);
    if (command == "setVariable")
        return handle_setVariable(seq, arguments_json);
    if (command == "disconnect")
        return handle_disconnect(seq, arguments_json);
    if (command == "disassemble")
        return handle_disassemble(seq, arguments_json);
    if (command == "keyboardEvent" || command == "keyboard")
        return handle_keyboardEvent(seq, arguments_json);
    if (command == "setDebuggerKeycodes" || command == "debuggerKeycodes")
        return handle_setDebuggerKeycodes(seq, arguments_json);
    if (command == "resetEmulation" || command == "reset")
        return handle_resetEmulation(seq, arguments_json);

    return BuildErrorResponse(seq, command, "Unknown command: " + command);
}

} // namespace DAP_Handler
