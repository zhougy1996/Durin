#include "Panels/WorldOutlinerPanel.h"

#include "Actors/CameraActor.h"
#include "Actors/DirectionalLightActor.h"
#include "Actors/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorTransaction.h"
#include "Icons/FontAwesomeIcons.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorHelpers.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "StaticMeshLevelAuthoring.h"

namespace Durin
{
	namespace
	{
		using LevelEditorHelpers::ClassDisplayName;
		using StringUtils::ContainsInsensitive;

		constexpr auto ActorPayloadType = "DURIN_OUTLINER_ACTOR";

		auto MakeUniqueActorName(DLevel& Level, FName Requested, const AActor* Ignored = nullptr) -> FName
		{
			if (AActor* Existing = Level.FindActorByName(Requested); !Existing || Existing == Ignored) return Requested;
			const std::string Base = Requested.ToString();
			for (uint32 Suffix = 2;; ++Suffix)
			{
				FName Candidate(std::format("{}_{}", Base, Suffix));
				if (AActor* Existing = Level.FindActorByName(Candidate); !Existing || Existing == Ignored) return Candidate;
			}
		}

		auto ExecuteStaticMeshRequest(FLevelEditorContext& Context,
			FStaticMeshLevelMutationRequest Request) -> FStaticMeshLevelMutationResult
		{
			Request.bReadOnly = Context.bReadOnly;
			const FStaticMeshLevelMutationPlan Plan = FStaticMeshLevelAuthoringService::Plan(Request);
			return FStaticMeshLevelAuthoringService::Execute(Plan, {
				.OpenLevel = Context.Level,
				.Transactions = GEditor ? &GEditor->GetTransactionManager() : nullptr,
				.bReadOnly = Context.bReadOnly,
			});
		}

		// Restores the level's primary-camera selection.
		class FPrimaryCameraTransaction final : public IEditorTransaction
		{
		public:
			FPrimaryCameraTransaction(DLevel* InLevel, ACameraActor* InBefore, ACameraActor* InAfter)
				: Level(InLevel), Before(InBefore), After(InAfter)
			{
				AffectedPackages.front() = InLevel ? InLevel->GetPackage() : nullptr;
			}
			auto GetDescription() const -> std::string_view override { return "Set primary camera"; }
			auto GetDetails(EEditorTransactionOperation) const -> std::string override
			{
				return After ? std::format("Set '{}' as the primary camera", After->GetName()) : "Clear the primary camera";
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Undo() -> bool override { return Level && Level->SetPrimaryCameraActor(Before.Get()); }
			auto Redo() -> bool override { return Level && Level->SetPrimaryCameraActor(After.Get()); }

		private:
			TObjectPtr<DLevel> Level;
			TObjectPtr<ACameraActor> Before;
			TObjectPtr<ACameraActor> After;
			std::array<DPackage*, 1> AffectedPackages{};
		};

		// Restores visibility for every actor changed by one outliner operation.
		class FActorVisibilityTransaction final : public IEditorTransaction
		{
		public:
			// Stores one actor's visibility before and after the outliner action.
			struct FEntry
			{
				TObjectPtr<AActor> Actor;
				bool bBefore = false;
				bool bAfter = false;
			};

			FActorVisibilityTransaction(std::vector<FEntry> InEntries, bool bInShow)
				: Entries(std::move(InEntries)), bShow(bInShow)
			{
				for (const FEntry& Entry : Entries)
				{
					DPackage* Package = Entry.Actor ? Entry.Actor->GetPackage() : nullptr;
					if (Package && std::ranges::find(AffectedPackages, Package) == AffectedPackages.end())
						AffectedPackages.push_back(Package);
				}
			}
			auto GetDescription() const -> std::string_view override { return bShow ? "Show actors" : "Hide actors"; }
			auto GetDetails(EEditorTransactionOperation Operation) const -> std::string override
			{
				const bool bApplyingAfter = Operation != EEditorTransactionOperation::Undo;
				const size_t HiddenCount = std::ranges::count_if(Entries, [bApplyingAfter](const FEntry& Entry) { return bApplyingAfter ? Entry.bAfter : Entry.bBefore; });
				return std::format("Set visibility for {} actor(s); {} hidden", Entries.size(), HiddenCount);
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Undo() -> bool override { return Apply(false); }
			auto Redo() -> bool override { return Apply(true); }

		private:
			auto Apply(bool bAfter) -> bool
			{
				bool bSuccess = true;
				for (const FEntry& Entry : Entries)
				{
					if (!Entry.Actor)
					{
						bSuccess = false;
						continue;
					}
					Entry.Actor->SetHidden(bAfter ? Entry.bAfter : Entry.bBefore);
				}
				return bSuccess;
			}

			std::vector<FEntry> Entries;
			std::vector<DPackage*> AffectedPackages;
			bool bShow = false;
		};

