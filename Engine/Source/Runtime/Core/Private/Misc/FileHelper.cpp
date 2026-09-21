#include "Misc/FileHelper.h"
#include <cerrno>
#include <cstdio>
#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Durin
{
	auto FFileError::ToString() const -> std::string
	{
		std::string_view Name = "unknown";
		switch (Operation)
		{
		case EFileOperation::NormalizePath: Name = "normalize path"; break;
		case EFileOperation::Inspect: Name = "inspect"; break;
		case EFileOperation::OpenRead: Name = "open for reading"; break;
		case EFileOperation::QuerySize: Name = "query size"; break;
		case EFileOperation::Read: Name = "read"; break;
		case EFileOperation::CreateParentDirectories: Name = "create parent directories"; break;
		case EFileOperation::OpenWrite: Name = "open for writing"; break;
		case EFileOperation::Write: Name = "write"; break;
		case EFileOperation::Close: Name = "close"; break;
		case EFileOperation::CreateTemporaryFile: Name = "create temporary file"; break;
		case EFileOperation::WriteTemporaryFile: Name = "write temporary file"; break;
		case EFileOperation::FlushTemporaryFile: Name = "flush temporary file"; break;
		case EFileOperation::CloseTemporaryFile: Name = "close temporary file"; break;
		case EFileOperation::ReplaceDestination: Name = "replace destination"; break;
		}
		size_t Longest = 0;
		for (const auto& Component : Path) Longest = std::max(Longest, Component.native().size());
		auto Message = std::format("File I/O failed to {}: {} ({}:{}: {}; path length {}, longest component {}).",
			Name, Path.generic_string(), NativeError.category().name(), NativeError.value(),
			NativeError.message(), Path.native().size(), Longest);
		if (!RelatedPath.empty()) Message += std::format(" Related path: {}.", RelatedPath.generic_string());
		if (Range) Message += std::format(" Offset: {}, size: {}.", Range->Offset, Range->Size);
		return Message;
	}
}

