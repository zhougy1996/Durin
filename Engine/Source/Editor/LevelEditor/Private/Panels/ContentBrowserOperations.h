#pragma once

#include "Assets/EditorAssetMoveCoordinator.h"
#include "Panels/ContentBrowserModel.h"

namespace Durin
{
	// Reports the post-operation focus requested by a successful content mutation.
	struct FContentBrowserOperationResult
	{
		Asset::FAssetResult Status;
		std::string FocusPhysicalPath;
		std::string RevealAssetPath;
		std::string OpenAssetClassName;

		explicit operator bool() const { return static_cast<bool>(Status); }
	};

	// Owns filesystem and asset-registry mutations initiated by the content browser.
	class FContentBrowserOperations
	{
	public:
		using FMoveAssets =
			std::function<Asset::FAssetResult(std::span<const FEditorAssetMove>)>;

		FContentBrowserOperations(
			FContentBrowserModel& InModel,
			FMoveAssets InMoveAssets
		);

		auto Rename(const FContentBrowserItem& Item, std::string_view NewName)
			-> FContentBrowserOperationResult;
		auto CreateFolder(std::string_view PhysicalDirectory)
			-> FContentBrowserOperationResult;
		auto CreateLevelAsset(std::string_view VirtualDirectory)
			-> FContentBrowserOperationResult;
		auto CreateMaterialAsset(
			std::string_view VirtualDirectory,
			bool bInstance) -> FContentBrowserOperationResult;
		auto Move(std::span<const FEditorAssetMove> Moves) -> Asset::FAssetResult;

		auto AnalyzeDeletion(
			std::span<const FContentBrowserItem> Items,
			const std::unordered_set<std::string>& Selection,
			std::vector<std::pair<std::string, Asset::FAssetDeleteAnalysis>>& Analyses,
			std::vector<std::pair<std::string, Asset::FAssetResult>>& Errors
		) const -> void;
		auto Delete(
			std::span<const FContentBrowserItem> Items,
			const std::unordered_set<std::string>& Selection
		) -> Asset::FAssetResult;

		auto IsManagedCompanion(const FContentBrowserItem& Item) const -> bool;
		auto ShowInExplorer(std::string_view PhysicalPath) const -> void;
		auto CopyToClipboard(std::string_view Text) const -> void;

	private:
		auto RenameFolder(const FContentBrowserItem& Item, std::string_view NewName)
			-> Asset::FAssetResult;
		auto DeleteEmptyFolder(const FContentBrowserItem& Item) const
			-> Asset::FAssetResult;

		FContentBrowserModel& Model;
		FMoveAssets MoveAssets;
	};
} // namespace Durin
