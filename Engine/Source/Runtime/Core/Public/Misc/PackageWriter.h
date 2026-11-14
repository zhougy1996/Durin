#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"
#include "Containers/ContainersFwd.h"
#include "Misc/FilePublication.h"
#include "Misc/UnifiedError.h"

namespace Durin
{
	// Process-local physical file admission. Tokens may cross threads and never
	// block: a conflicting closure is rejected atomically, without partial ownership.
	class CORE_API FPackageFileAccess
	{
	public:
		~FPackageFileAccess();
		static auto TryAcquire(std::span<const std::filesystem::path> Paths, bool bWrite)
			-> std::shared_ptr<FPackageFileAccess>;
		auto GetPathCount() const -> size_t { return Paths.size(); }
		static auto TryReadPackage(const std::filesystem::path& Main) -> std::shared_ptr<FPackageFileAccess>
		{
			auto Bulk = Main; Bulk.replace_extension(".dbulk");
			const std::array Paths{Main, Bulk};
			return TryAcquire(Paths, false);
		}
	private:
		FPackageFileAccess() = default;
		std::vector<std::filesystem::path> Paths;
		bool bWrite = false;
		bool bAcquired = false;
	};
	namespace PackageWriteErrors
	{
		inline constexpr UnifiedError::FErrorId IoError{"PackageWrite", "IoError"};
		inline constexpr UnifiedError::FErrorId StaleData{"PackageWrite", "StaleData"};
		inline constexpr UnifiedError::FErrorId CorruptFile{"PackageWrite", "CorruptFile"};
		inline constexpr UnifiedError::FErrorId InvalidState{"PackageWrite", "InvalidState"};
	}
	enum class EPackageWriteState : uint8 { NotCommitted, Committed, RecoveryRequired, PartiallyWritten };
	struct FPackageWriteResult
	{
		std::optional<UnifiedError::FError> Error;
		EPackageWriteState State = EPackageWriteState::NotCommitted;
		std::vector<std::filesystem::path> RecoveryFiles;
		std::vector<std::filesystem::path> AffectedFiles;
		auto Succeeded() const -> bool { return !Error; }
		explicit operator bool() const { return Succeeded(); }
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
