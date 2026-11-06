#include <app/McpServer.h>

#include <core/Base64.h>
#include <core/Guard.h>

#include <config.h>

#include <httplib.h>

#include <SDL3/SDL.h>

#include <chrono>


using namespace core;


namespace {

// The versions of the protocol answered; the client's when it is one of
// them, else the latest.
const char* const ProtocolVersions[] = { "2025-06-18", "2025-03-26", "2024-11-05" };

// A tool may take long (an export, a transcription).
constexpr auto CallTimeout = std::chrono::minutes(30);


Json rpcResult(const Json& id, const Json& result)
{
    Json answer = Json::object();
    answer["jsonrpc"] = "2.0";
    answer["id"] = id;
    answer["result"] = result;
    return answer;
}


Json rpcError(const Json& id, int code, const std::string& message)
{
    Json error = Json::object();
    error["code"] = code;
    error["message"] = message;

    Json answer = Json::object();
    answer["jsonrpc"] = "2.0";
    answer["id"] = id;
    answer["error"] = error;
    return answer;
}


// Requests from a web page (another origin) are refused: only local clients
// (no Origin header, or a local one) may drive the application.
bool localOrigin(const httplib::Request& request)
{
    if (!request.has_header("Origin")) {
        return true;
    }

    const std::string origin = request.get_header_value("Origin");

    for (const char* allowed : { "http://localhost", "http://127.0.0.1", "https://localhost", "https://127.0.0.1" }) {
        if (origin.rfind(allowed, 0) == 0) {
            return true;
        }
    }

    return false;
}

} // namespace


McpServer::McpServer() = default;


McpServer::~McpServer()
{
    stop();
}


bool McpServer::start(int port, const Json& tools, const std::string& instructions, std::string& error)
{
    stop();

    _tools = tools;
    _instructions = instructions;
    _server = std::make_unique<httplib::Server>();

    _server->Post("/mcp", [this](const httplib::Request& request, httplib::Response& response) {
        if (!localOrigin(request)) {
            response.status = 403;
            return;
        }

        Json message;

        try {
            message = Json::parse(request.body);
        }
        catch (const std::exception& e) {
            response.set_content(rpcError(nullptr, -32700, std::string("Parse error: ") + e.what()).dump(0), "application/json");
            return;
        }

        // A batch: the answers of its requests together.
        Json answer;

        if (message.isArray()) {
            answer = Json::array();

            for (const Json& item : message.items()) {
                if (const Json one = handle(item); !one.isNull()) answer.push_back(one);
            }

            if (answer.size() == 0) answer = Json();
        }
        else {
            answer = handle(message);
        }

        // Notifications and answers alone: accepted, nothing to send.
        if (answer.isNull()) {
            response.status = 202;
            return;
        }

        response.set_content(answer.dump(0), "application/json");
    });

    // No stream of messages from the server: only answers to requests.
    _server->Get("/mcp", [](const httplib::Request&, httplib::Response& response) {
        response.status = 405;
    });

    _server->Delete("/mcp", [](const httplib::Request&, httplib::Response& response) {
        response.status = 200;
    });

    if (!_server->bind_to_port("127.0.0.1", port)) {
        error = "Port " + std::to_string(port) + " is not free.";
        _server.reset();
        return false;
    }

    _port = port;
    _running = true;

    _thread = std::thread([this]() {
        guarded("MCP server", [this]() { _server->listen_after_bind(); });
        _running = false;
    });

    SDL_Log("MCP server: http://127.0.0.1:%d/mcp", port);
    return true;
}