namespace Durin::FFileHelper
{
	namespace
	{
		auto Normalize(const FFilePath& Path) -> std::expected<FFilePath, FFileError>
		{
			std::error_code Error;
			auto Absolute = std::filesystem::absolute(Path, Error);
			if (Error) return std::unexpected(FFileError{EFileOperation::NormalizePath, Error, Path});
			return Absolute.lexically_normal();
		}

#if defined(_WIN32)
		class FNativeFileHandle final : public IFileHandle
		{
		public:
			FNativeFileHandle(HANDLE InHandle, uint64 InSize, std::filesystem::path InPath)
				: Handle(InHandle), Size(InSize), Path(std::move(InPath)) {}
			~FNativeFileHandle() override { if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle); }
			auto GetSize() const -> uint64 override { return Size; }
			auto ReadAt(uint64 Offset, FMutableByteView Output) -> std::expected<void, FFileError> override
			{
				if (Offset > Size || Output.size_bytes() > Size - Offset)
				{
					return std::unexpected(FFileError{EFileOperation::Read,
						std::make_error_code(std::errc::result_out_of_range), Path, {}, FFileError::FRange{Offset, Output.size_bytes()}});
				}
				if (Output.empty()) return {};
				LARGE_INTEGER Position; Position.QuadPart = static_cast<LONGLONG>(Offset);
				if (!SetFilePointerEx(Handle, Position, nullptr, FILE_BEGIN))
				{
					return std::unexpected(FFileError{EFileOperation::Read,
						{static_cast<int>(GetLastError()), std::system_category()}, Path, {}, FFileError::FRange{Offset, Output.size_bytes()}});
				}
				size_t Complete = 0;
				while (Complete < Output.size_bytes())
				{
					const DWORD Requested = static_cast<DWORD>(std::min<size_t>(
						Output.size_bytes() - Complete, MAXDWORD));
					DWORD Read = 0;
					if (!ReadFile(Handle, Output.data() + Complete, Requested, &Read, nullptr)
						|| Read != Requested)
					{
						const DWORD Native = GetLastError();
						return std::unexpected(FFileError{EFileOperation::Read,
							{static_cast<int>(Native == ERROR_SUCCESS ? ERROR_HANDLE_EOF : Native),
								std::system_category()}, Path, {}, FFileError::FRange{Offset + Complete, Output.size_bytes() - Complete}});
					}
					Complete += Read;
				}
				return {};
			}
		private:
			HANDLE Handle = INVALID_HANDLE_VALUE;
			uint64 Size = 0;
			std::filesystem::path Path;
		};
#else
		class FNativeFileHandle final : public IFileHandle
		{
		public:
			FNativeFileHandle(int InFile, uint64 InSize, std::filesystem::path InPath)
				: File(InFile), Size(InSize), Path(std::move(InPath)) {}
			~FNativeFileHandle() override { if (File >= 0) close(File); }
			auto GetSize() const -> uint64 override { return Size; }
			auto ReadAt(uint64 Offset, FMutableByteView Output) -> std::expected<void, FFileError> override
			{
				if (Offset > Size || Output.size_bytes() > Size - Offset
					|| Offset > static_cast<uint64>(std::numeric_limits<off_t>::max()))
				{
					return std::unexpected(FFileError{EFileOperation::Read,
						std::make_error_code(std::errc::result_out_of_range), Path, {}, FFileError::FRange{Offset, Output.size_bytes()}});
				}
				size_t Complete = 0;
				while (Complete < Output.size_bytes())
				{
					const size_t Requested = std::min<size_t>(Output.size_bytes() - Complete,
						static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
					const ssize_t Read = pread(File, Output.data() + Complete, Requested,
						static_cast<off_t>(Offset + Complete));
					if (Read <= 0)
					{
						return std::unexpected(FFileError{EFileOperation::Read,
							{Read == 0 ? EIO : errno, std::system_category()}, Path, {}, FFileError::FRange{Offset + Complete, Output.size_bytes() - Complete}});
					}
					Complete += static_cast<size_t>(Read);
				}
				return {};
			}
		private:
			int File = -1;
			uint64 Size = 0;
			std::filesystem::path Path;
		};
#endif

