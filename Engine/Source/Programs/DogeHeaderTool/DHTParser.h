#pragma once

#include <vector>
#include <string>
#include <filesystem>

namespace DHT
{
namespace FS = std::filesystem;

class DHTParser
{
public:
	DHTParser();
	~DHTParser() = default;

	bool ParseHeaderFile(const std::string& InFilePath);

	bool ParseHeaderFile(const FS::path& InFilePath);


private:

	std::vector<const char*> ClangArguments_;
};
} // namespace DHT
