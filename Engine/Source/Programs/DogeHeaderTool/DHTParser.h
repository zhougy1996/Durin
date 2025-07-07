#pragma once

#include "DHTMetaData.h"

namespace DHT
{
struct DHTParseConfig
{
	std::set<std::string> ReflectionMacros = {"DCLASS", "DSTRUCT", "DPROPERTY", "DFUNCTION"};
};

struct DHTParseContext
{
	DHTFile* MetaFile;
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
	std::vector<const char*> ClangArguments_;

	DHTParseConfig ParseConfig;
};
} // namespace DHT