		auto MakeTemporaryPath(const std::filesystem::path& Destination) -> std::filesystem::path
		{
			static std::atomic_uint64_t UniquenessToken{
				static_cast<uint64>(std::chrono::steady_clock::now().time_since_epoch().count())};
#if defined(_WIN32)
			const uint64 ProcessId = static_cast<uint64>(GetCurrentProcessId());
#else
			const uint64 ProcessId = static_cast<uint64>(getpid());
#endif
			return Destination.parent_path()
				/ std::format(".durin-tmp-{:08x}-{:016x}.tmp", ProcessId, UniquenessToken.fetch_add(1, std::memory_order_relaxed));
		}

#if defined(_WIN32)
		auto WriteTemporaryFile(
			const std::filesystem::path& TemporaryPath,
			FByteView Array
		) -> std::expected<void, FFileError>
		{
			const HANDLE File = CreateFileW(
				TemporaryPath.c_str(),
				GENERIC_WRITE,
				0,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL,
				nullptr
			);
			if (File == INVALID_HANDLE_VALUE)
			{
				return std::unexpected(FFileError{EFileOperation::CreateTemporaryFile,
					{static_cast<int>(GetLastError()), std::system_category()}, TemporaryPath});
			}

			size_t Offset = 0;
			while (Offset < Array.size_bytes())
			{
				const DWORD ByteCount = static_cast<DWORD>(std::min<size_t>(Array.size_bytes() - Offset, MAXDWORD));
				DWORD BytesWritten = 0;
				if (!WriteFile(File, Array.data() + Offset, ByteCount, &BytesWritten, nullptr) || BytesWritten != ByteCount)
				{
					const DWORD Error = GetLastError();
					CloseHandle(File);
					return std::unexpected(FFileError{EFileOperation::WriteTemporaryFile,
						{static_cast<int>(Error), std::system_category()}, TemporaryPath});
				}
				Offset += BytesWritten;
			}

			if (!FlushFileBuffers(File))
			{
				const DWORD Error = GetLastError();
				CloseHandle(File);
				return std::unexpected(FFileError{EFileOperation::FlushTemporaryFile,
					{static_cast<int>(Error), std::system_category()}, TemporaryPath});
			}
			if (!CloseHandle(File))
			{
				return std::unexpected(FFileError{EFileOperation::CloseTemporaryFile,
					{static_cast<int>(GetLastError()), std::system_category()}, TemporaryPath});
			}
			return {};
		}
#else
		auto WriteTemporaryFile(
			const std::filesystem::path& TemporaryPath,
			FByteView Array
		) -> std::expected<void, FFileError>
		{
			const int File = open(TemporaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
			if (File == -1)
			{
				return std::unexpected(FFileError{EFileOperation::CreateTemporaryFile,
					{errno, std::system_category()}, TemporaryPath});
			}

			size_t Offset = 0;
			while (Offset < Array.size_bytes())
			{
				const ssize_t BytesWritten = write(File, Array.data() + Offset, Array.size_bytes() - Offset);
				if (BytesWritten <= 0)
				{
					const int Error = errno;
					close(File);
					return std::unexpected(FFileError{EFileOperation::WriteTemporaryFile,
						{Error, std::system_category()}, TemporaryPath});
				}
				Offset += static_cast<size_t>(BytesWritten);
			}

			if (fsync(File) != 0)
			{
				const int Error = errno;
				close(File);
				return std::unexpected(FFileError{EFileOperation::FlushTemporaryFile,
					{Error, std::system_category()}, TemporaryPath});
			}
			if (close(File) != 0)
			{
				return std::unexpected(FFileError{EFileOperation::CloseTemporaryFile,
					{errno, std::system_category()}, TemporaryPath});
			}
			return {};
		}
#endif
		template<typename T>
		auto Load(const FFilePath& Path) -> std::expected<T, FFileError>
		{
			auto File = OpenRead(Path);
			if (!File) return std::unexpected(std::move(File.error()));
			T Bytes;
			const uint64 Size = (*File)->GetSize();
			if (Size > Bytes.max_size())
				return std::unexpected(FFileError{EFileOperation::QuerySize,
					std::make_error_code(std::errc::file_too_large), Path});
			Bytes.resize(static_cast<size_t>(Size));
			auto Read = (*File)->ReadAt(0, std::as_writable_bytes(std::span(Bytes)));
			if (!Read) return std::unexpected(std::move(Read.error()));
			return Bytes;
		}
	}

