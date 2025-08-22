#include "DHTCodeGenerator.h"

#include "DHTMetaData.h"

namespace DHT
{
static std::string GetFilenameWithoutSuffix(const std::string& Filename)
{
	size_t DotPos = Filename.find_last_of('.');
	if (DotPos == std::string::npos)
	{
		return Filename;
	}
	return Filename.substr(0, DotPos);
}

bool DHTCodeGenerator::GenerateDHTHeaderFile(const DHTFile& MetaFile)
{
	std::string GeneratedHeaderFilePath = GetFilenameWithoutSuffix(MetaFile.Filename) + ".generated.h";
	std::ofstream HeaderFile(GeneratedHeaderFilePath, std::ios::out | std::ios::trunc);
	if (!HeaderFile.is_open())
	{
		std::cerr << "Failed to create DHTMetaData.h" << std::endl;
		return false;
	}
	HeaderFile << "#pragma once\n\n";
	HeaderFile << "#include <string>\n\n";
	HeaderFile << "namespace DHT\n{\n";
	HeaderFile << "struct DHTMetaData\n{\n";
	HeaderFile << "\tstd::string HeaderFilePath; // Path to the original header file\n";
	HeaderFile << "};\n";
	HeaderFile << "} // namespace DHT\n";
	HeaderFile.close();
	return true;
}

bool DHTCodeGenerator::GenerateDHTSourceFile(const DHTFile& MetaFile)
{
	return false;
}

} // namespace DHT