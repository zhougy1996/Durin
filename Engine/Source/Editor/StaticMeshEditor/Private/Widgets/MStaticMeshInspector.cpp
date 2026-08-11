#include "Widgets/MStaticMeshInspector.h"

#include "Asset/WorkspaceAssetOpenCompatibility.h"
#include "AssetSystem.h"
#include "DObject/Object.h"
#include "Editor/WorkspaceManager.h"
#include "Editor/WorkspaceUI.h"
#include "Materials/MaterialInterface.h"
#include "MonaImGui.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Workspace/StaticMeshEditorWorkspace.h"

namespace Durin::Editor::StaticMesh
{
	namespace
	{
		constexpr float DefaultPreviewPaneRatio = 0.70f;
		constexpr float WideLayoutMinimumWidth = 820.0f;
		constexpr float MinimumPreviewWidth = 440.0f;
		constexpr float MinimumDetailsWidth = 340.0f;
		constexpr float NarrowPreviewHeight = 430.0f;

		auto ReadinessLabel(EStaticMeshRenderResourceReadiness Readiness) -> const char*
		{
			switch (Readiness)
			{
			case EStaticMeshRenderResourceReadiness::Queued: return "Loading";
			case EStaticMeshRenderResourceReadiness::Ready: return "Ready";
			case EStaticMeshRenderResourceReadiness::Failed: return "Failed";
			case EStaticMeshRenderResourceReadiness::Unavailable: return "Unavailable";
			}
			return "Unavailable";
		}

		auto DrawInfoRow(const char* Label, std::string_view Value) -> void
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextDisabled("%s", Label);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(Value.data(), Value.data() + Value.size());
		}

		auto FormatVector(const FVector3& Value) -> std::string
		{
			return std::format("({:.3f}, {:.3f}, {:.3f})", Value.x, Value.y, Value.z);
		}

		auto CollisionModeLabel(EBodySetupCollisionSourceMode Mode) -> const char*
		{
			switch (Mode)
			{
			case EBodySetupCollisionSourceMode::None: return "None";
			case EBodySetupCollisionSourceMode::ConvexHullFromLOD0: return "Convex hull from LOD 0";
			case EBodySetupCollisionSourceMode::TriangleMeshFromLOD0: return "Triangle mesh from LOD 0";
			}
			return "Invalid";
		}

		auto CollisionPolicyLabel(EBodySetupCollisionQueryPolicy Policy) -> const char*
		{
			switch (Policy)
			{
			case EBodySetupCollisionQueryPolicy::SimpleOnly: return "Simple only";
			case EBodySetupCollisionQueryPolicy::ComplexOnly: return "Complex only";
			case EBodySetupCollisionQueryPolicy::SimpleAndComplex: return "Simple and complex";
			}
			return "Invalid";
		}

