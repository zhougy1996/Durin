#include "LevelEditorViewportEditing.h"

#include "Editor/Transaction.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/TransformGizmo.h"
#include "Workspace/LevelEditorContext.h"

namespace Durin::Editor::Level
{
	namespace
	{
		class FSelectViewportEditMode final : public ILevelViewportEditMode
		{
		public:
			auto Exit(FLevelEditorContext&, bool) -> void override
			{
				if (LastClient && PendingTicket)
				{
					LastClient->CancelViewportPick(PendingTicket);
					LastClient->ReleaseViewportPick(PendingTicket);
				}
				PendingTicket = {};
				LastClient = nullptr;
			}

			auto Tick(FLevelEditorContext& Context, FLevelEditorViewportClient& Client, const FSceneView& View,
				FLevelEditorViewportInput& Input, ::Durin::DTransactor* Transactions) -> bool override
			{
				LastClient = &Client;
				const FTransformGizmoTargetSet Targets = GetGizmoTargets(Context);
				Client.GetTransformGizmo().Update(Targets, View, Input, Transactions);
				if (Client.GetTransformGizmo().IsHovered() || Client.GetTransformGizmo().IsDragging())
				{
					if (Input.bRequestSelection) CancelPending(Client);
					return true;
				}
				if (Input.bRequestSelection)
				{
					CancelPending(Client);
					const FViewportPickSubmission Submission = Client.SubmitViewportPick(Context.Level, View, Input.MousePosition);
					if (Submission.Completion.Status == EViewportPickStatus::Pending)
					{
						PendingTicket = Submission.Ticket;
						bPendingCtrl = Input.bCtrl;
						return true;
					}
					ApplyCompletion(Context, Submission.Completion, Input.bCtrl);
					Client.ReleaseViewportPick(Submission.Ticket);
					return true;
				}
				if (!PendingTicket) return false;
				const FViewportPickCompletion Completion = Client.PollViewportPick(PendingTicket);
				if (Completion.Status == EViewportPickStatus::Pending) return false;
				const bool bApplied = ApplyCompletion(Context, Completion, bPendingCtrl);
				Client.ReleaseViewportPick(PendingTicket);
				PendingTicket = {};
				return bApplied;
			}

			auto GetGizmoTargets(const FLevelEditorContext& Context) const -> FTransformGizmoTargetSet override
			{
				return MakeActorTransformGizmoTargets(Context);
			}

		private:
			auto CancelPending(FLevelEditorViewportClient& Client) -> void
			{
				if (!PendingTicket) return;
				Client.CancelViewportPick(PendingTicket);
				Client.ReleaseViewportPick(PendingTicket);
				PendingTicket = {};
			}

			static auto ApplyCompletion(FLevelEditorContext& Context, const FViewportPickCompletion& Completion, bool bCtrl) -> bool
			{
				if (Completion.Status != EViewportPickStatus::Completed) return false;
				if (!Completion.Hit)
				{
					Context.ClearSelection();
					return true;
				}
				AActor* Actor = Completion.Hit->Actor.Get();
				DActorComponent* Component = Completion.Hit->Component.Get();
				if (!Actor || !Component) return false;
				if (Completion.Hit->Kind == EViewportPickHitKind::SceneGeometry)
				{
					if (bCtrl) Context.ToggleActorSelection(Actor);
					else Context.SelectActor(Actor);
					return true;
				}
				if (!Context.IsActorSelected(Actor)) Context.SelectActor(Actor);
				if (Completion.Hit->Element.IsValid()) Context.SelectSubElement(Component, Completion.Hit->Element);
				else Context.SelectComponent(Component);
				return true;
			}

			FLevelEditorViewportClient* LastClient = nullptr;
			FViewportPickTicket PendingTicket;
			bool bPendingCtrl = false;
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

	auto FLevelViewportEditModeRegistry::Register(
		FLevelViewportEditModeDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate) -> FLevelViewportEditModeHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (OwnerGate.IsValid() && !Call) return {};
		if (Descriptor.Id.empty() || Descriptor.DisplayName.empty() || !Descriptor.Factory || Find(Descriptor.Id)) return {};
		FModuleOwnedResourceLease Resource;
		if (OwnerGate.IsValid())
		{
			Resource = OwnerGate.RetainResource();
			if (!Resource) return {};
		}
		const uint64 Id = NextHandleId++;
		Entries.push_back({Id, std::move(Resource), std::move(OwnerGate), std::move(Descriptor)});
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
		const FEntry* Entry = FindEntry(Id);
		if (!Entry) return nullptr;
		auto Call = Entry->OwnerGate.TryEnter();
		return !Entry->OwnerGate.IsValid() || Call ? &Entry->Descriptor : nullptr;
	}