		auto LevelDisplayName(const DLevel* Level) -> std::string
		{
			if (!Level) return "No Level";
			if (const DPackage* Package = Level->GetPackage())
			{
				FAssetPath Path;
				if (FAssetPath::TryCreate(Package->GetPackagePath(), Path)) return std::string(Path.GetAssetName());
			}
			return Level->GetName().empty() ? "Transient Level" : Level->GetName();
		}

		auto ActorIcon(AActor* Actor) -> const char*
		{
			if (Actor->IsA<ACameraActor>()) return Icons::Camera;
			if (Actor->IsA<ADirectionalLightActor>()) return Icons::Lightbulb;
			if (Actor->FindComponentByClass<DStaticMeshComponent>()) return Icons::Cube;
			return Icons::Circle;
		}

	} // namespace

	auto FWorldOutlinerPanel::ResetHierarchyCache() -> void
	{
		HierarchyModel.Reset();
		VisibleActors.clear();
		LastVisibleActors.clear();
		ExpandedActors.clear();
	}

	auto FWorldOutlinerPanel::RebuildHierarchyCache(DLevel* Level) -> void
	{
		VisibleActors.clear();
		LastVisibleActors.clear();
		if (!Level)
		{
			HierarchyModel.Reset();
			ExpandedActors.clear();
			return;
		}

		std::vector<FWorldOutlinerHierarchyModel::FInput> Inputs;
		Inputs.reserve(Level->GetActors().size());
		for (const TObjectPtr<AActor>& ActorPtr : Level->GetActors())
		{
			if (AActor* Actor = ActorPtr.Get())
			{
				Inputs.push_back({
					.Key = Actor,
					.ParentKey = Actor->GetAttachParentActor(),
					.Name = Actor->GetName(),
					.TypeLabel = ClassDisplayName(Actor->GetClass())});
			}
		}
		HierarchyModel.Rebuild(Level, Level->GetEditorActorHierarchyRevision(), Inputs);
		std::erase_if(ExpandedActors, [this](const auto& Entry) { return !HierarchyModel.Contains(Entry.first); });
	}

	auto FWorldOutlinerPanel::RebuildFilterCache(std::string_view Filter) -> void
	{
		HierarchyModel.SetFilter(Filter);
	}

	auto FWorldOutlinerPanel::IsNodeVisible(uint32 NodeIndex) const -> bool
	{
		return HierarchyModel.IsNodeVisible(NodeIndex);
	}

	auto FWorldOutlinerPanel::IsDescendantOf(const AActor* Actor, const AActor* CandidateAncestor) const -> bool
	{
		return HierarchyModel.IsDescendantOf(Actor, CandidateAncestor);
	}

	auto FWorldOutlinerPanel::GetActorDepth(const AActor* Actor) const -> uint32
	{
		return HierarchyModel.GetDepth(Actor);
	}

	auto FWorldOutlinerPanel::SetActorVisibility(const std::vector<TObjectPtr<AActor>>& TargetActors, bool bHidden) -> void
	{
		std::vector<FActorVisibilityTransaction::FEntry> Entries;
		for (const TObjectPtr<AActor>& Actor : TargetActors)
		{
			if (Actor && Actor->IsHidden() != bHidden) Entries.push_back({Actor, Actor->IsHidden(), bHidden});
		}
		if (Entries.empty()) return;
		auto Transaction = std::make_unique<FActorVisibilityTransaction>(std::move(Entries), !bHidden);
		if (GEditor) GEditor->GetTransactionManager().Execute(std::move(Transaction));
		else Transaction->Redo();
	}

	auto FWorldOutlinerPanel::ShowAllActors() -> void
	{
		std::vector<TObjectPtr<AActor>> AllActors;
		AllActors.reserve(HierarchyModel.GetNodes().size());
		for (const FWorldOutlinerHierarchyModel::FNode& Node : HierarchyModel.GetNodes())
			AllActors.push_back(static_cast<AActor*>(Node.Key));
		SetActorVisibility(AllActors, false);
	}