		auto CollisionStatusLabel(EBodySetupCollisionBuildStatus Status) -> const char*
		{
			switch (Status)
			{
			case EBodySetupCollisionBuildStatus::None: return "None";
			case EBodySetupCollisionBuildStatus::Ready: return "Ready";
			case EBodySetupCollisionBuildStatus::CacheHit: return "Cache hit";
			case EBodySetupCollisionBuildStatus::Rebuilt: return "Rebuilt";
			case EBodySetupCollisionBuildStatus::SourceUnavailable: return "Source unavailable";
			case EBodySetupCollisionBuildStatus::Failed: return "Failed";
			case EBodySetupCollisionBuildStatus::CookedLoaded: return "Cooked loaded";
			}
			return "Invalid";
		}
	}

	MStaticMeshInspector::MStaticMeshInspector(::Durin::Editor::FWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager)
	{
	}

	MStaticMeshInspector::~MStaticMeshInspector() = default;

	auto MStaticMeshInspector::GetWorkspaceType() const -> const ::Durin::Editor::FWorkspaceTypeId&
	{
		return Workspace::Type;
	}

	auto MStaticMeshInspector::OpenDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return ::Durin::Editor::EDocumentOpenResult::Rejected;
		if (FindState(Document.ResourceId)) return ::Durin::Editor::EDocumentOpenResult::Opened;

		FAssetPath AssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			ErrorMessage = std::move(PathError);
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		::Durin::Editor::FWorkspaceAssetOpenCompatibility CompatibilityPolicy(AssetPath);
		DStaticMesh* Mesh = nullptr;
		Asset::FAssetLoadReport LoadReport;
		const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Mesh, &LoadReport);
		if (!Result || !Mesh)
		{
			ErrorMessage = Result ? "The selected asset is not a StaticMesh." : Result.Message;
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		std::string CompatibilityDiagnostic;
		if (CompatibilityPolicy.RejectIfIncompatible(LoadReport, CompatibilityDiagnostic))
		{
			ErrorMessage = std::move(CompatibilityDiagnostic);
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}

		FDocumentState State;
		State.Mesh = Mesh;
		Documents.emplace(Document.ResourceId, std::move(State));
		return ::Durin::Editor::EDocumentOpenResult::Opened;
	}

	auto MStaticMeshInspector::ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void
	{
		if (FindState(Document.ResourceId)) ActiveResourceId = Document.ResourceId;
		DocumentHost.RequestFocus(Document.Id);
	}

	auto MStaticMeshInspector::RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentCloseResult
	{
		if (FDocumentState* State = FindState(Document.ResourceId); State && State->Preview)
			State->Preview->SetVisible(false);
		Documents.erase(Document.ResourceId);
		if (ActiveResourceId == Document.ResourceId) ActiveResourceId.clear();
		return ::Durin::Editor::EDocumentCloseResult::Closed;
	}

	auto MStaticMeshInspector::DrawWorkspace(bool bActive) -> bool
	{
		if (!bActive)
			for (auto& [ResourceId, State] : Documents)
				if (State.Preview) State.Preview->SetVisible(false);
		return DocumentHost.DrawDocuments(
			WorkspaceManager,
			Workspace::Type,
			Workspace::RootKey,
			[this](const ::Durin::Editor::FDocumentTab& Document) { return FindState(Document.ResourceId) != nullptr; },
			[this](const ::Durin::Editor::FDocumentTab& Document) {
				if (FDocumentState* State = FindState(Document.ResourceId)) DrawDocument(Document, *State);
			});
	}

	auto MStaticMeshInspector::ResetLayout() -> void
	{
		PreviewPaneRatio = DefaultPreviewPaneRatio;
		for (auto& [ResourceId, State] : Documents)
		{
			State.SelectedLOD = 0;
			if (State.Preview) State.Preview->ResetView();
		}
	}

	auto MStaticMeshInspector::FindState(std::string_view ResourceId) -> FDocumentState*
	{
		const auto It = Documents.find(std::string(ResourceId));
		return It == Documents.end() ? nullptr : &It->second;
	}

	auto MStaticMeshInspector::FindState(std::string_view ResourceId) const -> const FDocumentState*
	{
		const auto It = Documents.find(std::string(ResourceId));
		return It == Documents.end() ? nullptr : &It->second;
	}

	auto MStaticMeshInspector::DrawDocument(const ::Durin::Editor::FDocumentTab& Document, FDocumentState& State) -> void
	{
		DStaticMesh* Mesh = State.Mesh.Get();
		if (!Mesh)
		{
			ImGui::TextWrapped("StaticMesh unavailable: the asset is no longer loaded.");
			return;
		}
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		const uint32 LODCount = RenderData ? static_cast<uint32>(RenderData->LODResources.size()) : 0;
		if (LODCount == 0) State.SelectedLOD = 0;
		else State.SelectedLOD = std::min(State.SelectedLOD, LODCount - 1);

		if (ImGui::Button("Frame Selection") && State.Preview) State.Preview->ResetView();
		ImGui::SameLine();
		bool bWireframe = State.Preview && State.Preview->IsWireframe();
		if (ImGui::Checkbox("Wireframe", &bWireframe) && State.Preview) State.Preview->SetWireframe(bWireframe);
		ImGui::SameLine();
		ImGui::TextDisabled("Read-only");
		ImGui::SameLine();
		ImGui::TextUnformatted(Document.ResourceId.c_str());
		ImGui::Spacing();

		const ImVec2 Available = ImGui::GetContentRegionAvail();
		if (Available.x >= MonaImGui::ScaleUI(WideLayoutMinimumWidth))
		{
			const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
			const float MinimumPreview = MonaImGui::ScaleUI(MinimumPreviewWidth);
			const float MinimumDetails = MonaImGui::ScaleUI(MinimumDetailsWidth);
			const float PreviewWidth = std::clamp(
				Available.x * PreviewPaneRatio,
				MinimumPreview,
				std::max(MinimumPreview, Available.x - Metrics.SplitterThickness - MinimumDetails));
			if (ImGui::BeginChild("StaticMeshPreviewPane", ImVec2(PreviewWidth, Available.y))) DrawPreview(State, 0.0f);
			ImGui::EndChild();
			ImGui::SameLine();
			MonaImGui::DrawSplitter("StaticMeshInspectorSplitter", MonaImGui::EUISplitterAxis::X,
				Available.y, Available.x, MinimumPreview, MinimumDetails, PreviewPaneRatio);
			ImGui::SameLine();
			DrawDetails(Document, State, Available.y);
		}
		else
		{
			DrawPreview(State, MonaImGui::ScaleUI(NarrowPreviewHeight));
			ImGui::Spacing();
			DrawDetails(Document, State, 0.0f);
		}

		if (ActiveResourceId == Document.ResourceId && !ErrorMessage.empty())
			ImGui::TextWrapped("%s", ErrorMessage.c_str());
	}

	auto MStaticMeshInspector::DrawPreview(FDocumentState& State, float Height) -> void
	{
		DStaticMesh* Mesh = State.Mesh.Get();
		const FStaticMeshRenderResourceStatus Status = Mesh ? Mesh->GetRenderResourceStatus() : FStaticMeshRenderResourceStatus{};
		if (!State.Preview) State.Preview = std::make_unique<FStaticMeshPreview>(NextPreviewId++);
		if (State.Preview) State.Preview->Draw(Mesh, Status.Revision, Height);
	}

	auto MStaticMeshInspector::DrawDetails(const ::Durin::Editor::FDocumentTab& Document, FDocumentState& State, float Height) -> void
	{
		if (!ImGui::BeginChild("StaticMeshDetails", ImVec2(0.0f, Height), ImGuiChildFlags_Borders))
		{
			ImGui::EndChild();
			return;
		}
		DStaticMesh* Mesh = State.Mesh.Get();
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		const FStaticMeshRenderResourceStatus Status = Mesh ? Mesh->GetRenderResourceStatus() : FStaticMeshRenderResourceStatus{};
		const uint32 LODCount = RenderData ? static_cast<uint32>(RenderData->LODResources.size()) : 0;

		ImGui::SeparatorText("Asset");
		if (ImGui::BeginTable("StaticMeshAssetInfo", 2, ImGuiTableFlags_SizingStretchProp))
		{
			DrawInfoRow("Path", Document.ResourceId);
			DrawInfoRow("LOD count", std::to_string(LODCount));
			DrawInfoRow("Material slots", std::to_string(Mesh ? Mesh->GetNumMaterialSlots() : 0));
			DrawInfoRow("Render resources", ReadinessLabel(Status.Readiness));
			DrawInfoRow("Revision", std::to_string(Status.Revision));
			if (const std::optional<FBox> Bounds = Mesh ? Mesh->GetLOD0LocalBounds() : std::nullopt)
			{
				DrawInfoRow("Bounds center", FormatVector(Bounds->GetCenter()));
				DrawInfoRow("Bounds extent", FormatVector(Bounds->GetExtent()));
			}
			else DrawInfoRow("Local bounds", "Unavailable");
			ImGui::EndTable();
		}

		ImGui::SeparatorText("LOD Statistics");
		if (LODCount == 0)
		{
			ImGui::TextWrapped("No validated render LOD data is available.");
		}
		else
		{
			ImGui::SetNextItemWidth(MonaImGui::ScaleUI(130.0f));
			const std::string LODLabel = std::format("LOD {}", State.SelectedLOD);
			if (ImGui::BeginCombo("Selected LOD", LODLabel.c_str()))
			{
				for (uint32 Index = 0; Index < LODCount; ++Index)
					if (ImGui::Selectable(std::format("LOD {}", Index).c_str(), State.SelectedLOD == Index)) State.SelectedLOD = Index;
				ImGui::EndCombo();
			}
			const FStaticMeshLODResources& LOD = RenderData->LODResources[State.SelectedLOD];
			if (ImGui::BeginTable("StaticMeshLODInfo", 2, ImGuiTableFlags_SizingStretchProp))
			{
				DrawInfoRow("Vertices", std::to_string(LOD.GetNumVertices()));
				DrawInfoRow("Indices", std::to_string(LOD.GetNumIndices()));
				DrawInfoRow("Triangles", std::to_string(LOD.GetNumIndices() / 3));
				DrawInfoRow("Sections", std::to_string(LOD.Sections.size()));
				ImGui::EndTable();
			}
		}

		ImGui::SeparatorText("Collision");
		const FStaticMeshCollisionInspection Collision = Mesh
			? Mesh->InspectCollision() : FStaticMeshCollisionInspection{};
		if (ImGui::BeginTable("StaticMeshCollisionInfo", 2, ImGuiTableFlags_SizingStretchProp))
		{
			DrawInfoRow("Mode", CollisionModeLabel(Collision.Mode));
			DrawInfoRow("Policy", CollisionPolicyLabel(Collision.Policy));
			DrawInfoRow("Cache / Cook status", CollisionStatusLabel(Collision.BuildStatus));
			DrawInfoRow("Source triangles", std::to_string(Collision.SourceTriangles));
			DrawInfoRow("Retained triangles", std::to_string(Collision.RetainedTriangles));
			DrawInfoRow("Removed triangles", std::to_string(Collision.RemovedTriangles));
			DrawInfoRow("BVH nodes", std::to_string(Collision.Nodes));
			DrawInfoRow("Payload bytes", std::to_string(Collision.PayloadBytes));
			DrawInfoRow("Runtime bytes", std::to_string(Collision.RuntimeBytes));
			DrawInfoRow("Builder / schema", std::format("{} / {}", Collision.BuilderVersion, Collision.SchemaVersion));
			DrawInfoRow("Build revision", std::to_string(Collision.BuildRevision));
			DrawInfoRow("Revision coherence", Collision.bRevisionCoherent ? "Coherent" : "Unavailable");
			if (Collision.Bounds)
			{
				DrawInfoRow("Bounds center", FormatVector(Collision.Bounds->GetCenter()));
				DrawInfoRow("Bounds extent", FormatVector(Collision.Bounds->GetExtent()));
			}
			DrawInfoRow("Cache key", Collision.CacheKey.empty() ? "None" : Collision.CacheKey);
			DrawInfoRow("Last diagnostic", Collision.Diagnostic.empty() ? "None" : Collision.Diagnostic);
			ImGui::EndTable();
		}

		ImGui::SeparatorText("Material Slots");
		if (!Mesh || Mesh->GetMaterialSlots().empty()) ImGui::TextDisabled("No material slots");
		else if (ImGui::BeginTable("StaticMeshMaterialSlots", 2,
			ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Slot");
			ImGui::TableSetupColumn("Default material");
			ImGui::TableHeadersRow();
			for (const FStaticMeshMaterialSlotDefinition& Slot : Mesh->GetMaterialSlots())
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(Slot.Name.ToString().c_str());
				ImGui::TableSetColumnIndex(1);
				const std::string MaterialPath = Slot.DefaultMaterial ? Slot.DefaultMaterial->GetObjectPath() : std::string("None");
				ImGui::TextUnformatted(MaterialPath.c_str());
			}
			ImGui::EndTable();
		}
		ImGui::EndChild();
	}
}
