#include "DHTCodeGenerator.h"
#include <iostream>
#include <fstream>

#include "DHTMetaData.h"

namespace DHT
{
bool DHTCodeGenerator::GenerateDHTHeaderFile(const DHTMetaData& MetaData)
{
	std::ofstream HeaderFile("DHTMetaData.h", std::ios::out | std::ios::trunc);
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

bool DHTCodeGenerator::GenerateDHTSourceFile(const DHTMetaData& MetaData)
{
	return false;
}

} // namespace DHT