	auto FWorldOutlinerPanel::AreAllActorsHidden(const std::vector<TObjectPtr<AActor>>& Actors) const -> bool
	{
		return std::ranges::all_of(Actors, [](const TObjectPtr<AActor>& Actor) { return Actor && Actor->IsHidden(); });
	}

	auto FWorldOutlinerPanel::HasSelectedAncestor(const std::vector<TObjectPtr<AActor>>& Actors, AActor* Candidate) const -> bool
	{
		return std::ranges::any_of(Actors, [this, Candidate](const TObjectPtr<AActor>& Other) {
			return Other.Get() != Candidate && IsDescendantOf(Candidate, Other.Get());
		});
	}

	auto FWorldOutlinerPanel::BeginActorRename(AActor* Actor) -> void
	{
		if (!Actor) return;
		RenamingActor = Actor;
		bRenamingLevel = false;
		RenameDialog.Open(Actor->GetName());
	}

	auto FWorldOutlinerPanel::BeginLevelRename(std::string_view LevelName) -> void
	{
		RenamingActor = nullptr;
		bRenamingLevel = true;
		RenameDialog.Open(LevelName);
	}

	auto FWorldOutlinerPanel::DrawActorContextMenu(FLevelEditorContext& Context, AActor* Actor, bool bPrimaryCamera, bool& bRequestDelete) -> void
	{
		if (!ImGui::BeginPopupContextItem("ActorContext")) return;
		bLevelSelected = false;
		if (!Context.IsActorSelected(Actor)) Context.SelectActor(Actor);
		if (ImGui::MenuItem("Focus", "F") && Context.FocusActor) Context.FocusActor(Actor);
		if (Context.bReadOnly)
		{
			ImGui::Separator();
			ImGui::TextDisabled("Runtime actor");
		}
		else
		{
			ImGui::Separator();
			const bool bAllSelectedHidden = AreAllActorsHidden(Context.GetSelectedActors());
			if (ImGui::MenuItem(bAllSelectedHidden ? "Show Selected" : "Hide Selected", "H")) SetActorVisibility(Context.GetSelectedActors(), !bAllSelectedHidden);
			if (ImGui::MenuItem("Show All Actors", "Ctrl+H")) ShowAllActors();
			ImGui::Separator();
			if (auto* Camera = Cast<ACameraActor>(Actor))
			{
				if (bPrimaryCamera)
				{
					ImGui::BeginDisabled();
					ImGui::MenuItem("Primary Camera", nullptr, true);
					ImGui::EndDisabled();
				}
				else if (ImGui::MenuItem("Set as Primary Camera"))
				{
					auto Transaction = std::make_unique<FPrimaryCameraTransaction>(Context.Level, Context.Level->GetPrimaryCameraActor(), Camera);
					if (GEditor) GEditor->GetTransactionManager().Execute(std::move(Transaction));
					else Context.Level->SetPrimaryCameraActor(Camera);
				}
			}
			if (ImGui::MenuItem("Rename", "F2")) BeginActorRename(Actor);
			if (ImGui::MenuItem("Delete", "Del")) bRequestDelete = true;
		}
		ImGui::EndPopup();
	}

	auto FWorldOutlinerPanel::DrawActorDragDrop(FLevelEditorContext& Context, AActor* Actor) -> void
	{
		if (!Context.bReadOnly && ImGui::BeginDragDropSource())
		{
			if (!Context.IsActorSelected(Actor)) Context.SelectActor(Actor);
			AActor* PayloadActor = Actor;
			ImGui::SetDragDropPayload(ActorPayloadType, &PayloadActor, sizeof(PayloadActor));
			ImGui::Text("Move %zu actor(s)", Context.GetSelectedActors().size());
			ImGui::EndDragDropSource();
		}
		if (!Context.bReadOnly && ImGui::BeginDragDropTarget())
		{
			if (ImGui::AcceptDragDropPayload(ActorPayloadType))
			{
				std::vector<AActor*> MoveActors;
				for (const TObjectPtr<AActor>& Selected : Context.GetSelectedActors())
				{
					AActor* Candidate = Selected.Get();
					if (!Candidate || Candidate == Actor || IsDescendantOf(Actor, Candidate)) continue;
					if (HasSelectedAncestor(Context.GetSelectedActors(), Candidate)) continue;
					MoveActors.push_back(Candidate);
				}
				for (AActor* Moving : MoveActors)
					if (!Moving->AttachToActor(Actor, EAttachmentTransformRule::KeepWorld))
						Context.SetError(std::format("Failed to attach '{}' to '{}'.", Moving->GetName(), Actor->GetName()));
					else
						Context.InvalidatePackageSavedState(Moving->GetPackage());
			}
			ImGui::EndDragDropTarget();
		}
	}

