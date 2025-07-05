#pragma once

#include "DHTMetaData.h"

namespace DHT
{

class DHTParser
{
public:
	DHTParser();
	~DHTParser() = default;

	DHTFile ParseHeaderFile(const std::string& InFilePath);

	DHTFile ParseHeaderFile(const FS::path& InFilePath);


private:

	std::vector<const char*> ClangArguments_;
};
} // namespace DHT
