#include "c_mem.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <regex>
#include <stdexcept>

#include <curl/curl.h>

namespace cmem {

static std::string
colText(sqlite3_stmt *stmt, int col)
{
	auto *p = sqlite3_column_text(stmt, col);
	return p ? std::string(reinterpret_cast<const char *>(p)) : std::string();
}

static std::string
utf8Truncate(const std::string &s, size_t maxBytes)
{
	if (s.size() <= maxBytes)
		return s;
	size_t pos = maxBytes;
	while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
		--pos;
	return s.substr(0, pos);
}

// =====================================================================
// 機密情報検出
// =====================================================================

struct Pattern {
	std::regex re;
	std::string label;
};

static const std::vector<Pattern> &
sensitivePatterns()
{
	static const std::vector<Pattern> pats = {
		{std::regex(R"((?:api[_-]?key|apikey)\s*[:=]\s*['"]?[\w\-]{20,})", std::regex::icase), "API Key"},
		{std::regex(R"((?:secret|token|password|passwd|pwd)\s*[:=]\s*['"]?[\S]{8,})", std::regex::icase), "Secret/Password/Token"},
		{std::regex(R"((?:aws_?access_?key_?id)\s*[:=]\s*['"]?AKI[\w]{16,})", std::regex::icase), "AWS Access Key"},
		{std::regex(R"((?:aws_?secret_?access_?key)\s*[:=]\s*['"]?[\w/+=]{30,})", std::regex::icase), "AWS Secret Key"},
		{std::regex(R"(sk-[a-zA-Z0-9]{20,})"), "OpenAI/Anthropic API Key"},
		{std::regex(R"(ghp_[a-zA-Z0-9]{36,})"), "GitHub Personal Access Token"},
		{std::regex(R"(-----BEGIN (?:RSA |EC |DSA )?PRIVATE KEY-----)"), "Private Key"},
		{std::regex(R"(\b\d{3}-?\d{2}-?\d{4}\b)"), "SSN-like Number"},
		{std::regex(R"((?:bearer\s+)[\w\-\.]{20,})", std::regex::icase), "Bearer Token"},
	};
	return pats;
}

std::vector<std::string>
detectSensitive(const std::string &text)
{
	std::vector<std::string> found;
	for (const auto &p : sensitivePatterns()) {
		if (std::regex_search(text, p.re))
			found.push_back(p.label);
	}
	return found;
}

std::string
redactSensitive(const std::string &text)
{
	std::string result = text;
	for (const auto &p : sensitivePatterns())
		result = std::regex_replace(result, p.re, "[REDACTED:" + p.label + "]");
	return result;
}

// =====================================================================
// Ollama Embedding (libcurl)
// =====================================================================

static size_t
curlWriteCb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *buf = static_cast<std::string *>(userdata);
	buf->append(ptr, size * nmemb);
	return size * nmemb;
}

std::vector<float>
getEmbedding(const std::string &ollamaUrl, const std::string &model, const std::string &text)
{
	json req = {{"model", model}, {"input", text}};
	std::string reqBody = req.dump();
	std::string resBody;

	CURL *c = curl_easy_init();
	if (!c)
		throw std::runtime_error("curl_easy_init failed");

	std::string url = ollamaUrl + "/api/embed";
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, reqBody.c_str());
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &resBody);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

	CURLcode res = curl_easy_perform(c);
	curl_slist_free_all(headers);
	curl_easy_cleanup(c);

	if (res != CURLE_OK)
		throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(res));

	auto j = json::parse(resBody);
	return j.at("embeddings").at(0).get<std::vector<float>>();
}

static std::vector<char>
serializeVec(const std::vector<float> &vec)
{
	std::vector<char> buf(vec.size() * sizeof(float));
	std::memcpy(buf.data(), vec.data(), buf.size());
	return buf;
}

// =====================================================================
// 現在時刻 (UTC ISO8601)
// =====================================================================

static std::string
nowUtc()
{
	auto now = std::chrono::system_clock::now();
	auto t = std::chrono::system_clock::to_time_t(now);
	std::tm tm{};
	gmtime_r(&t, &tm);
	char buf[64];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm);
	return buf;
}

// =====================================================================
// MemDB — SQLite + sqlite-vec
// =====================================================================

