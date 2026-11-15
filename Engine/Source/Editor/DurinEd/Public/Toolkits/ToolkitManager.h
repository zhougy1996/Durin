#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"

namespace Durin
{
	class DAssetEditorSubsystem;
	class FAssetEditorToolkit;
	class IToolkitHost;

	// Sole persistent C++ owner of open toolkits. Subsystems provide asset policy
	// and non-owning instance associations; presentation owners keep hosts alive.
	class FToolkitManager final
	{
	public:
		DURINED_API static auto Get() -> FToolkitManager&;
		DURINED_API auto RegisterNewToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit) -> bool;
		DURINED_API auto CloseToolkit(FAssetEditorToolkit& Toolkit) -> void;
		DURINED_API auto OnToolkitHostDestroyed(const IToolkitHost* Host) -> void;
		// Stable activation-order snapshot; an optional subsystem limits the owning scope.
		DURINED_API auto GetToolkits(const DAssetEditorSubsystem* Subsystem = nullptr) const
			-> std::vector<std::shared_ptr<FAssetEditorToolkit>>;
		DURINED_API auto Contains(const FAssetEditorToolkit& Toolkit) const -> bool;
	private:
		FToolkitManager() = default;
		FToolkitManager(const FToolkitManager&) = delete;
		auto operator=(const FToolkitManager&) -> FToolkitManager& = delete;
		std::vector<std::shared_ptr<FAssetEditorToolkit>> Toolkits;
	};
}