	auto OpenRead(const FFilePath& FilePath) -> std::expected<std::unique_ptr<IFileHandle>, FFileError>
	{
		auto Absolute = Normalize(FilePath);
		if (!Absolute) return std::unexpected(std::move(Absolute.error()));
		const auto& Path = *Absolute;
#if defined(_WIN32)
		const HANDLE File = CreateFileW(Path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (File == INVALID_HANDLE_VALUE)
		{
			return std::unexpected(FFileError{EFileOperation::OpenRead,
				{static_cast<int>(GetLastError()), std::system_category()}, Path});
		}
		LARGE_INTEGER Size;
		if (!GetFileSizeEx(File, &Size) || Size.QuadPart < 0)
		{
			const DWORD Error = GetLastError(); CloseHandle(File);
			return std::unexpected(FFileError{EFileOperation::QuerySize,
				{static_cast<int>(Error), std::system_category()}, Path});
		}
		return std::make_unique<FNativeFileHandle>(File, static_cast<uint64>(Size.QuadPart), Path);
#else
		const int File = open(Path.c_str(), O_RDONLY);
		if (File == -1)
		{
			return std::unexpected(FFileError{EFileOperation::OpenRead,
				{errno, std::system_category()}, Path});
		}
		struct stat Status{};
		if (fstat(File, &Status) != 0 || Status.st_size < 0)
		{
			const int Error = errno; close(File);
			return std::unexpected(FFileError{EFileOperation::QuerySize,
				{Error, std::system_category()}, Path});
		}
		return std::make_unique<FNativeFileHandle>(File, static_cast<uint64>(Status.st_size), Path);
#endif
	}

	auto FileExists(const FFilePath& Path) -> std::expected<bool, FFileError>
	{
		auto Absolute = Normalize(Path);
		if (!Absolute) return std::unexpected(std::move(Absolute.error()));
#if defined(_WIN32)
		if (GetFileAttributesW(Absolute->c_str()) != INVALID_FILE_ATTRIBUTES) return true;
		const DWORD NativeError = GetLastError();
		if (NativeError == ERROR_FILE_NOT_FOUND || NativeError == ERROR_PATH_NOT_FOUND) return false;
		return std::unexpected(FFileError{EFileOperation::Inspect,
			{static_cast<int>(NativeError), std::system_category()}, *Absolute});
#else
		std::error_code Error;
		const bool Exists = std::filesystem::exists(*Absolute, Error);
		if (Error) return std::unexpected(FFileError{EFileOperation::Inspect, Error, *Absolute});
		return Exists;
#endif
	}

	auto LoadFileToArray(const FFilePath& Path) -> std::expected<FByteBuffer, FFileError>
	{
		return Load<FByteBuffer>(Path);
	}

	auto LoadFileToString(const FFilePath& Path) -> std::expected<std::string, FFileError>
	{
		return Load<std::string>(Path);
	}

	auto HashFileXx128(const FFilePath& Path) -> std::expected<FXxHash128, FFileError>
	{
		auto File = OpenRead(Path);
		if (!File) return std::unexpected(std::move(File.error()));
		std::array<std::byte, 64 * 1024> Buffer{};
		FXxHash128Builder Builder;
		for (uint64 Offset = 0; Offset < (*File)->GetSize();)
		{
			const auto Count = static_cast<size_t>(std::min<uint64>(Buffer.size(), (*File)->GetSize() - Offset));
			auto Read = (*File)->ReadAt(Offset, std::span(Buffer).first(Count));
			if (!Read) return std::unexpected(std::move(Read.error()));
			Builder.Update(Buffer.data(), Count);
			Offset += Count;
		}
		return Builder.Finalize();
	}

	auto SaveArrayToFile(FByteView Bytes, const FFilePath& Path) -> std::expected<void, FFileError>
	{
		auto Absolute = Normalize(Path);
		if (!Absolute) return std::unexpected(std::move(Absolute.error()));
		std::error_code Error;
		std::filesystem::create_directories(Absolute->parent_path(), Error);
		if (Error) return std::unexpected(FFileError{EFileOperation::CreateParentDirectories, Error, *Absolute});
		std::FILE* File = nullptr;
#if defined(_WIN32)
		const int OpenError = _wfopen_s(&File, Absolute->c_str(), L"wb");
#else
		File = std::fopen(Absolute->c_str(), "wb");
		const int OpenError = File ? 0 : errno;
#endif
		if (!File) return std::unexpected(FFileError{EFileOperation::OpenWrite,
			{OpenError, std::generic_category()}, *Absolute});
		errno = 0;
		const size_t Written = Bytes.empty() ? 0 : std::fwrite(Bytes.data(), 1, Bytes.size_bytes(), File);
		if (Written != Bytes.size_bytes())
		{
			const int WriteError = errno ? errno : EIO;
			std::fclose(File);
			return std::unexpected(FFileError{EFileOperation::Write, {WriteError, std::generic_category()},
				*Absolute, {}, FFileError::FRange{Written, Bytes.size_bytes() - Written}});
		}
		errno = 0;
		if (std::fclose(File) != 0)
			return std::unexpected(FFileError{EFileOperation::Close,
				{errno ? errno : EIO, std::generic_category()}, *Absolute});
		return {};
	}

	auto SaveArrayToNewFile(
		FByteView Array,
		const std::filesystem::path& FilePath
	) -> std::expected<void, FFileError>
	{
		std::error_code ErrorCode;
		const auto Destination = std::filesystem::absolute(FilePath, ErrorCode).lexically_normal();
		if (ErrorCode)
		{
			return std::unexpected(FFileError{EFileOperation::NormalizePath, ErrorCode, FilePath});
		}
		std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
		if (ErrorCode)
		{
			return std::unexpected(FFileError{EFileOperation::CreateParentDirectories, ErrorCode, Destination});
		}
		auto Written = WriteTemporaryFile(Destination, Array);
		if (Written) return {};
		auto Error = std::move(Written.error());
		// A failed exclusive create never grants ownership of the existing path.
		if (Error.Operation != EFileOperation::CreateTemporaryFile)
			std::filesystem::remove(Destination, ErrorCode);
		return std::unexpected(std::move(Error));
	}

	auto SaveArrayToFileAtomically(
		FByteView Array,
		const std::filesystem::path& FilePath
	) -> std::expected<void, FFileError>
	{
		std::error_code ErrorCode;
		const std::filesystem::path Destination = std::filesystem::absolute(FilePath, ErrorCode).lexically_normal();
		if (ErrorCode)
		{
			return std::unexpected(FFileError{EFileOperation::NormalizePath, ErrorCode, FilePath});
		}

		std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
		if (ErrorCode)
		{
			return std::unexpected(FFileError{EFileOperation::CreateParentDirectories, ErrorCode, Destination});
		}

		FFileError Error{};
		std::filesystem::path TemporaryPath;
		bool bCreated = false;
		for (uint32 Attempt = 0; Attempt < 64 && !bCreated; ++Attempt)
		{
			TemporaryPath = MakeTemporaryPath(Destination);
			auto Written = WriteTemporaryFile(TemporaryPath, Array);
			bCreated = Written.has_value();
			if (!Written) Error = std::move(Written.error());
			if (!bCreated && Error.Operation == EFileOperation::CreateTemporaryFile)
			{
				bool bRetryable = Error.NativeError == std::errc::file_exists;
#if defined(_WIN32)
				bRetryable |= Error.NativeError.value() == ERROR_ACCESS_DENIED
					|| Error.NativeError.value() == ERROR_SHARING_VIOLATION;
#endif
				if (bRetryable && Attempt + 1 < 64)
				{
					if (Attempt < 8) std::this_thread::yield();
					else std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
			}
			if (!bCreated) break;
		}
		if (!bCreated)
		{
			if (Error.Operation != EFileOperation::CreateTemporaryFile)
			{
				std::filesystem::remove(TemporaryPath, ErrorCode);
			}
			return std::unexpected(std::move(Error));
		}

#if defined(_WIN32)
		ErrorCode.clear();
		if (std::filesystem::is_directory(Destination, ErrorCode))
		{
			Error = FFileError{EFileOperation::ReplaceDestination, {ERROR_ACCESS_DENIED, std::system_category()}, Destination};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}

		DWORD ReplacementError = ERROR_SUCCESS;
		bool bReplaced = false;
		for (uint32 Attempt = 0; Attempt < 128; ++Attempt)
		{
			if (MoveFileExW(TemporaryPath.c_str(), Destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				bReplaced = true;
				break;
			}

			ReplacementError = GetLastError();
			if (ReplacementError != ERROR_SHARING_VIOLATION && ReplacementError != ERROR_ACCESS_DENIED)
			{
				break;
			}

			if (Attempt < 16) std::this_thread::yield();
			else std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (!bReplaced)
		{
			Error = FFileError{EFileOperation::ReplaceDestination, {static_cast<int>(ReplacementError), std::system_category()}, Destination};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
#else
		std::filesystem::rename(TemporaryPath, Destination, ErrorCode);
		if (ErrorCode)
		{
			Error = FFileError{EFileOperation::ReplaceDestination, ErrorCode, Destination};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
#endif
		return {};
	}

	auto CopyFileAtomically(
		const std::filesystem::path& SourcePath,
		const std::filesystem::path& DestinationPath
	) -> std::expected<void, FFileError>
	{
		std::error_code ErrorCode;
		const std::filesystem::path Source =
			std::filesystem::absolute(SourcePath, ErrorCode).lexically_normal();
		if (ErrorCode)
		{
			return std::unexpected(FFileError{EFileOperation::NormalizePath, ErrorCode, SourcePath, SourcePath});
		}
		const std::filesystem::path Destination =
			std::filesystem::absolute(DestinationPath, ErrorCode).lexically_normal();
		if (ErrorCode)
		{
			return std::unexpected(FFileError{EFileOperation::NormalizePath, ErrorCode, DestinationPath, SourcePath});
		}
		std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
		if (ErrorCode)
		{
			return std::unexpected(FFileError{EFileOperation::CreateParentDirectories, ErrorCode, Destination, SourcePath});
		}

		FFileError Error{};
		std::filesystem::path TemporaryPath;
		bool bCreated = false;
		for (uint32 Attempt = 0; Attempt < 64 && !bCreated; ++Attempt)
		{
			TemporaryPath = MakeTemporaryPath(Destination);
			ErrorCode.clear();
			bCreated = std::filesystem::copy_file(Source, TemporaryPath,
				std::filesystem::copy_options::none, ErrorCode);
			if (!bCreated)
			{
				Error = FFileError{ErrorCode == std::errc::file_exists
						? EFileOperation::CreateTemporaryFile
						: EFileOperation::WriteTemporaryFile, ErrorCode, TemporaryPath, SourcePath};
				if (ErrorCode == std::errc::file_exists && Attempt + 1 < 64)
				{
					if (Attempt < 8) std::this_thread::yield();
					else std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				return std::unexpected(std::move(Error));
			}
		}
		if (!bCreated) return std::unexpected(std::move(Error));

#if defined(_WIN32)
		const HANDLE TemporaryFile = CreateFileW(TemporaryPath.c_str(), GENERIC_WRITE,
			0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (TemporaryFile == INVALID_HANDLE_VALUE || !FlushFileBuffers(TemporaryFile))
		{
			const DWORD NativeError = GetLastError();
			if (TemporaryFile != INVALID_HANDLE_VALUE) CloseHandle(TemporaryFile);
			Error = FFileError{EFileOperation::FlushTemporaryFile, {static_cast<int>(NativeError), std::system_category()}, TemporaryPath, SourcePath};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
		if (!CloseHandle(TemporaryFile))
		{
			Error = FFileError{EFileOperation::CloseTemporaryFile, {static_cast<int>(GetLastError()), std::system_category()}, TemporaryPath, SourcePath};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
		DWORD ReplacementError = ERROR_SUCCESS;
		bool bReplaced = false;
		for (uint32 Attempt = 0; Attempt < 128; ++Attempt)
		{
			if (MoveFileExW(TemporaryPath.c_str(), Destination.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				bReplaced = true;
				break;
			}
			ReplacementError = GetLastError();
			if (ReplacementError != ERROR_SHARING_VIOLATION
				&& ReplacementError != ERROR_ACCESS_DENIED) break;
			if (Attempt < 16) std::this_thread::yield();
			else std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (!bReplaced)
		{
			Error = FFileError{EFileOperation::ReplaceDestination, {static_cast<int>(ReplacementError), std::system_category()}, Destination, SourcePath};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
#else
		const int TemporaryFile = open(TemporaryPath.c_str(), O_RDWR);
		if (TemporaryFile == -1 || fsync(TemporaryFile) != 0)
		{
			const int NativeError = errno;
			if (TemporaryFile != -1) close(TemporaryFile);
			Error = FFileError{EFileOperation::FlushTemporaryFile, {NativeError, std::system_category()}, TemporaryPath, SourcePath};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
		if (close(TemporaryFile) != 0)
		{
			Error = FFileError{EFileOperation::CloseTemporaryFile, {errno, std::system_category()}, TemporaryPath, SourcePath};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
		std::filesystem::rename(TemporaryPath, Destination, ErrorCode);
		if (ErrorCode)
		{
			Error = FFileError{EFileOperation::ReplaceDestination, ErrorCode, Destination, SourcePath};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
#endif
		return {};
	}

}
