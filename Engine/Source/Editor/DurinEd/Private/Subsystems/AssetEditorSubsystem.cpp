#include "Subsystems/AssetEditorSubsystem.h"

#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/EditorEngine.h"
#include "Threading/RunnableThread.h"
#include "Documents/EditorDocumentManager.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitManager.h"

namespace Durin
{
	DAssetEditorSubsystem::DAssetEditorSubsystem(const FObjectInitializer& Initializer)
		: Super(Initializer) {}

	auto DAssetEditorSubsystem::Initialize(FSubsystemCollection&) -> void
	{
		AssetRelocationHandle = OnAssetRenamed().AddLambda([this](const FAssetData& Asset, const FSoftObjectPath& Previous) {
			OnAssetRelocated(Asset, Previous);
		});
	}

	auto DAssetEditorSubsystem::CanCall() const -> bool
	{
		if (!IsInGameThread() || !IsValid(this) || bClosingEditors) return false;
		auto* Host = GetEditor();
		return IsValid(Host) && !Host->IsShutdownRequested()
			&& Host->GetEditorSubsystem<DAssetEditorSubsystem>() == this;
	}

	auto DAssetEditorSubsystem::Deinitialize() noexcept -> void
	{
		OnAssetRenamed().Remove(AssetRelocationHandle);
		AssetRelocationHandle = {};
		CloseAssetEditorsForShutdown();
		ToolkitHostFactory = {};
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
					const auto* EditorInstance = dynamic_cast<DAssetEditor*>(Instance);
					const auto* Toolkit = dynamic_cast<FAssetEditorToolkit*>(Instance);
					const bool bCanonical = EditorInstance ? EditorInstance->GetInstanceKey().empty() : !Toolkit || Toolkit->GetInstanceKey().empty();
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
				.Assets = {{Path, Asset->GetClass()->GetQualifiedName().ToString()}}});
			bOpened = bOpened && CanCall();
		});
		return bOpened;
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
		auto ExternalEditors = GetAllOpenEditors();
		for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(this))
		{
			if (!CanCall()) return false;
			std::erase(ExternalEditors, Toolkit->GetEditorInstance());
			bAllClosed = Toolkit->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors) && bAllClosed;
		}
		for (auto* Instance : ExternalEditors)
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
		AddAssetAssociation(Asset, Instance, nullptr);
	}

	auto DAssetEditorSubsystem::AddAssetAssociation(DObject* Asset, IAssetEditorInstance* Instance,
		std::vector<FAssetEditorEvent>* Events) -> void
	{
		if (!CanCall() || !IsValid(Asset) || !Instance) return;
		const FObjectKey Key(Asset);
		auto& Editors = OpenedAssets[Key];
		if (std::ranges::find(Editors, Instance) != Editors.end()) return;
		Editors.push_back(Instance);
		OpenedEditors[Instance].push_back(Key);
		FSoftObjectPath Path;
		if (FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Path))
		{
			FAssetEditorEvent Event{.Type = EAssetEditorEvent::Opened,
				.Target = {Path, Asset->GetClass()->GetQualifiedName().ToString()},
				.Editor = FObjectKey(dynamic_cast<DAssetEditor*>(Instance)), .Method = Instance->GetOpenMethod(),
				.InstanceId = std::string(Instance->GetInstanceId())};
			if (Events) Events->push_back(std::move(Event));
			else PublishEvent(std::move(Event));
		}
	}

	auto DAssetEditorSubsystem::NotifyAssetClosed(DObject* Asset, IAssetEditorInstance* Instance) -> void
	{
		// Cleanup remains legal after admission closes, while host consumers shut down.
		if (Asset) RemoveAssetAssociation(FObjectKey(Asset), Instance, nullptr);
	}

	auto DAssetEditorSubsystem::RemoveAssetAssociation(const FObjectKey& Key, IAssetEditorInstance* Instance,
		std::vector<FAssetEditorEvent>* Events) -> void
	{
		if (!IsInGameThread() || !Instance) return;
		const auto FoundEditor = OpenedEditors.find(Instance);
		if (FoundEditor == OpenedEditors.end() || std::ranges::find(FoundEditor->second, Key) == FoundEditor->second.end()) return;
		FAssetOpenTarget Target;
		if (const auto* Asset = Key.ResolveObjectPtr())
		{
			FSoftObjectPath::TryCreate(Asset->GetObjectPath(), Target.AssetPath);
			Target.AssetClassName = Asset->GetClass()->GetQualifiedName().ToString();
		}
		FAssetEditorEvent Event{.Type = EAssetEditorEvent::AssetRemoved, .Target = std::move(Target),
			.Editor = FObjectKey(dynamic_cast<DAssetEditor*>(Instance)), .Method = Instance->GetOpenMethod(), .InstanceId = std::string(Instance->GetInstanceId())};
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
					.Editor = FObjectKey(dynamic_cast<DAssetEditor*>(Instance)), .Method = Instance->GetOpenMethod(), .InstanceId = std::string(Instance->GetInstanceId())});
			}
		}
		for (auto& Event : Events) PublishEvent(std::move(Event));
	}
}
