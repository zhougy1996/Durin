add_durin_module(HTTPServer)
target_link_libraries(HTTPServer PRIVATE httplib::httplib)
# Winsock must precede the platform/PCH headers on Windows.
set_source_files_properties(Private/HTTPServer.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
