#pragma once
#include <clang-c/Index.h>

#include "DHTMetaData.h"

namespace DHT
{
class DHTParser;

struct DHTParseConfig
{
	std::set<std::string> ReflectionMacros = {"DCLASS", "DSTRUCT", "DPROPERTY", "DFUNCTION"};
};

struct DHTParseContext
{
	DHTParser* Parser;
	DHTFile* MetaFile;
	DHTNamespaceStack NamespaceStack;
	const DHTParseConfig* ParseConfig;
};

class DHTParser
{
public:
	DHTParser();
	~DHTParser() = default;

	std::optional<DHTFile> ParseHeaderFile(const std::string& InFilePath);

	std::optional<DHTFile> ParseHeaderFile(const FS::path& InFilePath);

private:
	void ParseNamespaceRecursively(CXCursor NamespaceCursor, DHTParseContext& Context);

	void ParseClass(CXCursor ClassCursor, DHTParseContext& Context);

	std::vector<const char*> ClangArguments_;

	DHTParseConfig ParseConfig;
};
} // namespace DHT
