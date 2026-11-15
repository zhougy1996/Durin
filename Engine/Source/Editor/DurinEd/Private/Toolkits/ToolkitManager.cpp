#include "Toolkits/ToolkitManager.h"

#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace Durin
{
	auto FToolkitManager::Get() -> FToolkitManager&
	{
		static FToolkitManager Manager;
		return Manager;
	}
	auto FToolkitManager::Contains(const FAssetEditorToolkit& Toolkit) const -> bool
	{
		return std::ranges::any_of(Toolkits, [&](const auto& Entry) { return Entry.get() == &Toolkit; });
	}
	auto FToolkitManager::RegisterNewToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit) -> bool
	{
		if (!Toolkit || !Toolkit->bInitialized || Toolkit->IsClosed()
			|| Toolkit->Host.expired() || Contains(*Toolkit)) return false;
		Toolkits.push_back(std::move(Toolkit));
		return true;
	}
	auto FToolkitManager::GetToolkits(const DAssetEditorSubsystem* Subsystem) const
		-> std::vector<std::shared_ptr<FAssetEditorToolkit>>
	{
		std::vector<std::shared_ptr<FAssetEditorToolkit>> Result;
		const FObjectKey OwnerKey(Subsystem);
		for (const auto& Toolkit : Toolkits)
			if (!Subsystem || Toolkit->AssetEditors.GetKey() == OwnerKey) Result.push_back(Toolkit);
		std::ranges::sort(Result, [](const auto& A, const auto& B) {
			if (A->GetLastActivationTime() != B->GetLastActivationTime()) return A->GetLastActivationTime() < B->GetLastActivationTime();
			return A->GetWindowId() < B->GetWindowId();
		});
		return Result;
	}
	auto FToolkitManager::CloseToolkit(FAssetEditorToolkit& Toolkit) -> void
	{
		if (Toolkit.IsClosed() || !Contains(Toolkit)) return;
		const auto Lease = Toolkit.shared_from_this();
		const auto Editor = Toolkit.OwningAssetEditor;
		auto* Instance = Toolkit.GetEditorInstance();
		auto* Subsystem = Toolkit.GetAssetEditorSubsystem();
		Toolkit.NotifyClosed();
		std::erase(Toolkits, Lease);
		Toolkit.OwningAssetEditor.Reset();
		if (Subsystem) Subsystem->FinishToolkitClose(Toolkit, Editor.Get(), Instance);
	}
	auto FToolkitManager::OnToolkitHostDestroyed(const IToolkitHost* Host) -> void
	{
		const auto Snapshot = Toolkits;
		for (const auto& Toolkit : Snapshot)
			if (Toolkit->HostIdentity == Host) CloseToolkit(*Toolkit);
	}
}
