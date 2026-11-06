#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "AssetDefinition/AssetOpenTypes.h"

namespace Durin
{
	class FAssetEditorToolkit;
	struct FEditorTabWindowConfig;

	// Presentation attachment never becomes another persistent session owner.
	class IToolkitHost
	{
	public:
		virtual ~IToolkitHost() = default;
		virtual auto GetToolkitMode() const -> EToolkitMode = 0;
		virtual auto AttachToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit) -> bool = 0;
		virtual auto DetachToolkit(FAssetEditorToolkit& Toolkit) -> void = 0;
		virtual auto FocusToolkit(FAssetEditorToolkit& Toolkit) -> void = 0;
		virtual auto RequestClose() -> bool = 0;
		virtual auto IsAvailable() const -> bool = 0;
		virtual auto IsReadyForFocus() const -> bool { return IsAvailable(); }
		virtual auto PrepareRootWindow(FEditorTabWindowConfig&) -> void {}
		virtual auto OnRootWindowShown(uint32) -> void {}
	};

	// Semantic host for direct tests and non-window editing; dirty close can veto retirement.
	class FNonWindowToolkitHost : public IToolkitHost
	{
	public:
		explicit FNonWindowToolkitHost(EToolkitMode InMode = EToolkitMode::Standalone) : Mode(InMode) {}
		auto GetToolkitMode() const -> EToolkitMode override { return Mode; }
		DURINED_API auto AttachToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit) -> bool override;
		DURINED_API auto DetachToolkit(FAssetEditorToolkit& Toolkit) -> void override;
		auto FocusToolkit(FAssetEditorToolkit&) -> void override {}
		DURINED_API auto RequestClose() -> bool override;
		auto IsAvailable() const -> bool override { return !bClosed; }
	private:
		std::vector<std::weak_ptr<FAssetEditorToolkit>> Toolkits;
		EToolkitMode Mode;
		bool bClosed = false;
	};
}