	auto FLevelViewportEditModeRegistry::FindEntry(std::string_view Id) const
		-> const FEntry*
	{
		const auto It = std::ranges::find_if(Entries,
			[Id](const FEntry& Entry) { return Entry.Descriptor.Id == Id; });
		return It == Entries.end() ? nullptr : &*It;
	}

	auto FLevelViewportEditModeRegistry::GetAvailable(const FLevelEditorContext& Context) const -> std::vector<const FLevelViewportEditModeDescriptor*>
	{
		std::vector<const FLevelViewportEditModeDescriptor*> Result;
		for (const FEntry& Entry : Entries)
		{
			auto Call = Entry.OwnerGate.TryEnter();
			if (Entry.OwnerGate.IsValid() && !Call) continue;
			if (!Entry.Descriptor.CanActivate || Entry.Descriptor.CanActivate(Context))
				Result.push_back(&Entry.Descriptor);
		}
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
		const FLevelViewportEditModeRegistry::FEntry* Entry =
			FLevelViewportEditModeRegistry::Get().FindEntry(Id);
		if (!Entry) return false;
		auto Call = Entry->OwnerGate.TryEnter();
		if (Entry->OwnerGate.IsValid() && !Call) return false;
		const FLevelViewportEditModeDescriptor* Descriptor = &Entry->Descriptor;
		if (Descriptor->CanActivate && !Descriptor->CanActivate(Context)) return false;
		if (ActiveId == Id && ActiveMode) return true;
		if (ActiveMode)
		{
			auto ActiveCall = ActiveOwnerGate.TryEnter();
			if (!ActiveOwnerGate.IsValid() || ActiveCall) ActiveMode->Exit(Context, false);
		}
		FModuleOwnedResourceLease Resource;
		if (Entry->OwnerGate.IsValid())
		{
			Resource = Entry->OwnerGate.RetainResource();
			if (!Resource) return false;
		}
		ActiveMode = Descriptor->Factory();
		if (!ActiveMode) { ActiveId.clear(); return false; }
		ActiveOwnerResource = std::move(Resource);
		ActiveOwnerGate = Entry->OwnerGate;
		ActiveId = Descriptor->Id;
		LastContext = &Context;
		ActiveMode->Enter(Context);
		return true;
	}

	auto FLevelViewportEditModeManager::Synchronize(FLevelEditorContext& Context) -> void
	{
		LastContext = &Context;
		const FLevelViewportEditModeRegistry::FEntry* Active =
			FLevelViewportEditModeRegistry::Get().FindEntry(ActiveId);
		auto ActiveCall = ActiveOwnerGate.TryEnter();
		if (ActiveMode && (!Active || (ActiveOwnerGate.IsValid() && !ActiveCall)
			|| (Active->Descriptor.CanActivate && !Active->Descriptor.CanActivate(Context))))
		{
			if (!ActiveOwnerGate.IsValid() || ActiveCall)
				ActiveMode->Exit(Context, true);
			ActiveMode.reset();
			ActiveOwnerResource = {};
			ActiveOwnerGate = {};
			ActiveId.clear();
		}
		if (!ActiveMode) Activate("Select", Context);
	}

	auto FLevelViewportEditModeManager::Tick(FLevelEditorContext& Context, FLevelEditorViewportClient& Client, const FSceneView& View,
		FLevelEditorViewportInput& Input, ::Durin::DTransactor* Transactions) -> bool
	{
		Synchronize(Context);
		if (!ActiveMode) return false;
		auto Call = ActiveOwnerGate.TryEnter();
		if (ActiveOwnerGate.IsValid() && !Call) return false;
		const bool bHandled = ActiveMode->Tick(Context, Client, View, Input, Transactions);
		if (ActiveMode && ActiveMode->ShouldExit()) Activate("Select", Context);
		return bHandled;
	}

	auto FLevelViewportEditModeManager::Shutdown(FLevelEditorContext* Context) -> void
	{
		if (ActiveMode && (Context || LastContext)) ActiveMode->Exit(Context ? *Context : *LastContext, true);
		ActiveMode.reset();
		ActiveOwnerResource = {};
		ActiveOwnerGate = {};
		ActiveId.clear();
		LastContext = nullptr;
	}
} // namespace Durin::Editor::Level