MemDB::MemDB(const std::string &path, const std::string &vecExtPath)
{
	namespace fs = std::filesystem;
	fs::create_directories(fs::path(path).parent_path());

	if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
		throw std::runtime_error("sqlite3_open failed: " + path);

	// sqlite-vec 拡張読み込み
	sqlite3_enable_load_extension(db_, 1);
	char *errMsg = nullptr;
	if (sqlite3_load_extension(db_, vecExtPath.c_str(), nullptr, &errMsg) != SQLITE_OK) {
		std::string msg = errMsg ? errMsg : "unknown error";
		sqlite3_free(errMsg);
		throw std::runtime_error("sqlite-vec load failed: " + msg);
	}
	sqlite3_enable_load_extension(db_, 0);

	// テーブル作成
	const char *ddl =
		"CREATE TABLE IF NOT EXISTS memories ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  content TEXT NOT NULL,"
		"  tags TEXT DEFAULT '[]',"
		"  created_at TEXT NOT NULL,"
		"  updated_at TEXT NOT NULL"
		");";
	sqlite3_exec(db_, ddl, nullptr, nullptr, nullptr);

	std::string vecDdl =
		"CREATE VIRTUAL TABLE IF NOT EXISTS memories_vec "
		"USING vec0(id INTEGER PRIMARY KEY, embedding float[" +
		std::to_string(EMBED_DIM) + "])";
	sqlite3_exec(db_, vecDdl.c_str(), nullptr, nullptr, nullptr);
}

MemDB::~MemDB()
{
	if (db_)
		sqlite3_close(db_);
}

// =====================================================================
// CMem
// =====================================================================

CMem::CMem()
{
	curl_global_init(CURL_GLOBAL_DEFAULT);

	ollamaUrl_ = "http://localhost:11434";
	if (auto *p = std::getenv("CMEM_OLLAMA_URL"))
		ollamaUrl_ = p;

	embedModel_ = "nomic-embed-text";

	vecExtPath_ = "vec0";
	if (auto *p = std::getenv("CMEM_VEC_EXT"))
		vecExtPath_ = p;

	// グローバルDB
	std::string home = std::getenv("HOME");
	std::string globalPath = home + "/.local/share/c-mem/global.db";
	globalDb_ = std::make_unique<MemDB>(globalPath, vecExtPath_);

	// プロジェクトDB (任意)
	if (auto *p = std::getenv("CMEM_DB"))
		projectDb_ = std::make_unique<MemDB>(p, vecExtPath_);
}

sqlite3 *
CMem::getConn(const std::string &scope)
{
	if (scope == "global")
		return globalDb_->handle();
	if (!projectDb_ || !projectDb_->valid())
		throw std::runtime_error(
			"CMEM_DB が設定されていません。"
			"プロジェクト固有の記憶を使うには .mcp.json で CMEM_DB を指定してください。");
	return projectDb_->handle();
}

std::vector<char>
CMem::embed(const std::string &text)
{
	auto vec = getEmbedding(ollamaUrl_, embedModel_, text);
	return serializeVec(vec);
}

// --- store ---

