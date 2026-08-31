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
		: public Asset::IAssetReferenceStore
	{
	public:
		using FPathChanged = std::function<void(const FPackagePath&)>;
		using FProjectResolver = std::function<const FProjectInfo*()>;

		explicit FProjectDefaultLevelReferenceStore(
			FPathChanged InPathChanged = {},
			FProjectResolver InProjectResolver = {});

		auto CaptureSnapshot(
			Asset::FAssetReferenceStoreSnapshot& OutSnapshot)
			-> Asset::FAssetResult override;
		auto PrepareRewrite(
			std::span<const Asset::FAssetReferenceRewrite> Rewrites,
			std::string_view ExpectedFingerprint,
			Asset::FAssetReferenceStoreRewriteContribution& OutContribution)
			-> Asset::FAssetResult override;

	private:
		FPathChanged PathChanged;
		FProjectResolver ProjectResolver;
	};
}
