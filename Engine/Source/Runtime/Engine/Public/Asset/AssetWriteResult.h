#pragma once

#include "EngineAPI.h"

namespace Durin
{
	struct FAssetReadResult;
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
		auto Succeeded() const -> bool { return Error == EAssetWriteError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	// Explicit boundary: a read failure rejects preparation without write effects.
	ENGINE_API auto AssetWriteResultFromRead(const FAssetReadResult& Result) -> FAssetWriteResult;
	ENGINE_API auto AssetWriteResultFromEncoding(const ObjectPackage::FPackageWriterResult& Result) -> FAssetWriteResult;
}