std::string
CMem::doStore(sqlite3 *conn, const std::string &content, const json &tags, bool force)
{
	auto sensitive = detectSensitive(content);
	if (!sensitive.empty() && !force) {
		std::string list;
		for (const auto &s : sensitive)
			list += (list.empty() ? "" : ", ") + s;
		return "機密情報が検出されました: " + list + "\n"
		       "このまま保存すると機密部分はマスクされます。\n"
		       "マスク済みで保存するには force=true で再度呼び出してください。";
	}

	std::string safe = redactSensitive(content);
	std::string now = nowUtc();
	auto emb = embed(safe);

	// INSERT memories
	sqlite3_stmt *stmt = nullptr;
	sqlite3_prepare_v2(conn,
		"INSERT INTO memories (content, tags, created_at, updated_at) VALUES (?,?,?,?)",
		-1, &stmt, nullptr);
	std::string tagsStr = tags.dump();
	sqlite3_bind_text(stmt, 1, safe.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, tagsStr.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, now.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	int64_t id = sqlite3_last_insert_rowid(conn);

	// INSERT memories_vec
	sqlite3_prepare_v2(conn,
		"INSERT INTO memories_vec (id, embedding) VALUES (?,?)",
		-1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_blob(stmt, 2, emb.data(), static_cast<int>(emb.size()), SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	std::string result = "記憶を保存しました (ID: " + std::to_string(id) + ")";
	if (!sensitive.empty()) {
		std::string list;
		for (const auto &s : sensitive)
			list += (list.empty() ? "" : ", ") + s;
		result += "\n注意: 以下の機密情報をマスクしました: " + list;
	}
	return result;
}

json
CMem::toolStore(const json &args)
{
	auto content = args.at("content").get<std::string>();
	json tags = args.contains("tags") && !args["tags"].is_null() ? args["tags"] : json::array();
	bool force = args.value("force", false);
	std::string scope = args.value("scope", "project");
	return doStore(getConn(scope), content, tags, force);
}

// --- search ---

std::vector<CMem::SearchRow>
CMem::searchInDb(sqlite3 *db, const std::string &source,
                 const std::vector<char> &embedding, int limit, const std::string &tag)
{
	// レコード0件チェック
	sqlite3_stmt *cnt = nullptr;
	sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memories", -1, &cnt, nullptr);
	sqlite3_step(cnt);
	int count = sqlite3_column_int(cnt, 0);
	sqlite3_finalize(cnt);
	if (count == 0)
		return {};

	int k = tag.empty() ? limit : limit * 3;

	sqlite3_stmt *stmt = nullptr;
	sqlite3_prepare_v2(db,
		"SELECT m.id, m.content, m.tags, m.created_at, v.distance "
		"FROM memories_vec v "
		"JOIN memories m ON m.id = v.id "
		"WHERE v.embedding MATCH ? AND k = ? "
		"ORDER BY v.distance",
		-1, &stmt, nullptr);
	sqlite3_bind_blob(stmt, 1, embedding.data(), static_cast<int>(embedding.size()), SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 2, k);

	std::vector<SearchRow> rows;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		SearchRow r;
		r.source = source;
		r.id = sqlite3_column_int64(stmt, 0);
		r.content = colText(stmt, 1);
		r.tagsJson = colText(stmt, 2);
		r.createdAt = colText(stmt, 3);
		r.distance = static_cast<float>(sqlite3_column_double(stmt, 4));
		rows.push_back(std::move(r));
	}
	sqlite3_finalize(stmt);

	if (!tag.empty()) {
		rows.erase(
			std::remove_if(rows.begin(), rows.end(), [&tag](const SearchRow &r) {
				auto t = json::parse(r.tagsJson);
				return std::find(t.begin(), t.end(), tag) == t.end();
			}),
			rows.end());
		if (static_cast<int>(rows.size()) > limit)
			rows.resize(limit);
	}
	return rows;
}

json
CMem::toolSearch(const json &args)
{
	auto query = args.at("query").get<std::string>();
	int limit = args.value("limit", 5);
	std::string tag = args.contains("tag") && !args["tag"].is_null()
	                  ? args["tag"].get<std::string>() : "";
	std::string scope = args.value("scope", "all");

	auto emb = embed(query);

	std::vector<SearchRow> all;
	if ((scope == "project" || scope == "all") && projectDb_ && projectDb_->valid())
		for (auto &r : searchInDb(projectDb_->handle(), "project", emb, limit, tag))
			all.push_back(std::move(r));
	if (scope == "global" || scope == "all")
		for (auto &r : searchInDb(globalDb_->handle(), "global", emb, limit, tag))
			all.push_back(std::move(r));

	std::sort(all.begin(), all.end(), [](const SearchRow &a, const SearchRow &b) {
		return a.distance < b.distance;
	});
	if (static_cast<int>(all.size()) > limit)
		all.resize(limit);

	if (all.empty())
		return "関連する記憶が見つかりませんでした。";

	std::string result = "検索結果 (" + std::to_string(all.size()) + "件):\n\n";
	for (const auto &r : all) {
		float similarity = 1.0f - r.distance;
		auto tagsList = json::parse(r.tagsJson);
		char sim[16];
		std::snprintf(sim, sizeof(sim), "%.3f", similarity);
		result += "[" + r.source + ":ID:" + std::to_string(r.id) + "]"
		          " (類似度:" + sim + ", タグ:" + tagsList.dump() + ")\n"
		          "  作成日: " + r.createdAt + "\n"
		          "  " + r.content + "\n\n";
	}
	return result;
}

// --- forget ---

json
CMem::toolForget(const json &args)
{
	int64_t id = args.at("memory_id").get<int64_t>();
	std::string scope = args.value("scope", "project");
	sqlite3 *conn = getConn(scope);

	sqlite3_stmt *stmt = nullptr;
	sqlite3_prepare_v2(conn, "SELECT id FROM memories WHERE id = ?", -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, id);
	bool found = sqlite3_step(stmt) == SQLITE_ROW;
	sqlite3_finalize(stmt);

	if (!found)
		return "ID " + std::to_string(id) + " の記憶は見つかりませんでした。(" + scope + ")";

	sqlite3_prepare_v2(conn, "DELETE FROM memories WHERE id = ?", -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	sqlite3_prepare_v2(conn, "DELETE FROM memories_vec WHERE id = ?", -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	return "記憶 " + scope + ":ID:" + std::to_string(id) + " を削除しました。";
}

// --- list ---

json
CMem::toolList(const json &args)
{
	std::string tag = args.contains("tag") && !args["tag"].is_null()
	                  ? args["tag"].get<std::string>() : "";
	int limit = args.value("limit", 20);
	std::string scope = args.value("scope", "all");

	struct Entry {
		std::string source;
		int64_t id;
		std::string content;
		std::string tagsJson;
		std::string createdAt;
	};
	std::vector<Entry> all;

	auto fetch = [&](sqlite3 *db, const std::string &src) {
		sqlite3_stmt *stmt = nullptr;
		if (tag.empty()) {
			sqlite3_prepare_v2(db,
				"SELECT id, content, tags, created_at FROM memories "
				"ORDER BY created_at DESC LIMIT ?",
				-1, &stmt, nullptr);
			sqlite3_bind_int(stmt, 1, limit);
		} else {
			sqlite3_prepare_v2(db,
				"SELECT id, content, tags, created_at FROM memories "
				"ORDER BY created_at DESC",
				-1, &stmt, nullptr);
		}

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			Entry e;
			e.source = src;
			e.id = sqlite3_column_int64(stmt, 0);
			e.content = colText(stmt, 1);
			e.tagsJson = colText(stmt, 2);
			e.createdAt = colText(stmt, 3);

			if (!tag.empty()) {
				auto t = json::parse(e.tagsJson);
				if (std::find(t.begin(), t.end(), tag) == t.end())
					continue;
			}
			all.push_back(std::move(e));
			if (static_cast<int>(all.size()) >= limit)
				break;
		}
		sqlite3_finalize(stmt);
	};

	if ((scope == "project" || scope == "all") && projectDb_ && projectDb_->valid())
		fetch(projectDb_->handle(), "project");
	if (scope == "global" || scope == "all")
		fetch(globalDb_->handle(), "global");

	if (all.empty())
		return "保存されている記憶はありません。";

	std::string result = "記憶一覧 (" + std::to_string(all.size()) + "件表示):\n\n";
	for (const auto &e : all) {
		auto tagsList = json::parse(e.tagsJson);
		std::string preview = utf8Truncate(e.content, 100);
		if (preview.size() < e.content.size())
			preview += "...";
		result += "[" + e.source + ":ID:" + std::to_string(e.id) + "]"
		          " タグ:" + tagsList.dump() + " (" + e.createdAt + ")\n"
		          "  " + preview + "\n\n";
	}
	return result;
}

// --- update ---

json
CMem::toolUpdate(const json &args)
{
	int64_t id = args.at("memory_id").get<int64_t>();
	auto content = args.at("content").get<std::string>();
	bool hasTags = args.contains("tags") && !args["tags"].is_null();
	json tags = hasTags ? args["tags"] : json();
	std::string scope = args.value("scope", "project");
	sqlite3 *conn = getConn(scope);

	// 存在確認
	sqlite3_stmt *stmt = nullptr;
	sqlite3_prepare_v2(conn, "SELECT id FROM memories WHERE id = ?", -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, id);
	bool found = sqlite3_step(stmt) == SQLITE_ROW;
	sqlite3_finalize(stmt);

	if (!found)
		return "ID " + std::to_string(id) + " の記憶は見つかりませんでした。(" + scope + ")";

	auto sensitive = detectSensitive(content);
	std::string safe = redactSensitive(content);
	std::string now = nowUtc();
	auto emb = embed(safe);

	if (hasTags) {
		sqlite3_prepare_v2(conn,
			"UPDATE memories SET content = ?, tags = ?, updated_at = ? WHERE id = ?",
			-1, &stmt, nullptr);
		std::string tagsStr = tags.dump();
		sqlite3_bind_text(stmt, 1, safe.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, tagsStr.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 4, id);
	} else {
		sqlite3_prepare_v2(conn,
			"UPDATE memories SET content = ?, updated_at = ? WHERE id = ?",
			-1, &stmt, nullptr);
		sqlite3_bind_text(stmt, 1, safe.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, id);
	}
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	// ベクトル更新
	sqlite3_prepare_v2(conn, "DELETE FROM memories_vec WHERE id = ?", -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	sqlite3_prepare_v2(conn,
		"INSERT INTO memories_vec (id, embedding) VALUES (?,?)",
		-1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_blob(stmt, 2, emb.data(), static_cast<int>(emb.size()), SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	std::string result = "記憶 " + scope + ":ID:" + std::to_string(id) + " を更新しました。";
	if (!sensitive.empty()) {
		std::string list;
		for (const auto &s : sensitive)
			list += (list.empty() ? "" : ", ") + s;
		result += "\n注意: 以下の機密情報をマスクしました: " + list;
	}
	return result;
}

// --- check_sensitive ---

json
CMem::toolCheckSensitive(const json &args)
{
	auto text = args.at("text").get<std::string>();
	auto sensitive = detectSensitive(text);

	if (sensitive.empty())
		return "機密情報は検出されませんでした。安全に保存できます。";

	std::string list;
	for (const auto &s : sensitive)
		list += (list.empty() ? "" : ", ") + s;
	return "以下の機密情報が検出されました: " + list + "\n\n"
	       "マスク後の内容:\n" + redactSensitive(text);
}

// =====================================================================
// ツール登録
// =====================================================================

void
CMem::registerTools(mcp::Server &server)
{
	server.addTool({
		.name = "memory_store",
		.description = "記憶を保存する。機密情報が含まれる場合は警告し、自動的にマスクする。",
		.params = {
			{"content",  "string",  "保存する記憶の内容", true},
			{"tags",     "array",   "分類用タグのリスト", false},
			{"force",    "boolean", "機密情報の警告を無視して保存", false},
			{"scope",    "string",  "project または global", false},
		},
		.handler = [this](const json &args) { return toolStore(args); },
	});

	server.addTool({
		.name = "memory_search",
		.description = "クエリに関連する記憶をセマンティック検索で取得する。",
		.params = {
			{"query",  "string",  "検索クエリ（自然言語）", true},
			{"limit",  "integer", "返す結果の最大数", false},
			{"tag",    "string",  "特定タグでフィルタリング", false},
			{"scope",  "string",  "project, global, または all", false},
		},
		.handler = [this](const json &args) { return toolSearch(args); },
	});

	server.addTool({
		.name = "memory_forget",
		.description = "指定IDの記憶を削除する。",
		.params = {
			{"memory_id", "integer", "削除する記憶のID", true},
			{"scope",     "string",  "project または global", false},
		},
		.handler = [this](const json &args) { return toolForget(args); },
	});

	server.addTool({
		.name = "memory_list",
		.description = "保存されている記憶の一覧を表示する。",
		.params = {
			{"tag",   "string",  "特定タグでフィルタリング", false},
			{"limit", "integer", "返す結果の最大数", false},
			{"scope", "string",  "project, global, または all", false},
		},
		.handler = [this](const json &args) { return toolList(args); },
	});

	server.addTool({
		.name = "memory_update",
		.description = "既存の記憶を更新する。",
		.params = {
			{"memory_id", "integer", "更新する記憶のID", true},
			{"content",   "string",  "新しい内容", true},
			{"tags",      "array",   "新しいタグリスト（省略時はタグを変更しない）", false},
			{"scope",     "string",  "project または global", false},
		},
		.handler = [this](const json &args) { return toolUpdate(args); },
	});

	server.addTool({
		.name = "memory_check_sensitive",
		.description = "テキストに機密情報が含まれるかチェックする（保存はしない）。",
		.params = {
			{"text", "string", "チェック対象のテキスト", true},
		},
		.handler = [this](const json &args) { return toolCheckSensitive(args); },
	});
}

} // namespace cmem
