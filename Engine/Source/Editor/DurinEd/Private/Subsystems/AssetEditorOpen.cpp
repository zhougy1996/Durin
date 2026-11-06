#include "Subsystems/AssetEditorSubsystem.h"

#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "Documents/EditorDocumentManager.h"
#include "Editor/EditorEngine.h"
#include "Toolkits/ToolkitHost.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	auto DAssetEditorSubsystem::CanOpenAsset(const FAssetOpenTarget& Target, EAssetOpenMethod Method) -> bool
	{
		if (!CanCall() || !Target.AssetPath.IsValid() || Target.AssetClassName.empty()) return false;
		if (const auto* Definition = FAssetDefinitionRegistry::Get().FindDefinition(Target.AssetClassName))
			{
			const auto Support = Definition->GetAssetOpenSupport({Method});
			return Support.OpenMethod == Method && Support.ResolveToolkitMode(EToolkitMode::Standalone).has_value();
		}
		return false;
	}

	auto DAssetEditorSubsystem::OpenAssets(const FAssetOpenArgs& Args) -> FAssetOpenResult
	{
		CancelRestoreFocus();
		FAssetOpenResult Result;
		struct FSelection
		{
			FAssetOpenItemResult Result;
			FStrongObjectPtr Definition;
			FAssetOpenSupport Support;
			EToolkitMode Mode = EToolkitMode::Standalone;
			bool bDispatch = false;
		};
		std::vector<FSelection> Selection;
		std::vector<size_t> Inputs;
		std::unordered_map<FObjectPath, size_t> Seen;
		std::vector<FObjectPath> Reserved;
		struct FPathScope
		{
			std::unordered_set<FObjectPath>& Paths;
			std::vector<FObjectPath>& Reserved;
			~FPathScope() { for (const auto& Path : Reserved) Paths.erase(Path); }
		} PathScope{OpeningPaths, Reserved};
		for (const auto& Target : Args.Assets)
		{
			if (const auto Duplicate = Seen.find(Target.AssetPath); Duplicate != Seen.end()
				&& Selection[Duplicate->second].Result.Target.AssetClassName == Target.AssetClassName)
			{ Inputs.push_back(Duplicate->second); continue; }
			const bool Conflict = Seen.contains(Target.AssetPath);
			Inputs.push_back(Selection.size());
			Seen.try_emplace(Target.AssetPath, Selection.size());
			auto& Entry = Selection.emplace_back();
			auto& Item = Entry.Result;
			Item = {.Target = Target, .Status = EAssetOpenStatus::Rejected};
			if (!CanCall() || Args.AssetEditors != this) Item.Message = "The requesting editor is unavailable or does not own this request.";
			else if (!Target.AssetPath.IsValid() || Target.AssetClassName.empty()) Item.Message = "An asset path and class identity are required.";
			else if ((Args.OpenMethod != EAssetOpenMethod::Edit && Args.OpenMethod != EAssetOpenMethod::View)
				|| (Args.ToolkitMode != EToolkitMode::Standalone && Args.ToolkitMode != EToolkitMode::WorldCentric)) Item.Message = "The requested method or host mode is invalid.";
			else if (Conflict) Item.Message = "Conflicting class metadata for the same asset.";
			else if (OpeningPaths.contains(Target.AssetPath)) Item.Message = "Recursive opening of the same asset is unavailable.";
			else if (auto* Definition = FAssetDefinitionRegistry::Get().FindDefinition(Target.AssetClassName))
			{
				Entry.Definition = FStrongObjectPtr(const_cast<DAssetDefinition*>(Definition));
				Entry.Support = Definition->GetAssetOpenSupport({Args.OpenMethod});
				const auto Mode = Entry.Support.ResolveToolkitMode(Args.ToolkitMode, Args.Host ? std::optional(Args.Host->GetToolkitMode()) : std::nullopt);
				if (Entry.Support.OpenMethod != Args.OpenMethod || !Mode || (Args.Host && !Args.Host->IsAvailable()))
					Item.Message = "The asset definition does not support the requested method or host.";
				else
				{
					Entry.Mode = *Mode;
					Entry.bDispatch = true;
					OpeningPaths.insert(Target.AssetPath);
					Reserved.push_back(Target.AssetPath);
				}
			}
			else { Item.Status = EAssetOpenStatus::Unhandled; Item.Message = "No active asset definition handles this class."; }
		}
		if (Args.bCreateSingleSession)
		{
			std::optional<EToolkitMode> Required;
			bool Compatible = true;
			for (const auto& Entry : Selection) if (Entry.bDispatch && Entry.Support.RequiredToolkitMode)
			{
				Compatible &= !Required || Required == Entry.Support.RequiredToolkitMode;
				Required = Entry.Support.RequiredToolkitMode;
			}
			for (auto& Entry : Selection) if (Entry.bDispatch)
			{
				if (Compatible) Entry.Mode = Required.value_or(Entry.Mode);
				else { Entry.bDispatch = false; Entry.Result.Message = "The group requires incompatible toolkit hosts."; }
			}
		}
		// A requested single session is atomic at metadata preflight.
		if (Args.bCreateSingleSession && std::ranges::any_of(Selection, [](const auto& Entry) { return !Entry.bDispatch; }))
			for (auto& Entry : Selection) if (Entry.bDispatch)
			{ Entry.bDispatch = false; Entry.Result.Message = "The group contains an unavailable selection."; }
		std::vector<std::vector<size_t>> Batches;
		for (size_t Index = 0; Index < Selection.size(); ++Index)
		{
			if (!Selection[Index].bDispatch) continue;
			auto Batch = std::ranges::find_if(Batches, [&](const auto& Group) {
				return Args.bCreateSingleSession || Selection[Group.front()].Definition.GetKey() == Selection[Index].Definition.GetKey();
			});
			if (Batch == Batches.end()) Batches.push_back({Index}); else Batch->push_back(Index);
		}
		for (const auto& Batch : Batches)
		{
			FAssetOpenArgs Selected = Args;
			Selected.ToolkitMode = Selection[Batch.front()].Mode;
			Selected.Assets.clear();
			for (const auto Index : Batch)
			{
				Selected.Assets.push_back(Selection[Index].Result.Target);
				PublishEvent({.Type = EAssetEditorEvent::Requested, .Target = Selection[Index].Result.Target, .Method = Args.OpenMethod});
			}
			if (CanCall()) DispatchEditorCallback([&] {
				const auto Provider = Selection[Batch.front()].Definition;
				const auto* Definition = Cast<DAssetDefinition>(Provider.Get());
				struct FDispatchScope
				{
					FStrongObjectPtr& Slot;
					FStrongObjectPtr Previous;
					EAssetOpenMethod& Method;
					EAssetOpenMethod PreviousMethod;
					~FDispatchScope() { Slot = std::move(Previous); Method = PreviousMethod; }
				} DispatchScope{DispatchingDefinition, std::move(DispatchingDefinition), DispatchingOpenMethod, DispatchingOpenMethod};
				DispatchingDefinition = Provider;
				DispatchingOpenMethod = Args.OpenMethod;
				auto Opened = Definition->OpenAssets(Selected);
				bool Valid = Opened.Items.size() == Batch.size();
				if (Valid) for (size_t I = 0; I < Batch.size(); ++I)
					Valid = Valid && Opened.Items[I].Target.AssetPath == Selected.Assets[I].AssetPath
						&& Opened.Items[I].Target.AssetClassName == Selected.Assets[I].AssetClassName;
				for (size_t I = 0; I < Batch.size(); ++I)
				{
					auto& Item = Selection[Batch[I]].Result;
					if (!Valid) { Item.Status = EAssetOpenStatus::Failed; Item.Message = "The asset definition returned an invalid result selection."; continue; }
					Item = std::move(Opened.Items[I]);
					if (Item.Status == EAssetOpenStatus::Pending)
					{
						const auto Pending = OpenRequests.find(Item.RequestId.Value);
						if (Pending == OpenRequests.end() || Pending->second.Result.Target.AssetPath != Item.Target.AssetPath
							|| Pending->second.Result.Target.AssetClassName != Item.Target.AssetClassName || Pending->second.ProviderIdentity != Provider.GetKey())
						{ Item.Status = EAssetOpenStatus::Failed; Item.Message = "The asset definition did not register its pending request."; }
						else Item = Pending->second.Result;
					}
				}
			});
			if (!CanCall()) for (const auto Index : Batch)
			{ Selection[Index].Result.Status = EAssetOpenStatus::Cancelled; Selection[Index].Result.Message = "The editor stopped during opening."; }
		}
		for (const auto& Entry : Selection)
			if (Entry.Result.Status != EAssetOpenStatus::Pending && !Entry.Result.RequestId.IsValid())
				PublishEvent({.Type = EAssetEditorEvent::RequestFinished, .Target = Entry.Result.Target, .Method = Args.OpenMethod, .Status = Entry.Result.Status});
		for (const auto Index : Inputs) Result.Items.push_back(Selection[Index].Result);
		return Result;
	}

	auto DAssetEditorSubsystem::BeginOpenRequest(FAssetOpenTarget Target,
		std::function<bool()> Cancel) -> FAssetOpenRequestId
	{
		if (!CanCall() || !DispatchingDefinition || !Target.AssetPath.IsValid() || !Cancel) return {};
		const FAssetOpenRequestId Id{NextOpenRequestId++};
		OpenRequests.emplace(Id.Value, FOpenRequest{
			.Result = {.Target = std::move(Target), .Status = EAssetOpenStatus::Pending, .RequestId = Id},
			.Definition = DispatchingDefinition, .ProviderIdentity = DispatchingDefinition.GetKey(), .Cancel = std::move(Cancel), .Method = DispatchingOpenMethod});
		return Id;
	}

	auto DAssetEditorSubsystem::CompleteOpenRequest(FAssetOpenRequestId Id,
		EAssetOpenStatus Status, std::string Message) -> bool
	{
		if (!IsInGameThread() || (Status != EAssetOpenStatus::Completed
			&& Status != EAssetOpenStatus::Failed && Status != EAssetOpenStatus::Cancelled)) return false;
		const auto Found = OpenRequests.find(Id.Value);
		if (Found == OpenRequests.end() || Found->second.Result.Status != EAssetOpenStatus::Pending) return false;
		Found->second.Result.Status = Status;
		Found->second.Result.Message = std::move(Message);
		const auto Result = Found->second.Result;
		const auto Method = Found->second.Method;
		auto Cancel = std::move(Found->second.Cancel);
		auto Provider = std::move(Found->second.Definition);
		CompletedRequests.push_back(Id.Value);
		if (CompletedRequests.size() > 256)
		{
			OpenRequests.erase(CompletedRequests.front());
			CompletedRequests.erase(CompletedRequests.begin());
		}
		PublishEvent({.Type = EAssetEditorEvent::RequestFinished, .Target = Result.Target, .Method = Method, .Status = Status, .RequestId = Id});
		return true;
	}

	auto DAssetEditorSubsystem::GetOpenRequest(FAssetOpenRequestId Id) const -> std::optional<FAssetOpenItemResult>
	{
		if (!IsInGameThread()) return std::nullopt;
		const auto Found = OpenRequests.find(Id.Value);
		return Found == OpenRequests.end() ? std::nullopt : std::optional(Found->second.Result);
	}

	auto DAssetEditorSubsystem::CancelOpenRequest(FAssetOpenRequestId Id) -> bool
	{
		if (!IsInGameThread()) return false;
		const auto Found = OpenRequests.find(Id.Value);
		if (Found == OpenRequests.end() || Found->second.Result.Status != EAssetOpenStatus::Pending || !Found->second.Cancel)
			return false;
		// The callback may finish this request or rehash the request table.
		const auto Cancel = Found->second.Cancel;
		const auto KeepProvider = Found->second.Definition;
		if (!Cancel()) return false;
		(void)CompleteOpenRequest(Id, EAssetOpenStatus::Cancelled);
		const auto Final = GetOpenRequest(Id);
		return Final && Final->Status == EAssetOpenStatus::Cancelled;
	}

	auto DAssetEditorSubsystem::BindOpenRequestToDocument(FAssetOpenRequestId Id, uint64 DocumentId) -> bool
	{
		const auto Found = OpenRequests.find(Id.Value);
		if (!CanCall() || !DocumentId || Found == OpenRequests.end() || Found->second.Result.Status != EAssetOpenStatus::Pending) return false;
		Found->second.DocumentId = DocumentId;
		return true;
	}



	auto DAssetEditorSubsystem::CompleteDocumentOpen(uint64 DocumentId, EAssetOpenStatus Status) -> void
	{
		std::vector<FAssetOpenRequestId> Requests;
		for (const auto& [Id, Request] : OpenRequests)
			if (Request.DocumentId == DocumentId && Request.Result.Status == EAssetOpenStatus::Pending)
				Requests.push_back({Id});
		for (const auto Id : Requests) (void)CompleteOpenRequest(Id, Status);
	}


}
