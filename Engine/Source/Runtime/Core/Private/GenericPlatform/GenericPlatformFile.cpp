#include "HAL/PlatformFile.h"
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Durin
{
	auto FFileSystemError::GetMessage() const -> std::string
	{
		return NativeError ? std::format("{} [{}:{}: {}]", Message, NativeError.category().name(),
			NativeError.value(), NativeError.message()) : Message;
	}

	auto ToError(const FFileSystemError& File) -> UnifiedError::FError
	{
		return UnifiedError::FError{{"File", "IoError"}, File.GetMessage()};
	}

	namespace
	{
		class FNativeFileHandle final : public IFileHandle
		{
		public:
#if defined(_WIN32)
			explicit FNativeFileHandle(HANDLE InHandle) : Handle(InHandle) {}
			~FNativeFileHandle() override { CloseHandle(Handle); }
#else
			explicit FNativeFileHandle(int InHandle) : Handle(InHandle) {}
			~FNativeFileHandle() override { close(Handle); }
#endif
			auto Tell() -> int64 override
			{
#if defined(_WIN32)
				LARGE_INTEGER Zero{}, Position{};
				return SetFilePointerEx(Handle, Zero, &Position, FILE_CURRENT) ? Position.QuadPart : -1;
#else
				return lseek(Handle, 0, SEEK_CUR);
#endif
			}
			auto Seek(int64 Position) -> bool override
			{
				if (Position < 0) return false;
#if defined(_WIN32)
				LARGE_INTEGER Offset; Offset.QuadPart = Position;
				return SetFilePointerEx(Handle, Offset, nullptr, FILE_BEGIN) != 0;
#else
				return lseek(Handle, Position, SEEK_SET) >= 0;
#endif
			}
			auto SeekFromEnd(int64 Offset) -> bool override
			{
				const int64 Length = Size();
				return Offset <= 0 && Length >= 0 && Offset >= -Length && Seek(Length + Offset);
			}
			auto Size() -> int64 override
			{
#if defined(_WIN32)
				LARGE_INTEGER Length{};
				return GetFileSizeEx(Handle, &Length) ? Length.QuadPart : -1;
#else
				struct stat Status{};
				return fstat(Handle, &Status) == 0 && S_ISREG(Status.st_mode) ? Status.st_size : -1;
#endif
			}
			auto Read(uint8* Destination, int64 Count) -> bool override
			{
				if (Count < 0 || (Count && !Destination)) return false;
				while (Count > 0)
				{
#if defined(_WIN32)
					DWORD Done = 0;
					if (!ReadFile(Handle, Destination, static_cast<DWORD>(std::min<int64>(Count, MAXDWORD)), &Done, nullptr) || Done == 0) return false;
#else
					const auto Done = read(Handle, Destination, static_cast<size_t>(std::min<int64>(Count, std::numeric_limits<ssize_t>::max())));
					if (Done < 0 && errno == EINTR) continue;
					if (Done <= 0) return false;
#endif
					Destination += Done;
					Count -= Done;
				}
				return true;
			}
			auto ReadAt(uint8* Destination, int64 Count, int64 Offset) -> bool override
			{
				const int64 Length = Size();
				if (Offset < 0 || Count < 0 || Offset > Length || Count > Length - Offset) return false;
				const int64 Position = Tell();
				if (Position < 0 || !Seek(Offset)) return false;
				const bool bRead = Read(Destination, Count);
				const bool bRestored = Seek(Position);
				return bRead && bRestored;
			}
			auto Write(const uint8* Source, int64 Count) -> bool override
			{
				if (Count < 0 || (Count && !Source)) return false;
				while (Count > 0)
				{
#if defined(_WIN32)
					DWORD Done = 0;
					if (!WriteFile(Handle, Source, static_cast<DWORD>(std::min<int64>(Count, MAXDWORD)), &Done, nullptr) || Done == 0) return false;
#else
					const auto Done = write(Handle, Source, static_cast<size_t>(std::min<int64>(Count, std::numeric_limits<ssize_t>::max())));
					if (Done < 0 && errno == EINTR) continue;
					if (Done <= 0) return false;
#endif
					Source += Done;
					Count -= Done;
				}
				return true;
			}
			auto Flush(bool bFullFlush) -> bool override
			{
#if defined(_WIN32)
				return FlushFileBuffers(Handle) != 0;
#else
#if defined(__APPLE__)
				if (bFullFlush) return fcntl(Handle, F_FULLFSYNC) == 0;
#endif
				return fsync(Handle) == 0;
#endif
			}
			auto Truncate(int64 Length) -> bool override
			{
				if (Length < 0) return false;
#if defined(_WIN32)
				const int64 Position = Tell();
				if (Position < 0 || !Seek(Length)) return false;
				const bool bTruncated = SetEndOfFile(Handle) != 0;
				const bool bRestored = Seek(Position);
				return bTruncated && bRestored;
#else
				return ftruncate(Handle, Length) == 0;
#endif
			}
		private:
#if defined(_WIN32)
			HANDLE Handle;
#else
			int Handle;
#endif
		};

		auto OpenNative(const FFilePath& Filename, bool bWrite, uint8 Flags) -> FFileOpenResult
		{
			std::error_code Error;
			const auto Path = std::filesystem::absolute(Filename, Error).lexically_normal();
			if (Error) return std::unexpected(FFileSystemError{std::format("Cannot resolve '{}'.", Filename.generic_string()), Error});
			const bool bAppend = bWrite && (Flags & static_cast<uint8>(IPlatformFile::EOpenWriteFlags::Append));
#if defined(_WIN32)
			DWORD Access = bWrite ? GENERIC_WRITE : GENERIC_READ;
			DWORD Share = bWrite ? 0 : FILE_SHARE_READ;
			DWORD Attributes = FILE_ATTRIBUTE_NORMAL;
			if (bWrite)
			{
				if (Flags & static_cast<uint8>(IPlatformFile::EOpenWriteFlags::AllowRead)) { Access |= GENERIC_READ; Share |= FILE_SHARE_READ; }
			}
			else
			{
				if (Flags & static_cast<uint8>(IPlatformFile::EOpenReadFlags::AllowWrite)) Share |= FILE_SHARE_WRITE;
				if (Flags & static_cast<uint8>(IPlatformFile::EOpenReadFlags::AllowDelete)) Share |= FILE_SHARE_DELETE;
				if (Flags & static_cast<uint8>(IPlatformFile::EOpenReadFlags::NoBuffering)) Attributes |= FILE_FLAG_NO_BUFFERING;
			}
			const auto Native = CreateFileW(Path.c_str(), Access, Share, nullptr,
				bWrite ? (bAppend ? OPEN_ALWAYS : CREATE_ALWAYS) : OPEN_EXISTING, Attributes, nullptr);
			if (Native == INVALID_HANDLE_VALUE) Error = {static_cast<int>(GetLastError()), std::system_category()};
#else
			if (!bWrite && (Flags & static_cast<uint8>(IPlatformFile::EOpenReadFlags::NoBuffering)))
				return std::unexpected(FFileSystemError{"Unbuffered file reads are not implemented on this platform.", std::make_error_code(std::errc::operation_not_supported)});
			const int Mode = bWrite ? ((Flags & static_cast<uint8>(IPlatformFile::EOpenWriteFlags::AllowRead)) ? O_RDWR : O_WRONLY) | O_CREAT | (bAppend ? 0 : O_TRUNC) : O_RDONLY;
			const int Native = open(Path.c_str(), Mode, 0666);
			if (Native < 0) Error = {errno, std::system_category()};
#endif
			if (Error) return std::unexpected(FFileSystemError{std::format("Cannot open '{}' for {}.", Path.generic_string(), bWrite ? "writing" : "reading"), Error});
			auto Handle = std::make_unique<FNativeFileHandle>(Native);
			if (Handle->Size() < 0 || (bAppend && !Handle->SeekFromEnd(0)))
				return std::unexpected(FFileSystemError{std::format("Cannot initialize file handle '{}'.", Path.generic_string()), std::make_error_code(std::errc::io_error)});
			return Handle;
		}
	}

	auto IPlatformFile::OpenRead(const FFilePath& Filename, EOpenReadFlags Flags) -> FFileOpenResult
	{
		return OpenNative(Filename, false, static_cast<uint8>(Flags));
	}
	auto IPlatformFile::OpenWrite(const FFilePath& Filename, EOpenWriteFlags Flags) -> FFileOpenResult
	{
		return OpenNative(Filename, true, static_cast<uint8>(Flags));
	}
}
