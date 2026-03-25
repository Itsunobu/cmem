#pragma once

#include "mcp_server.h"
#include <cstdint>
#include <memory>
#include <sqlite3.h>
#include <vector>

namespace cmem {

using json = nlohmann::json;

static constexpr int EMBED_DIM = 768;

// --- 機密情報検出 ---
std::vector<std::string> detectSensitive(const std::string &text);
std::string redactSensitive(const std::string &text);

// --- Embedding ---
std::vector<float> getEmbedding(const std::string &ollamaUrl,
                                const std::string &model,
                                const std::string &text);

// --- SQLite ラッパー ---

class MemDB
{
	sqlite3 *db_ = nullptr;

public:
	explicit MemDB(const std::string &path, const std::string &vecExtPath);
	~MemDB();

	MemDB(const MemDB &) = delete;
	MemDB &operator=(const MemDB &) = delete;

	sqlite3 *handle() { return db_; }
	bool valid() const { return db_ != nullptr; }
};

// --- c-mem 本体 ---

class CMem
{
	std::unique_ptr<MemDB> globalDb_;
	std::unique_ptr<MemDB> projectDb_;
	std::string ollamaUrl_;
	std::string embedModel_;
	std::string vecExtPath_;

	sqlite3 *getConn(const std::string &scope);
	std::vector<char> embed(const std::string &text);

	struct SearchRow {
		std::string source;
		int64_t id;
		std::string content;
		std::string tagsJson;
		std::string createdAt;
		float distance;
	};

	std::vector<SearchRow> searchInDb(sqlite3 *db, const std::string &source,
	                                  const std::vector<char> &embedding,
	                                  int limit, const std::string &tag);

	std::string doStore(sqlite3 *conn, const std::string &content,
	                    const json &tags, bool force);

public:
	CMem();

	json toolStore(const json &args);
	json toolSearch(const json &args);
	json toolForget(const json &args);
	json toolList(const json &args);
	json toolUpdate(const json &args);
	json toolCheckSensitive(const json &args);

	void registerTools(mcp::Server &server);
};

} // namespace cmem
