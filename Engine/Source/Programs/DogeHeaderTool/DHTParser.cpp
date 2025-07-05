#include "DHTParser.h"

#include <iostream>

#include <clang-c/Index.h>

namespace DHT
{

DHTParser::DHTParser()
{
	// Initialize the Clang arguments for parsing
	ClangArguments_ =
		{"-x",
		 "c++",
		 "-std=c++23",
		 "-D__REFLECTION_PARSER__",
		 "-DNDEBUG",
		 "-D_MSC_VER=1930",
		 "-w",
		 "-MG",
		 "-M",
		 "-ferror-limit=0",
		 "-o clangLog.txt"};
}

bool DHTParser::ParseHeaderFile(const std::string& InFilePath)
{
	return ParseHeaderFile(FS::path(InFilePath));
}

bool DHTParser::ParseHeaderFile(const FS::path& InFilePath)
{
	if (!FS::exists(InFilePath))
	{
		std::cerr << "Header file does not exist: " << InFilePath << std::endl;
		return false;
	}

	std::u8string FilePathStrU8 = InFilePath.u8string();
	std::string FilePathStr = std::string(FilePathStrU8.begin(), FilePathStrU8.end());

	auto Index = clang_createIndex(0, 0);
	CXTranslationUnit TranslationUnit = clang_parseTranslationUnit(
		Index, FilePathStr.c_str(), ClangArguments_.data(), (int)ClangArguments_.size(), nullptr, 0, CXTranslationUnit_None);
	if (!TranslationUnit)
	{
		std::cerr << "Failed to parse translation unit." << std::endl;
		return false;
	}

	clang_disposeTranslationUnit(TranslationUnit);
	return true;
}


} // namespace DHT