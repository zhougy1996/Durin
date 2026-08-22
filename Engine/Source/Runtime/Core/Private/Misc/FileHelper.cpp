#include "Misc/FileHelper.h"

#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Durin
{
	namespace FFileHelper
	{
		namespace
		{
			auto AtomicFileOperationName(EAtomicFileOperation Operation) -> std::string_view
			{
				switch (Operation)
				{
				case EAtomicFileOperation::None: return "none";
				case EAtomicFileOperation::NormalizeDestination: return "normalize destination";
				case EAtomicFileOperation::CreateParentDirectories: return "create parent directories";
				case EAtomicFileOperation::CreateTemporaryFile: return "create temporary file";
				case EAtomicFileOperation::WriteTemporaryFile: return "write temporary file";
				case EAtomicFileOperation::FlushTemporaryFile: return "flush temporary file";
				case EAtomicFileOperation::CloseTemporaryFile: return "close temporary file";
				case EAtomicFileOperation::ReplaceDestination: return "replace destination";
				}
				return "unknown";
			}

			auto SetAtomicFileError(
				FAtomicFileError* OutError,
				EAtomicFileOperation Operation,
				std::error_code NativeError,
				const std::filesystem::path& Path
			) -> void
			{
				if (!OutError) return;

				OutError->Operation = Operation;
				OutError->NativeError = NativeError;
				OutError->Path = Path;
				OutError->PathLength = Path.native().size();
				OutError->LongestComponentLength = 0;
				for (const std::filesystem::path& Component : Path)
				{
					OutError->LongestComponentLength = std::max(OutError->LongestComponentLength, Component.native().size());
				}
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
					/ std::format(".durin-tmp-{:08x}-{:016x}", ProcessId, UniquenessToken.fetch_add(1, std::memory_order_relaxed));
			}

#if defined(_WIN32)
			auto WriteTemporaryFile(
				const std::filesystem::path& TemporaryPath,
				std::span<const std::byte> Array,
				FAtomicFileError* OutError
			) -> bool
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
					SetAtomicFileError(
						OutError,
						EAtomicFileOperation::CreateTemporaryFile,
						{static_cast<int>(GetLastError()), std::system_category()},
						TemporaryPath
					);
					return false;
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
						SetAtomicFileError(
							OutError,
							EAtomicFileOperation::WriteTemporaryFile,
							{static_cast<int>(Error), std::system_category()},
							TemporaryPath
						);
						return false;
					}
					Offset += BytesWritten;
				}

				if (!FlushFileBuffers(File))
				{
					const DWORD Error = GetLastError();
					CloseHandle(File);
					SetAtomicFileError(
						OutError,
						EAtomicFileOperation::FlushTemporaryFile,
						{static_cast<int>(Error), std::system_category()},
						TemporaryPath
					);
					return false;
				}
				if (!CloseHandle(File))
				{
					SetAtomicFileError(
						OutError,
						EAtomicFileOperation::CloseTemporaryFile,
						{static_cast<int>(GetLastError()), std::system_category()},
						TemporaryPath
					);
					return false;
				}
				return true;
			}
