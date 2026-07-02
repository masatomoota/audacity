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
#include <sstream>
#include <vector>

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

/// Quote a command parameter value for embedding in an Audacity macro command
/// string.  Always wraps the value in double quotes (safe even when the value
/// has no spaces) and escapes embedded backslashes and double quotes so the
/// wxCMD_LINE_SPLIT_UNIX-based command-line splitter round-trips it exactly.
/// Backslashes must be escaped FIRST, otherwise escaping the quotes would
/// double-escape the backslashes just inserted.
std::string QuoteParam(const std::string &value)
{
   std::string out;
   out.reserve(value.size() + 2);
   out.push_back('"');
   for (char c : value) {
      if (c == '\\' || c == '"')
         out.push_back('\\');
      out.push_back(c);
   }
   out.push_back('"');
   return out;
}

/// Format a double for embedding in a command string.  Avoids trailing zeros
/// / excessive precision from naive std::to_string (e.g. "0.800000").
std::string FormatNumber(double v)
{
   // Integral values print without a decimal point (e.g. "10" not "10.0"),
   // matching how these parameters typically appear in hand-written macro
   // commands and existing GetInfo output.
   if (v == static_cast<long long>(v)) {
      return std::to_string(static_cast<long long>(v));
   }
   std::ostringstream oss;
   oss.precision(9);
   oss << v;
   return oss.str();
}

