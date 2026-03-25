# c-mem

C++ で実装した MCP サーバー。[Ollama](https://ollama.com/) のエンベディングと [SQLite](https://www.sqlite.org/) + [sqlite-vec](https://github.com/asg017/sqlite-vec) によるベクトル検索で、セマンティックな永続メモリを提供する。

stdin/stdout 上の JSON-RPC で通信する（MCP プロトコルバージョン `2024-11-05`）。

## 特徴

- **セマンティック検索** — `nomic-embed-text` エンベディング（768次元）によるベクトル類似度検索
- **デュアルスコープ** — グローバル DB（`~/.local/share/c-mem/global.db`）とプロジェクト単位の DB を併用可能
- **機密情報検出** — クレデンシャルや API キーなどを自動検出し、明示的に強制しない限り保存をブロック
- **タグフィルタリング** — タグによるメモリの整理・絞り込み
- **MCP ツール** — `memory_store`, `memory_search`, `memory_forget`, `memory_list`, `memory_update`, `memory_check_sensitive`

## メリット

- **完全ローカル動作** — エンベディング生成を含む全処理がローカルで完結するため、外部 API への送信が不要でプライバシーを確保できる
- **高速・軽量** — C++ ネイティブバイナリと SQLite により、起動・検索ともに高速でリソース消費が少ない
- **クラウド API 不要** — Ollama をローカルで動かすため、API キーや課金なしで利用できる
- **プロジェクト横断の知識管理** — グローバル DB に蓄積した知識をどのプロジェクトからも参照でき、プロジェクト固有の記憶は分離して管理できる
- **安全なメモリ保存** — 機密情報の自動検出により、意図しないクレデンシャルの保存を防止する

## 必要なもの

- C++20 対応コンパイラ
- CMake 3.20+
- [nlohmann/json](https://github.com/nlohmann/json)
- libcurl
- SQLite3
- [Ollama](https://ollama.com/)（ローカルで起動しておくこと）
- [sqlite-vec](https://github.com/asg017/sqlite-vec) 拡張

## ビルド

```bash
cmake -B build
cmake --build build
```

出力バイナリ: `build/c-mem`

## 設定

環境変数:

| 変数 | 説明 | デフォルト |
|---|---|---|
| `CMEM_OLLAMA_URL` | Ollama API エンドポイント | `http://localhost:11434` |
| `CMEM_DB` | プロジェクト用 DB のパス | （なし — プロジェクトスコープ無効） |
| `CMEM_VEC_EXT` | sqlite-vec 拡張のパス | `vec0` |

## 使い方

### Claude Code

MCP サーバー設定に追加する:

```json
{
  "mcpServers": {
    "c-mem": {
      "command": "/path/to/c-mem",
      "env": {
        "CMEM_DB": ".c-mem.db"
      }
    }
  }
}
```

### 単体実行

`c-mem` は stdin から JSON-RPC リクエストを読み、stdout にレスポンスを返す:

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"0.1"}}}' | ./build/c-mem
```

## アーキテクチャ

```
main.cc            エントリーポイント — CMem ツールを MCP Server に登録して実行
mcp_server.h/cc    汎用 JSON-RPC サーバー（任意の MCP ツールサーバーとして再利用可能）
c_mem.h/cc         メモリロジック — store, search, forget, list, update, check_sensitive
```

## ライセンス

MIT
