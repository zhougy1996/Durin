#include "Toolkits/ToolkitManager.h"

#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/ToolkitHost.h"

namespace Durin
{
	auto FToolkitManager::RegisterToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit) -> bool
	{
		const auto* Session = Toolkit ? Toolkit->GetOwningAssetEditor() : nullptr;
		const auto Host = Session ? Session->GetToolkitHost() : nullptr;
		if (!Host || !Host->IsAvailable()) return false;
		std::erase_if(Toolkits, [](const auto& Entry) { return Entry.Toolkit.expired(); });
		for (const auto& Entry : Toolkits) if (Entry.Toolkit.lock() == Toolkit) return false;
		Toolkits.push_back({Toolkit});
		return true;
	}
	auto FToolkitManager::UnregisterToolkit(FAssetEditorToolkit& Toolkit) -> void
	{
		std::erase_if(Toolkits, [&](const auto& Entry) {
			const auto Existing = Entry.Toolkit.lock();
			return !Existing || Existing.get() == &Toolkit;
		});
	}
	auto FToolkitManager::GetToolkits() const -> std::vector<std::shared_ptr<FAssetEditorToolkit>>
	{
		std::vector<std::shared_ptr<FAssetEditorToolkit>> Result;
		for (const auto& Entry : Toolkits) if (auto Toolkit = Entry.Toolkit.lock()) Result.push_back(std::move(Toolkit));
		return Result;
	}
	auto FToolkitManager::GetHost(const FAssetEditorToolkit& Toolkit) const -> std::shared_ptr<IToolkitHost>
	{
		for (const auto& Entry : Toolkits) if (Entry.Toolkit.lock().get() == &Toolkit) return Toolkit.GetOwningAssetEditor() ? Toolkit.GetOwningAssetEditor()->GetToolkitHost() : nullptr;
		return {};
	}

}
