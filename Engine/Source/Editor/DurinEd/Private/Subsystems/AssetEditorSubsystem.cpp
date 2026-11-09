#include "Subsystems/AssetEditorSubsystem.h"

#include "Asset/Load.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/EditorEngine.h"
#include "Threading/RunnableThread.h"
#include "Documents/EditorDocumentManager.h"
#include "AssetRegistry/Catalog.h"
#include "DObject/SoftObjectPtr.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/ToolkitManager.h"

namespace Durin
{
	DAssetEditorSubsystem::DAssetEditorSubsystem(const FObjectInitializer& Initializer)
		: Super(Initializer) {}

	auto DAssetEditorSubsystem::Initialize() -> FSubsystemResult
	{
		ToolkitManager = std::make_shared<FToolkitManager>();
		return {};
	}

	auto DAssetEditorSubsystem::CanCall() const -> bool
	{
		if (!IsInGameThread() || !IsValid(this) || bStoppingSessions) return false;
		const auto Gate = GetWorkGate();
		auto* Host = GetEditor();
		return Gate && Gate->IsOpen() && IsValid(Host)
			&& Host->GetEditorSubsystem<DAssetEditorSubsystem>() == this;
	}

	auto DAssetEditorSubsystem::Deinitialize() noexcept -> void
	{
		std::vector<FAssetOpenRequestId> Pending;
		for (const auto& [Id, Request] : OpenRequests)
			if (Request.Result.Status == EAssetOpenStatus::Pending) Pending.push_back({Id});
		for (const auto Id : Pending)
			requiref(CancelOpenRequest(Id), "Asset-editor pending work must drain before subsystem retirement");
		ShutdownAssetEditorSessions();
		ToolkitHostFactory = {};
		OpenRequests.clear();
		CompletedRequests.clear();
		Lifecycle.reset();
		DispatchingDefinition.Reset();
		OpeningPaths.clear();
		OpenedAssets.clear();
		OpenedEditors.clear();
		OpeningAssets.clear();
	}

