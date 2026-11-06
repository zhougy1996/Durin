#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"

namespace Durin
{
	class FAssetEditorToolkit;
	class IToolkitHost;

	// Per-editor live toolkit index; sessions own toolkits and callers borrow bounded leases.
	class FToolkitManager
	{
	public:
		DURINED_API auto RegisterToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit) -> bool;
		DURINED_API auto UnregisterToolkit(FAssetEditorToolkit& Toolkit) -> void;
		DURINED_API auto GetToolkits() const -> std::vector<std::shared_ptr<FAssetEditorToolkit>>;
		DURINED_API auto GetHost(const FAssetEditorToolkit& Toolkit) const -> std::shared_ptr<IToolkitHost>;
		auto Clear() -> void { Toolkits.clear(); }
	private:
		struct FEntry
		{
			std::weak_ptr<FAssetEditorToolkit> Toolkit;
		};
		std::vector<FEntry> Toolkits;
	};
}