#else
			auto WriteTemporaryFile(
				const std::filesystem::path& TemporaryPath,
				std::span<const std::byte> Array,
				FAtomicFileError* OutError
			) -> bool
			{
				const int File = open(TemporaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
				if (File == -1)
				{
					SetAtomicFileError(
						OutError,
						EAtomicFileOperation::CreateTemporaryFile,
						{errno, std::system_category()},
						TemporaryPath
					);
					return false;
				}

				size_t Offset = 0;
				while (Offset < Array.size_bytes())
				{
					const ssize_t BytesWritten = write(File, Array.data() + Offset, Array.size_bytes() - Offset);
					if (BytesWritten <= 0)
					{
						const int Error = errno;
						close(File);
						SetAtomicFileError(
							OutError,
							EAtomicFileOperation::WriteTemporaryFile,
							{Error, std::system_category()},
							TemporaryPath
						);
						return false;
					}
					Offset += static_cast<size_t>(BytesWritten);
				}

				if (fsync(File) != 0)
				{
					const int Error = errno;
					close(File);
					SetAtomicFileError(
						OutError,
						EAtomicFileOperation::FlushTemporaryFile,
						{Error, std::system_category()},
						TemporaryPath
					);
					return false;
				}
				if (close(File) != 0)
				{
					SetAtomicFileError(
						OutError,
						EAtomicFileOperation::CloseTemporaryFile,
						{errno, std::system_category()},
						TemporaryPath
					);
					return false;
				}
				return true;
			}
#endif
		}

		auto FAtomicFileError::ToString() const -> std::string
		{
			return std::format(
				"Atomic file publication failed while attempting to {} (native error {}: {}) for {} "
				"[path length: {}, longest component: {}].",
				AtomicFileOperationName(Operation),
				NativeError.value(),
				NativeError.message(),
				Path.generic_string(),
				PathLength,
				LongestComponentLength
			);
		}

		bool FileExists(std::string_view FileName)
		{
			return std::filesystem::exists(FileName);
		}

		template<typename ElementType>
		static auto LoadFileToArrayInternal(
			std::vector<ElementType>& Result,
			const std::filesystem::path& FilePath) -> bool
		{
			if (!std::filesystem::exists(FilePath))
			{
				DURIN_WARN("Failed to load file. File {} does not exist.", FilePath.generic_string());
				return false;
			}

			std::ifstream File(FilePath, std::ios::binary);
			if (!File.is_open())
			{
				return false;
			}

			std::error_code ErrorCode;
			const uintmax_t FileSize = std::filesystem::file_size(FilePath, ErrorCode);
			if (ErrorCode || FileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
			{
				return false;
			}

			const uintmax_t ElementCount = FileSize / sizeof(ElementType) + (FileSize % sizeof(ElementType) != 0);
			std::vector<ElementType> Loaded;
			if (ElementCount > Loaded.max_size())
			{
				return false;
			}
			Loaded.resize(static_cast<size_t>(ElementCount));

			if (FileSize > 0)
			{
				const std::streamsize ReadSize = static_cast<std::streamsize>(FileSize);
				File.read(reinterpret_cast<char*>(Loaded.data()), ReadSize);
				if (!File || File.gcount() != ReadSize)
				{
					return false;
				}
			}

			Result = std::move(Loaded);
			return true;
		};

		bool LoadFileToArray(std::vector<std::byte>& Result, const std::filesystem::path& FilePath)
		{
			return LoadFileToArrayInternal(Result, FilePath);
		}

		bool LoadFileToArray(std::vector<uint32>& Result, const std::filesystem::path& FilePath)
		{
			return LoadFileToArrayInternal(Result, FilePath);
		}

		bool LoadFileToString(std::string& Result, std::string_view FileName)
		{
			const std::filesystem::path FilePath(FileName);
			if (!std::filesystem::exists(FilePath))
			{
				DURIN_WARN("Failed to load file. File {} does not exist.", FileName);
				return false;
			}

			std::ifstream File(FilePath, std::ios::binary);
			if (!File.is_open())
			{
				return false;
			}

			std::error_code ErrorCode;
			const uintmax_t FileSize = std::filesystem::file_size(FilePath, ErrorCode);
			if (ErrorCode
				|| FileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())
				|| FileSize > std::string{}.max_size())
			{
				return false;
			}

			std::string Loaded(static_cast<size_t>(FileSize), '\0');
			if (FileSize > 0)
			{
				const std::streamsize ReadSize = static_cast<std::streamsize>(FileSize);
				File.read(Loaded.data(), ReadSize);
				if (!File || File.gcount() != ReadSize)
				{
					return false;
				}
			}

			Result = std::move(Loaded);
			return true;
		}

		auto HashFileXx128(
			const std::filesystem::path& FilePath,
			FXxHash128& OutHash,
			std::error_code& OutError) -> bool
		{
			OutHash = {};
			OutError.clear();
			std::ifstream Stream(FilePath, std::ios::binary);
			if (!Stream.is_open())
			{
				OutError = std::make_error_code(std::errc::io_error);
				return false;
			}
			constexpr size_t BufferSize = 64 * 1024;
			std::array<char, BufferSize> Buffer{};
			FXxHash128Builder Builder;
			while (Stream)
			{
				Stream.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
				const std::streamsize Read = Stream.gcount();
				if (Read > 0)
					Builder.Update(Buffer.data(), static_cast<uint64>(Read));
			}
			if (Stream.bad())
			{
				OutError = std::make_error_code(std::errc::io_error);
				return false;
			}
			OutHash = Builder.Finalize();
			return true;
		}

		bool SaveArrayToFile(const std::span<const std::byte>& Array, const std::filesystem::path& FilePath)
		{
			// Ensure the parent directory exists
			if (FilePath.has_parent_path())
			{
				std::error_code ErrorCode;
				std::filesystem::create_directories(FilePath.parent_path(), ErrorCode);
				if (ErrorCode)
				{
					DURIN_ERROR("Failed to create directories for path {}: {}", FilePath.parent_path().string(), ErrorCode.message());
					return false;
				}
			}

			// Open the file stream in binary mode
			std::ofstream File(FilePath, std::ios::binary | std::ios::out);

			if (!File.is_open())
			{
				DURIN_ERROR("Failed to open file for writing: {}", FilePath.string());
				return false;
			}

			File.write(reinterpret_cast<const char*>(Array.data()), Array.size_bytes());

			if (File.fail())
			{
				DURIN_ERROR("Failed to write data to file {}", FilePath.string());
				return false;
			}

			File.close();

			if (File.fail())
			{
				DURIN_ERROR("Failed to close file {} after writing", FilePath.string());
				return false;
			}

			return true;
		}

		bool SaveArrayToFile(const std::span<const uint32>& Array, const std::filesystem::path& FilePath)
		{
			return SaveArrayToFile(std::span{reinterpret_cast<const std::byte*>(Array.data()), Array.size() * sizeof(uint32)}, FilePath);
		}

		auto SaveArrayToFileAtomically(
			std::span<const std::byte> Array,
			const std::filesystem::path& FilePath,
			FAtomicFileError* OutError
		) -> bool
		{
			FAtomicFileError LocalError;
			FAtomicFileError* Error = OutError ? OutError : &LocalError;
			*Error = {};

			std::error_code ErrorCode;
			const std::filesystem::path Destination = std::filesystem::absolute(FilePath, ErrorCode).lexically_normal();
			if (ErrorCode)
			{
				SetAtomicFileError(Error, EAtomicFileOperation::NormalizeDestination, ErrorCode, FilePath);
				return false;
			}

			std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
			if (ErrorCode)
			{
				SetAtomicFileError(Error, EAtomicFileOperation::CreateParentDirectories, ErrorCode, Destination);
				return false;
			}

			std::filesystem::path TemporaryPath;
			bool bCreated = false;
			for (uint32 Attempt = 0; Attempt < 64 && !bCreated; ++Attempt)
			{
				TemporaryPath = MakeTemporaryPath(Destination);
				bCreated = WriteTemporaryFile(TemporaryPath, Array, Error);
				if (!bCreated && Error->Operation == EAtomicFileOperation::CreateTemporaryFile)
				{
					bool bRetryable = Error->NativeError == std::errc::file_exists;
#if defined(_WIN32)
					bRetryable |= Error->NativeError.value() == ERROR_ACCESS_DENIED
						|| Error->NativeError.value() == ERROR_SHARING_VIOLATION;
#endif
					if (bRetryable && Attempt + 1 < 64)
					{
						*Error = {};
						if (Attempt < 8) std::this_thread::yield();
						else std::this_thread::sleep_for(std::chrono::milliseconds(1));
						continue;
					}
				}
				if (!bCreated) break;
			}
			if (!bCreated)
			{
				if (Error->Operation != EAtomicFileOperation::CreateTemporaryFile)
				{
					std::filesystem::remove(TemporaryPath, ErrorCode);
				}
				return false;
			}

#if defined(_WIN32)
			ErrorCode.clear();
			if (std::filesystem::is_directory(Destination, ErrorCode))
			{
				SetAtomicFileError(
					Error,
					EAtomicFileOperation::ReplaceDestination,
					{ERROR_ACCESS_DENIED, std::system_category()},
					Destination
				);
				std::filesystem::remove(TemporaryPath, ErrorCode);
				return false;
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
				SetAtomicFileError(
					Error,
					EAtomicFileOperation::ReplaceDestination,
					{static_cast<int>(ReplacementError), std::system_category()},
					Destination
				);
				std::filesystem::remove(TemporaryPath, ErrorCode);
				return false;
			}
#else
			std::filesystem::rename(TemporaryPath, Destination, ErrorCode);
			if (ErrorCode)
			{
				SetAtomicFileError(Error, EAtomicFileOperation::ReplaceDestination, ErrorCode, Destination);
				std::filesystem::remove(TemporaryPath, ErrorCode);
				return false;
			}
#endif
			return true;
		}

	} // namespace FFileHelper
} // namespace Durin
