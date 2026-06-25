#[[
mod-mcp-server — Phase-0 MCP (Model Context Protocol) HTTP server for Audacity.

Exposes POST /mcp (JSON-RPC 2.0, localhost:4830) and proxies commands into
Audacity through ScriptCommandRelay, exactly as mod-script-pipe does via named
pipes.  Header-only dependencies (httplib.h, nlohmann json.hpp) must be vendored
into the lib/ sub-directories by the build manager before configuring.
]]

set( SOURCES
   MCPServerCallback.cpp
   MCPHttpServer.cpp
)

set( DEFINES
   PRIVATE
      BUILDING_MCP_SERVER

      # Prevent wxWidgets debug.h from emitting Windows pragma-lib directives
      # for the wrong wx build variant (same reason as mod-script-pipe).
      wxDEBUG_LEVEL=0
)

set( LIBRARIES
   PRIVATE
      Audacity
)

audacity_module( mod-mcp-server "${SOURCES}" "${LIBRARIES}"
   "${DEFINES}" "" )

# The audacity_module macro already adds CMAKE_CURRENT_SOURCE_DIR to the include
# path (for our own headers).  Add the vendored single-header library directories
# explicitly AFTER the macro call, targeting the module by name.
target_include_directories( mod-mcp-server
   PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/lib/cpp-httplib"
      "${CMAKE_CURRENT_SOURCE_DIR}/lib/nlohmann-json"
)
