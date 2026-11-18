#include "HAL/FileManager.h"
#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#endif

namespace Durin
{
	namespace
	{
		auto InspectEntry(const FFilePath& Path, bool& bIsDirectory, bool& bIsLink) -> bool
		{
#if defined(_WIN32)
			const DWORD Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES) return false;
			bIsDirectory = (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			bIsLink = (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error || !std::filesystem::exists(Status)) return false;
			bIsDirectory = std::filesystem::is_directory(Status);
			bIsLink = std::filesystem::is_symlink(Status);
#endif
			return true;
		}

		class FPhysicalPlatformFile final : public IPlatformFile
		{
		public:
			auto FileExists(const FFilePath& Filename) -> bool override
			{
				std::error_code Error;
				return std::filesystem::is_regular_file(Filename, Error);
			}
			auto DirectoryExists(const FFilePath& Directory) -> bool override
			{
				std::error_code Error;
				return std::filesystem::is_directory(Directory, Error);
			}
			auto FileSize(const FFilePath& Filename) -> int64 override
			{
				if (!FileExists(Filename)) return -1;
				std::error_code Error;
				const auto Size = std::filesystem::file_size(Filename, Error);
				return Error || Size > static_cast<uintmax_t>(std::numeric_limits<int64>::max()) ? -1 : static_cast<int64>(Size);
			}
			auto CreateDirectory(const FFilePath& Directory) -> bool override
			{
				std::error_code Error;
				return std::filesystem::create_directory(Directory, Error) || DirectoryExists(Directory);
			}
			auto CreateDirectoryTree(const FFilePath& Directory) -> bool override
			{
				std::error_code Error;
				return std::filesystem::create_directories(Directory, Error) || DirectoryExists(Directory);
			}
			auto IterateDirectory(const FFilePath& Directory, FDirectoryVisitor& Visitor) -> bool override
			{
				std::error_code Error;
				const auto Root = std::filesystem::absolute(Directory, Error).lexically_normal();
				if (Error) return false;
				bool bIsDirectory = false, bIsLink = false;
				if (!InspectEntry(Root, bIsDirectory, bIsLink)) return false;
				if (bIsLink) return true;
				if (!bIsDirectory) return false;
				std::filesystem::directory_iterator Entry(Root, Error), End;
				if (Error) return DirectoryExists(Root);
				while (Entry != End)
				{
					const auto Path = Entry->path();
					if (Visitor.ShouldVisitLeafPathname(Path.filename().string())
						&& InspectEntry(Path, bIsDirectory, bIsLink) && !bIsLink
						&& !Visitor.Visit(Path, bIsDirectory)) return false;
					Entry.increment(Error);
					if (Error) break;
				}
				return true;
			}
		};

		class FDirectoryVisitorFuncWrapper final : public IPlatformFile::FDirectoryVisitor
		{
		public:
			explicit FDirectoryVisitorFuncWrapper(IPlatformFile::FDirectoryVisitorFunc InVisitor) : Visitor(std::move(InVisitor)) {}
			auto Visit(const FFilePath& Path, bool bIsDirectory) -> bool override { return Visitor(Path, bIsDirectory); }
		private:
			IPlatformFile::FDirectoryVisitorFunc Visitor;
		};

		class FFileManagerGeneric final : public IFileManager
		{
		public:
			auto FileExists(const FFilePath& Path) -> bool override { return IPlatformFile::GetPlatformPhysical().FileExists(Path); }
			auto DirectoryExists(const FFilePath& Path) -> bool override { return IPlatformFile::GetPlatformPhysical().DirectoryExists(Path); }
			auto FileSize(const FFilePath& Path) -> int64 override { return IPlatformFile::GetPlatformPhysical().FileSize(Path); }
			auto MakeDirectory(const FFilePath& Path, bool Tree) -> bool override
			{
				auto& Platform = IPlatformFile::GetPlatformPhysical();
				return Tree ? Platform.CreateDirectoryTree(Path) : Platform.CreateDirectory(Path);
			}
			auto IterateDirectory(const FFilePath& Path, IPlatformFile::FDirectoryVisitor& Visitor) -> bool override
			{ return IPlatformFile::GetPlatformPhysical().IterateDirectory(Path, Visitor); }
			auto IterateDirectory(const FFilePath& Path, IPlatformFile::FDirectoryVisitorFunc Visitor) -> bool override
			{ return IPlatformFile::GetPlatformPhysical().IterateDirectory(Path, std::move(Visitor)); }
			auto IterateDirectoryRecursively(const FFilePath& Path, IPlatformFile::FDirectoryVisitor& Visitor) -> bool override
			{ return IPlatformFile::GetPlatformPhysical().IterateDirectoryRecursively(Path, Visitor); }
			auto IterateDirectoryRecursively(const FFilePath& Path, IPlatformFile::FDirectoryVisitorFunc Visitor) -> bool override
			{ return IPlatformFile::GetPlatformPhysical().IterateDirectoryRecursively(Path, std::move(Visitor)); }
		};
	}

	auto IPlatformFile::GetPlatformPhysical() -> IPlatformFile&
	{
		static FPhysicalPlatformFile Instance;
		return Instance;
	}

	auto IFileManager::Get() -> IFileManager&
	{
		static FFileManagerGeneric Instance;
		return Instance;
	}

	auto IPlatformFile::IterateDirectory(const FFilePath& Directory, FDirectoryVisitorFunc Visitor) -> bool
	{
		FDirectoryVisitorFuncWrapper Wrapper(std::move(Visitor));
		return IterateDirectory(Directory, Wrapper);
	}

	auto IPlatformFile::IterateDirectoryRecursively(const FFilePath& Directory, FDirectoryVisitor& Visitor) -> bool
	{
		struct FRecursiveVisitor final : FDirectoryVisitor
		{
			FDirectoryVisitor& Inner;
			std::vector<FFilePath> Pending;
			bool bContinue = true;
			explicit FRecursiveVisitor(FDirectoryVisitor& InVisitor) : Inner(InVisitor) {}
			auto ShouldVisitLeafPathname(std::string_view Name) -> bool override { return Inner.ShouldVisitLeafPathname(Name); }
			auto Visit(const FFilePath& Path, bool bIsDirectory) -> bool override
			{
				bContinue = Inner.Visit(Path, bIsDirectory);
				if (bContinue && bIsDirectory) Pending.push_back(Path);
				return bContinue;
			}
		} Wrapper(Visitor);
		if (!IterateDirectory(Directory, Wrapper)) return false;
		while (Wrapper.bContinue && !Wrapper.Pending.empty())
		{
			auto Path = std::move(Wrapper.Pending.back());
			Wrapper.Pending.pop_back();
			IterateDirectory(Path, Wrapper);
		}
		return Wrapper.bContinue;
	}

	auto IPlatformFile::IterateDirectoryRecursively(const FFilePath& Directory, FDirectoryVisitorFunc Visitor) -> bool
	{
		FDirectoryVisitorFuncWrapper Wrapper(std::move(Visitor));
		return IterateDirectoryRecursively(Directory, Wrapper);
	}
}
