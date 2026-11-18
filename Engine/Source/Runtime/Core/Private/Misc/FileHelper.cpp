#include "Misc/FileHelper.h"
#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Durin::FFileHelper
{
	namespace
	{
		auto Normalize(const FFilePath& Path) -> std::expected<FFilePath, FFileSystemError>
		{
			std::error_code Error;
			auto Absolute = std::filesystem::absolute(Path, Error);
			if (Error) return std::unexpected(FFileSystemError{std::format("Failed to normalize path: {}.", FFilePath(Path).generic_string()), Error});
			return Absolute.lexically_normal();
		}


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
		auto WriteNewFile(
			const std::filesystem::path& TemporaryPath,
			FByteView Array, bool& bCreated
		) -> std::expected<void, FFileSystemError>
		{
			bCreated = false;
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
				return std::unexpected(FFileSystemError{std::format("Failed to create temporary file: {}.", FFilePath(TemporaryPath).generic_string()), {static_cast<int>(GetLastError()), std::system_category()}});
			}

			bCreated = true;
			size_t Offset = 0;
			while (Offset < Array.size_bytes())
			{
				const DWORD ByteCount = static_cast<DWORD>(std::min<size_t>(Array.size_bytes() - Offset, MAXDWORD));
				DWORD BytesWritten = 0;
				if (!WriteFile(File, Array.data() + Offset, ByteCount, &BytesWritten, nullptr) || BytesWritten != ByteCount)
				{
					const DWORD Error = GetLastError();
					CloseHandle(File);
					return std::unexpected(FFileSystemError{std::format("Failed to write temporary file: {}.", FFilePath(TemporaryPath).generic_string()), {static_cast<int>(Error), std::system_category()}});
				}
				Offset += BytesWritten;
			}

			if (!FlushFileBuffers(File))
			{
				const DWORD Error = GetLastError();
				CloseHandle(File);
				return std::unexpected(FFileSystemError{std::format("Failed to flush temporary file: {}.", FFilePath(TemporaryPath).generic_string()), {static_cast<int>(Error), std::system_category()}});
			}
			if (!CloseHandle(File))
			{
				return std::unexpected(FFileSystemError{std::format("Failed to close temporary file: {}.", FFilePath(TemporaryPath).generic_string()), {static_cast<int>(GetLastError()), std::system_category()}});
			}
			return {};
		}
#else
		auto WriteNewFile(
			const std::filesystem::path& TemporaryPath,
			FByteView Array, bool& bCreated
		) -> std::expected<void, FFileSystemError>
		{
			bCreated = false;
			const int File = open(TemporaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
			if (File == -1)
			{
				return std::unexpected(FFileSystemError{std::format("Failed to create temporary file: {}.", FFilePath(TemporaryPath).generic_string()), {errno, std::system_category()}});
			}

			bCreated = true;
			size_t Offset = 0;
			while (Offset < Array.size_bytes())
			{
				const ssize_t BytesWritten = write(File, Array.data() + Offset, Array.size_bytes() - Offset);
				if (BytesWritten <= 0)
				{
					const int Error = errno;
					close(File);
					return std::unexpected(FFileSystemError{std::format("Failed to write temporary file: {}.", FFilePath(TemporaryPath).generic_string()), {Error, std::system_category()}});
				}
				Offset += static_cast<size_t>(BytesWritten);
			}

			if (fsync(File) != 0)
			{
				const int Error = errno;
				close(File);
				return std::unexpected(FFileSystemError{std::format("Failed to flush temporary file: {}.", FFilePath(TemporaryPath).generic_string()), {Error, std::system_category()}});
			}
			if (close(File) != 0)
			{
				return std::unexpected(FFileSystemError{std::format("Failed to close temporary file: {}.", FFilePath(TemporaryPath).generic_string()), {errno, std::system_category()}});
			}
			return {};
		}
#endif
		template<typename T>
		auto Load(const FFilePath& Path) -> std::expected<T, FFileSystemError>
		{
			auto File = IPlatformFile::GetPlatformPhysical().OpenRead(Path, IPlatformFile::EOpenReadFlags::AllowWrite | IPlatformFile::EOpenReadFlags::AllowDelete);
			if (!File) return std::unexpected(std::move(File.error()));
			T Bytes;
			const int64 FileSize = (*File)->Size();
			if (FileSize < 0) return std::unexpected(FFileSystemError{std::format("Cannot query file size: {}.", Path.generic_string()), std::make_error_code(std::errc::io_error)});
			const uint64 Size = static_cast<uint64>(FileSize);
			if (Size > Bytes.max_size())
				return std::unexpected(FFileSystemError{std::format("Failed to query size: {}.", FFilePath(Path).generic_string()), std::make_error_code(std::errc::file_too_large)});
			Bytes.resize(static_cast<size_t>(Size));
			auto Read = (*File)->Read(reinterpret_cast<uint8*>(Bytes.data()), static_cast<int64>(Size));
			if (!Read) return std::unexpected(FFileSystemError{std::format("Cannot read file: {}.", Path.generic_string()), std::make_error_code(std::errc::io_error)});
			return Bytes;
		}
	}

	auto LoadFileToArray(const FFilePath& Path) -> std::expected<FByteBuffer, FFileSystemError>
	{
		return Load<FByteBuffer>(Path);
	}

	auto LoadFileToArray(const FFilePath& Path, FFileReadOptions Options) -> std::expected<FByteBuffer, FFileSystemError>
	{
		auto File = IPlatformFile::GetPlatformPhysical().OpenRead(Path, IPlatformFile::EOpenReadFlags::AllowWrite | IPlatformFile::EOpenReadFlags::AllowDelete);
		if (!File) return std::unexpected(std::move(File.error()));
		const int64 FileSize = (*File)->Size();
		if (FileSize < 0) return std::unexpected(FFileSystemError{std::format("Cannot query file size: {}.", Path.generic_string()), std::make_error_code(std::errc::io_error)});
		const uint64 Size = static_cast<uint64>(FileSize);
		if (Options.ExpectedBytes && Size != *Options.ExpectedBytes)
			return std::unexpected(FFileSystemError{std::format("Failed to query size: {}. Offset: {}, size: {}.", FFilePath(Path).generic_string(), 0, Size), std::make_error_code(std::errc::message_size)});
		if (Size > Options.MaximumBytes || Size > FByteBuffer{}.max_size())
			return std::unexpected(FFileSystemError{std::format("Failed to query size: {}. Offset: {}, size: {}.", FFilePath(Path).generic_string(), 0, Size), std::make_error_code(std::errc::file_too_large)});
		FByteBuffer Bytes(static_cast<size_t>(Size));
		if (auto Read = (*File)->Read(reinterpret_cast<uint8*>(Bytes.data()), static_cast<int64>(Size)); !Read) return std::unexpected(FFileSystemError{std::format("Cannot read file: {}.", Path.generic_string()), std::make_error_code(std::errc::io_error)});
		return Bytes;
	}

	auto LoadFileToString(const FFilePath& Path) -> std::expected<std::string, FFileSystemError>
	{
		return Load<std::string>(Path);
	}

	auto HashFileXx128(const FFilePath& Path) -> std::expected<FXxHash128, FFileSystemError>
	{
		auto File = IPlatformFile::GetPlatformPhysical().OpenRead(Path, IPlatformFile::EOpenReadFlags::AllowWrite | IPlatformFile::EOpenReadFlags::AllowDelete);
		if (!File) return std::unexpected(std::move(File.error()));
		std::array<std::byte, 64 * 1024> Buffer{};
		FXxHash128Builder Builder;
		const int64 Size = (*File)->Size();
		if (Size < 0) return std::unexpected(FFileSystemError{std::format("Cannot query file size: {}.", Path.generic_string()), std::make_error_code(std::errc::io_error)});
		for (int64 Offset = 0; Offset < Size;)
		{
			const auto Count = static_cast<size_t>(std::min<uint64>(Buffer.size(), Size - Offset));
			auto Read = (*File)->Read(reinterpret_cast<uint8*>(Buffer.data()), static_cast<int64>(Count));
			if (!Read) return std::unexpected(FFileSystemError{std::format("Cannot read file: {}.", Path.generic_string()), std::make_error_code(std::errc::io_error)});
			Builder.Update(Buffer.data(), Count);
			Offset += Count;
		}
		return Builder.Finalize();
	}

	auto SaveArrayToFile(FByteView Bytes, const FFilePath& Path) -> std::expected<void, FFileSystemError>
	{
		auto Absolute = Normalize(Path);
		if (!Absolute) return std::unexpected(std::move(Absolute.error()));
		std::error_code Error;
		std::filesystem::create_directories(Absolute->parent_path(), Error);
		if (Error) return std::unexpected(FFileSystemError{std::format("Failed to create parent directories: {}.", FFilePath(*Absolute).generic_string()), Error});
		auto File = IPlatformFile::GetPlatformPhysical().OpenWrite(*Absolute, IPlatformFile::EOpenWriteFlags::None);
		if (!File) return std::unexpected(std::move(File.error()));
		if (Bytes.size_bytes() > static_cast<uint64>(std::numeric_limits<int64>::max())
			|| !(*File)->Write(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int64>(Bytes.size_bytes())))
			return std::unexpected(FFileSystemError{std::format("Cannot write file: {}.", Absolute->generic_string()), std::make_error_code(std::errc::io_error)});
		return {};
	}

	auto SaveArrayToNewFile(
		FByteView Array,
		const std::filesystem::path& FilePath
	) -> std::expected<void, FFileSystemError>
	{
		std::error_code ErrorCode;
		const auto Destination = std::filesystem::absolute(FilePath, ErrorCode).lexically_normal();
		if (ErrorCode)
		{
			return std::unexpected(FFileSystemError{std::format("Failed to normalize path: {}.", FFilePath(FilePath).generic_string()), ErrorCode});
		}
		std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
		if (ErrorCode)
		{
			return std::unexpected(FFileSystemError{std::format("Failed to create parent directories: {}.", FFilePath(Destination).generic_string()), ErrorCode});
		}
		bool bCreated = false;
		auto Written = WriteNewFile(Destination, Array, bCreated);
		if (Written) return {};
		auto Error = std::move(Written.error());
		// A failed exclusive create never grants ownership of the existing path.
		if (bCreated)
			std::filesystem::remove(Destination, ErrorCode);
		return std::unexpected(std::move(Error));
	}

	auto SaveArrayToFileAtomically(
		FByteView Array,
		const std::filesystem::path& FilePath
	) -> std::expected<void, FFileSystemError>
	{
		std::error_code ErrorCode;
		const std::filesystem::path Destination = std::filesystem::absolute(FilePath, ErrorCode).lexically_normal();
		if (ErrorCode)
		{
			return std::unexpected(FFileSystemError{std::format("Failed to normalize path: {}.", FFilePath(FilePath).generic_string()), ErrorCode});
		}

		std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
		if (ErrorCode)
		{
			return std::unexpected(FFileSystemError{std::format("Failed to create parent directories: {}.", FFilePath(Destination).generic_string()), ErrorCode});
		}

		FFileSystemError Error{};
		std::filesystem::path TemporaryPath;
		bool bCreated = false;
		bool bWritten = false;
		for (uint32 Attempt = 0; Attempt < 64 && !bWritten; ++Attempt)
		{
			TemporaryPath = MakeTemporaryPath(Destination);
			auto Written = WriteNewFile(TemporaryPath, Array, bCreated);
			bWritten = Written.has_value();
			if (!Written) Error = std::move(Written.error());
			if (!bWritten && !bCreated)
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
			if (!bWritten) break;
		}
		if (!bWritten)
		{
			if (bCreated)
			{
				std::filesystem::remove(TemporaryPath, ErrorCode);
			}
			return std::unexpected(std::move(Error));
		}

#if defined(_WIN32)
		ErrorCode.clear();
		if (std::filesystem::is_directory(Destination, ErrorCode))
		{
			Error = FFileSystemError{std::format("Failed to replace destination: {}.", FFilePath(Destination).generic_string()), {ERROR_ACCESS_DENIED, std::system_category()}};
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
			Error = FFileSystemError{std::format("Failed to replace destination: {}.", FFilePath(Destination).generic_string()), {static_cast<int>(ReplacementError), std::system_category()}};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
#else
		std::filesystem::rename(TemporaryPath, Destination, ErrorCode);
		if (ErrorCode)
		{
			Error = FFileSystemError{std::format("Failed to replace destination: {}.", FFilePath(Destination).generic_string()), ErrorCode};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
#endif
		return {};
	}

	auto CopyFileAtomically(
		const std::filesystem::path& SourcePath,
		const std::filesystem::path& DestinationPath
	) -> std::expected<void, FFileSystemError>
	{
		std::error_code ErrorCode;
		const std::filesystem::path Source =
			std::filesystem::absolute(SourcePath, ErrorCode).lexically_normal();
		if (ErrorCode)
		{
			return std::unexpected(FFileSystemError{std::format("Failed to normalize path: {}. Related path: {}.", FFilePath(SourcePath).generic_string(), FFilePath(SourcePath).generic_string()), ErrorCode});
		}
		const std::filesystem::path Destination =
			std::filesystem::absolute(DestinationPath, ErrorCode).lexically_normal();
		if (ErrorCode)
		{
			return std::unexpected(FFileSystemError{std::format("Failed to normalize path: {}. Related path: {}.", FFilePath(DestinationPath).generic_string(), FFilePath(SourcePath).generic_string()), ErrorCode});
		}
		std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
		if (ErrorCode)
		{
			return std::unexpected(FFileSystemError{std::format("Failed to create parent directories: {}. Related path: {}.", FFilePath(Destination).generic_string(), FFilePath(SourcePath).generic_string()), ErrorCode});
		}

		FFileSystemError Error{};
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
				Error = FFileSystemError{std::format("Failed to {}: {}. Related path: {}.", ErrorCode == std::errc::file_exists
						? "create temporary file"
						: "write temporary file", FFilePath(TemporaryPath).generic_string(), FFilePath(SourcePath).generic_string()), ErrorCode};
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
			Error = FFileSystemError{std::format("Failed to flush temporary file: {}. Related path: {}.", FFilePath(TemporaryPath).generic_string(), FFilePath(SourcePath).generic_string()), {static_cast<int>(NativeError), std::system_category()}};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
		if (!CloseHandle(TemporaryFile))
		{
			Error = FFileSystemError{std::format("Failed to close temporary file: {}. Related path: {}.", FFilePath(TemporaryPath).generic_string(), FFilePath(SourcePath).generic_string()), {static_cast<int>(GetLastError()), std::system_category()}};
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
			Error = FFileSystemError{std::format("Failed to replace destination: {}. Related path: {}.", FFilePath(Destination).generic_string(), FFilePath(SourcePath).generic_string()), {static_cast<int>(ReplacementError), std::system_category()}};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
#else
		const int TemporaryFile = open(TemporaryPath.c_str(), O_RDWR);
		if (TemporaryFile == -1 || fsync(TemporaryFile) != 0)
		{
			const int NativeError = errno;
			if (TemporaryFile != -1) close(TemporaryFile);
			Error = FFileSystemError{std::format("Failed to flush temporary file: {}. Related path: {}.", FFilePath(TemporaryPath).generic_string(), FFilePath(SourcePath).generic_string()), {NativeError, std::system_category()}};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
		if (close(TemporaryFile) != 0)
		{
			Error = FFileSystemError{std::format("Failed to close temporary file: {}. Related path: {}.", FFilePath(TemporaryPath).generic_string(), FFilePath(SourcePath).generic_string()), {errno, std::system_category()}};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
		std::filesystem::rename(TemporaryPath, Destination, ErrorCode);
		if (ErrorCode)
		{
			Error = FFileSystemError{std::format("Failed to replace destination: {}. Related path: {}.", FFilePath(Destination).generic_string(), FFilePath(SourcePath).generic_string()), ErrorCode};
			std::filesystem::remove(TemporaryPath, ErrorCode);
			return std::unexpected(std::move(Error));
		}
#endif
		return {};
	}

}
