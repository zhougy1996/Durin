#include "DHTParser.h"
#include "DHTCodeGenerator.h"
#include "DHTMetaData.h"

int main()
{
	std::string TestHeaderFile = "D:/Studyspace/Doge/Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshVertexData.h";

	DHT::DHTParser Parser;
	DHT::DHTCodeGenerator Generator;

	Parser.ParseHeaderFile(TestHeaderFile);
	DHT::DHTMetaData MetaData;
	Generator.GenerateDHTHeaderFile(MetaData);
}