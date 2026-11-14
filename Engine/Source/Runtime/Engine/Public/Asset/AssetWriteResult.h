#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "EngineAPI.h"

namespace Durin
{
	namespace ObjectPackage { struct FPackageWriterResult; }

	enum class EAssetWriteError : uint8
	{
		None, InvalidPath, AlreadyExists, NotFound, IoError, InvalidData,
		UnsupportedVersion, InUse, StaleData, ReadOnlyMode, ShuttingDown, Cancelled,
		ProjectionPending
	};
	enum class EAssetWriteEffect : uint8
	{
		None, ContentCommittedProjectionPending, PartiallyWritten, ContentUncertain
	};
	// Describes observed write effects, never whether an owning job can retry.
	struct FAssetWriteResult
	{
		EAssetWriteError Error = EAssetWriteError::None;
		std::string Message;
		EAssetWriteEffect Effect = EAssetWriteEffect::None;
		std::optional<UnifiedError::FError> Cause;
		auto Succeeded() const -> bool { return Error == EAssetWriteError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	// Explicit boundary: a read failure rejects preparation without write effects.
	ENGINE_API auto AssetWriteResultFromRead(const UnifiedError::FError& Error) -> FAssetWriteResult;
	template<typename T>
	[[nodiscard]] auto AssetWriteResultFromRead(const std::expected<T, Durin::UnifiedError::FError>& Result) -> FAssetWriteResult
	{
		return Result ? FAssetWriteResult{} : AssetWriteResultFromRead(Result.error());
	}
	ENGINE_API auto AssetWriteResultFromEncoding(const ObjectPackage::FPackageWriterResult& Result) -> FAssetWriteResult;
}
