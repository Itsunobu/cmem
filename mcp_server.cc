#include "mcp_server.h"
#include <iostream>

namespace mcp {

Server::Server(const std::string &name, const std::string &version)
	: name_(name), version_(version)
{
}

void
Server::addTool(Tool tool)
{
	tools_[tool.name] = std::move(tool);
}

json
Server::buildToolSchema(const Tool &tool)
{
	json properties = json::object();
	std::vector<std::string> req;

	for (const auto &p : tool.params) {
		properties[p.name] = {
			{"type", p.type},
			{"description", p.description},
		};
		if (p.required)
			req.push_back(p.name);
	}

	return {
		{"name", tool.name},
		{"description", tool.description},
		{"inputSchema", {
			{"type", "object"},
			{"properties", properties},
			{"required", req},
		}},
	};
}

json
Server::handleInitialize(const json &/*params*/)
{
	return {
		{"protocolVersion", "2024-11-05"},
		{"capabilities", {
			{"tools", json::object()},
		}},
		{"serverInfo", {
			{"name", name_},
			{"version", version_},
		}},
	};
}

json
Server::handleToolsList()
{
	json list = json::array();
	for (const auto &[name, tool] : tools_)
		list.push_back(buildToolSchema(tool));
	return {{"tools", list}};
}

json
Server::handleToolsCall(const json &params)
{
	auto name = params.at("name").get<std::string>();
	auto it = tools_.find(name);
	if (it == tools_.end())
		return makeError(nullptr, -32602, "Unknown tool: " + name);

	json args = params.contains("arguments") ? params["arguments"] : json::object();

	try {
		json result = it->second.handler(args);
		std::string text = result.is_string() ? result.get<std::string>() : result.dump(-1, ' ', false, json::error_handler_t::replace);
		return {
			{"content", json::array({
				{{"type", "text"}, {"text", text}},
			})},
		};
	} catch (const std::exception &e) {
		return {
			{"content", json::array({
				{{"type", "text"}, {"text", std::string("Error: ") + e.what()}},
			})},
			{"isError", true},
		};
	}
}

json
Server::makeResponse(const json &id, const json &result)
{
	return {
		{"jsonrpc", "2.0"},
		{"id", id},
		{"result", result},
	};
}

json
Server::makeError(const json &id, int code, const std::string &msg)
{
	return {
		{"jsonrpc", "2.0"},
		{"id", id},
		{"error", {{"code", code}, {"message", msg}}},
	};
}

void
Server::run()
{
	std::string line;
	while (std::getline(std::cin, line)) {
		if (line.empty())
			continue;

		json req;
		try {
			req = json::parse(line);
		} catch (...) {
			auto err = makeError(nullptr, -32700, "Parse error");
			std::cout << err.dump(-1, ' ', false, json::error_handler_t::replace) << "\n" << std::flush;
			continue;
		}

		json id = req.contains("id") ? req["id"] : json(nullptr);
		std::string method = req.value("method", "");
		json params = req.value("params", json::object());

		json result;
		if (method == "initialize")
			result = handleInitialize(params);
		else if (method == "notifications/initialized")
			continue;  // notification, no response
		else if (method == "tools/list")
			result = handleToolsList();
		else if (method == "tools/call")
			result = handleToolsCall(params);
		else {
			auto err = makeError(id, -32601, "Method not found: " + method);
			std::cout << err.dump(-1, ' ', false, json::error_handler_t::replace) << "\n" << std::flush;
			continue;
		}

		std::cout << makeResponse(id, result).dump(-1, ' ', false, json::error_handler_t::replace) << "\n" << std::flush;
	}
}

} // namespace mcp
