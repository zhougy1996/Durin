#pragma once

#include "HAL/PlatformFile.h"

namespace Durin
{
	class IFileManager
	{
	public:
		virtual ~IFileManager() = default;
		CORE_API static auto Get() -> IFileManager&;
		virtual auto FileExists(const FFilePath& Filename) -> bool = 0;
		virtual auto DirectoryExists(const FFilePath& Directory) -> bool = 0;
		virtual auto FileSize(const FFilePath& Filename) -> int64 = 0;
		virtual auto MakeDirectory(const FFilePath& Path, bool Tree = false) -> bool = 0;
		virtual auto IterateDirectory(const FFilePath& Directory, IPlatformFile::FDirectoryVisitor& Visitor) -> bool = 0;
		virtual auto IterateDirectory(const FFilePath& Directory, IPlatformFile::FDirectoryVisitorFunc Visitor) -> bool = 0;
		virtual auto IterateDirectoryRecursively(const FFilePath& Directory, IPlatformFile::FDirectoryVisitor& Visitor) -> bool = 0;
		virtual auto IterateDirectoryRecursively(const FFilePath& Directory, IPlatformFile::FDirectoryVisitorFunc Visitor) -> bool = 0;
	};
}
