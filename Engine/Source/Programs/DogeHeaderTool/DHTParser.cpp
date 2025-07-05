#include "DHTParser.h"

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

DHTFile DHTParser::ParseHeaderFile(const std::string& InFilePath)
{
	return ParseHeaderFile(FS::path(InFilePath));
}

static std::string ToString(const FS::path& FilePath)
{
	std::u8string FilePathStrU8 = FilePath.u8string();
	return std::string(FilePathStrU8.begin(), FilePathStrU8.end());
}

DHTFile DHTParser::ParseHeaderFile(const FS::path& InFilePath)
{
	static DHTFile EmptyMetaFile;

	DHTFile MetaFile;

	if (!FS::exists(InFilePath))
	{
		std::cerr << "Header file does not exist: " << InFilePath << std::endl;
		return EmptyMetaFile;
	}

	const std::string Filename = ToString(InFilePath.filename());
	const std::string FilePath = ToString(InFilePath);

	auto Index = clang_createIndex(0, 0);
	CXTranslationUnit TranslationUnit = clang_parseTranslationUnit(
		Index, FilePath.c_str(), ClangArguments_.data(), (int)ClangArguments_.size(), nullptr, 0, CXTranslationUnit_None);
	if (!TranslationUnit)
	{
		std::cerr << "Failed to parse translation unit." << std::endl;
		return EmptyMetaFile;
	}

	clang_disposeTranslationUnit(TranslationUnit);

	MetaFile.Filename = Filename;
	MetaFile.FilePath = FilePath;
	return MetaFile;
}


} // namespace DHT