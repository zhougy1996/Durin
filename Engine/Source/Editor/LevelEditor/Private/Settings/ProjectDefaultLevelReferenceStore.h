#pragma once

#include "Asset/MutationExtensions.h"

namespace Durin
{
	struct FProjectInfo;
}

namespace Durin::Editor::Level
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
			-> FAssetResult override;
		auto PrepareRewrite(
			std::span<const FAssetReferenceRewrite> Rewrites,
			std::string_view ExpectedFingerprint,
			FAssetReferenceStoreRewriteContribution& OutContribution)
			-> FAssetResult override;

	private:
		FPathChanged PathChanged;
		FProjectResolver ProjectResolver;
	};
}
