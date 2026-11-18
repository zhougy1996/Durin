#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"
#include "Misc/FilePath.h"
#include "HAL/Platform.h"
#include "CoreAPI.h"
#include "Misc/UnifiedError.h"

namespace Durin
{
	struct FFileSystemError
	{
		std::string Message;
		std::error_code NativeError;
		CORE_API auto GetMessage() const -> std::string;
	};
	CORE_API auto ToError(const FFileSystemError& Error) -> UnifiedError::FError;

	class IFileHandle
	{
	public:
		virtual ~IFileHandle() = default;
		virtual auto Tell() -> int64 = 0;
		virtual auto Seek(int64 NewPosition) -> bool = 0;
		virtual auto SeekFromEnd(int64 NewPositionRelativeToEnd = 0) -> bool = 0;
		virtual auto Read(uint8* Destination, int64 BytesToRead) -> bool = 0;
		virtual auto ReadAt(uint8* Destination, int64 BytesToRead, int64 Offset) -> bool = 0;
		virtual auto Write(const uint8* Source, int64 BytesToWrite) -> bool = 0;
		virtual auto Flush(bool bFullFlush = false) -> bool = 0;
		virtual auto Truncate(int64 NewSize) -> bool = 0;
		virtual auto Size() -> int64 = 0;
	};
	using FFileOpenResult = std::expected<std::unique_ptr<IFileHandle>, FFileSystemError>;

	class IPlatformFile
	{
	public:
		class FDirectoryVisitor
		{
		public:
			virtual ~FDirectoryVisitor() = default;
			virtual auto ShouldVisitLeafPathname(std::string_view LeafPathname) -> bool { return true; }
			virtual auto Visit(const FFilePath& FilenameOrDirectory, bool bIsDirectory) -> bool = 0;
		};
		using FDirectoryVisitorFunc = std::function<bool(const FFilePath&, bool)>;

		virtual ~IPlatformFile() = default;
		CORE_API static auto GetPlatformPhysical() -> IPlatformFile&;
		enum class EOpenReadFlags : uint8 { None = 0, AllowWrite = 1 << 0, AllowDelete = 1 << 1, NoBuffering = 1 << 2 };
		enum class EOpenWriteFlags : uint8 { None = 0, AllowRead = 1 << 0, Append = 1 << 1 };
		CORE_API virtual auto OpenRead(const FFilePath& Filename, EOpenReadFlags Flags) -> FFileOpenResult;
		CORE_API virtual auto OpenWrite(const FFilePath& Filename, EOpenWriteFlags Flags) -> FFileOpenResult;
		virtual auto FileExists(const FFilePath& Filename) -> bool = 0;
		virtual auto DirectoryExists(const FFilePath& Directory) -> bool = 0;
		virtual auto FileSize(const FFilePath& Filename) -> int64 = 0;
		virtual auto CreateDirectory(const FFilePath& Directory) -> bool = 0;
		virtual auto CreateDirectoryTree(const FFilePath& Directory) -> bool = 0;
		virtual auto IterateDirectory(const FFilePath& Directory, FDirectoryVisitor& Visitor) -> bool = 0;
		CORE_API virtual auto IterateDirectory(const FFilePath& Directory, FDirectoryVisitorFunc Visitor) -> bool;
		CORE_API virtual auto IterateDirectoryRecursively(const FFilePath& Directory, FDirectoryVisitor& Visitor) -> bool;
		CORE_API virtual auto IterateDirectoryRecursively(const FFilePath& Directory, FDirectoryVisitorFunc Visitor) -> bool;
	};
	constexpr auto operator|(IPlatformFile::EOpenReadFlags A, IPlatformFile::EOpenReadFlags B) -> IPlatformFile::EOpenReadFlags
	{
		return static_cast<IPlatformFile::EOpenReadFlags>(static_cast<uint8>(A) | static_cast<uint8>(B));
	}
	constexpr auto operator|(IPlatformFile::EOpenWriteFlags A, IPlatformFile::EOpenWriteFlags B) -> IPlatformFile::EOpenWriteFlags
	{
		return static_cast<IPlatformFile::EOpenWriteFlags>(static_cast<uint8>(A) | static_cast<uint8>(B));
	}
}
