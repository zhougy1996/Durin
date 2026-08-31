#include "Misc/FileHelper.h"

#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Durin
{
	namespace FFileHelper
	{
		namespace
		{
			auto FileIoOperationName(EFileIoOperation Operation) -> std::string_view
			{
				switch (Operation)
				{
				case EFileIoOperation::None: return "none";
				case EFileIoOperation::OpenRead: return "open for reading";
				case EFileIoOperation::QuerySize: return "query size";
				case EFileIoOperation::Read: return "read";
				}
				return "unknown";
			}

			auto SetFileIoError(FFileIoError* OutError, EFileIoOperation Operation,
				std::error_code NativeError, const std::filesystem::path& Path,
				uint64 Offset = 0, uint64 Size = 0) -> void
			{
				if (!OutError) return;
				*OutError = {.Operation = Operation, .NativeError = NativeError,
					.Path = Path, .Offset = Offset, .Size = Size};
			}

#if defined(_WIN32)
			class FNativeFileHandle final : public IFileHandle
			{
			public:
				FNativeFileHandle(HANDLE InHandle, uint64 InSize, std::filesystem::path InPath)
					: Handle(InHandle), Size(InSize), Path(std::move(InPath)) {}
				~FNativeFileHandle() override { if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle); }
				auto GetSize() const -> uint64 override { return Size; }
				auto ReadAt(uint64 Offset, std::span<std::byte> Output,
					FFileIoError* OutError) -> bool override
				{
					if (OutError) *OutError = {};
					if (Offset > Size || Output.size_bytes() > Size - Offset)
					{
						SetFileIoError(OutError, EFileIoOperation::Read,
							std::make_error_code(std::errc::result_out_of_range), Path,
							Offset, Output.size_bytes());
						return false;
					}
					if (Output.empty()) return true;
					LARGE_INTEGER Position; Position.QuadPart = static_cast<LONGLONG>(Offset);
					if (!SetFilePointerEx(Handle, Position, nullptr, FILE_BEGIN))
					{
						SetFileIoError(OutError, EFileIoOperation::Read,
							{static_cast<int>(GetLastError()), std::system_category()}, Path,
							Offset, Output.size_bytes());
						return false;
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
							SetFileIoError(OutError, EFileIoOperation::Read,
								{static_cast<int>(Native == ERROR_SUCCESS ? ERROR_HANDLE_EOF : Native),
									std::system_category()}, Path, Offset + Complete,
								Output.size_bytes() - Complete);
							return false;
						}
						Complete += Read;
					}
					return true;
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
				auto ReadAt(uint64 Offset, std::span<std::byte> Output,
					FFileIoError* OutError) -> bool override
				{
					if (OutError) *OutError = {};
					if (Offset > Size || Output.size_bytes() > Size - Offset
						|| Offset > static_cast<uint64>(std::numeric_limits<off_t>::max()))
					{
						SetFileIoError(OutError, EFileIoOperation::Read,
							std::make_error_code(std::errc::result_out_of_range), Path,
							Offset, Output.size_bytes());
						return false;
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
							SetFileIoError(OutError, EFileIoOperation::Read,
								{Read == 0 ? EIO : errno, std::system_category()}, Path,
								Offset + Complete, Output.size_bytes() - Complete);
							return false;
						}
						Complete += static_cast<size_t>(Read);
					}
					return true;
				}
			private:
				int File = -1;
				uint64 Size = 0;
				std::filesystem::path Path;
			};
#endif

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

		auto FFileIoError::ToString() const -> std::string
		{
			const uint64 End = Size > std::numeric_limits<uint64>::max() - Offset
				? std::numeric_limits<uint64>::max() : Offset + Size;
			return std::format("File I/O failed while attempting to {} at [{}, {}) "
				"(native error {}: {}) for {}.", FileIoOperationName(Operation), Offset, End,
				NativeError.value(), NativeError.message(), Path.generic_string());
		}

		auto OpenRead(const std::filesystem::path& FilePath,
			FFileIoError* OutError) -> std::unique_ptr<IFileHandle>
		{
			if (OutError) *OutError = {};
			const std::filesystem::path Path = std::filesystem::absolute(FilePath).lexically_normal();
#if defined(_WIN32)
			const HANDLE File = CreateFileW(Path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (File == INVALID_HANDLE_VALUE)
			{
				SetFileIoError(OutError, EFileIoOperation::OpenRead,
					{static_cast<int>(GetLastError()), std::system_category()}, Path);
				return nullptr;
			}
			LARGE_INTEGER Size;
			if (!GetFileSizeEx(File, &Size) || Size.QuadPart < 0)
			{
				const DWORD Error = GetLastError(); CloseHandle(File);
				SetFileIoError(OutError, EFileIoOperation::QuerySize,
					{static_cast<int>(Error), std::system_category()}, Path);
				return nullptr;
			}
			return std::make_unique<FNativeFileHandle>(File, static_cast<uint64>(Size.QuadPart), Path);
#else
			const int File = open(Path.c_str(), O_RDONLY);
			if (File == -1)
			{
				SetFileIoError(OutError, EFileIoOperation::OpenRead,
					{errno, std::system_category()}, Path);
				return nullptr;
			}
			struct stat Status{};
			if (fstat(File, &Status) != 0 || Status.st_size < 0)
			{
				const int Error = errno; close(File);
				SetFileIoError(OutError, EFileIoOperation::QuerySize,
					{Error, std::system_category()}, Path);
				return nullptr;
			}
			return std::make_unique<FNativeFileHandle>(File, static_cast<uint64>(Status.st_size), Path);
#endif
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
			for (uint32 Attempt = 0; Attempt < 64; ++Attempt)
			{
				auto File = OpenRead(FilePath);
				if (File)
				{
					const uint64 FileSize = File->GetSize();
					if (FileSize <= static_cast<uint64>(std::numeric_limits<size_t>::max()))
					{
						const uint64 ElementCount = FileSize / sizeof(ElementType)
							+ (FileSize % sizeof(ElementType) != 0);
						std::vector<ElementType> Loaded;
						if (ElementCount <= Loaded.max_size())
						{
							Loaded.resize(static_cast<size_t>(ElementCount));
							if (File->ReadAt(0, std::as_writable_bytes(std::span(Loaded)).first(
								static_cast<size_t>(FileSize))))
							{
								Result = std::move(Loaded);
								return true;
							}
						}
					}
				}

				std::error_code ExistsError;
				if (!std::filesystem::exists(FilePath, ExistsError) || ExistsError)
				{
					DURIN_WARN("Failed to load file. File {} does not exist.", FilePath.generic_string());
					break;
				}
				if (Attempt < 8) std::this_thread::yield();
				else std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return false;
		};

		bool LoadFileToArray(FByteArray& Result, const std::filesystem::path& FilePath)
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
			FFileIoError FileError;
			auto File = OpenRead(FilePath, &FileError);
			if (!File)
			{
				OutError = FileError.NativeError ? FileError.NativeError
					: std::make_error_code(std::errc::io_error);
				return false;
			}
			constexpr size_t BufferSize = 64 * 1024;
			std::array<std::byte, BufferSize> Buffer{};
			FXxHash128Builder Builder;
			for (uint64 Offset = 0; Offset < File->GetSize();)
			{
				const size_t Count = static_cast<size_t>(std::min<uint64>(Buffer.size(), File->GetSize() - Offset));
				if (!File->ReadAt(Offset, std::span(Buffer).first(Count), &FileError))
				{
					OutError = FileError.NativeError ? FileError.NativeError
						: std::make_error_code(std::errc::io_error);
					return false;
				}
				Builder.Update(Buffer.data(), Count);
				Offset += Count;
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

		auto CopyFileAtomically(
			const std::filesystem::path& SourcePath,
			const std::filesystem::path& DestinationPath,
			FAtomicFileError* OutError
		) -> bool
		{
			FAtomicFileError LocalError;
			FAtomicFileError* Error = OutError ? OutError : &LocalError;
			*Error = {};

			std::error_code ErrorCode;
			const std::filesystem::path Source =
				std::filesystem::absolute(SourcePath, ErrorCode).lexically_normal();
			if (ErrorCode)
			{
				SetAtomicFileError(Error, EAtomicFileOperation::NormalizeDestination,
					ErrorCode, SourcePath);
				return false;
			}
			const std::filesystem::path Destination =
				std::filesystem::absolute(DestinationPath, ErrorCode).lexically_normal();
			if (ErrorCode)
			{
				SetAtomicFileError(Error, EAtomicFileOperation::NormalizeDestination,
					ErrorCode, DestinationPath);
				return false;
			}
			std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
			if (ErrorCode)
			{
				SetAtomicFileError(Error, EAtomicFileOperation::CreateParentDirectories,
					ErrorCode, Destination);
				return false;
			}

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
					SetAtomicFileError(Error,
						ErrorCode == std::errc::file_exists
							? EAtomicFileOperation::CreateTemporaryFile
							: EAtomicFileOperation::WriteTemporaryFile,
						ErrorCode, TemporaryPath);
					if (ErrorCode == std::errc::file_exists && Attempt + 1 < 64)
					{
						*Error = {};
						if (Attempt < 8) std::this_thread::yield();
						else std::this_thread::sleep_for(std::chrono::milliseconds(1));
						continue;
					}
					return false;
				}
			}
			if (!bCreated) return false;

#if defined(_WIN32)
			const HANDLE TemporaryFile = CreateFileW(TemporaryPath.c_str(), GENERIC_WRITE,
				0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (TemporaryFile == INVALID_HANDLE_VALUE || !FlushFileBuffers(TemporaryFile))
			{
				const DWORD NativeError = GetLastError();
				if (TemporaryFile != INVALID_HANDLE_VALUE) CloseHandle(TemporaryFile);
				SetAtomicFileError(Error, EAtomicFileOperation::FlushTemporaryFile,
					{static_cast<int>(NativeError), std::system_category()}, TemporaryPath);
				std::filesystem::remove(TemporaryPath, ErrorCode);
				return false;
			}
			if (!CloseHandle(TemporaryFile))
			{
				SetAtomicFileError(Error, EAtomicFileOperation::CloseTemporaryFile,
					{static_cast<int>(GetLastError()), std::system_category()}, TemporaryPath);
				std::filesystem::remove(TemporaryPath, ErrorCode);
				return false;
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
				SetAtomicFileError(Error, EAtomicFileOperation::ReplaceDestination,
					{static_cast<int>(ReplacementError), std::system_category()}, Destination);
				std::filesystem::remove(TemporaryPath, ErrorCode);
				return false;
			}
#else
			const int TemporaryFile = open(TemporaryPath.c_str(), O_RDWR);
			if (TemporaryFile == -1 || fsync(TemporaryFile) != 0)
			{
				const int NativeError = errno;
				if (TemporaryFile != -1) close(TemporaryFile);
				SetAtomicFileError(Error, EAtomicFileOperation::FlushTemporaryFile,
					{NativeError, std::system_category()}, TemporaryPath);
				std::filesystem::remove(TemporaryPath, ErrorCode);
				return false;
			}
			if (close(TemporaryFile) != 0)
			{
				SetAtomicFileError(Error, EAtomicFileOperation::CloseTemporaryFile,
					{errno, std::system_category()}, TemporaryPath);
				std::filesystem::remove(TemporaryPath, ErrorCode);
				return false;
			}
			std::filesystem::rename(TemporaryPath, Destination, ErrorCode);
			if (ErrorCode)
			{
				SetAtomicFileError(Error, EAtomicFileOperation::ReplaceDestination,
					ErrorCode, Destination);
				std::filesystem::remove(TemporaryPath, ErrorCode);
				return false;
			}
#endif
			return true;
		}

	} // namespace FFileHelper
} // namespace Durin
