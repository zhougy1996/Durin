#include "ContentBrowser/ContentBrowserTool.h"

#include "Asset/Result.h"
#include "Editor/AssetRelocation.h"
#include "Panels/ContentBrowserPanel.h"

namespace Durin::Editor::ContentBrowser
{
	auto CreateContentBrowserTool(
		FConstructionServices Services,
		FPresentationSettings Settings,
		FSavePresentationSettings SaveSettings)
		-> std::unique_ptr<IContentBrowserTool>
	{
		auto MoveAssets = [Move = std::move(Services.MoveAssets)](
			std::span<const ::Durin::Editor::ContentBrowser::Private::FEditorAssetMove> Moves) {
			if (!Move)
				return Asset::FAssetResult{
					Asset::EAssetError::ShuttingDown,
					"Asset relocation is unavailable."};
			const FActionResult Result = Move(Moves);
			return Result.bSucceeded
				? Asset::FAssetResult{}
				: Asset::FAssetResult{
					Asset::EAssetError::IoError, Result.Message};
		};
		return std::make_unique<::Durin::Editor::ContentBrowser::Private::FContentBrowserPanel>(
			std::move(Settings), std::move(SaveSettings),
			std::move(Services.OpenAsset),
			std::move(MoveAssets), std::move(Services.ExecuteTransaction),
			std::move(Services.GetMountedContentMutationRevision),
			std::move(Services.NotifyMountedContentMutation),
			std::move(Services.OpenImport),
			std::move(Services.ClassifyReimport),
			std::move(Services.Reimport),
			std::move(Services.DrawImportDialogs),
			std::make_shared<::Durin::Editor::ContentBrowser::Private::FMountedContentReconciliationState>(),
			std::move(Services.ThumbnailTaskScope));
	}

	auto ExecuteAssetMoves(
		FTransactionManager& Transactions, std::span<const FAssetMove> Moves)
		-> FActionResult
	{
		std::vector<Asset::FAssetRelocationMapping> Mappings;
		Mappings.reserve(Moves.size());
		for (const FAssetMove& Move : Moves)
			Mappings.push_back({Move.OldPath, Move.NewPath});
		const Asset::FAssetResult Result =
			ExecuteAssetRelocations(Transactions, Mappings);
		return {
			.bSucceeded = static_cast<bool>(Result),
			.Message = Result.Message};
	}
} // namespace Durin::Editor::ContentBrowser
