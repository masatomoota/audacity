/**********************************************************************

  Audacity: A Digital Audio Editor

  mod-mcp-server / MCPServerCallback.cpp

  Module ABI entry points.

  This file mirrors the structure of mod-script-pipe/ScripterCallback.cpp
  exactly, replacing the PipeServer() call with MCPHttpServer::Start().

  Lifecycle:
    1. Audacity's module loader opens the .so/.dylib and calls
       GetVersionString() — provided by the DEFINE_VERSION_CHECK macro.
       The string must equal AUDACITY_VERSION_STRING or the module is refused.

    2. ModuleDispatch(ModuleInitialize) is called.  We call
       ScriptCommandRelay::StartScriptServer(RegMcpServerFunc), which
       detaches a std::thread running:
           while (true) { RegMcpServerFunc(ExecFromWorker); }

    3. On that background thread, RegMcpServerFunc receives the
       ExecFromWorker function pointer (tpExecScriptServerFunc), stores it,
       and calls gMcpServer.Start() which enters the httplib listen loop.
       This mirrors exactly what PipeServer() does for the pipe transport.

    4. If gMcpServer.Start() ever returns (e.g. Stop() called on shutdown),
       the while(true) in StartScriptServer re-invokes RegMcpServerFunc,
       which calls gMcpServer.Start() again.  To avoid a tight restart loop
       on permanent failure we check a shutdown flag.

    5. ModuleDispatch(AppQuiting) calls gMcpServer.Stop(), which signals
       httplib to unblock the listen loop, allowing the thread to exit
       cleanly on the next StartScriptServer iteration.

  Threading model (CRITICAL):
    The HTTP worker thread calls mExecFn (ExecFromWorker) directly.
    ExecFromWorker posts an AppCommandEvent to the wx main thread via
    wxTheApp->AddPendingEvent() and BLOCKS on a wxSemaphore.  The main
    thread processes the command and calls ResponseTarget::Flush() which
    posts the semaphore, unblocking the HTTP thread.  This is safe and
    is the same mechanism the pipe transport uses.  Never call ExecFromMain
    from the HTTP thread — that would deadlock.

**********************************************************************/

#include <wx/wx.h>
#include <atomic>
#include <thread>
#include <chrono>

#include "MCPHttpServer.h"

// Audacity module ABI headers (in the main Audacity target).
#include "ModuleConstants.h"
#include "commands/ScriptCommandRelay.h"

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

/// The single server instance for this module.
static MCPHttpServer gMcpServer;

/// Set to true once AppQuiting has been received so RegMcpServerFunc can
/// exit instead of restarting the server.
static std::atomic<bool> gShuttingDown { false };

// ---------------------------------------------------------------------------
// Forward declaration of the registration callback (defined below)
// ---------------------------------------------------------------------------
extern "C" int DLL_API RegMcpServerFunc(tpExecScriptServerFunc pFn);

// ---------------------------------------------------------------------------
// ABI entry points
// ---------------------------------------------------------------------------

/// Entry point 1 of 2 — version gate.
/// The DEFINE_VERSION_CHECK macro expands to:
///   extern "C" { DLL_API const wchar_t * GetVersionString() { return AUDACITY_VERSION_STRING; } }
DEFINE_VERSION_CHECK

/// Entry point 2 of 2 — lifecycle dispatcher.
extern "C" DLL_API int ModuleDispatch(ModuleDispatchTypes type)
{
   switch (type) {
   case ModuleInitialize:
      // Wire this module into the ScriptCommandRelay.  StartScriptServer
      // detaches a thread that calls RegMcpServerFunc(ExecFromWorker) in a
      // while(true) loop — mirroring what mod-script-pipe does.
      ScriptCommandRelay::StartScriptServer(RegMcpServerFunc);
      break;

   case AppQuiting:
      // Signal the server to stop so the relay thread can exit.
      gShuttingDown.store(true, std::memory_order_release);
      gMcpServer.Stop();
      break;

   case ModuleTerminate:
      // Belt-and-suspenders: ensure the server is stopped if AppQuiting
      // was not received (e.g. abnormal shutdown path).
      gShuttingDown.store(true, std::memory_order_release);
      gMcpServer.Stop();
      break;

   default:
      break;
   }
   return 1;
}

// ---------------------------------------------------------------------------
// Registration callback — the tpRegScriptServerFunc the relay calls
// ---------------------------------------------------------------------------

/// Called by ScriptCommandRelay's background thread, passing ExecFromWorker
/// as pFn.  Must block for the server lifetime (mirrors RegScriptServerFunc
/// in ScripterCallback.cpp).
///
/// The while(true) wrapper in StartScriptServer means this function will be
/// called again if it ever returns.  We guard against tight restart loops
/// after shutdown by checking gShuttingDown.
extern "C" int DLL_API RegMcpServerFunc(tpExecScriptServerFunc pFn)
{
   if (pFn == nullptr)
      return 4;

   if (gShuttingDown.load(std::memory_order_acquire)) {
      // StartScriptServer wraps this in while(true); after shutdown, sleep so we
      // do not busy-spin until the process exits.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      return 4;   // Do not restart after intentional shutdown.
   }

   // Store the ExecFromWorker function pointer so the HTTP handlers can use it.
   // tpExecScriptServerFunc and tpMcpExecFunc share the same underlying
   // signature; cast explicitly to bridge the two independent typedefs.
   gMcpServer.SetExecFunc(reinterpret_cast<tpMcpExecFunc>(pFn));

   // Enter the blocking HTTP listen loop on this thread.
   // Returns when gMcpServer.Stop() is called (AppQuiting / ModuleTerminate).
   gMcpServer.Start(MCPHttpServer::kDefaultPort);

   return 4;
}
