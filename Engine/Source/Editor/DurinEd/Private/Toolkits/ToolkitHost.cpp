#include "Toolkits/ToolkitHost.h"

#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"

namespace Durin
{
	auto FNonWindowToolkitHost::AttachToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit) -> bool
	{
		if (!IsAvailable() || !Toolkit) return false;
		for (const auto& Existing : Toolkits) if (Existing.lock() == Toolkit) return false;
		Toolkits.push_back(Toolkit);
		return true;
	}
	auto FNonWindowToolkitHost::DetachToolkit(FAssetEditorToolkit& Toolkit) -> void
	{
		std::erase_if(Toolkits, [&](const auto& Weak) {
			const auto Existing = Weak.lock();
			return !Existing || Existing.get() == &Toolkit;
		});
	}
	auto FNonWindowToolkitHost::RequestClose() -> bool
	{
		const auto Snapshot = Toolkits;
		bool Closed = true;
		for (const auto& Weak : Snapshot)
			if (const auto Toolkit = Weak.lock())
				if (auto* Editor = Toolkit->GetEditorInstance())
					Closed = Editor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed) && Closed;
		if (Closed) { Toolkits.clear(); bClosed = true; }
		return Closed;
	}
}
