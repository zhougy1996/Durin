#include "Subsystems/AssetEditorSubsystem.h"

#include "Asset/Load.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/EditorEngine.h"
#include "Threading/RunnableThread.h"
#include "Workspace/WorkspaceManager.h"

namespace Durin
{
	DAssetEditorSubsystem::DAssetEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DAssetEditorSubsystem::CanCall() const -> bool
	{
		if (!IsInGameThread() || !IsValid(this)) return false;
		const auto Gate = GetWorkGate();
		auto* Host = GetEditor();
		return Gate && Gate->IsOpen() && IsValid(Host)
			&& Host->GetEditorSubsystem<DAssetEditorSubsystem>() == this;
	}

	auto DAssetEditorSubsystem::Deinitialize() noexcept -> void
	{
		OpenedAssets.clear();
		OpenedEditors.clear();
		OpeningAssets.clear();
		WorkspaceManager = nullptr;
	}

	auto DAssetEditorSubsystem::CanOpenEditorForAsset(DObject* Asset, std::string* OutErrorMsg) -> bool
	{
		FObjectPath Path;
		const bool bAllowed = CanCall() && IsValid(Asset) && !Asset->IsTemplateObject()
			&& FObjectPath::TryCreate(Asset->GetObjectPath(), Path)
			&& WorkspaceManager && WorkspaceManager->CanOpenAsset(Asset->GetClass()->GetQualifiedName().ToString());
		if (OutErrorMsg) *OutErrorMsg = bAllowed ? std::string{} : "The asset or its editor is unavailable.";
		return bAllowed;
	}

	auto DAssetEditorSubsystem::OpenEditorForAsset(DObject* Asset) -> bool
	{
		if (!CanCall() || !IsValid(Asset)) return false;
		const FObjectKey Key(Asset);
		if (!OpeningAssets.insert(Key).second) return false;
		struct FOpenScope
		{
			std::unordered_set<FObjectKey, FObjectKeyHash>& Assets;
			FObjectKey Key;
			~FOpenScope() { Assets.erase(Key); }
		} OpenScope{OpeningAssets, Key};
		bool bOpened = false;
		GetEditor()->DispatchSubsystemCallback([&] {
			// Keep the object alive through editor callbacks and possible collection.
			TStrongObjectPtr<DObject> KeepAlive(Asset);
			if (FindEditorForAsset(Asset, true)) { bOpened = CanCall(); return; }
			if (!CanOpenEditorForAsset(Asset)) return;
			bOpened = WorkspaceManager->OpenAsset(Asset->GetObjectPath(), Asset->GetClass()->GetQualifiedName().ToString());
			bOpened = bOpened && CanCall();
		});
		return bOpened;
	}

	auto DAssetEditorSubsystem::OpenEditorForAsset(const std::string& AssetPathName) -> void
	{
		if (!CanCall()) return;
		FObjectPath Path;
		if (FObjectPath::TryCreate(AssetPathName, Path)) OpenEditorForAsset(Path);
	}

	auto DAssetEditorSubsystem::OpenEditorForAsset(const FObjectPath& AssetPath) -> void
	{
		if (!CanCall() || !AssetPath.IsValid()) return;
		GetEditor()->DispatchSubsystemCallback([&] {
			const auto Loaded = LoadObject<DObject>(AssetPath);
			if (Loaded && *Loaded) (void)OpenEditorForAsset(*Loaded);
		});
	}

	auto DAssetEditorSubsystem::OpenEditorForAssets(const std::vector<DObject*>& Assets) -> bool
	{
		if (!CanCall() || Assets.empty()) return false;
		bool bAllOpened = true;
		std::vector<TStrongObjectPtr<DObject>> KeepAlive;
		std::unordered_set<DObject*> Seen;
		for (auto* Asset : Assets)
			if (Seen.insert(Asset).second) KeepAlive.emplace_back(Asset);
		for (const auto& Asset : KeepAlive) bAllOpened = OpenEditorForAsset(Asset.Get()) && bAllOpened;
		return bAllOpened;
	}

	auto DAssetEditorSubsystem::OpenEditorsForAssets(const std::vector<std::string>& AssetsToOpen) -> void
	{
		for (const auto& Path : AssetsToOpen) OpenEditorForAsset(Path);
	}

	auto DAssetEditorSubsystem::FindEditorsForAsset(DObject* Asset) -> std::vector<IAssetEditorInstance*>
	{
		if (!CanCall() || !IsValid(Asset)) return {};
		const auto Found = OpenedAssets.find(FObjectKey(Asset));
		return Found == OpenedAssets.end() ? std::vector<IAssetEditorInstance*>{} : Found->second;
	}