	auto FWorldOutlinerPanel::DrawActorNode(FLevelEditorContext& Context, uint32 NodeIndex, std::string_view Filter, bool bRestoreExpansion, bool& bRequestDelete) -> void
	{
		if (!IsNodeVisible(NodeIndex)) return;
		const FWorldOutlinerHierarchyModel::FNode& Node = HierarchyModel.GetNodes()[NodeIndex];
		AActor* Actor = static_cast<AActor*>(Node.Key);
		if (!Actor) return;
		VisibleActors.push_back(Actor);
		const bool bHasVisibleChildren = std::ranges::any_of(Node.Children, [this](uint32 ChildIndex) { return IsNodeVisible(ChildIndex); });
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
		if (!bHasVisibleChildren) Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		if (Context.IsActorSelected(Actor)) Flags |= ImGuiTreeNodeFlags_Selected;
		if (!Filter.empty() || ExpandRequest != 0)
			ImGui::SetNextItemOpen(!Filter.empty() || ExpandRequest > 0, ImGuiCond_Always);
		else if (bRestoreExpansion)
		{
			if (const auto It = ExpandedActors.find(Actor); It != ExpandedActors.end()) ImGui::SetNextItemOpen(It->second, ImGuiCond_Always);
		}
		ImGui::PushID(Actor);
		const bool bPrimaryCamera = Context.Level->GetPrimaryCameraActor() == Actor;
		const std::string VisibilityIcon = Actor->IsHidden() ? std::format("{}  ", Icons::EyeSlash) : std::string();
		const std::string Label = bPrimaryCamera ? std::format("{}{}  {}  [Primary]", VisibilityIcon, ActorIcon(Actor), Actor->GetName()) : std::format("{}{}  {}", VisibilityIcon, ActorIcon(Actor), Actor->GetName());
		if (Actor->IsHidden()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		const bool bOpen = MonaImGui::CompactTreeNode("ActorNode", Flags, "%s", Label.c_str());
		if (Actor->IsHidden()) ImGui::PopStyleColor();
		if (Filter.empty()) ExpandedActors[Actor] = bOpen;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Actor->GetClass()->GetName().c_str());
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
		{
			bLevelSelected = false;
			const ImGuiIO& IO = ImGui::GetIO();
			if (IO.KeyShift)
				Context.SelectActorRange(Actor, LastVisibleActors.empty() ? VisibleActors : LastVisibleActors);
			else if (IO.KeyCtrl)
				Context.ToggleActorSelection(Actor);
			else
				Context.SelectActor(Actor);
		}
		DrawActorContextMenu(Context, Actor, bPrimaryCamera, bRequestDelete);
		DrawActorDragDrop(Context, Actor);
		if (bOpen && bHasVisibleChildren)
		{
			for (uint32 ChildIndex : Node.Children) DrawActorNode(Context, ChildIndex, Filter, bRestoreExpansion, bRequestDelete);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}

	auto FWorldOutlinerPanel::DrawLevelDragDrop(FLevelEditorContext& Context) -> void
	{
		if (Context.bReadOnly || !ImGui::BeginDragDropTarget()) return;
		if (ImGui::AcceptDragDropPayload(ActorPayloadType))
		{
			for (const TObjectPtr<AActor>& Selected : Context.GetSelectedActors())
			{
				AActor* Actor = Selected.Get();
				if (!Actor || !Actor->GetAttachParentActor()) continue;
				if (HasSelectedAncestor(Context.GetSelectedActors(), Actor)) continue;
				if (!Actor->DetachFromActor(EDetachmentTransformRule::KeepWorld))
					Context.SetError(std::format("Failed to detach '{}'.", Actor->GetName()));
				else
					Context.InvalidatePackageSavedState(Actor->GetPackage());
			}
		}
		ImGui::EndDragDropTarget();
	}

	auto FWorldOutlinerPanel::DrawLevelNode(FLevelEditorContext& Context, std::string_view LevelName, std::string_view Filter, bool bRestoreExpansion, bool& bRequestDelete) -> void
	{
		if (!Filter.empty() || ExpandRequest != 0)
			ImGui::SetNextItemOpen(!Filter.empty() || ExpandRequest > 0, ImGuiCond_Always);
		else
			ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		ImGui::PushID(Context.Level);
		const std::string LevelLabel = std::format("{}  {}", Icons::FolderOpen, LevelName);
		ImGuiTreeNodeFlags LevelFlags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
		if (bLevelSelected) LevelFlags |= ImGuiTreeNodeFlags_Selected;
		const bool bLevelOpen = MonaImGui::CompactTreeNode("LevelNode", LevelFlags, "%s", LevelLabel.c_str());
		if (ImGui::IsItemHovered() && Context.Level->GetPackage()) ImGui::SetTooltip("%s", Context.Level->GetPackage()->GetPackagePath().c_str());
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
		{
			Context.ClearSelection();
			bLevelSelected = true;
		}
		if (ImGui::BeginPopupContextItem("LevelContext"))
		{
			Context.ClearSelection();
			bLevelSelected = true;
			if (Context.bReadOnly) ImGui::BeginDisabled();
			if (ImGui::IsWindowAppearing()) ActorTypeSearchText.fill(0);
			if (ImGui::BeginMenu("Add Actors"))
			{
				ImGui::SetNextItemWidth(std::min(MonaImGui::ScaleUI(240.0f), ImGui::GetContentRegionAvail().x));
				ImGui::InputTextWithHint("###ActorTypeSearch", "Search actor types...", ActorTypeSearchText.data(), ActorTypeSearchText.size());
				for (DClass* Class : GetDerivedClasses(AActor::StaticClass(), true))
				{
					if (!CanConstructObjectOfClass(Class, AActor::StaticClass())) continue;
					const std::string DisplayName = ClassDisplayName(Class);
					if (!ContainsInsensitive(DisplayName, ActorTypeSearchText.data())) continue;
					if (ImGui::MenuItem(DisplayName.c_str()))
					{
						AActor* Actor = nullptr;
						if (Class == AStaticMeshActor::StaticClass())
						{
							auto Request = FStaticMeshLevelAuthoringService::CaptureTarget(*Context.Level);
							Request.Description = "Create static mesh actor";
							const FName Name = MakeUniqueActorName(*Context.Level, FName(Class->GetDefaultObjectName()));
							Request.Mutations.push_back({.Kind = EStaticMeshLevelMutationKind::Create, .TargetName = Name});
							const FStaticMeshLevelMutationResult Result = ExecuteStaticMeshRequest(Context, std::move(Request));
							if (!Result)
							{
								Context.SetError(Result.Diagnostic.Message);
								continue;
							}
							else if (!Result.ResultActorNames.empty()) Actor = Context.Level->FindActorByName(Result.ResultActorNames.front());
						}
						else Actor = Context.World->SpawnActor(Class, FName(Class->GetDefaultObjectName()));
						if (Actor)
						{
							if (Class != AStaticMeshActor::StaticClass()) Context.InvalidatePackageSavedState(Actor->GetPackage());
							Context.SelectActor(Actor);
							bLevelSelected = false;
						}
						else
							Context.SetError(std::format("Failed to create actor of class {}.", Class->GetQualifiedName().ToString()));
					}
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Show All Actors", "Ctrl+H")) ShowAllActors();
			ImGui::Separator();
			if (ImGui::MenuItem("Rename", "F2")) BeginLevelRename(LevelName);
			if (Context.bReadOnly) ImGui::EndDisabled();
			ImGui::EndPopup();
		}
		DrawLevelDragDrop(Context);
		if (bLevelOpen)
		{
			for (uint32 RootIndex : HierarchyModel.GetRootNodeIndices()) DrawActorNode(Context, RootIndex, Filter, bRestoreExpansion, bRequestDelete);
			if (VisibleActors.empty()) ImGui::TextDisabled(Filter.empty() ? "No actors in this level." : "No actors match '%s'.", SearchText.data());
			ImGui::TreePop();
		}
		ImGui::PopID();
	}

	auto FWorldOutlinerPanel::DrawShortcuts(FLevelEditorContext& Context, std::string_view LevelName, bool& bRequestDelete) -> void
	{
		const ImGuiIO& IO = ImGui::GetIO();
		const bool bOutlinerFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (bOutlinerFocused && !IO.WantTextInput && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) Context.SetSelectedActors(VisibleActors);
		if (bOutlinerFocused && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false))
		{
			if (AActor* Actor = Context.GetPrimarySelectedActor(); Actor && Context.FocusActor) Context.FocusActor(Actor);
		}
		if (!Context.bReadOnly && bOutlinerFocused && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2, false))
		{
			if (AActor* Actor = Context.GetPrimarySelectedActor())
				BeginActorRename(Actor);
			else if (bLevelSelected)
				BeginLevelRename(LevelName);
		}
		if (!Context.bReadOnly && bOutlinerFocused && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_H, false))
		{
			if (IO.KeyCtrl)
				ShowAllActors();
			else if (!Context.GetSelectedActors().empty())
				SetActorVisibility(Context.GetSelectedActors(), !AreAllActorsHidden(Context.GetSelectedActors()));
		}
		if (!Context.bReadOnly && !Context.GetSelectedActors().empty() && bOutlinerFocused && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
			bRequestDelete = true;
	}

	auto FWorldOutlinerPanel::DrawRenameDialog(FLevelEditorContext& Context, std::string_view LevelName) -> void
	{
		const bool bRenameActor = RenamingActor.IsValid();
		const char* RenamePopupTitle = bRenameActor ? "Rename Actor###OutlinerRename" : "Rename Level###OutlinerRename";
		const std::string CurrentRenameName = bRenameActor ? RenamingActor->GetName() : std::string(LevelName);
		const EEditorRenameDialogResult RenameResult = RenameDialog.Draw(RenamePopupTitle, CurrentRenameName, [&](std::string_view NewName) -> std::string {
			if (bRenameActor)
			{
				if (auto* StaticMeshActor = Cast<AStaticMeshActor>(RenamingActor.Get());
					StaticMeshActor && FStaticMeshLevelAuthoringService::IsSupportedActor(*StaticMeshActor))
				{
					auto Request = FStaticMeshLevelAuthoringService::CaptureTarget(*Context.Level);
					Request.Description = "Rename static mesh actor";
					Request.Mutations.push_back({
						.Kind = EStaticMeshLevelMutationKind::Rename,
						.TargetName = StaticMeshActor->GetFName(),
						.Desired = {.Name = MakeUniqueActorName(*Context.Level, FName(NewName), StaticMeshActor)},
					});
					const FStaticMeshLevelMutationResult Result = ExecuteStaticMeshRequest(Context, std::move(Request));
					if (!Result) return Result.Diagnostic.Message;
					if (!Result.ResultActorNames.empty()) Context.SelectActor(Context.Level->FindActorByName(Result.ResultActorNames.front()));
				}
				else
				{
					if (!Context.Level->RenameActor(RenamingActor.Get(), FName(NewName))) return "Failed to rename actor.";
					Context.InvalidatePackageSavedState(RenamingActor->GetPackage());
				}
				return {};
			}
			return Context.RenameLevel && Context.RenameLevel(NewName) ? std::string() : "Failed to rename level.";
		});
		if (RenameResult != EEditorRenameDialogResult::None)
		{
			RenamingActor = nullptr;
			bRenamingLevel = false;
		}
	}

	auto FWorldOutlinerPanel::DrawDeletePopup(FLevelEditorContext& Context) -> void
	{
		if (ImGui::BeginPopupModal("Delete Actors?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			const bool bAllSupportedStaticMeshes = !PendingDeleteActors.empty()
				&& std::ranges::all_of(PendingDeleteActors, [](const TObjectPtr<AActor>& Actor) {
					auto* StaticMeshActor = Cast<AStaticMeshActor>(Actor.Get());
					return StaticMeshActor && FStaticMeshLevelAuthoringService::IsSupportedActor(*StaticMeshActor);
				});
			ImGui::Text("Delete %zu actor(s)?", PendingDeleteActors.size());
			for (size_t Index = 0; Index < std::min<size_t>(PendingDeleteActors.size(), 5); ++Index)
				if (PendingDeleteActors[Index]) ImGui::BulletText("%s", PendingDeleteActors[Index]->GetName().c_str());
			if (PendingDeleteActors.size() > 5) ImGui::TextDisabled("... and %zu more", PendingDeleteActors.size() - 5);
			ImGui::TextDisabled(bAllSupportedStaticMeshes ? "This action can be undone." : "This action cannot be undone.");
			if (ImGui::Button("Delete"))
			{
				std::ranges::sort(PendingDeleteActors, [this](const TObjectPtr<AActor>& Left, const TObjectPtr<AActor>& Right) { return GetActorDepth(Left.Get()) > GetActorDepth(Right.Get()); });
				bool bDestroyedAny = false;
				if (bAllSupportedStaticMeshes)
				{
					auto Request = FStaticMeshLevelAuthoringService::CaptureTarget(*Context.Level);
					Request.Description = PendingDeleteActors.size() == 1 ? "Delete static mesh actor" : "Delete static mesh actors";
					for (const TObjectPtr<AActor>& Actor : PendingDeleteActors)
						Request.Mutations.push_back({.Kind = EStaticMeshLevelMutationKind::Remove, .TargetName = Actor->GetFName()});
					const FStaticMeshLevelMutationResult Result = ExecuteStaticMeshRequest(Context, std::move(Request));
					if (!Result) Context.SetError(Result.Diagnostic.Message);
					else bDestroyedAny = Result.bChanged;
				}
				else
				{
					for (const TObjectPtr<AActor>& Actor : PendingDeleteActors)
					{
						if (!Actor) continue;
						if (!Context.World->DestroyActor(Actor.Get()))
							Context.SetError(std::format("Failed to delete '{}'.", Actor->GetName()));
						else bDestroyedAny = true;
					}
					if (bDestroyedAny) Context.InvalidatePackageSavedState();
				}
				Context.ClearSelection();
				PendingDeleteActors.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				PendingDeleteActors.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	auto FWorldOutlinerPanel::Draw(FLevelEditorContext& Context) -> void
	{
		if (!EditorWorkspaceUI::BeginDockablePanel(LevelEditorWorkspace::Type, "World Outliner", "WorldOutliner", GetOpenPtr()))
		{
			ImGui::End();
			return;
		}
		if (Context.bReadOnly)
		{
			ImGui::TextColored(MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning), "Runtime World (read-only)");
			ImGui::Separator();
		}

		if (ImGui::Button(Icons::Expand)) ExpandRequest = 1;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Expand all");
		ImGui::SameLine();
		if (ImGui::Button(Icons::Compress)) ExpandRequest = -1;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collapse all");

		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("###OutlinerSearch", "Search actors or types...", SearchText.data(), SearchText.size());
		ImGui::Separator();

		if (Context.Level == nullptr)
		{
			DisplayedLevel = nullptr;
			ResetHierarchyCache();
			bLevelSelected = false;
			bRenamingLevel = false;
			RenamingActor = nullptr;
			RenameDialog.Cancel();
			ImGui::TextDisabled("No level is open.");
			ImGui::End();
			return;
		}
		if (DisplayedLevel.Get() != Context.Level)
		{
			DisplayedLevel = Context.Level;
			bLevelSelected = false;
			bRenamingLevel = false;
			RenamingActor = nullptr;
			RenameDialog.Cancel();
		}

		if (HierarchyModel.NeedsRebuild(Context.Level, Context.Level->GetEditorActorHierarchyRevision()))
			RebuildHierarchyCache(Context.Level);
		const std::string_view Filter(SearchText.data());
		if (HierarchyModel.GetFilter() != Filter) RebuildFilterCache(Filter);
		const bool bRestoreExpansion = bWasSearching && Filter.empty();

		VisibleActors.clear();
		bool bRequestDelete = false;

		const std::string LevelName = LevelDisplayName(Context.Level);
		DrawLevelNode(Context, LevelName, Filter, bRestoreExpansion, bRequestDelete);
		ExpandRequest = 0;
		bWasSearching = !Filter.empty();

		ImGui::Spacing();
		ImGui::TextDisabled("%zu actors | %zu selected", HierarchyModel.GetNodes().size(), Context.GetSelectedActors().size());
		DrawShortcuts(Context, LevelName, bRequestDelete);
		DrawRenameDialog(Context, LevelName);
		if (bRequestDelete)
		{
			PendingDeleteActors = Context.GetSelectedActors();
			ImGui::OpenPopup("Delete Actors?");
		}
		DrawDeletePopup(Context);

		if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
		{
			Context.ClearSelection();
			bLevelSelected = false;
		}
		LastVisibleActors.swap(VisibleActors);
		ImGui::End();
	}
} // namespace Durin
