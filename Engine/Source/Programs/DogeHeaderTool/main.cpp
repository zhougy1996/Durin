#include <iostream>
#include <vector>
#include <filesystem>
#include <clang-c/Index.h>

namespace fs = std::filesystem;

int main()
{
	std::vector<const char*> arguments = {{"-x",
										   "c++",
										   "-std=c++23",
										   "-D__REFLECTION_PARSER__",
										   "-DNDEBUG",
										   "-D_MSC_VER=1930",
										   "-w",
										   "-MG",
										   "-M",
										   "-ferror-limit=0",
										   "-o clangLog.txt"}};

	fs::path headerPath = "D:/Studyspace/Doge/Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshVertexData.h";
	if (!fs::exists(headerPath))
	{
		std::cerr << "Header file does not exist: " << headerPath << std::endl;
		return 1;
	}

	auto index = clang_createIndex(0, 0);
	CXTranslationUnit translator = clang_parseTranslationUnit(
		index, "D:/Studyspace/Doge/Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshVertexData.h", arguments.data(), (int)arguments.size(), nullptr, 0, CXTranslationUnit_None);
	if (!translator)
	{
		std::cerr << "Failed to parse translation unit." << std::endl;
		return 1;
	}
}