add_durin_module(DurinMCP)
target_link_libraries(DurinMCP PRIVATE httplib::httplib)
# Include the socket library before platform/PCH headers on Windows.
set_source_files_properties(Private/MCPServer.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
