#include "Viewport/SelectViewportEditMode.h"

#include "Transactions/Transaction.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/TransformGizmo.h"
#include "LevelEditorTransformTargets.h"
#include "Workspace/LevelEditorContext.h"
#include "Actors/EditorActorSubsystem.h"

namespace Durin::LevelEditor
{
	namespace
	{
		class FSelectViewportEditMode final : public ILevelViewportEditMode
		{
		public:
			auto Exit(FLevelEditorContext& Context, bool) -> void override
			{
				if (LastClient) LastClient->GetTransformGizmo().CancelUpdate();
				if (LastClient && LastClient->GetTransformGizmo().IsDragging())
					throw std::runtime_error("Unable to restore the transform gesture.");
				LastClient = nullptr;
			}
			auto Tick(FLevelEditorContext& Context, FLevelEditorViewportClient& Client, const FSceneView& View,
				FLevelEditorViewportInput& Input, DTransactor* Transactions) -> bool override
			{
				if (!Context.bReadOnly && Context.Level && !Context.GetSelectedActors().empty() && (Input.bDuplicate || Input.bDelete))
				{
					std::vector<AActor*> Actors;
					for (const auto& Actor : Context.GetSelectedActors()) Actors.push_back(Actor.Get());
					const bool bDuplicate = Input.bDuplicate;
                    auto* Editor = Context.GetActorEditor();
                    if (!Editor) Context.SetError("Actor editing service is unavailable.");
                    else if (bDuplicate)
                    {
                        const auto Copies = Editor->DuplicateActors(*Context.Level, Actors, {Context.bReadOnly});
                        if (Copies.empty()) Context.SetError("Actor duplication failed.");
                        else
                        {
                            Actors.clear();
                            for (const auto& Actor : Copies) Actors.push_back(Actor.Get());
                            Context.SetSelectedActors(Actors);
                        }
                    }
                    else if (Editor->DestroyActors(*Context.Level, Actors, {Context.bReadOnly})) Context.SetSelectedActors({});
                    else Context.SetError("Actor deletion failed.");
					Input.bDuplicate = Input.bDelete = false;
					return true;
				}
				const FTransformGizmoTargetSet Targets = GetGizmoTargets(Context);
				if (!Client.GetTransformGizmo().Update(Targets, View, Input, Transactions))
					Context.SetError("Unable to apply or restore the transform gesture.");
				LastClient = Client.GetTransformGizmo().IsDragging() ? &Client : nullptr;
				if (Client.GetTransformGizmo().IsHovered() || Client.GetTransformGizmo().IsDragging())
				{
					return true;
				}
				if (!Input.bRequestSelection) return false;
				ApplyResult(Context, Client.PickViewport(Context.Level, View, Input.MousePosition), Input.bCtrl);
				return true;
			}

			auto GetGizmoTargets(const FLevelEditorContext& Context) const -> FTransformGizmoTargetSet override
			{
				return MakeActorTransformGizmoTargets(Context);
			}

		private:
			FLevelEditorViewportClient* LastClient = nullptr;
			static auto ApplyResult(FLevelEditorContext& Context, const FViewportPickResult& Result, bool bCtrl) -> bool
			{
				if (Result.Status != EViewportPickStatus::Completed) return false;
				if (!Result.Hit)
				{
					Context.ClearSelection();
					return true;
				}
				AActor* Actor = Result.Hit->Actor.Get();
				DActorComponent* Component = Result.Hit->Component.Get();
				if (!Actor || !Component) return false;
				if (Result.Hit->Kind == EViewportPickHitKind::SceneGeometry)
				{
					if (bCtrl) Context.ToggleActorSelection(Actor);
					else Context.SelectActor(Actor);
					return true;
				}
				if (!Context.IsActorSelected(Actor)) Context.SelectActor(Actor);
				if (Result.Hit->Element.IsValid()) Context.SelectSubElement(Component, Result.Hit->Element);
				else Context.SelectComponent(Component);
				return true;
			}

		};

	} // namespace

	auto RegisterSelectViewportEditMode() -> FLevelViewportEditModeHandle
	{
		return FLevelViewportEditModeRegistry::Get().Register({
			.Id = "Select", .DisplayName = "Select", .Priority = std::numeric_limits<int32>::min(),
			.CanActivate = [](const FLevelEditorContext&) { return true; },
			.Factory = [] { return std::make_unique<FSelectViewportEditMode>(); },
		});
	}
} // namespace Durin::LevelEditor
