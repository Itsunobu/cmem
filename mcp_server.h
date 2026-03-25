#pragma once

#include <string>
#include <vector>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>

namespace mcp {

using json = nlohmann::json;

struct ToolParam {
	std::string name;
	std::string type;
	std::string description;
	bool required = true;
};

struct Tool {
	std::string name;
	std::string description;
	std::vector<ToolParam> params;
	std::function<json(const json &args)> handler;
};

class Server
{
	std::string name_;
	std::string version_;
	std::map<std::string, Tool> tools_;

	json handleInitialize(const json &params);
	json handleToolsList();
	json handleToolsCall(const json &params);
	json makeResponse(const json &id, const json &result);
	json makeError(const json &id, int code, const std::string &msg);
	json buildToolSchema(const Tool &tool);

public:
	Server(const std::string &name, const std::string &version);

	void addTool(Tool tool);
	void run();
};

} // namespace mcp