/// True if `j[key]` is present and is a JSON string.
bool HasString(const json &j, const char *key)
{
   return j.contains(key) && j[key].is_string();
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

std::string MCPHttpServer::RunCommandSequence(
   const std::vector<std::string> &commands, bool *outFailed)
{
   std::string lastResponse;
   for (size_t i = 0; i < commands.size(); ++i) {
      const std::string &cmd = commands[i];
      std::string response = ExecCommand(cmd);
      if (ResponseIsFailed(response)) {
         *outFailed = true;
         std::string displayText = Trim(response).empty()
            ? "Empty response from Otis — is a project window open?"
            : response;
         return "Step " + std::to_string(i + 1) + "/" +
                std::to_string(commands.size()) + " failed: '" + cmd +
                "' -> " + displayText;
      }
      lastResponse = response;
   }
   *outFailed = false;
   return Trim(lastResponse).empty()
      ? "OK"
      : lastResponse;
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
            "selected.  Prefer the dedicated tools (import_audio, "
            "export_audio, select_audio, list_tracks, generate_tone, "
            "apply_effect, set_track, remove_track) for common operations — "
            "they handle track/selection setup safely.  Use run_command only "
            "for operations not covered by a dedicated tool."},
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
      },
      {
         {"name", "import_audio"},
         {"description",
            "Import an audio file into Otis as a new track."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {
               {"path", {
                  {"type",        "string"},
                  {"description", "Absolute path to the audio file to import."}
               }}
            }},
            {"required", json::array({"path"})}
         }}
      },
      {
         {"name", "export_audio"},
         {"description",
            "Export audio to a file.  When both 'start' and 'end' are given, "
            "that time range is selected and exported; otherwise the whole "
            "project is exported (Export2's default when nothing is "
            "selected)."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {
               {"path", {
                  {"type",        "string"},
                  {"description", "Absolute path to write the exported file to."}
               }},
               {"num_channels", {
                  {"type",        "integer"},
                  {"description", "Number of output channels (1=mono, 2=stereo)."},
                  {"default",     2}
               }},
               {"start", {
                  {"type",        "number"},
                  {"description", "Selection start time in seconds.  Must be "
                                  "given together with 'end'."}
               }},
               {"end", {
                  {"type",        "number"},
                  {"description", "Selection end time in seconds.  Must be "
                                  "given together with 'start'."}
               }}
            }},
            {"required", json::array({"path"})}
         }}
      },
      {
         {"name", "select_audio"},
         {"description",
            "Set the time and/or track selection.  Use this instead of "
            "run_command's composite Select: to avoid its side effect of "
            "resetting selection state unexpectedly."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {
               {"mode", {
                  {"type",        "string"},
                  {"description", "'range' selects a time/track range "
                                  "(default), 'all' selects everything, "
                                  "'none' clears the selection."},
                  {"default",     "range"}
               }},
               {"start", {
                  {"type",        "number"},
                  {"description", "Selection start time in seconds (mode=range)."}
               }},
               {"end", {
                  {"type",        "number"},
                  {"description", "Selection end time in seconds (mode=range)."}
               }},
               {"track", {
                  {"type",        "integer"},
                  {"description", "0-based track index to select (mode=range)."}
               }},
               {"track_count", {
                  {"type",        "integer"},
                  {"description", "Number of consecutive tracks to select, "
                                  "starting at 'track' (mode=range)."},
                  {"default",     1}
               }}
            }},
            {"required", json::array()}
         }}
      },
      {
         {"name", "list_tracks"},
         {"description",
            "List all tracks in the current project (name, selected, "
            "focused, kind, start/end time, pan, volume, channels, solo, "
            "mute).  The array order matches the 0-based track index used by "
            "select_audio/set_track/remove_track's 'track' parameter."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"required", json::array()}
         }}
      },
      {
         {"name", "generate_tone"},
         {"description",
            "Generate a tone (sine/square/sawtooth/triangle) safely — no "
            "pre-existing track or selection required.  If 'track' is "
            "omitted, a new mono track is created for the tone; if given, "
            "the tone is generated into that existing track (replacing its "
            "content over the generated duration)."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {
               {"frequency", {
                  {"type",        "number"},
                  {"description", "Tone frequency in Hz."},
                  {"default",     440}
               }},
               {"amplitude", {
                  {"type",        "number"},
                  {"description", "Tone amplitude, 0..1."},
                  {"default",     0.8}
               }},
               {"duration", {
                  {"type",        "number"},
                  {"description", "Tone duration in seconds."},
                  {"default",     10}
               }},
               {"waveform", {
                  {"type",        "string"},
                  {"description", "Sine, Square, Sawtooth, or Triangle."},
                  {"default",     "Sine"}
               }},
               {"track", {
                  {"type",        "integer"},
                  {"description", "0-based index of an existing track to "
                                  "generate into.  Omit to create a new "
                                  "mono track."}
               }}
            }},
            {"required", json::array()}
         }}
      },
      {
         {"name", "apply_effect"},
         {"description",
            "Apply a named Audacity effect with parameters (e.g. name="
            "'Amplify', params={\"Ratio\": 0.5}).  Select the target audio "
            "first with select_audio — apply_effect does not change the "
            "selection."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {
               {"name", {
                  {"type",        "string"},
                  {"description", "Effect command name, e.g. 'Amplify', "
                                  "'Normalize', 'FadeOut'."}
               }},
               {"params", {
                  {"type",        "object"},
                  {"description", "Effect parameters as key/value pairs.  "
                                  "Values must be strings, numbers, or "
                                  "booleans (no nested objects/arrays)."}
               }}
            }},
            {"required", json::array({"name"})}
         }}
      },
      {
         {"name", "set_track"},
         {"description",
            "Update a track's name, gain, pan, mute, or solo state.  Only "
            "the fields provided are changed."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {
               {"track", {
                  {"type",        "integer"},
                  {"description", "0-based track index (see list_tracks)."}
               }},
               {"name", {
                  {"type",        "string"},
                  {"description", "New track name."}
               }},
               {"gain_db", {
                  {"type",        "number"},
                  {"description", "Track volume in dB, -36..36."}
               }},
               {"pan", {
                  {"type",        "number"},
                  {"description", "Track pan, -100 (left) .. 100 (right)."}
               }},
               {"mute", {
                  {"type",        "boolean"},
                  {"description", "Mute (true) or unmute (false) the track."}
               }},
               {"solo", {
                  {"type",        "boolean"},
                  {"description", "Solo (true) or unsolo (false) the track."}
               }}
            }},
            {"required", json::array({"track"})}
         }}
      },
      {
         {"name", "remove_track"},
         {"description", "Delete a track from the project."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {
               {"track", {
                  {"type",        "integer"},
                  {"description", "0-based track index (see list_tracks)."}
               }}
            }},
            {"required", json::array({"track"})}
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

   std::vector<std::string> commands;

   if (toolName == "run_command") {
      if (!args.contains("command") || !args["command"].is_string()) {
         return MakeError(id, -32602,
                          "run_command requires a 'command' string argument");
      }
      commands.push_back(args["command"].get<std::string>());

   } else if (toolName == "get_info") {
      std::string type   = args.value("type",   "Commands");
      std::string format = args.value("format", "JSON");
      commands.push_back("GetInfo: Type=" + type + " Format=" + format);

   } else if (toolName == "import_audio") {
      if (!HasString(args, "path")) {
         return MakeError(id, -32602,
                          "import_audio requires a 'path' string argument");
      }
      commands.push_back(
         "Import2: Filename=" + QuoteParam(args["path"].get<std::string>()));

   } else if (toolName == "export_audio") {
      if (!HasString(args, "path")) {
         return MakeError(id, -32602,
                          "export_audio requires a 'path' string argument");
      }
      if (args.contains("num_channels") && !args["num_channels"].is_number_integer()) {
         return MakeError(id, -32602,
                          "export_audio 'num_channels' must be an integer");
      }
      bool hasStart = args.contains("start");
      bool hasEnd   = args.contains("end");
      if (hasStart != hasEnd) {
         return MakeError(id, -32602,
                          "export_audio 'start' and 'end' must be given together");
      }
      if (hasStart && (!args["start"].is_number() || !args["end"].is_number())) {
         return MakeError(id, -32602,
                          "export_audio 'start'/'end' must be numbers");
      }
      int numChannels = args.value("num_channels", 2);
      if (hasStart && hasEnd) {
         commands.push_back("SelectTime: Start=" +
            FormatNumber(args["start"].get<double>()) + " End=" +
            FormatNumber(args["end"].get<double>()));
      }
      commands.push_back("Export2: Filename=" +
         QuoteParam(args["path"].get<std::string>()) +
         " NumChannels=" + std::to_string(numChannels));

   } else if (toolName == "select_audio") {
      std::string mode = args.value("mode", "range");
      if (mode == "all") {
         commands.push_back("SelectAll:");
      } else if (mode == "none") {
         commands.push_back("SelectNone:");
      } else if (mode == "range") {
         if (args.contains("track")) {
            if (!args["track"].is_number_integer()) {
               return MakeError(id, -32602,
                                "select_audio 'track' must be an integer");
            }
            if (args.contains("track_count") && !args["track_count"].is_number_integer()) {
               return MakeError(id, -32602,
                                "select_audio 'track_count' must be an integer");
            }
            int track      = args["track"].get<int>();
            int trackCount = args.value("track_count", 1);
            commands.push_back("SelectTracks: Track=" + std::to_string(track) +
               " TrackCount=" + std::to_string(trackCount) + " Mode=Set");
         }
         bool hasStart = args.contains("start");
         bool hasEnd   = args.contains("end");
         if (hasStart != hasEnd) {
            return MakeError(id, -32602,
                             "select_audio 'start' and 'end' must be given together");
         }
         if (hasStart) {
            if (!args["start"].is_number() || !args["end"].is_number()) {
               return MakeError(id, -32602,
                                "select_audio 'start'/'end' must be numbers");
            }
            commands.push_back("SelectTime: Start=" +
               FormatNumber(args["start"].get<double>()) + " End=" +
               FormatNumber(args["end"].get<double>()));
         }
         if (commands.empty()) {
            return MakeError(id, -32602,
               "select_audio mode=range requires at least 'track' or "
               "'start'+'end'");
         }
      } else {
         return MakeError(id, -32602,
                          "select_audio 'mode' must be 'range', 'all', or 'none'");
      }

   } else if (toolName == "list_tracks") {
      commands.push_back("GetInfo: Type=Tracks Format=JSON");

   } else if (toolName == "generate_tone") {
      if (args.contains("frequency") && !args["frequency"].is_number()) {
         return MakeError(id, -32602, "generate_tone 'frequency' must be a number");
      }
      if (args.contains("amplitude") && !args["amplitude"].is_number()) {
         return MakeError(id, -32602, "generate_tone 'amplitude' must be a number");
      }
      if (args.contains("duration") && !args["duration"].is_number()) {
         return MakeError(id, -32602, "generate_tone 'duration' must be a number");
      }
      if (args.contains("waveform") && !args["waveform"].is_string()) {
         return MakeError(id, -32602, "generate_tone 'waveform' must be a string");
      }
      if (args.contains("track") && !args["track"].is_number_integer()) {
         return MakeError(id, -32602, "generate_tone 'track' must be an integer");
      }
      double frequency = args.value("frequency", 440.0);
      double amplitude = args.value("amplitude", 0.8);
      double duration  = args.value("duration", 10.0);
      std::string waveform = args.value("waveform", "Sine");

      if (args.contains("track")) {
         int track = args["track"].get<int>();
         commands.push_back("SelectTracks: Track=" + std::to_string(track) +
            " TrackCount=1 Mode=Set");
      } else {
         commands.push_back("NewMonoTrack:");
      }
      commands.push_back("SelectTime: Start=0 End=" + FormatNumber(duration));
      commands.push_back("Tone: Frequency=" + FormatNumber(frequency) +
         " Amplitude=" + FormatNumber(amplitude) + " Waveform=" + waveform);

   } else if (toolName == "apply_effect") {
      if (!HasString(args, "name")) {
         return MakeError(id, -32602,
                          "apply_effect requires a 'name' string argument");
      }
      std::string effectCmd = args["name"].get<std::string>() + ":";
      if (args.contains("params")) {
         if (!args["params"].is_object()) {
            return MakeError(id, -32602, "apply_effect 'params' must be an object");
         }
         for (auto it = args["params"].begin(); it != args["params"].end(); ++it) {
            const json &v = it.value();
            std::string valueStr;
            if (v.is_string()) {
               valueStr = QuoteParam(v.get<std::string>());
            } else if (v.is_boolean()) {
               valueStr = v.get<bool>() ? "1" : "0";
            } else if (v.is_number()) {
               valueStr = FormatNumber(v.get<double>());
            } else {
               return MakeError(id, -32602,
                  "apply_effect params['" + it.key() +
                  "'] must be a string, number, or boolean");
            }
            effectCmd += " " + it.key() + "=" + valueStr;
         }
      }
      commands.push_back(effectCmd);

   } else if (toolName == "set_track") {
      if (!args.contains("track") || !args["track"].is_number_integer()) {
         return MakeError(id, -32602,
                          "set_track requires a 'track' integer argument");
      }
      if (args.contains("name") && !args["name"].is_string()) {
         return MakeError(id, -32602, "set_track 'name' must be a string");
      }
      if (args.contains("gain_db") && !args["gain_db"].is_number()) {
         return MakeError(id, -32602, "set_track 'gain_db' must be a number");
      }
      if (args.contains("pan") && !args["pan"].is_number()) {
         return MakeError(id, -32602, "set_track 'pan' must be a number");
      }
      if (args.contains("mute") && !args["mute"].is_boolean()) {
         return MakeError(id, -32602, "set_track 'mute' must be a boolean");
      }
      if (args.contains("solo") && !args["solo"].is_boolean()) {
         return MakeError(id, -32602, "set_track 'solo' must be a boolean");
      }

      int track = args["track"].get<int>();
      commands.push_back("SelectTracks: Track=" + std::to_string(track) +
         " TrackCount=1 Mode=Set");

      if (args.contains("name")) {
         commands.push_back("SetTrackStatus: Name=" +
            QuoteParam(args["name"].get<std::string>()));
      }

      bool hasAudioParam = args.contains("gain_db") || args.contains("pan") ||
                           args.contains("mute") || args.contains("solo");
      if (hasAudioParam) {
         std::string audioCmd = "SetTrackAudio:";
         if (args.contains("mute")) {
            audioCmd += std::string(" Mute=") + (args["mute"].get<bool>() ? "1" : "0");
         }
         if (args.contains("solo")) {
            audioCmd += std::string(" Solo=") + (args["solo"].get<bool>() ? "1" : "0");
         }
         if (args.contains("gain_db")) {
            audioCmd += " Volume=" + FormatNumber(args["gain_db"].get<double>());
         }
         if (args.contains("pan")) {
            audioCmd += " Pan=" + FormatNumber(args["pan"].get<double>());
         }
         commands.push_back(audioCmd);
      }

   } else if (toolName == "remove_track") {
      if (!args.contains("track") || !args["track"].is_number_integer()) {
         return MakeError(id, -32602,
                          "remove_track requires a 'track' integer argument");
      }
      int track = args["track"].get<int>();
      commands.push_back("SelectTracks: Track=" + std::to_string(track) +
         " TrackCount=1 Mode=Set");
      commands.push_back("RemoveTracks:");

   } else {
      return MakeError(id, -32601,
                       "Unknown tool: " + toolName);
   }

   // Execute via the relay — blocks until the main thread responds.
   std::string displayText;
   bool failed = false;
   try {
      displayText = RunCommandSequence(commands, &failed);
   } catch (const std::exception &e) {
      return MakeError(id, -32603,
                       std::string("Internal relay error: ") + e.what());
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
