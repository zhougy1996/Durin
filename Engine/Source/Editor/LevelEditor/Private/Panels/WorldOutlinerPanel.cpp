#include "Panels/WorldOutlinerPanel.h"

#include "Actors/CameraActor.h"
#include "Actors/DirectionalLightActor.h"
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
#include "LevelEditorContext.h"
#include "LevelEditorHelpers.h"
#include "LevelEditorWorkspace.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		using LevelEditorHelpers::ClassDisplayName;
		using StringUtils::ContainsInsensitive;

		constexpr auto ActorPayloadType = "DURIN_OUTLINER_ACTOR";

		class FPrimaryCameraTransaction final : public IEditorTransaction
		{
		public:
			FPrimaryCameraTransaction(DLevel* InLevel, ACameraActor* InBefore, ACameraActor* InAfter)
				: Level(InLevel), Before(InBefore), After(InAfter) {}
			auto GetDescription() const -> std::string_view override { return "Set primary camera"; }
			auto GetDetails(EEditorTransactionOperation) const -> std::string override
			{
				return After ? std::format("Set '{}' as the primary camera", After->GetName()) : "Clear the primary camera";
			}
			auto Undo() -> bool override { return Level && Level->SetPrimaryCameraActor(Before.Get()); }
			auto Redo() -> bool override { return Level && Level->SetPrimaryCameraActor(After.Get()); }

		private:
			TObjectPtr<DLevel> Level;
			TObjectPtr<ACameraActor> Before;
			TObjectPtr<ACameraActor> After;
		};

		auto ActorMatchesFilter(const AActor* Actor, std::string_view Filter) -> bool
		{
			return Actor && (ContainsInsensitive(Actor->GetName(), Filter) || ContainsInsensitive(ClassDisplayName(Actor->GetClass()), Filter));
		}

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

		auto ActorDepth(AActor* Actor, const std::unordered_set<AActor*>& Actors) -> size_t
		{
			size_t Depth = 0;
			std::unordered_set<AActor*> Visited;
			for (AActor* Parent = Actor ? Actor->GetAttachParentActor() : nullptr; Parent && Actors.contains(Parent) && Visited.insert(Parent).second; Parent = Parent->GetAttachParentActor())
				++Depth;
			return Depth;
		}

		auto IsDescendantOf(AActor* Actor, AActor* CandidateAncestor) -> bool
		{
			std::unordered_set<AActor*> Visited;
			for (AActor* Parent = Actor ? Actor->GetAttachParentActor() : nullptr; Parent && Visited.insert(Parent).second; Parent = Parent->GetAttachParentActor())
			{
				if (Parent == CandidateAncestor) return true;
			}
			return false;
		}
	} // namespace

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

		std::vector<AActor*> Actors;
		std::unordered_set<AActor*> ActorSet;
		for (const TObjectPtr<AActor>& ActorPtr : Context.World->GetActors())
		{
			if (AActor* Actor = ActorPtr.Get())
			{
				Actors.push_back(Actor);
				ActorSet.insert(Actor);
			}
		}
		const auto SortByName = [](AActor* Left, AActor* Right) { return Left->GetName() < Right->GetName(); };
		std::ranges::sort(Actors, SortByName);

		std::unordered_map<AActor*, std::vector<AActor*>> Children;
		std::vector<AActor*> Roots;
		for (AActor* Actor : Actors)
		{
			AActor* Parent = Actor->GetAttachParentActor();
			if (Parent && ActorSet.contains(Parent) && !IsDescendantOf(Parent, Actor))
				Children[Parent].push_back(Actor);
			else
				Roots.push_back(Actor);
		}
		for (auto& [Parent, Nodes] : Children)
			std::ranges::sort(Nodes, SortByName);

		const std::string_view Filter(SearchText.data());
		const bool bRestoreExpansion = bWasSearching && Filter.empty();
		std::unordered_map<AActor*, bool> FilterVisibility;
		std::function<bool(AActor*, std::unordered_set<AActor*>&)> IsVisible = [&](AActor* Actor, std::unordered_set<AActor*>& Stack) {
			if (const auto It = FilterVisibility.find(Actor); It != FilterVisibility.end()) return It->second;
			if (!Stack.insert(Actor).second) return FilterVisibility[Actor] = ActorMatchesFilter(Actor, Filter);
			bool bVisible = ActorMatchesFilter(Actor, Filter);
			for (AActor* Child : Children[Actor])
				bVisible |= IsVisible(Child, Stack);
			Stack.erase(Actor);
			return FilterVisibility[Actor] = bVisible;
		};
		for (AActor* Root : Roots)
		{
			std::unordered_set<AActor*> Stack;
			IsVisible(Root, Stack);
		}

		std::vector<AActor*> VisibleActors;
		bool bRequestDelete = false;
		auto BeginActorRename = [&](AActor* Actor) {
			if (!Actor) return;
			RenamingActor = Actor;
			bRenamingLevel = false;
			RenameDialog.Open(Actor->GetName());
		};
		std::function<void(AActor*, std::unordered_set<AActor*>&)> DrawNode = [&](AActor* Actor, std::unordered_set<AActor*>& Stack) {
			if (!Filter.empty() && !FilterVisibility[Actor]) return;
			if (!Stack.insert(Actor).second) return;
			VisibleActors.push_back(Actor);
			const bool bHasVisibleChildren = std::ranges::any_of(Children[Actor], [&](AActor* Child) { return Filter.empty() || FilterVisibility[Child]; });
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
			const std::string Label = bPrimaryCamera ? std::format("{}  {}  [Primary]", ActorIcon(Actor), Actor->GetName()) : std::format("{}  {}", ActorIcon(Actor), Actor->GetName());
			const bool bOpen = MonaImGui::CompactTreeNode("ActorNode", Flags, "%s", Label.c_str());
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

			if (ImGui::BeginPopupContextItem("ActorContext"))
			{
				bLevelSelected = false;
				if (!Context.IsActorSelected(Actor)) Context.SelectActor(Actor);
				if (ImGui::MenuItem("Focus", "F") && Context.FocusActor) Context.FocusActor(Actor);
				ImGui::Separator();
				if (Context.bReadOnly)
				{
					ImGui::TextDisabled("Runtime actor");
				}
				else if (ImGui::MenuItem("Rename", "F2"))
					BeginActorRename(Actor);
				if (!Context.bReadOnly)
				{
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
				}
				if (!Context.bReadOnly && ImGui::MenuItem("Delete", "Del")) bRequestDelete = true;
				ImGui::EndPopup();
			}

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
						const bool bHasSelectedAncestor = std::ranges::any_of(Context.GetSelectedActors(), [&](const TObjectPtr<AActor>& Other) { return Other.Get() != Candidate && IsDescendantOf(Candidate, Other.Get()); });
						if (!bHasSelectedAncestor) MoveActors.push_back(Candidate);
					}
					for (AActor* Moving : MoveActors)
						if (!Moving->AttachToActor(Actor, EAttachmentTransformRule::KeepWorld))
							Context.SetError(std::format("Failed to attach '{}' to '{}'.", Moving->GetName(), Actor->GetName()));
						else
							Moving->MarkPackageDirty();
				}
				ImGui::EndDragDropTarget();
			}

			if (bOpen && bHasVisibleChildren)
			{
				for (AActor* Child : Children[Actor])
					DrawNode(Child, Stack);
				ImGui::TreePop();
			}
			Stack.erase(Actor);
			ImGui::PopID();
		};

		const std::string LevelName = LevelDisplayName(Context.Level);
		auto BeginLevelRename = [&]() {
			RenamingActor = nullptr;
			bRenamingLevel = true;
			RenameDialog.Open(LevelName);
		};
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
			if (ImGui::MenuItem("Rename", "F2"))
				BeginLevelRename();
			ImGui::Separator();
			if (ImGui::BeginMenu("Add Actor"))
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
						AActor* Actor = Context.World->SpawnActor(Class, FName(Class->GetDefaultObjectName()));
						if (Actor)
						{
							Context.SelectActor(Actor);
							bLevelSelected = false;
						}
						else
							Context.SetError(std::format("Failed to create actor of class {}.", Class->GetQualifiedName().ToString()));
					}
				}
				ImGui::EndMenu();
			}
			if (Context.bReadOnly) ImGui::EndDisabled();
			ImGui::EndPopup();
		}

		if (!Context.bReadOnly && ImGui::BeginDragDropTarget())
		{
			if (ImGui::AcceptDragDropPayload(ActorPayloadType))
			{
				for (const TObjectPtr<AActor>& Selected : Context.GetSelectedActors())
				{
					AActor* Actor = Selected.Get();
					if (!Actor || !Actor->GetAttachParentActor()) continue;
					const bool bHasSelectedAncestor = std::ranges::any_of(Context.GetSelectedActors(), [&](const TObjectPtr<AActor>& Other) { return Other.Get() != Actor && IsDescendantOf(Actor, Other.Get()); });
					if (!bHasSelectedAncestor && !Actor->DetachFromActor(EDetachmentTransformRule::KeepWorld))
						Context.SetError(std::format("Failed to detach '{}'.", Actor->GetName()));
					else
						Actor->MarkPackageDirty();
				}
			}
			ImGui::EndDragDropTarget();
		}

		if (bLevelOpen)
		{
			for (AActor* Root : Roots)
			{
				std::unordered_set<AActor*> Stack;
				DrawNode(Root, Stack);
			}
			if (VisibleActors.empty()) ImGui::TextDisabled(Filter.empty() ? "No actors in this level." : "No actors match '%s'.", SearchText.data());
			ImGui::TreePop();
		}
		ImGui::PopID();
		ExpandRequest = 0;
		bWasSearching = !Filter.empty();

		ImGui::Spacing();
		ImGui::TextDisabled("%zu actors | %zu selected", Actors.size(), Context.GetSelectedActors().size());

		const ImGuiIO& IO = ImGui::GetIO();
		const bool bOutlinerFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (bOutlinerFocused && !IO.WantTextInput && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) Context.SetSelectedActors(VisibleActors);
		if (!Context.bReadOnly && bOutlinerFocused && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2, false))
		{
			if (AActor* Actor = Context.GetPrimarySelectedActor())
				BeginActorRename(Actor);
			else if (bLevelSelected)
				BeginLevelRename();
		}

		const bool bRenameActor = RenamingActor.IsValid();
		const char* RenamePopupTitle = bRenameActor ? "Rename Actor###OutlinerRename" : "Rename Level###OutlinerRename";
		const std::string CurrentRenameName = bRenameActor ? RenamingActor->GetName() : LevelName;
		const EEditorRenameDialogResult RenameResult = RenameDialog.Draw(RenamePopupTitle, CurrentRenameName, [&](std::string_view NewName) -> std::string {
			if (bRenameActor)
			{
				return Context.Level->RenameActor(RenamingActor.Get(), FName(NewName)) ? std::string() : "Failed to rename actor.";
			}
			return Context.RenameLevel && Context.RenameLevel(NewName) ? std::string() : "Failed to rename level.";
		});
		if (RenameResult != EEditorRenameDialogResult::None)
		{
			RenamingActor = nullptr;
			bRenamingLevel = false;
		}
		if (!Context.bReadOnly && !Context.GetSelectedActors().empty() && bOutlinerFocused && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) bRequestDelete = true;
		if (bRequestDelete)
		{
			PendingDeleteActors = Context.GetSelectedActors();
			ImGui::OpenPopup("Delete Actors?");
		}
		if (ImGui::BeginPopupModal("Delete Actors?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::Text("Delete %zu actor(s)?", PendingDeleteActors.size());
			for (size_t Index = 0; Index < std::min<size_t>(PendingDeleteActors.size(), 5); ++Index)
				if (PendingDeleteActors[Index]) ImGui::BulletText("%s", PendingDeleteActors[Index]->GetName().c_str());
			if (PendingDeleteActors.size() > 5) ImGui::TextDisabled("... and %zu more", PendingDeleteActors.size() - 5);
			ImGui::TextDisabled("This action cannot be undone.");
			if (ImGui::Button("Delete"))
			{
				std::ranges::sort(PendingDeleteActors, [&](const TObjectPtr<AActor>& Left, const TObjectPtr<AActor>& Right) { return ActorDepth(Left.Get(), ActorSet) > ActorDepth(Right.Get(), ActorSet); });
				for (const TObjectPtr<AActor>& Actor : PendingDeleteActors)
					if (Actor && !Context.World->DestroyActor(Actor.Get())) Context.SetError(std::format("Failed to delete '{}'.", Actor->GetName()));
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

		if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
		{
			Context.ClearSelection();
			bLevelSelected = false;
		}
		LastVisibleActors = std::move(VisibleActors);
		ImGui::End();
	}
} // namespace Durin
