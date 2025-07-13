#include "DHTParser.h"


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
	std::string Result = clang_getCString(str);
	clang_disposeString(str);
	return Result;
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

std::string GetAnnotateAttrText(CXCursor Cursor)
{
	std::optional<CXString> Result = std::nullopt;

	auto Visitor = [](CXCursor c, CXCursor parent, CXClientData clientData) {
		std::optional<CXString>* attrText = static_cast<std::optional<CXString>*>(clientData);
		if (clang_getCursorKind(c) == CXCursor_AnnotateAttr)
		{
			*attrText = clang_getCursorDisplayName(c);
			return CXChildVisit_Break;
		}
		return CXChildVisit_Continue;
	};

	clang_visitChildren(Cursor, Visitor, &Result);

	return Result.has_value() ? ToString(Result.value()) : "";
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
	std::string AnnotateAttrText = GetAnnotateAttrText(Cursor);

	std::string ClassName = ToString(clang_getCursorSpelling(Cursor));

	DHTClass MetaClass;

	std::cout << "Annotation: " << AnnotateAttrText << std::endl;
	if (ClassName.empty())
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

	auto HeaderFileVisitor = [](CXCursor Child, CXCursor Parent, CXClientData ClientData) -> CXChildVisitResult {
		DHTParseContext* VisitContext = static_cast<DHTParseContext*>(ClientData);

		if (ShouldSkipCursor(Child, *VisitContext))
		{
			return CXChildVisit_Continue;
		}
		CXCursorKind CursorType = clang_getCursorKind(Child);
		switch (CursorType)
		{
		case CXCursor_Namespace:
			{
				VisitContext->Parser->ParseNamespaceRecursively(Child, *VisitContext);
			}
			break;
		case CXCursor_ClassDecl:
			{
				VisitContext->Parser->ParseClass(Child, *VisitContext);
			}
			break;
		default:
			break;
		}
		return CXChildVisit_Continue;
	};

	DHTParseContext ParseContext;
	ParseContext.Parser = this;
	ParseContext.MetaFile = &MetaFile;
	ParseContext.ParseConfig = nullptr;

	CXCursor RootCursor = clang_getTranslationUnitCursor(TranslationUnit);
	clang_visitChildren(RootCursor, HeaderFileVisitor, &ParseContext);
	clang_disposeTranslationUnit(TranslationUnit);

	return MetaFile;
}

void DHTParser::ParseNamespaceRecursively(CXCursor NamespaceCursor, DHTParseContext& Context)
{
	Context.NamespaceStack.push_back(ToString(clang_getCursorSpelling(NamespaceCursor)));

	auto NamespaceVisitor = [](CXCursor Child, CXCursor Parent, CXClientData ClientData) -> CXChildVisitResult {
		DHTParseContext* VisitContext = static_cast<DHTParseContext*>(ClientData);
		CXCursorKind CursorType = clang_getCursorKind(Child);
		switch (CursorType)
		{
		case CXCursor_Namespace:
			{
				VisitContext->Parser->ParseNamespaceRecursively(Child, *VisitContext);
			}
			break;
		case CXCursor_ClassDecl:
			{
				VisitContext->Parser->ParseClass(Child, *VisitContext);
			}
			break;
		default:
			break;
		}
		return CXChildVisit_Continue;
	};

	clang_visitChildren(NamespaceCursor, NamespaceVisitor, &Context);
	Context.NamespaceStack.pop_back();
}

void DHTParser::ParseClass(CXCursor ClassCursor, DHTParseContext& Context)
{
	std::string AnnotateAttrText = GetAnnotateAttrText(ClassCursor);
	if (!AnnotateAttrText.starts_with("DCLASS"))
	{
		return;
	}

	DHTClass MetaClass;
	MetaClass.ClassName = ToString(clang_getCursorSpelling(ClassCursor));
	for (const auto& Namespace : Context.NamespaceStack)
	{
		if (MetaClass.Namespace.empty())
		{
			MetaClass.Namespace += Namespace;
		}
		else
		{
			MetaClass.Namespace = MetaClass.Namespace + "::" + Namespace;
		}
	}

	std::cout << "Class: " << MetaClass.ClassName << std::endl;
	std::cout << "- Annotation: " << AnnotateAttrText << std::endl;
	std::cout << "- Namespace: " << MetaClass.Namespace << std::endl;

	Context.MetaFile->Classes.push_back(std::move(MetaClass));
}


} // namespace DHT