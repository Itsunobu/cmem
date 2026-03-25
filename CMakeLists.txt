cmake_minimum_required(VERSION 3.20)
project(c-mem-cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)

add_executable(c-mem
	main.cc
	mcp_server.cc
	c_mem.cc
)

target_link_libraries(c-mem PRIVATE
	nlohmann_json::nlohmann_json
	CURL::libcurl
	SQLite::SQLite3
)
