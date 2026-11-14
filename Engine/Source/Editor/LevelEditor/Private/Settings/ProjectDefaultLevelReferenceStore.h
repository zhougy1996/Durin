#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "Asset/MutationExtensions.h"

namespace Durin
{
	struct FProjectInfo;
}

namespace Durin::LevelEditor
{

	// Owns the project YAML default-level occurrence for redirector Fix Up.
	class FProjectDefaultLevelReferenceStore final
		: public IAssetReferenceStore
	{
	public:
		using FPathChanged = std::function<void(const FPackagePath&)>;
		using FProjectResolver = std::function<const FProjectInfo*()>;

		explicit FProjectDefaultLevelReferenceStore(
			FPathChanged InPathChanged = {},
			FProjectResolver InProjectResolver = {});

		auto CaptureSnapshot(
			FAssetReferenceStoreSnapshot& OutSnapshot)
			-> std::expected<void, Durin::UnifiedError::FError> override;
		auto PrepareRewrite(
			std::span<const FAssetReferenceRewrite> Rewrites,
			std::string_view ExpectedFingerprint,
			FAssetReferenceStoreRewriteContribution& OutContribution)
			-> FAssetWriteResult override;

	private:
		FPathChanged PathChanged;
		FProjectResolver ProjectResolver;
	};
}
