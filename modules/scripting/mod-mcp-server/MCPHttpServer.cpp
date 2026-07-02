/**********************************************************************

  Audacity: A Digital Audio Editor

  mod-mcp-server / MCPHttpServer.cpp

  Phase-0 MCP HTTP server implementation.

  Transport contract (mirrors PipeServer.cpp / DoSrv / DoSrvMore):
    - One Audacity command per HTTP request (command string in JSON field).
    - Call (*mExecFn)(&wxIn, &wxOut) from a detached relay thread.
      This blocks synchronously until the wx main thread processes the
      AppCommandEvent and calls Flush() on the ResponseTarget semaphore.
    - The response is accumulated in wxOut.  Because mExecFn is the same
      ExecFromWorker pointer the pipe transport uses, the threading model
      is identical.

  Relay timeout (critical — see ExecCommand):
    mExecFn can block indefinitely (e.g. a modal dialog is waiting for a
    human).  If the httplib worker thread called it synchronously and held
    mExecMutex the whole time, one wedged command would starve every future
    HTTP request forever.  Instead ExecCommand hands the relay call off to a
    detached std::thread that owns the (already-acquired) mutex lock for its
    lifetime, and the HTTP thread waits on a std::future with a bounded
    timeout.  If the previous relay call is still in flight, a subsequent
    ExecCommand fails fast (try_lock_for) rather than queuing forever.

  Thread-safety note:
    DoSrv / DoSrvMore in ScripterCallback.cpp use module-level globals
    (aStr, currentLine, currentPosition).  MCPHttpServer does NOT call
    DoSrv/DoSrvMore directly — it calls mExecFn (ExecFromWorker), which
    goes through CommandBuilder + ResponseTarget and is safe from any
    non-GUI thread.  Access to mExecFn itself is serialised by the fact
    that SetExecFunc is called before Start() and never mutated after.
    Concurrent relay calls are serialised via mExecMutex (see ExecCommand).

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
#include <thread>
#include <future>
#include <chrono>
#include <memory>

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
   // dump() already produces a properly quoted/escaped JSON token for both
   // string and numeric ids.
   return j["id"].dump();
}

/// Trim leading/trailing ASCII whitespace (mirrors what CommandBuilder emits
/// around empty/blank responses).
std::string Trim(const std::string &s)
{
   const char *ws = " \t\r\n";
   auto start = s.find_first_not_of(ws);
   if (start == std::string::npos)
      return "";
   auto end = s.find_last_not_of(ws);
   return s.substr(start, end - start + 1);
}

/// True when the Audacity relay response indicates command failure.
bool ResponseIsFailed(const std::string &response)
{
   std::string trimmed = Trim(response);
   return trimmed.empty()
       || response.find("finished: Failed!") != std::string::npos
       || response.find("BatchCommand finished: Failed") != std::string::npos
       || response.find("Syntax error") != std::string::npos
       || response.find("Unrecognized parameter") != std::string::npos
       || response.find("Parameter string is missing") != std::string::npos
       || response.find("Invalid value for parameter") != std::string::npos;
}

/// Bundles the in/out strings and completion signal for a single relay call
/// that runs on a detached thread.  Held via shared_ptr so that a timed-out
/// ExecCommand() can walk away while the relay thread is still writing into
/// it without triggering a use-after-free.
struct RelayJob
{
   wxString in;
   wxString out;
   std::promise<void> done;
};

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
   // serialize all relay calls.  Fail fast instead of blocking forever if a
   // previous command is still stuck (e.g. a modal dialog is waiting on a
   // human) — otherwise every httplib worker thread would eventually wedge
   // on this mutex and the whole MCP server would appear dead.
   std::unique_lock<std::timed_mutex> lock(mExecMutex, std::defer_lock);
   if (!lock.try_lock_for(std::chrono::seconds(5))) {
      throw std::runtime_error(
         "Previous command still executing — Otis may be showing a modal "
         "dialog that needs human attention. Retry later.");
   }

   // Run the actual relay call on a detached thread that owns the lock for
   // its lifetime.  If the relay call itself hangs, this ExecCommand() call
   // still returns (via the future timeout below) and the lock is only
   // released when the relay call eventually completes — so the fail-fast
   // above is what keeps subsequent requests unblocked.
   auto job = std::make_shared<RelayJob>();
   job->in = wxString::FromUTF8(cmd.c_str(), cmd.size());
   std::future<void> future = job->done.get_future();

   std::thread([job, fn = mExecFn, lk = std::move(lock)]() mutable {
      // This call BLOCKS until the wx main thread processes the
      // AppCommandEvent and posts the wxSemaphore in ResponseTarget::Flush().
      (*fn)(&job->in, &job->out);
      job->done.set_value();
      // lk (and therefore mExecMutex) is released here, when the thread
      // object is destroyed at the end of this lambda.
   }).detach();

   if (future.wait_for(std::chrono::seconds(300)) == std::future_status::timeout) {
      throw std::runtime_error(
         "Command timed out after 300s (a modal dialog may be blocking "
         "Otis). The command may still complete in the background.");
   }

   // Convert response back to UTF-8 std::string.
   // ToStdString(wxConvUTF8) avoids potential ambiguity in std::string
   // constructor overload resolution with wxScopedCharBuffer.
   return job->out.ToStdString(wxConvUTF8);
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
            "'CommandName: param1=val1 param2=val2'.  Values containing "
            "spaces MUST be double-quoted, e.g. Import2: "
            "Filename=\\\"/path with spaces/x.wav\\\".  Export2 exports the "
            "current time selection, or the whole project when nothing is "
            "selected."},
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

   if (!args.is_object()) {
      return MakeError(id, -32602,
                       "tools/call arguments must be a JSON object");
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

   // Give the LLM a concrete hint when the relay returned nothing — this
   // typically means there is no active project window to run commands
   // against, rather than a genuine command failure.
   std::string displayText = response;
   if (Trim(response).empty()) {
      displayText = "Empty response from Otis — is a project window open?";
   }

   // Build an MCP tools/call result payload.
   json result = {
      {"content", json::array({
         {
            {"type", "text"},
            {"text", displayText}
         }
      })},
      {"isError", failed}
   };

   if (failed) {
      result["_meta"] = {{"errorMessage", displayText}};
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

   // If Stop() was already requested (e.g. shutdown raced us before we got
   // here), don't start listening at all — otherwise we could enter the
   // blocking listen() loop just after the shutdown signal was sent, and
   // nothing would ever call Stop() again to unblock it.
   if (mStopRequested.load(std::memory_order_acquire))
      return;

   // Bind strictly to localhost — no external access.
   if (!mServer->listen("127.0.0.1", port)) {
      // listen() returns false if the port is already in use or another error
      // occurs.  Log to stderr, then sleep before returning so the
      // while(true) in StartScriptServer's caller doesn't busy-spin retrying
      // the bind immediately.
      fprintf(stderr,
              "mod-mcp-server: failed to listen on 127.0.0.1:%d\n", port);
      std::this_thread::sleep_for(std::chrono::seconds(3));
   }
}

void MCPHttpServer::Stop()
{
   mStopRequested.store(true, std::memory_order_release);
   mServer->stop();
}
