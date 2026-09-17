#pragma once
#include "Misc/FilePublication.h"

namespace Durin
{
	enum class EPackageWriteError : uint8 { None, IoError, StaleData, CorruptFile, InvalidState };
	enum class EPackageWriteState : uint8 { NotCommitted, Committed, RecoveryRequired };
	struct FPackageWriteResult
	{
		EPackageWriteError Error = EPackageWriteError::None;
		std::string Message;
		EPackageWriteState State = EPackageWriteState::NotCommitted;
		std::vector<std::filesystem::path> RecoveryFiles;
		explicit operator bool() const { return Error == EPackageWriteError::None; }
	};
	struct FPackageWriteFile
	{
		FFileReplacement Replacement;
		FFilePublicationStamp Expected;
		FByteBuffer Bytes;
	};
	// Exclusive per-save state. The owner must drain Stage before other calls or
	// destruction. Empty Staged denotes removal; bytes are moved, never borrowed.
	class CORE_API IPackageWriteOperation
	{
	public:
		virtual ~IPackageWriteOperation() = default;
		virtual auto Stage() -> FPackageWriteResult = 0;
		virtual auto Commit() -> FPackageWriteResult = 0;
		virtual auto Finalize() -> FPackageWriteResult = 0;
		virtual auto Rollback() -> FPackageWriteResult = 0;
	};
	// Reusable configuration; Begin returns isolated operation state. No scheduler,
	// object, package-format or registry knowledge belongs in this boundary.
	class CORE_API IPackageWriter
	{
	public:
		virtual ~IPackageWriter() = default;
		virtual auto Begin(std::vector<FPackageWriteFile> Files)
			-> std::unique_ptr<IPackageWriteOperation> = 0;
	};
	CORE_API auto GetFilePackageWriter() -> std::shared_ptr<IPackageWriter>;
}
