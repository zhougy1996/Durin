#pragma once

namespace DHT
{
struct DHTFile;

class DHTCodeGenerator
{
public:
	bool GenerateDHTHeaderFile(const DHTFile& MetaFile);

	bool GenerateDHTSourceFile(const DHTFile& MetaFile);
};
} // namespace DHT
