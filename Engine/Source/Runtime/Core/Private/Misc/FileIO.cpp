#include "Misc/FileIO.h"
#include "Misc/FileHelper.h"
#include <cerrno>
#include <cstdio>
#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#endif

namespace Durin::FFileIO
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

		auto Convert(const FFileHelper::FFileIoError& Error) -> FFileError
		{
			EFileOperation Operation = EFileOperation::OpenRead;
			if (Error.Operation == FFileHelper::EFileIoOperation::QuerySize) Operation = EFileOperation::QuerySize;
			if (Error.Operation == FFileHelper::EFileIoOperation::Read) Operation = EFileOperation::Read;
			FFileError Result{Operation, Error.NativeError, Error.Path};
			if (Operation == EFileOperation::Read) Result.Range = {Error.Offset, Error.Size};
			return Result;
		}

		auto Convert(const FFileHelper::FAtomicFileError& Error) -> FFileError
		{
			using enum FFileHelper::EAtomicFileOperation;
			EFileOperation Operation = EFileOperation::NormalizePath;
			switch (Error.Operation)
			{
			case None: case NormalizeDestination: break;
			case CreateParentDirectories: Operation = EFileOperation::CreateParentDirectories; break;
			case CreateTemporaryFile: Operation = EFileOperation::CreateTemporaryFile; break;
			case WriteTemporaryFile: Operation = EFileOperation::WriteTemporaryFile; break;
			case FlushTemporaryFile: Operation = EFileOperation::FlushTemporaryFile; break;
			case CloseTemporaryFile: Operation = EFileOperation::CloseTemporaryFile; break;
			case ReplaceDestination: Operation = EFileOperation::ReplaceDestination; break;
			}
			return {Operation, Error.NativeError, Error.Path};
		}

		class FReadHandle final : public IFileHandle
		{
		public:
			explicit FReadHandle(std::unique_ptr<FFileHelper::IFileHandle> InHandle)
				: Handle(std::move(InHandle)) {}
			auto GetSize() const -> uint64 override { return Handle->GetSize(); }
			auto ReadAt(uint64 Offset, FMutableByteView Output) -> std::expected<void, FFileError> override
			{
				FFileHelper::FFileIoError Error;
				if (!Handle->ReadAt(Offset, Output, &Error)) return std::unexpected(Convert(Error));
				return {};
			}
		private:
			std::unique_ptr<FFileHelper::IFileHandle> Handle;
		};

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

	auto OpenRead(const FFilePath& Path) -> std::expected<std::unique_ptr<IFileHandle>, FFileError>
	{
		auto Absolute = Normalize(Path);
		if (!Absolute) return std::unexpected(std::move(Absolute.error()));
		FFileHelper::FFileIoError Error;
		auto Handle = FFileHelper::OpenRead(*Absolute, &Error);
		if (!Handle) return std::unexpected(Convert(Error));
		return std::make_unique<FReadHandle>(std::move(Handle));
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

	auto SaveArrayToNewFile(FByteView Bytes, const FFilePath& Path) -> std::expected<void, FFileError>
	{
		FFileHelper::FAtomicFileError Error;
		if (!FFileHelper::SaveArrayToNewFile(Bytes, Path, &Error)) return std::unexpected(Convert(Error));
		return {};
	}

	auto SaveArrayToFileAtomically(FByteView Bytes, const FFilePath& Path) -> std::expected<void, FFileError>
	{
		FFileHelper::FAtomicFileError Error;
		if (!FFileHelper::SaveArrayToFileAtomically(Bytes, Path, &Error)) return std::unexpected(Convert(Error));
		return {};
	}

	auto CopyFileAtomically(const FFilePath& Source, const FFilePath& Destination) -> std::expected<void, FFileError>
	{
		FFileHelper::FAtomicFileError Error;
		if (!FFileHelper::CopyFileAtomically(Source, Destination, &Error))
		{
			auto Result = Convert(Error);
			Result.RelatedPath = Source;
			return std::unexpected(std::move(Result));
		}
		return {};
	}
}
