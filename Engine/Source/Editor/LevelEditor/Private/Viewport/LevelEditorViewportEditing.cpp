#include "LevelEditorViewportEditing.h"

#include "Editor/EditorTransaction.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/TransformGizmo.h"
#include "Workspace/LevelEditorContext.h"

namespace Durin
{
	namespace
	{
		class FSelectViewportEditMode final : public ILevelViewportEditMode
		{
		public:
			auto Tick(FLevelEditorContext& Context, FLevelEditorViewportClient& Client, const FSceneView& View,
				FLevelEditorViewportInput& Input, FEditorTransactionManager* Transactions) -> bool override
			{
				const FTransformGizmoTargetSet Targets = GetGizmoTargets(Context);
				Client.GetTransformGizmo().Update(Targets, View, Input, Transactions);
				if (Client.GetTransformGizmo().IsHovered() || Client.GetTransformGizmo().IsDragging()) return true;
				if (!Input.bRequestSelection) return false;
				AActor* Actor = Client.PickActorWithView(Context.Level, View, Input.MousePosition);
				const FEditorVisualizationHit Visualization = Client.PickVisualizationWithView(Context.Level, View, Input.MousePosition);
				if (Actor && Visualization.Actor == Actor && Visualization.Component)
				{
					if (!Context.IsActorSelected(Visualization.Actor)) Context.SelectActor(Visualization.Actor);
					if (Visualization.Element.IsValid()) Context.SelectSubElement(Visualization.Component, Visualization.Element);
					else Context.SelectComponent(Visualization.Component);
					return true;
				}
				if (Actor)
				{
					if (Input.bCtrl) Context.ToggleActorSelection(Actor);
					else Context.SelectActor(Actor);
				}
				else Context.ClearSelection();
				return true;
			}

			auto GetGizmoTargets(const FLevelEditorContext& Context) const -> FTransformGizmoTargetSet override
			{
				return MakeActorTransformGizmoTargets(Context);
			}
		};

		auto EnsureSelectModeRegistered() -> void
		{
			auto& Registry = FLevelViewportEditModeRegistry::Get();
			if (Registry.Find("Select")) return;
			Registry.Register({
				.Id = "Select",
				.DisplayName = "Select",
				.Priority = std::numeric_limits<int32>::min(),
				.CanActivate = [](const FLevelEditorContext&) { return true; },
				.Factory = [] { return std::make_unique<FSelectViewportEditMode>(); },
			});
		}
	} // namespace

	auto FLevelViewportEditModeRegistry::Get() -> FLevelViewportEditModeRegistry&
	{
		static FLevelViewportEditModeRegistry Registry;
		return Registry;
	}

	auto FLevelViewportEditModeRegistry::Register(FLevelViewportEditModeDescriptor Descriptor) -> FLevelViewportEditModeHandle
	{
		if (Descriptor.Id.empty() || Descriptor.DisplayName.empty() || !Descriptor.Factory || Find(Descriptor.Id)) return {};
		const uint64 Id = NextHandleId++;
		Entries.push_back({Id, std::move(Descriptor)});
		return {Id};
	}

	auto FLevelViewportEditModeRegistry::Unregister(FLevelViewportEditModeHandle Handle) -> bool
	{
		const auto It = std::ranges::find_if(Entries, [Handle](const FEntry& Entry) { return Entry.HandleId == Handle.Id; });
		if (It == Entries.end()) return false;
		Entries.erase(It);
		return true;
	}

	auto FLevelViewportEditModeRegistry::Find(std::string_view Id) const -> const FLevelViewportEditModeDescriptor*
	{
		const auto It = std::ranges::find_if(Entries, [Id](const FEntry& Entry) { return Entry.Descriptor.Id == Id; });
		return It == Entries.end() ? nullptr : &It->Descriptor;
	}

	auto FLevelViewportEditModeRegistry::GetAvailable(const FLevelEditorContext& Context) const -> std::vector<const FLevelViewportEditModeDescriptor*>
	{
		std::vector<const FLevelViewportEditModeDescriptor*> Result;
		for (const FEntry& Entry : Entries)
			if (!Entry.Descriptor.CanActivate || Entry.Descriptor.CanActivate(Context)) Result.push_back(&Entry.Descriptor);
		std::ranges::sort(Result, [](const auto* A, const auto* B) { return A->Priority > B->Priority; });
		return Result;
	}

	FLevelViewportEditModeManager::FLevelViewportEditModeManager()
	{
		EnsureSelectModeRegistered();
	}

	FLevelViewportEditModeManager::~FLevelViewportEditModeManager()
	{
		Shutdown();
	}

	auto FLevelViewportEditModeManager::Activate(std::string_view Id, FLevelEditorContext& Context) -> bool
	{
		const FLevelViewportEditModeDescriptor* Descriptor = FLevelViewportEditModeRegistry::Get().Find(Id);
		if (!Descriptor || (Descriptor->CanActivate && !Descriptor->CanActivate(Context))) return false;
		if (ActiveId == Id && ActiveMode) return true;
		if (ActiveMode) ActiveMode->Exit(Context, false);
		ActiveMode = Descriptor->Factory();
		if (!ActiveMode) { ActiveId.clear(); return false; }
		ActiveId = Descriptor->Id;
		LastContext = &Context;
		ActiveMode->Enter(Context);
		return true;
	}

	auto FLevelViewportEditModeManager::Synchronize(FLevelEditorContext& Context) -> void
	{
		LastContext = &Context;
		const FLevelViewportEditModeDescriptor* Active = FLevelViewportEditModeRegistry::Get().Find(ActiveId);
		if (ActiveMode && (!Active || (Active->CanActivate && !Active->CanActivate(Context))))
		{
			ActiveMode->Exit(Context, true);
			ActiveMode.reset();
			ActiveId.clear();
		}
		if (!ActiveMode) Activate("Select", Context);
	}

	auto FLevelViewportEditModeManager::Tick(FLevelEditorContext& Context, FLevelEditorViewportClient& Client, const FSceneView& View,
		FLevelEditorViewportInput& Input, FEditorTransactionManager* Transactions) -> bool
	{
		Synchronize(Context);
		return ActiveMode && ActiveMode->Tick(Context, Client, View, Input, Transactions);
	}

	auto FLevelViewportEditModeManager::Shutdown(FLevelEditorContext* Context) -> void
	{
		if (ActiveMode && (Context || LastContext)) ActiveMode->Exit(Context ? *Context : *LastContext, true);
		ActiveMode.reset();
		ActiveId.clear();
		LastContext = nullptr;
	}
} // namespace Durin