void McpServer::stop()
{
    // The calls being done (main thread): answered first, so that the
    // threads of the server waiting for them end.
    for (const std::shared_ptr<Call>& call : _active) {
        call->result.set_value(textResult("QuickEdit stopped its MCP server.", true));
    }

    _active.clear();

    if (_server) {
        _server->stop();
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    _server.reset();
    _running = false;

    // The calls not answered: answered now (their client is told).
    std::scoped_lock lock(_mutex);

    for (const std::shared_ptr<Call>& call : _waiting) {
        call->result.set_value(textResult("QuickEdit stopped its MCP server.", true));
    }

    _waiting.clear();
}


std::string McpServer::lastTool() const
{
    std::scoped_lock lock(_mutex);
    return _lastTool;
}


Json McpServer::handle(const Json& message)
{
    const Json id = message["id"];
    const std::string method = message["method"].toString();
    const bool notification = !message.contains("id");

    if (method.empty()) {
        // An answer of the client (none expected): nothing to do.
        return notification ? Json() : rpcError(id, -32600, "Invalid request");
    }

    if (notification) {
        return Json();
    }

    if (method == "initialize") {
        const std::string asked = message["params"]["protocolVersion"].toString();
        std::string version = ProtocolVersions[0];

        for (const char* known : ProtocolVersions) {
            if (asked == known) version = known;
        }

        Json capabilities = Json::object();
        capabilities["tools"] = Json::object();

        Json info = Json::object();
        info["name"] = "QuickEdit";
        info["version"] = APP_VERSION_STR;

        Json result = Json::object();
        result["protocolVersion"] = version;
        result["capabilities"] = capabilities;
        result["serverInfo"] = info;
        result["instructions"] = _instructions;
        return rpcResult(id, result);
    }

    if (method == "ping") {
        return rpcResult(id, Json::object());
    }

    if (method == "tools/list") {
        Json result = Json::object();
        result["tools"] = _tools;
        return rpcResult(id, result);
    }

    if (method == "tools/call") {
        const std::string tool = message["params"]["name"].toString();
        Json arguments = message["params"]["arguments"];

        if (!arguments.isObject()) {
            arguments = Json::object();
        }

        // Some clients send numbers and booleans as strings ("1920"): read
        // as the type of the schema, instead of falling back to the default.
        for (const Json& definition : _tools.items()) {
            if (definition["name"].toString() != tool) {
                continue;
            }

            const Json& properties = definition["inputSchema"]["properties"];

            for (const auto& [key, value] : Json(arguments).members()) {
                const std::string type = properties[key]["type"].toString();

                if (!value.isString() || (type != "number" && type != "integer" && type != "boolean")) {
                    continue;
                }

                try {
                    const Json parsed = Json::parse(value.toString());

                    if ((type == "boolean") ? parsed.isBool() : parsed.isNumber()) {
                        arguments[key] = parsed;
                    }
                }
                catch (const std::exception&) {
                }
            }
        }

        return rpcResult(id, callTool(tool, arguments));
    }

    return rpcError(id, -32601, "Method not found: " + method);
}


Json McpServer::callTool(const std::string& tool, const Json& arguments)
{
    auto call = std::make_shared<Call>();
    call->tool = tool;
    call->arguments = arguments;
    std::future<Json> result = call->result.get_future();

    {
        std::scoped_lock lock(_mutex);
        _waiting.push_back(call);
        _lastTool = tool;
    }

    _calls++;

    if (result.wait_for(CallTimeout) != std::future_status::ready) {
        return textResult("The tool took too long.", true);
    }

    try {
        return result.get();
    }
    catch (const std::exception& e) {
        return textResult(std::string("The tool failed: ") + e.what(), true);
    }
}


void McpServer::process(const Handler& handler)
{
    {
        std::scoped_lock lock(_mutex);

        while (!_waiting.empty()) {
            _active.push_back(_waiting.front());
            _waiting.pop_front();
        }
    }

    for (auto it = _active.begin(); it != _active.end();) {
        Call& call = **it;
        std::optional<Json> answer;

        try {
            answer = handler(call.tool, call.arguments, call.attempt++);
        }
        catch (const std::exception& e) {
            answer = textResult(e.what(), true);
        }

        if (!answer) {
            ++it;
            continue;
        }

        call.result.set_value(*answer);
        it = _active.erase(it);
    }
}


Json McpServer::textResult(const std::string& text, bool error)
{
    Json item = Json::object();
    item["type"] = "text";
    item["text"] = text;

    Json content = Json::array();
    content.push_back(item);

    Json result = Json::object();
    result["content"] = content;
    result["isError"] = error;
    return result;
}


Json McpServer::jsonResult(const Json& value)
{
    return textResult(value.dump(0));
}


Json McpServer::imageResult(const std::vector<uint8_t>& png, const std::string& caption)
{
    Json image = Json::object();
    image["type"] = "image";
    image["data"] = base64Encode(png);
    image["mimeType"] = "image/png";

    Json text = Json::object();
    text["type"] = "text";
    text["text"] = caption;

    Json content = Json::array();
    content.push_back(image);
    content.push_back(text);

    Json result = Json::object();
    result["content"] = content;
    result["isError"] = false;
    return result;
}
