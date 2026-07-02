/**********************************************************************

  Audacity: A Digital Audio Editor

  mod-mcp-server / MCPHttpServer.h

  Phase-0 MCP (Model Context Protocol) HTTP server.

  Binds to 127.0.0.1:4830 and exposes a single endpoint:
      POST /mcp   (JSON-RPC 2.0 / MCP protocol)

  Command execution is delegated to Audacity's ScriptCommandRelay
  (ExecFromWorker) in exactly the same way that mod-script-pipe uses
  its stored tpExecScriptServerFunc pointer.  The HTTP worker thread
  calls that function pointer synchronously; the relay marshals the
  command to the wx main thread via AppCommandEvent + wxSemaphore and
  blocks until the response is ready.

**********************************************************************/

#ifndef __MCP_HTTP_SERVER__
#define __MCP_HTTP_SERVER__

#include <string>
#include <atomic>
#include <memory>
#include <mutex>
#include <chrono>

// Forward-declare the httplib Server to avoid pulling the full header here.
namespace httplib { class Server; }

// Function pointer type from ScriptCommandRelay.h (re-declared here to avoid
// including wx headers in this header).
class wxString;
typedef int (*tpMcpExecFunc)(wxString *pIn, wxString *pOut);

/**
 * MCPHttpServer
 *
 * Owns the cpp-httplib Server instance.  Call Start() from the relay
 * registration callback (the thread that ScriptCommandRelay::StartScriptServer
 * detaches) to enter the blocking listen loop.  Call Stop() from a separate
 * thread (e.g. ModuleTerminate) to break out of that loop.
 */
class MCPHttpServer
{
public:
   /// Default MCP port (localhost-only).
   static constexpr int kDefaultPort = 4830;

   MCPHttpServer();
   ~MCPHttpServer();

   // Not copyable or movable.
   MCPHttpServer(const MCPHttpServer &) = delete;
   MCPHttpServer &operator=(const MCPHttpServer &) = delete;

   /**
    * Store the ExecFromWorker function pointer that ScriptCommandRelay hands
    * to our registration callback.  Must be called before Start().
    */
   void SetExecFunc(tpMcpExecFunc fn);

   /**
    * Register routes and begin listening on 127.0.0.1:<port>.
    * Blocks until Stop() is called (or the server encounters an error).
    * Must be called from a non-GUI thread.
    */
   void Start(int port = kDefaultPort);

   /**
    * Signal the server to stop listening.  Safe to call from any thread.
    */
   void Stop();

private:
   // Execute an Audacity command string via the stored relay pointer.
   // Waits (with a bounded timeout) for the main thread to process the
   // command.  Throws std::runtime_error if a previous command is still in
   // flight (e.g. a modal dialog is blocking Otis) or if this command does
   // not complete within the timeout.
   // Must NOT be called from the wx main/GUI thread.
   std::string ExecCommand(const std::string &cmd);

   // JSON-RPC dispatch helpers — each returns a JSON value (as std::string).
   std::string HandleInitialize(const std::string &id);
   std::string HandlePing(const std::string &id);
   std::string HandleToolsList(const std::string &id);
   std::string HandleToolsCall(const std::string &id,
                               const std::string &toolName,
                               const std::string &argumentsJson);

   // Build a standard JSON-RPC error response.
   static std::string MakeError(const std::string &id, int code,
                                const std::string &message);

   // Build a standard JSON-RPC success response (result already serialised).
   static std::string MakeResult(const std::string &id,
                                 const std::string &resultJson);

   std::unique_ptr<httplib::Server> mServer;
   tpMcpExecFunc mExecFn { nullptr };
   std::timed_mutex mExecMutex;   // serialize relay calls (single-command contract)
   std::atomic<bool> mStopRequested { false };
};

#endif /* End of include guard: __MCP_HTTP_SERVER__ */
