/**********************************************************************

  Audacity: A Digital Audio Editor

  mod-mcp-server / MCPHttpServer.cpp

  Phase-0 MCP HTTP server implementation.

  Transport contract (mirrors PipeServer.cpp / DoSrv / DoSrvMore):
    - One Audacity command per HTTP request (command string in JSON field).
    - Call (*mExecFn)(&wxIn, &wxOut) from the httplib worker thread.
      This blocks synchronously until the wx main thread processes the
      AppCommandEvent and calls Flush() on the ResponseTarget semaphore.
    - The response is accumulated in wxOut.  Because mExecFn is the same
      ExecFromWorker pointer the pipe transport uses, the threading model
      is identical.

  Thread-safety note:
    DoSrv / DoSrvMore in ScripterCallback.cpp use module-level globals
    (aStr, currentLine, currentPosition).  MCPHttpServer does NOT call
    DoSrv/DoSrvMore directly — it calls mExecFn (ExecFromWorker), which
    goes through CommandBuilder + ResponseTarget and is safe from any
    non-GUI thread.  Access to mExecFn itself is serialised by the fact
    that SetExecFunc is called before Start() and never mutated after.
    If concurrent HTTP requests are ever needed, wrap the mExecFn call
    in a std::mutex.

**********************************************************************/

#include "MCPHttpServer.h"

// Pull in the vendored single-header libraries.
// The CMakeLists.txt adds their parent directories to the include path.
#include "httplib.h"
#include "json.hpp"

#include <wx/string.h>

#include <cassert>
#include <string>
#include <stdexcept>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Extract the JSON-RPC "id" field as a string (handles number and string ids).
std::string IdToString(const json &j)
{
   if (!j.contains("id") || j["id"].is_null())
      return "null";
   if (j["id"].is_string())
      return "\"" + j["id"].get<std::string>() + "\"";
   // numeric id — return bare number
   return j["id"].dump();
}

/// True when the Audacity relay response indicates command failure.
bool ResponseIsFailed(const std::string &response)
{
   return response.find("finished: Failed!") != std::string::npos
       || response.find("BatchCommand finished: Failed") != std::string::npos;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// MCPHttpServer
// ---------------------------------------------------------------------------

MCPHttpServer::MCPHttpServer()
   : mServer(std::make_unique<httplib::Server>())
{
}

MCPHttpServer::~MCPHttpServer() = default;

void MCPHttpServer::SetExecFunc(tpMcpExecFunc fn)
{
   mExecFn = fn;
}

// ---------------------------------------------------------------------------
// Command execution (the transport boundary replacement)
// ---------------------------------------------------------------------------

std::string MCPHttpServer::ExecCommand(const std::string &cmd)
{
   // mExecFn is ExecFromWorker — must NOT be called from the wx main thread.
   assert(mExecFn != nullptr);

   // httplib dispatches requests on multiple worker threads; the relay honors a
   // single-command-at-a-time contract (inherited from the pipe transport), so
   // serialize all relay calls.
   std::lock_guard<std::mutex> lock(mExecMutex);

   // The relay expects wxString by pointer.
   wxString wxIn  = wxString::FromUTF8(cmd.c_str(), cmd.size());
   wxString wxOut;

   // This call BLOCKS until the wx main thread processes the AppCommandEvent
   // and posts the wxSemaphore in ResponseTarget::Flush().
   (*mExecFn)(&wxIn, &wxOut);

   // Convert response back to UTF-8 std::string.
   // ToStdString(wxConvUTF8) avoids potential ambiguity in std::string
   // constructor overload resolution with wxScopedCharBuffer.
   return wxOut.ToStdString(wxConvUTF8);
}

// ---------------------------------------------------------------------------
// JSON-RPC method handlers
// ---------------------------------------------------------------------------

std::string MCPHttpServer::HandleInitialize(const std::string &id)
{
   json result = {
      {"protocolVersion", "2025-03-26"},
      {"capabilities", {
         {"tools", {{"listChanged", false}}}
      }},
      {"serverInfo", {
         {"name",    "audacity-mcp"},
         {"version", "0.1.0"}
      }}
   };
   return MakeResult(id, result.dump());
}

std::string MCPHttpServer::HandlePing(const std::string &id)
{
   return MakeResult(id, "{}");
}

std::string MCPHttpServer::HandleToolsList(const std::string &id)
{
   json tools = json::array({
      {
         {"name", "run_command"},
         {"description",
            "Execute an arbitrary Audacity scripting command and return its "
            "response.  The command string follows Audacity's macro syntax: "
            "'CommandName: param1=val1 param2=val2'."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {
               {"command", {
                  {"type",        "string"},
                  {"description", "Full Audacity command line, e.g. "
                                  "'Select: Start=0 End=10'"}
               }}
            }},
            {"required", json::array({"command"})}
         }}
      },
      {
         {"name", "get_info"},
         {"description",
            "Run Audacity's GetInfo command to retrieve metadata about "
            "tracks, clips, labels, commands, menus, preferences, etc."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {
               {"type", {
                  {"type",        "string"},
                  {"description", "Info type: Commands, Menus, Preferences, "
                                  "Tracks, Clips, Envelopes, Labels, "
                                  "Boxes.  Defaults to 'Commands'."},
                  {"default",     "Commands"}
               }},
               {"format", {
                  {"type",        "string"},
                  {"description", "Output format: JSON or LISP.  "
                                  "Defaults to 'JSON'."},
                  {"default",     "JSON"}
               }}
            }},
            {"required", json::array()}
         }}
      }
   });

   json result = {{"tools", tools}};
   return MakeResult(id, result.dump());
}

