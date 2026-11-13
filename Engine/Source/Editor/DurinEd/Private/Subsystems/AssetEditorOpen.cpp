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
		if (const auto* Definition = FAssetDefinitionRegistry::Get().GetAssetDefinitionForClass(Target.AssetClassName))
			{
			const auto Support = Definition->GetAssetOpenSupport({Method});
			return Support.OpenMethod == Method && Support.ResolveToolkitMode(EToolkitMode::Standalone).has_value();
		}
		return false;
	}

	auto DAssetEditorSubsystem::OpenAssets(const FAssetOpenArgs& Args) -> bool
	{
		if (!CanCall() || Args.AssetEditors != this || Args.Assets.empty()) return false;
		CancelRestoreFocus();
		struct FSelection
		{
			FAssetOpenTarget Target;
			FStrongObjectPtr Definition;
			FAssetOpenSupport Support;
			EToolkitMode Mode = EToolkitMode::Standalone;
		};
		std::vector<FSelection> Selection;
		std::unordered_map<FSoftObjectPath, std::string> Seen;
		std::vector<FSoftObjectPath> Reserved;
		struct FPathScope
		{
			std::unordered_set<FSoftObjectPath>& Paths;
			std::vector<FSoftObjectPath>& Reserved;
			~FPathScope() { for (const auto& Path : Reserved) Paths.erase(Path); }
		} PathScope{OpeningPaths, Reserved};
		bool Opened = true;
		for (const auto& Target : Args.Assets)
		{
			if (const auto Found = Seen.find(Target.AssetPath); Found != Seen.end())
			{ if (Found->second != Target.AssetClassName) Opened = false; continue; }
			Seen.emplace(Target.AssetPath, Target.AssetClassName);
			if (!Target.AssetPath.IsValid() || Target.AssetClassName.empty() || OpeningPaths.contains(Target.AssetPath))
			{ Opened = false; continue; }
			const auto* Definition = FAssetDefinitionRegistry::Get().GetAssetDefinitionForClass(Target.AssetClassName);
			if (!Definition) { Opened = false; continue; }
			const auto Support = Definition->GetAssetOpenSupport({Args.OpenMethod});
			const auto Mode = Support.ResolveToolkitMode(Args.ToolkitMode, Args.Host ? std::optional(Args.Host->GetToolkitMode()) : std::nullopt);
			if (Support.OpenMethod != Args.OpenMethod || !Mode || (Args.Host && !Args.Host->IsAvailable()))
			{ Opened = false; continue; }
			Selection.push_back({Target, FStrongObjectPtr(const_cast<DAssetDefinition*>(Definition)), Support, *Mode});
			OpeningPaths.insert(Target.AssetPath);
			Reserved.push_back(Target.AssetPath);
		}
		if (Args.bCreateSingleSession)
		{
			if (!Opened) return false;
			std::optional<EToolkitMode> Required;
			for (const auto& Entry : Selection) if (Entry.Support.RequiredToolkitMode)
			{
				if (Required && Required != Entry.Support.RequiredToolkitMode) return false;
				Required = Entry.Support.RequiredToolkitMode;
			}
			if (Required) for (auto& Entry : Selection) Entry.Mode = *Required;
		}
		std::vector<std::vector<size_t>> Batches;
		for (size_t Index = 0; Index < Selection.size(); ++Index)
		{
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
				Selected.Assets.push_back(Selection[Index].Target);
				PublishEvent({.Type = EAssetEditorEvent::Requested, .Target = Selection[Index].Target, .Method = Args.OpenMethod});
			}
			if (!CanCall()) return false;
			DispatchEditorCallback([&] {
				struct FDispatchScope
				{
					FStrongObjectPtr& Slot;
					FStrongObjectPtr Previous;
					~FDispatchScope() { Slot = std::move(Previous); }
				} Scope{DispatchingDefinition, std::move(DispatchingDefinition)};
				DispatchingDefinition = Selection[Batch.front()].Definition;
				const auto* Definition = Cast<DAssetDefinition>(DispatchingDefinition.Get());
				if (Definition->OpenAssets(Selected) != EAssetCommandResult::Handled) Opened = false;
			});
			if (!CanCall()) return false;
		}
		return Opened;
	}
	auto DAssetEditorSubsystem::IsAssetOpen(const FSoftObjectPath& Path) const -> bool
	{
		if (!CanCall() || !Path.IsValid()) return false;
		const auto Text = Path.ToString();
		return std::ranges::any_of(OpenedAssets, [&](const auto& Entry) {
			const auto* Asset = Entry.first.ResolveObjectPtr();
			return Asset && Asset->GetObjectPath() == Text;
		});
	}
}
