#pragma once

#include <core/Json.h>

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>


namespace httplib { class Server; }


// A Model Context Protocol server (MCP, "Streamable HTTP" transport, JSON-RPC
// 2.0) on 127.0.0.1, through which an AI assistant (Claude...) uses
// QuickEdit: it lists the tools of the application and calls them. The
// requests come on a thread of their own; the tools run on the main thread,
// between two frames (see process()).
class McpServer
{
public:
    // Its answer, or nothing while it is not finished (asked again at the
    // next frame: a tool may wait for the decoders, a render, a job).
    using Handler = std::function<std::optional<core::Json>(const std::string& tool, const core::Json& arguments, int attempt)>;

    McpServer();
    ~McpServer();

    // tools: the definitions given to the clients (name, description,
    // inputSchema); instructions: how to use them, given at the start.
    bool start(int port, const core::Json& tools, const std::string& instructions, std::string& error);
    void stop();

    bool running() const { return _running.load(); }
    int port() const { return _port; }

    // Calls done so far, and the name of the last tool (for the interface).
    int calls() const { return _calls.load(); }
    std::string lastTool() const;

    // On the main thread, each frame: the calls waiting, given to handler.
    void process(const Handler& handler);

    // A tool result: text (and an image, PNG data), an error.
    static core::Json textResult(const std::string& text, bool error = false);
    static core::Json jsonResult(const core::Json& value);
    static core::Json imageResult(const std::vector<uint8_t>& png, const std::string& caption);

private:
    struct Call
    {
        std::string tool;
        core::Json arguments;
        int attempt = 0;
        std::promise<core::Json> result;
    };

    // A JSON-RPC message (not a batch): its answer, null for a notification.
    core::Json handle(const core::Json& message);
    core::Json callTool(const std::string& tool, const core::Json& arguments);

    std::unique_ptr<httplib::Server> _server;
    std::thread _thread;
    std::atomic<bool> _running = false;
    int _port = 0;

    core::Json _tools;
    std::string _instructions;

    mutable std::mutex _mutex;
    std::deque<std::shared_ptr<Call>> _waiting;     // Not taken by the main thread yet
    std::string _lastTool;
    std::atomic<int> _calls = 0;

    // Main thread only: the calls not finished.
    std::vector<std::shared_ptr<Call>> _active;
};
