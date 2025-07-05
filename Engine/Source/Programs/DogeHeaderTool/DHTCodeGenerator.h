#pragma once

#include <string>

namespace DHT
{
struct DHTMetaData;

class DHTCodeGenerator
{
public:
	bool GenerateDHTHeaderFile(const DHTMetaData& MetaData);

	bool GenerateDHTSourceFile(const DHTMetaData& MetaData);
};
} // namespace DHT
