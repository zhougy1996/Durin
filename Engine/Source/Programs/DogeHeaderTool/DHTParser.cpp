#include "DHTParser.h"

#include <clang-c/Index.h>

namespace DHT
{
namespace
{
std::string ToString(const FS::path& FilePath)
{
	std::u8string FilePathStrU8 = FilePath.u8string();
	return std::string(FilePathStrU8.begin(), FilePathStrU8.end());
}

std::string ToString(CXString str)
{
	std::string result = clang_getCString(str);
	clang_disposeString(str);
	return result;
}

std::ostream& operator<<(std::ostream& stream, const CXString& str)
{
	auto c_str = ToString(str);
	stream << c_str;
	return stream;
}

bool ShouldSkipCursor(CXCursor Cursor, DHTParseContext& Context)
{
	CXSourceLocation Location = clang_getCursorLocation(Cursor);
	if (clang_Location_isInSystemHeader(Location))
	{
		return true;
	}

	CXFile File;
	unsigned Line, Column;
	clang_getFileLocation(Location, &File, &Line, &Column, nullptr);

	if (Context.MetaFile->Filename == ToString(clang_getFileName(File)))
	{
		return false;
	}
	return true;
}

bool HasReflectionMacro(CXCursor Cursor, const std::string ReflectionMacro)
{
	CXTranslationUnit TranslationUnit = clang_Cursor_getTranslationUnit(Cursor);

	CXToken* Tokens = nullptr;
	unsigned NumTokens = 0;
	CXSourceRange Range = clang_getCursorExtent(Cursor);
	clang_tokenize(clang_Cursor_getTranslationUnit(Cursor), Range, &Tokens, &NumTokens);

	for (unsigned i = 0; i < NumTokens; ++i)
	{
		CXToken Token = Tokens[i];
		std::string TokenString = ToString(clang_getTokenSpelling(TranslationUnit, Token));

		if (TokenString == ReflectionMacro)
		{
			return true;
		}
	}

	return false;
}

void ParseClass(CXCursor Cursor, DHTParseContext& Context)
{
	DHTClass MetaClass;
	std::string ClassName = ToString(clang_getCursorSpelling(Cursor));
	if (ClassName.empty() || !HasReflectionMacro(Cursor, "DClass"))
	{
		return;
	}
}

} // anonymous namespace

DHTParser::DHTParser()
{
	// Initialize the Clang arguments for parsing
	ClangArguments_ =
		{"-x",
		 "c++",
		 "-std=c++23",
		 "-D_DHT_PARSER",
		 "-DNDEBUG",
		 "-D_MSC_VER=1930",
		 "-w",
		 "-MG",
		 "-M",
		 "-ferror-limit=0",
		 "-o clangLog.txt"};
}

std::optional<DHTFile> DHTParser::ParseHeaderFile(const std::string& InFilePath)
{
	return ParseHeaderFile(FS::path(InFilePath));
}

static CXChildVisitResult DHTHeaderFileParseVisitor(CXCursor Cursor, CXCursor Parent, CXClientData ClientData)
{
	DHTParseContext* Context = static_cast<DHTParseContext*>(ClientData);
	DHTFile* MetaFile = Context->MetaFile;
	if (ShouldSkipCursor(Cursor, *Context))
	{
		return CXChildVisit_Continue;
	}

	CXCursorKind CursorType = clang_getCursorKind(Cursor);

	std::cout << "Type: " << std::setw(20) << clang_getCursorKindSpelling(clang_getCursorKind(Cursor)) << "\t|\t" << "Cursor: " << std::setw(20) << clang_getCursorSpelling(Cursor) << std::endl;

	switch (CursorType)
	{
	case CXCursor_StructDecl:
	case CXCursor_ClassDecl:
		ParseClass(Cursor, *Context);
		break;
	default:
		break;
	}

	return CXChildVisit_Recurse;
}

std::optional<DHTFile> DHTParser::ParseHeaderFile(const FS::path& InFilePath)
{
	DHTFile MetaFile;

	if (!FS::exists(InFilePath))
	{
		std::cerr << "Header file does not exist: " << InFilePath << std::endl;
		return std::nullopt;
	}

	const std::string Filename = ToString(InFilePath.filename());
	const std::string FilePath = ToString(InFilePath);

	auto Index = clang_createIndex(0, 0);
	CXTranslationUnit TranslationUnit = clang_parseTranslationUnit(
		Index, FilePath.c_str(), ClangArguments_.data(), (int)ClangArguments_.size(), nullptr, 0, CXTranslationUnit_None);
	if (!TranslationUnit)
	{
		std::cerr << "Failed to parse translation unit." << std::endl;
		return std::nullopt;
	}

	MetaFile.Filename = Filename;
	MetaFile.FilePath = FilePath;

	DHTParseContext ParseContext = {&MetaFile};
	CXCursor RootCursor = clang_getTranslationUnitCursor(TranslationUnit);
	bool bParseSuccess = clang_visitChildren(RootCursor, DHTHeaderFileParseVisitor, &ParseContext);

	if (!bParseSuccess)
	{
		return std::nullopt;
	}

	clang_disposeTranslationUnit(TranslationUnit);

	return MetaFile;
}


} // namespace DHT