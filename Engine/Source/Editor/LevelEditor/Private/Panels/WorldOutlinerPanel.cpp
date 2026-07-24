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
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorHelpers.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		using LevelEditorHelpers::ClassDisplayName;
		using StringUtils::ContainsInsensitive;

		constexpr auto ActorPayloadType = "DURIN_OUTLINER_ACTOR";

		// Restores the level's primary-camera selection.
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
				: Entries(std::move(InEntries)), bShow(bInShow) {}
			auto GetDescription() const -> std::string_view override { return bShow ? "Show actors" : "Hide actors"; }
			auto GetDetails(EEditorTransactionOperation Operation) const -> std::string override
			{
				const bool bApplyingAfter = Operation != EEditorTransactionOperation::Undo;
				const size_t HiddenCount = std::ranges::count_if(Entries, [bApplyingAfter](const FEntry& Entry) { return bApplyingAfter ? Entry.bAfter : Entry.bBefore; });
				return std::format("Set visibility for {} actor(s); {} hidden", Entries.size(), HiddenCount);
			}
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
			bool bShow = false;
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

	} // namespace

	auto FWorldOutlinerPanel::ResetHierarchyCache() -> void
	{
		CachedHierarchyLevel = nullptr;
		CachedHierarchyRevision = 0;
		HierarchyNodes.clear();
		RootNodeIndices.clear();
		ActorToNode.clear();
		FilterVisibility.clear();
		CachedFilter.clear();
		bFilterCacheValid = false;
		VisibleActors.clear();
		LastVisibleActors.clear();
		ExpandedActors.clear();
	}

	auto FWorldOutlinerPanel::RebuildHierarchyCache(DLevel* Level) -> void
	{
		CachedHierarchyLevel = Level;
		CachedHierarchyRevision = Level ? Level->GetEditorActorHierarchyRevision() : 0;
		HierarchyNodes.clear();
		RootNodeIndices.clear();
		ActorToNode.clear();
		FilterVisibility.clear();
		bFilterCacheValid = false;
		VisibleActors.clear();
		LastVisibleActors.clear();

		if (!Level)
		{
			ExpandedActors.clear();
			return;
		}

		std::vector<AActor*> SortedActors;
		SortedActors.reserve(Level->GetActors().size());
		for (const TObjectPtr<AActor>& ActorPtr : Level->GetActors())
		{
			if (AActor* Actor = ActorPtr.Get()) SortedActors.push_back(Actor);
		}
		std::ranges::sort(SortedActors, [](AActor* Left, AActor* Right) { return Left->GetName() < Right->GetName(); });

		HierarchyNodes.reserve(SortedActors.size());
		ActorToNode.reserve(SortedActors.size());
		for (AActor* Actor : SortedActors)
		{
			const uint32 NodeIndex = static_cast<uint32>(HierarchyNodes.size());
			HierarchyNodes.push_back({.Actor = Actor});
			ActorToNode.emplace(Actor, NodeIndex);
		}

		for (uint32 NodeIndex = 0; NodeIndex < HierarchyNodes.size(); ++NodeIndex)
		{
			AActor* Parent = HierarchyNodes[NodeIndex].Actor->GetAttachParentActor();
			if (const auto It = ActorToNode.find(Parent); It != ActorToNode.end() && It->second != NodeIndex)
				HierarchyNodes[NodeIndex].Parent = It->second;
		}

		// Loaded levels already reject cycles. Keep the cache defensive without walking every parent chain.
		std::vector<uint8> VisitState(HierarchyNodes.size(), 0);
		std::vector<uint32> Path;
		Path.reserve(HierarchyNodes.size());
		for (uint32 StartIndex = 0; StartIndex < HierarchyNodes.size(); ++StartIndex)
		{
			if (VisitState[StartIndex] != 0) continue;
			Path.clear();
			uint32 NodeIndex = StartIndex;
			while (NodeIndex != InvalidNodeIndex && VisitState[NodeIndex] == 0)
			{
				VisitState[NodeIndex] = 1;
				Path.push_back(NodeIndex);
				NodeIndex = HierarchyNodes[NodeIndex].Parent;
			}
			if (NodeIndex != InvalidNodeIndex && VisitState[NodeIndex] == 1)
				HierarchyNodes[Path.back()].Parent = InvalidNodeIndex;
			for (uint32 PathNode : Path) VisitState[PathNode] = 2;
		}

		RootNodeIndices.reserve(HierarchyNodes.size());
		for (uint32 NodeIndex = 0; NodeIndex < HierarchyNodes.size(); ++NodeIndex)
		{
			const uint32 ParentIndex = HierarchyNodes[NodeIndex].Parent;
			if (ParentIndex == InvalidNodeIndex)
				RootNodeIndices.push_back(NodeIndex);
			else
				HierarchyNodes[ParentIndex].Children.push_back(NodeIndex);
		}

		uint32 TraversalPosition = 0;
		auto CacheTraversal = [&](auto&& Self, uint32 NodeIndex, uint32 Depth) -> void {
			FOutlinerNode& Node = HierarchyNodes[NodeIndex];
			Node.Depth = Depth;
			Node.TraversalBegin = TraversalPosition++;
			for (uint32 ChildIndex : Node.Children) Self(Self, ChildIndex, Depth + 1);
			Node.TraversalEnd = TraversalPosition;
		};
		for (uint32 RootIndex : RootNodeIndices) CacheTraversal(CacheTraversal, RootIndex, 0);

		std::erase_if(ExpandedActors, [&](const auto& Entry) { return !ActorToNode.contains(Entry.first); });
	}

	auto FWorldOutlinerPanel::RebuildFilterCache(std::string_view Filter) -> void
	{
		CachedFilter = Filter;
		FilterVisibility.assign(HierarchyNodes.size(), Filter.empty());
		bFilterCacheValid = true;
		if (Filter.empty()) return;

		auto CacheVisibility = [&](auto&& Self, uint32 NodeIndex) -> bool {
			bool bVisible = ActorMatchesFilter(HierarchyNodes[NodeIndex].Actor.Get(), Filter);
			for (uint32 ChildIndex : HierarchyNodes[NodeIndex].Children)
				bVisible |= Self(Self, ChildIndex);
			FilterVisibility[NodeIndex] = bVisible;
			return bVisible;
		};
		for (uint32 RootIndex : RootNodeIndices) CacheVisibility(CacheVisibility, RootIndex);
	}

	auto FWorldOutlinerPanel::IsNodeVisible(uint32 NodeIndex) const -> bool
	{
		return NodeIndex < FilterVisibility.size() && FilterVisibility[NodeIndex] != 0;
	}

	auto FWorldOutlinerPanel::IsDescendantOf(const AActor* Actor, const AActor* CandidateAncestor) const -> bool
	{
		if (!Actor || !CandidateAncestor || Actor == CandidateAncestor) return false;
		const auto ActorIt = ActorToNode.find(Actor);
		const auto AncestorIt = ActorToNode.find(CandidateAncestor);
		if (ActorIt == ActorToNode.end() || AncestorIt == ActorToNode.end()) return false;
		const FOutlinerNode& Node = HierarchyNodes[ActorIt->second];
		const FOutlinerNode& Ancestor = HierarchyNodes[AncestorIt->second];
		return Ancestor.TraversalBegin <= Node.TraversalBegin && Node.TraversalEnd <= Ancestor.TraversalEnd;
	}

	auto FWorldOutlinerPanel::GetActorDepth(const AActor* Actor) const -> uint32
	{
		const auto It = ActorToNode.find(Actor);
		return It != ActorToNode.end() ? HierarchyNodes[It->second].Depth : 0;
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

		if (CachedHierarchyLevel.Get() != Context.Level
			|| CachedHierarchyRevision != Context.Level->GetEditorActorHierarchyRevision())
			RebuildHierarchyCache(Context.Level);
		const std::string_view Filter(SearchText.data());
		if (!bFilterCacheValid || CachedFilter != Filter) RebuildFilterCache(Filter);
		const bool bRestoreExpansion = bWasSearching && Filter.empty();

		VisibleActors.clear();
		bool bRequestDelete = false;
		auto SetActorVisibility = [&](const std::vector<TObjectPtr<AActor>>& TargetActors, bool bHidden) {
			std::vector<FActorVisibilityTransaction::FEntry> Entries;
			for (const TObjectPtr<AActor>& Actor : TargetActors)
			{
				if (Actor && Actor->IsHidden() != bHidden) Entries.push_back({Actor, Actor->IsHidden(), bHidden});
			}
			if (Entries.empty()) return;
			auto Transaction = std::make_unique<FActorVisibilityTransaction>(std::move(Entries), !bHidden);
			if (GEditor) GEditor->GetTransactionManager().Execute(std::move(Transaction));
			else Transaction->Redo();
		};
		auto ShowAllActors = [&]() {
			std::vector<TObjectPtr<AActor>> AllActors;
			AllActors.reserve(HierarchyNodes.size());
			for (const FOutlinerNode& Node : HierarchyNodes) AllActors.push_back(Node.Actor);
			SetActorVisibility(AllActors, false);
		};
		auto BeginActorRename = [&](AActor* Actor) {
			if (!Actor) return;
			RenamingActor = Actor;
			bRenamingLevel = false;
			RenameDialog.Open(Actor->GetName());
		};
		auto DrawNode = [&](auto&& Self, uint32 NodeIndex) -> void {
			if (!IsNodeVisible(NodeIndex)) return;
			const FOutlinerNode& Node = HierarchyNodes[NodeIndex];
			AActor* Actor = Node.Actor.Get();
			if (!Actor) return;
			VisibleActors.push_back(Actor);
			const bool bHasVisibleChildren = std::ranges::any_of(Node.Children, [&](uint32 ChildIndex) { return IsNodeVisible(ChildIndex); });
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

			if (ImGui::BeginPopupContextItem("ActorContext"))
			{
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
					const bool bAllSelectedHidden = std::ranges::all_of(Context.GetSelectedActors(), [](const TObjectPtr<AActor>& Selected) { return Selected && Selected->IsHidden(); });
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
				for (uint32 ChildIndex : Node.Children) Self(Self, ChildIndex);
				ImGui::TreePop();
			}
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
			if (ImGui::MenuItem("Show All Actors", "Ctrl+H")) ShowAllActors();
			ImGui::Separator();
			if (ImGui::MenuItem("Rename", "F2")) BeginLevelRename();
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
			for (uint32 RootIndex : RootNodeIndices) DrawNode(DrawNode, RootIndex);
			if (VisibleActors.empty()) ImGui::TextDisabled(Filter.empty() ? "No actors in this level." : "No actors match '%s'.", SearchText.data());
			ImGui::TreePop();
		}
		ImGui::PopID();
		ExpandRequest = 0;
		bWasSearching = !Filter.empty();

		ImGui::Spacing();
		ImGui::TextDisabled("%zu actors | %zu selected", HierarchyNodes.size(), Context.GetSelectedActors().size());

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
				BeginLevelRename();
		}
		if (!Context.bReadOnly && bOutlinerFocused && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_H, false))
		{
			if (IO.KeyCtrl)
				ShowAllActors();
			else if (!Context.GetSelectedActors().empty())
			{
				const bool bAllSelectedHidden = std::ranges::all_of(Context.GetSelectedActors(), [](const TObjectPtr<AActor>& Actor) { return Actor && Actor->IsHidden(); });
				SetActorVisibility(Context.GetSelectedActors(), !bAllSelectedHidden);
			}
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
				std::ranges::sort(PendingDeleteActors, [&](const TObjectPtr<AActor>& Left, const TObjectPtr<AActor>& Right) { return GetActorDepth(Left.Get()) > GetActorDepth(Right.Get()); });
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
		LastVisibleActors.swap(VisibleActors);
		ImGui::End();
	}
} // namespace Durin