std::string MCPHttpServer::HandleToolsCall(const std::string &id,
                                           const std::string &toolName,
                                           const std::string &argumentsJson)
{
   // Parse the arguments object passed by the MCP client.
   json args;
   try {
      args = json::parse(argumentsJson.empty() ? "{}" : argumentsJson);
   } catch (const json::parse_error &e) {
      return MakeError(id, -32602,
                       std::string("Invalid arguments JSON: ") + e.what());
   }

   std::string command;

   if (toolName == "run_command") {
      if (!args.contains("command") || !args["command"].is_string()) {
         return MakeError(id, -32602,
                          "run_command requires a 'command' string argument");
      }
      command = args["command"].get<std::string>();

   } else if (toolName == "get_info") {
      std::string type   = args.value("type",   "Commands");
      std::string format = args.value("format", "JSON");
      command = "GetInfo: Type=" + type + " Format=" + format;

   } else {
      return MakeError(id, -32601,
                       "Unknown tool: " + toolName);
   }

   // Execute via the relay — blocks until the main thread responds.
   std::string response;
   try {
      response = ExecCommand(command);
   } catch (const std::exception &e) {
      return MakeError(id, -32603,
                       std::string("Internal relay error: ") + e.what());
   }

   bool failed = ResponseIsFailed(response);

   // Build an MCP tools/call result payload.
   json result = {
      {"content", json::array({
         {
            {"type", "text"},
            {"text", response}
         }
      })},
      {"isError", failed}
   };

   if (failed) {
      result["_meta"] = {{"errorMessage", response}};
   }

   return MakeResult(id, result.dump());
}

// ---------------------------------------------------------------------------
// Static helpers: MakeResult / MakeError
// ---------------------------------------------------------------------------

/*static*/ std::string MCPHttpServer::MakeResult(const std::string &id,
                                                  const std::string &resultJson)
{
   // resultJson is already serialised — embed it verbatim to avoid double-
   // encoding.  We construct the envelope manually.
   return R"({"jsonrpc":"2.0","id":)" + id +
          R"(,"result":)" + resultJson + "}";
}

/*static*/ std::string MCPHttpServer::MakeError(const std::string &id,
                                                 int code,
                                                 const std::string &message)
{
   json envelope = {
      {"jsonrpc", "2.0"},
      {"id",      nullptr},     // will be replaced below
      {"error", {
         {"code",    code},
         {"message", message}
      }}
   };
   // Embed the raw id token (may be a number, a quoted string, or "null").
   std::string s = envelope.dump();
   // Replace the "id":null placeholder with the real id token.
   // This is safe because nlohmann::json serialises null as "null".
   auto pos = s.find(R"("id":null)");
   if (pos != std::string::npos)
      s.replace(pos, 9 /* len("\"id\":null") */, "\"id\":" + id);
   return s;
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

void MCPHttpServer::Start(int port)
{
   // POST /mcp — the single MCP endpoint.
   mServer->Post("/mcp", [this](const httplib::Request &req,
                                httplib::Response &res)
   {
      res.set_header("Content-Type", "application/json");

      // Parse the incoming JSON-RPC envelope.
      json rpc;
      try {
         rpc = json::parse(req.body);
      } catch (const json::parse_error &e) {
         res.status = 400;
         res.set_content(
            MakeError("null", -32700,
                      std::string("Parse error: ") + e.what()),
            "application/json");
         return;
      }

      // Validate minimum JSON-RPC 2.0 shape.
      if (!rpc.contains("method") || !rpc["method"].is_string()) {
         res.status = 400;
         res.set_content(
            MakeError(IdToString(rpc), -32600, "Invalid Request: missing method"),
            "application/json");
         return;
      }

      std::string method = rpc["method"].get<std::string>();
      std::string id     = IdToString(rpc);

      // Dispatch.
      if (method == "initialize") {
         res.set_content(HandleInitialize(id), "application/json");

      } else if (method == "notifications/initialized") {
         // Notification — no response body per MCP spec.
         res.status = 202;
         res.set_content("", "application/json");

      } else if (method == "ping") {
         res.set_content(HandlePing(id), "application/json");

      } else if (method == "tools/list") {
         res.set_content(HandleToolsList(id), "application/json");

      } else if (method == "tools/call") {
         // Extract tool name and arguments.
         std::string toolName;
         std::string argsJson;

         if (rpc.contains("params") && rpc["params"].is_object()) {
            const json &params = rpc["params"];
            if (params.contains("name") && params["name"].is_string())
               toolName = params["name"].get<std::string>();
            if (params.contains("arguments"))
               argsJson = params["arguments"].dump();
         }

         if (toolName.empty()) {
            res.status = 400;
            res.set_content(
               MakeError(id, -32602, "tools/call: missing params.name"),
               "application/json");
            return;
         }

         res.set_content(HandleToolsCall(id, toolName, argsJson),
                         "application/json");

      } else {
         // Method not found.
         res.status = 404;
         res.set_content(
            MakeError(id, -32601, "Method not found: " + method),
            "application/json");
      }
   });

   // Bind strictly to localhost — no external access.
   if (!mServer->listen("127.0.0.1", port)) {
      // listen() returns false if the port is already in use or another error
      // occurs.  Log to stderr; StartScriptServer will retry via its while(true).
      fprintf(stderr,
              "mod-mcp-server: failed to listen on 127.0.0.1:%d\n", port);
   }
}

void MCPHttpServer::Stop()
{
   mServer->stop();
}