	auto DAssetEditorSubsystem::FindEditorForAsset(DObject* Asset, bool bFocusIfOpen) -> IAssetEditorInstance*
	{
		for (auto* Instance : FindEditorsForAsset(Asset))
		{
			if (!HasEditor(Instance) || !Instance->IsPrimaryEditor()) continue;
			if (bFocusIfOpen) GetEditor()->DispatchSubsystemCallback([&] { Instance->FocusWindow(Asset); });
			return CanCall() && HasEditor(Instance) ? Instance : nullptr;
		}
		return nullptr;
	}

	auto DAssetEditorSubsystem::GetAllEditedAssets() -> std::vector<DObject*>
	{
		std::vector<DObject*> Result;
		if (CanCall()) for (const auto& [Key, Editors] : OpenedAssets)
			if (auto* Asset = Key.ResolveObjectPtr()) Result.push_back(Asset);
		return Result;
	}

	auto DAssetEditorSubsystem::GetAllOpenEditors() -> std::vector<IAssetEditorInstance*>
	{
		std::vector<IAssetEditorInstance*> Result;
		if (CanCall()) for (const auto& [Instance, Assets] : OpenedEditors) Result.push_back(Instance);
		return Result;
	}

	auto DAssetEditorSubsystem::HasEditor(IAssetEditorInstance* Instance) const -> bool
	{
		return OpenedEditors.contains(Instance);
	}

	auto DAssetEditorSubsystem::CloseAllEditorsForAsset(DObject* Asset) -> int32
	{
		int32 Closed = 0;
		for (auto* Instance : FindEditorsForAsset(Asset))
		{
			if (!CanCall()) break;
			if (HasEditor(Instance)) GetEditor()->DispatchSubsystemCallback([&] {
				if (Instance->CloseWindow(EAssetEditorCloseReason::CloseAllEditorsForAsset)) ++Closed;
			});
		}
		return Closed;
	}

	auto DAssetEditorSubsystem::CloseOtherEditors(DObject* Asset, IAssetEditorInstance* OnlyEditor) -> void
	{
		for (auto* Instance : FindEditorsForAsset(Asset))
		{
			if (!CanCall()) break;
			if (Instance != OnlyEditor && HasEditor(Instance))
				GetEditor()->DispatchSubsystemCallback([&] { (void)Instance->CloseWindow(EAssetEditorCloseReason::CloseOtherEditors); });
		}
	}

	auto DAssetEditorSubsystem::CloseAllAssetEditors() -> bool
	{
		if (!CanCall()) return false;
		bool bAllClosed = true;
		for (auto* Instance : GetAllOpenEditors())
		{
			if (!CanCall()) return false;
			if (HasEditor(Instance)) GetEditor()->DispatchSubsystemCallback([&] {
				bAllClosed = Instance->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors) && bAllClosed;
			});
		}
		return bAllClosed && CanCall() && OpenedEditors.empty();
	}

	auto DAssetEditorSubsystem::NotifyAssetOpened(DObject* Asset, IAssetEditorInstance* Instance) -> void
	{
		if (!CanCall() || !IsValid(Asset) || !Instance) return;
		const FObjectKey Key(Asset);
		auto& Editors = OpenedAssets[Key];
		if (std::ranges::find(Editors, Instance) != Editors.end()) return;
		Editors.push_back(Instance);
		OpenedEditors[Instance].push_back(Key);
	}

	auto DAssetEditorSubsystem::NotifyAssetsOpened(const std::vector<DObject*>& Assets, IAssetEditorInstance* Instance) -> void
	{
		for (auto* Asset : Assets) NotifyAssetOpened(Asset, Instance);
	}

	auto DAssetEditorSubsystem::NotifyAssetClosed(DObject* Asset, IAssetEditorInstance* Instance) -> void
	{
		// Cleanup remains legal after admission closes, while host consumers retire.
		if (!IsInGameThread() || !Asset || !Instance) return;
		const FObjectKey Key(Asset);
		if (auto Found = OpenedAssets.find(Key); Found != OpenedAssets.end())
		{
			std::erase(Found->second, Instance);
			if (Found->second.empty()) OpenedAssets.erase(Found);
		}
		if (auto Found = OpenedEditors.find(Instance); Found != OpenedEditors.end())
		{
			std::erase(Found->second, Key);
			if (Found->second.empty()) OpenedEditors.erase(Found);
		}
	}

	auto DAssetEditorSubsystem::NotifyEditorClosed(IAssetEditorInstance* Instance) -> void
	{
		if (!IsInGameThread()) return;
		const auto Found = OpenedEditors.find(Instance);
		if (Found == OpenedEditors.end()) return;
		for (const auto& Key : Found->second)
		{
			const auto Asset = OpenedAssets.find(Key);
			if (Asset == OpenedAssets.end()) continue;
			std::erase(Asset->second, Instance);
			if (Asset->second.empty()) OpenedAssets.erase(Asset);
		}
		OpenedEditors.erase(Found);
	}
}