	auto DAssetEditorSubsystem::CanOpenEditorForAsset(DObject* Asset, std::string* OutErrorMsg) -> bool
	{
		FSoftObjectPath Path;
		const bool bAllowed = CanCall() && IsValid(Asset) && !Asset->IsTemplateObject()
			&& FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Path)
			&& CanOpenAsset({Path, Asset->GetClass()->GetQualifiedName().ToString()});
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
		DispatchEditorCallback([&] {
			// Keep the object alive through editor callbacks and possible collection.
			TStrongObjectPtr<DObject> KeepAlive(Asset);
			IAssetEditorInstance* Best = nullptr;
			bool bBestCanonical = false;
			double BestTime = 0;
			for (auto* Instance : FindEditorsForAsset(Asset))
				if (HasEditor(Instance) && Instance->IsPrimaryEditor() && Instance->GetOpenMethod() == EAssetOpenMethod::Edit)
				{
					const auto* Session = dynamic_cast<DAssetEditor*>(Instance);
					const bool bCanonical = !Session || Session->GetInstanceKey().empty();
					const double ActivationTime = Instance->GetLastActivationTime();
					if (!Best || (bCanonical && !bBestCanonical) || (bCanonical == bBestCanonical && ActivationTime > BestTime))
					{ Best = Instance; bBestCanonical = bCanonical; BestTime = ActivationTime; }
				}
			if (Best && HasEditor(Best))
			{
				Best->FocusWindow(Asset);
				if (CanCall() && HasEditor(Best)) { bOpened = true; return; }
			}
			if (!CanOpenEditorForAsset(Asset)) return;
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Path)) return;
			bOpened = OpenAssets({.AssetEditors = this,
				.Assets = {{Path, Asset->GetClass()->GetQualifiedName().ToString()}}}).IsAccepted();
			bOpened = bOpened && CanCall();
		});
		return bOpened;
	}

	auto DAssetEditorSubsystem::OpenEditorForAsset(const std::string& AssetPathName) -> void
	{
		if (!CanCall()) return;
		FSoftObjectPath Path;
		if (FSoftObjectPath::TryCreate(AssetPathName, Path)) OpenEditorForAsset(Path);
	}

	auto DAssetEditorSubsystem::OpenEditorForAsset(const FSoftObjectPath& AssetPath) -> void
	{
		if (!CanCall() || !AssetPath.IsValid()) return;
		DispatchEditorCallback([&] {
			FSoftObjectPtr Resident(AssetPath);
			if (auto* Object = Resident.Get()) { (void)OpenEditorForAsset(Object); return; }
			const auto Metadata = ResolveAssetObjectPath(AssetPath);
			if (Metadata && Metadata.FinalAssetData)
			{
				(void)OpenAssets({.AssetEditors = this,
					.Assets = {{Metadata.FinalPath, Metadata.FinalAssetData->AssetClassName}}});
				return;
			}
			const auto Loaded = LoadObjectDetailed<DObject>(AssetPath);
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
			if (bFocusIfOpen) DispatchEditorCallback([&] { Instance->FocusWindow(Asset); });
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
			if (HasEditor(Instance)) DispatchEditorCallback([&] {
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
				DispatchEditorCallback([&] { (void)Instance->CloseWindow(EAssetEditorCloseReason::CloseOtherEditors); });
		}
	}

	auto DAssetEditorSubsystem::CloseAllAssetEditors() -> bool
	{
		if (!CanCall()) return false;
		bool bAllClosed = true;
		for (auto* Instance : GetAllOpenEditors())
		{
			if (!CanCall()) return false;
			if (HasEditor(Instance)) DispatchEditorCallback([&] {
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
		FSoftObjectPath Path;
		if (FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Path))
			PublishEvent({.Type = EAssetEditorEvent::Opened, .Target = {Path, Asset->GetClass()->GetQualifiedName().ToString()},
				.Session = FObjectKey(dynamic_cast<DAssetEditor*>(Instance)), .Method = Instance->GetOpenMethod(), .Status = EAssetOpenStatus::Completed});
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
		const auto FoundEditor = OpenedEditors.find(Instance);
		if (FoundEditor == OpenedEditors.end() || std::ranges::find(FoundEditor->second, Key) == FoundEditor->second.end()) return;
		FSoftObjectPath Path;
		FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Path);
		FAssetEditorEvent Event{.Type = EAssetEditorEvent::AssetRemoved, .Target = {Path, Asset->GetClass()->GetQualifiedName().ToString()},
			.Session = FObjectKey(dynamic_cast<DAssetEditor*>(Instance)), .Method = Instance->GetOpenMethod()};
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
		PublishEvent(std::move(Event));
	}

	auto DAssetEditorSubsystem::NotifyEditorClosed(IAssetEditorInstance* Instance) -> void
	{
		if (!IsInGameThread()) return;
		const auto Found = OpenedEditors.find(Instance);
		if (Found == OpenedEditors.end()) return;
		const auto Keys = Found->second;
		OpenedEditors.erase(Found);
		std::vector<FAssetEditorEvent> Events;
		for (const auto& Key : Keys)
		{
			const auto Asset = OpenedAssets.find(Key);
			if (Asset == OpenedAssets.end()) continue;
			std::erase(Asset->second, Instance);
			if (Asset->second.empty()) OpenedAssets.erase(Asset);
			if (auto* Object = Key.ResolveObjectPtr())
			{
				FSoftObjectPath Path; FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path);
				Events.push_back({.Type = EAssetEditorEvent::AssetRemoved, .Target = {Path, Object->GetClass()->GetQualifiedName().ToString()},
					.Session = FObjectKey(dynamic_cast<DAssetEditor*>(Instance)), .Method = Instance->GetOpenMethod()});
			}
		}
		for (auto& Event : Events) PublishEvent(std::move(Event));
	}
}
