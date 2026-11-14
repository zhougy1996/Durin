#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"

namespace Durin::AssetWriteErrors
{
	inline constexpr UnifiedError::FErrorId InvalidPath{"AssetWrite", "InvalidPath"};
	inline constexpr UnifiedError::FErrorId AlreadyExists{"AssetWrite", "AlreadyExists"};
	inline constexpr UnifiedError::FErrorId NotFound{"AssetWrite", "NotFound"};
	inline constexpr UnifiedError::FErrorId IoError{"AssetWrite", "IoError"};
	inline constexpr UnifiedError::FErrorId InvalidData{"AssetWrite", "InvalidData"};
	inline constexpr UnifiedError::FErrorId InUse{"AssetWrite", "InUse"};
	inline constexpr UnifiedError::FErrorId StaleData{"AssetWrite", "StaleData"};
	inline constexpr UnifiedError::FErrorId ReadOnlyMode{"AssetWrite", "ReadOnlyMode"};
	inline constexpr UnifiedError::FErrorId Cancelled{"AssetWrite", "Cancelled"};
}

namespace Durin
{
	enum class EAssetWriteEffect : uint8
	{
		None, ContentCommittedProjectionPending, PartiallyWritten, ContentUncertain
	};
	// One owning failure plus observed write effects. Success is absence of Error.
	// Effects never determine retry policy and may remain present after failure.
	struct FAssetWriteResult
	{
		std::optional<UnifiedError::FError> Error;
		EAssetWriteEffect Effect = EAssetWriteEffect::None;
		auto Succeeded() const -> bool { return !Error; }
		explicit operator bool() const { return Succeeded(); }
	};
}
