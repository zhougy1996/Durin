#pragma once

#include "ContentBrowser/ContentBrowserContracts.h"
#include "ContentBrowserAPI.h"

namespace Durin::Editor::ContentBrowser
{
	// Carries browser-owned presentation state across construction boundaries.
	struct FPresentationSettings
	{
		static constexpr float MinimumIconSize = 56.0f;
		static constexpr float DefaultIconSize = 88.0f;
		static constexpr float MaximumIconSize = 160.0f;
		static constexpr float MinimumTreeRatio = 0.15f;
		static constexpr float DefaultTreeRatio = 0.24f;
		static constexpr float MaximumTreeRatio = 0.55f;

		uint8 ViewMode = 0;
		float IconSize = DefaultIconSize;
		bool bIconSizeLocked = false;
		float TreeWidth = DefaultTreeRatio;
		bool bShowHiddenFiles = false;
		std::string LastDirectory;

		auto operator==(const FPresentationSettings&) const -> bool = default;
	};

	using FSavePresentationSettings =
		std::function<void(const FPresentationSettings&)>;

	// Owns the browser body while the temporary Level adapter owns its window.
	class IContentBrowserTool : public IContentBrowser
	{
	public:
		virtual auto TickWhenHidden() -> void = 0;
		virtual auto DrawContents(bool bAllowAssetMutation) -> void = 0;
		virtual auto DrawHostPresenters(bool bAllowAssetMutation) -> void = 0;
	};

	CONTENTBROWSER_API auto CreateContentBrowserTool(
		FConstructionServices Services,
		FPresentationSettings Settings,
		FSavePresentationSettings SaveSettings)
		-> std::unique_ptr<IContentBrowserTool>;

	CONTENTBROWSER_API auto ExecuteAssetMoves(std::span<const FAssetMove> Moves)
		-> FActionResult;

	CONTENTBROWSER_API auto LoadPresentationSettings(
		FPresentationSettings& Settings, std::string* OutWarning = nullptr)
		-> bool;
	CONTENTBROWSER_API auto SavePresentationSettings(
		const FPresentationSettings& Settings) -> bool;
} // namespace Durin::Editor::ContentBrowser
