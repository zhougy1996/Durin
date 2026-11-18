#include "FStaticMeshEditor.h"
#include "Misc/UnifiedError.h"

#include "Asset/Asset.h"
#include "Diagnostics/StaticMeshPayloadInspection.h"
#include "DObject/Object.h"
#include "Framework/TabLayout.h"
#include "Materials/MaterialInterface.h"
#include "MonaImGui.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Workspace/StaticMeshEditorWorkspace.h"

namespace Durin::StaticMeshEditor
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
			case EStaticMeshRenderResourceReadiness::InvalidData: return "Invalid CPU Data";
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
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(Value.data(), Value.data() + Value.size());
			ImGui::PopTextWrapPos();
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
			case EBodySetupCollisionSourceMode::TriangleMeshFromLOD0: return "Triangle mesh from collision LOD";
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

	}

	FStaticMeshEditor::FStaticMeshEditor() = default;
	FStaticMeshEditor::~FStaticMeshEditor() = default;

	auto FStaticMeshEditor::OnClose() -> void
	{
		MeshPreviews.clear();
		FBaseAssetToolkit::OnClose();
	}

	auto FStaticMeshEditor::GetToolkitId() const -> const FAssetEditorToolkitId&
	{
		return Workspace::Type;
	}

	auto FStaticMeshEditor::OnOpenAsset(DObject& Object) -> bool
	{
		auto* Mesh = Cast<DStaticMesh>(&Object);
		if (!Mesh) return false;
		static uint64 NextPreviewId = 1;
		FMeshPreviewState State;
		State.PreviewId = NextPreviewId++;
		MeshPreviews.emplace(Object.GetObjectPath(), std::move(State));
		return true;
	}

	auto FStaticMeshEditor::OnCloseAsset(DObject& Object) -> void
	{
		if (auto* State = FindState(Object.GetObjectPath()); State && State->Preview)
			State->Preview->SetVisible(false);
		MeshPreviews.erase(Object.GetObjectPath());
	}

	auto FStaticMeshEditor::OnDrawToolkit(bool bActive) -> bool
	{
		if (!bActive)
			for (auto& [Resource, State] : MeshPreviews)
				if (State.Preview) State.Preview->SetVisible(false);
		return DrawAssetTabs(Workspace::RootKey, [this](DObject& Object) {
			auto* Mesh = Cast<DStaticMesh>(&Object);
			if (auto* State = Mesh ? FindState(Mesh->GetObjectPath()) : nullptr) DrawMesh(*Mesh, *State);
		}, [this](DObject& Object, bool bVisible) {
			if (auto* State = FindState(Object.GetObjectPath()); !bVisible && State && State->Preview)
				State->Preview->SetVisible(false);
		});
	}

	auto FStaticMeshEditor::ResetLayout() -> void
	{
		PreviewPaneRatio = DefaultPreviewPaneRatio;
		for (auto& [ResourceId, State] : MeshPreviews)
		{
			State.SelectedLOD = 0;
			if (State.Preview) State.Preview->ResetView();
		}
	}

	auto FStaticMeshEditor::FindState(std::string_view ResourceId) -> FMeshPreviewState*
	{
		const auto It = MeshPreviews.find(std::string(ResourceId));
		return It == MeshPreviews.end() ? nullptr : &It->second;
	}

	auto FStaticMeshEditor::FindState(std::string_view ResourceId) const -> const FMeshPreviewState*
	{
		const auto It = MeshPreviews.find(std::string(ResourceId));
		return It == MeshPreviews.end() ? nullptr : &It->second;
	}

	auto FStaticMeshEditor::DrawMesh(DStaticMesh& Mesh, FMeshPreviewState& State) -> void
	{
		const auto Resource = Mesh.GetObjectPath();
		const FStaticMeshRenderData* RenderData = Mesh.GetRenderData();
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
		ImGui::TextUnformatted(Resource.data());
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
			if (ImGui::BeginChild("StaticMeshPreviewPane", ImVec2(PreviewWidth, Available.y))) DrawPreview(Mesh, State, 0.0f);
			ImGui::EndChild();
			ImGui::SameLine();
			MonaImGui::DrawSplitter("StaticMeshInspectorSplitter", MonaImGui::EUISplitterAxis::X,
				Available.y, Available.x, MinimumPreview, MinimumDetails, PreviewPaneRatio);
			ImGui::SameLine();
			DrawDetails(Mesh, Resource, State, Available.y);
		}
		else
		{
			DrawPreview(Mesh, State, MonaImGui::ScaleUI(NarrowPreviewHeight));
			ImGui::Spacing();
			DrawDetails(Mesh, Resource, State, 0.0f);
		}

		if (!ErrorMessage.empty())
			ImGui::TextWrapped("%s", ErrorMessage.c_str());
	}

	auto FStaticMeshEditor::DrawPreview(DStaticMesh& Mesh, FMeshPreviewState& State, float Height) -> void
	{
		const auto Status = Mesh.GetRenderResourceStatus();
		if (!State.Preview) State.Preview = std::make_shared<FStaticMeshPreview>(State.PreviewId);
		const auto DrawingPreview = State.Preview;
		DrawingPreview->Draw(&Mesh, Status.Revision, Height);
	}

	auto FStaticMeshEditor::DrawDetails(DStaticMesh& Asset, std::string_view Resource, FMeshPreviewState& State, float Height) -> void
	{
		if (!ImGui::BeginChild("StaticMeshDetails", ImVec2(0.0f, Height), ImGuiChildFlags_Borders))
		{
			ImGui::EndChild();
			return;
		}
		DStaticMesh* Mesh = &Asset;
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		const FStaticMeshRenderResourceStatus Status = Mesh ? Mesh->GetRenderResourceStatus() : FStaticMeshRenderResourceStatus{};
		const uint32 LODCount = RenderData ? static_cast<uint32>(RenderData->LODResources.size()) : 0;

		ImGui::SeparatorText("Asset");
		if (ImGui::BeginTable("StaticMeshAssetInfo", 2, ImGuiTableFlags_SizingStretchProp))
		{
			DrawInfoRow("Path", Resource);
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

		if (Mesh)
		{
			const auto Inspection = InspectStaticMeshPayloads(*Mesh);
			ImGui::SeparatorText("Payload storage");
			ImGui::TextWrapped("Metadata presence does not prove readable payloads. Inspection never loads or repairs data.");
			for (const auto& Field : Inspection.Fields)
			{
				ImGui::PushID(Field.Field.c_str());
				if (ImGui::BeginTable("Payload", 2, ImGuiTableFlags_SizingStretchProp))
				{
					DrawInfoRow("Field", Field.Field);
					DrawInfoRow("State / placement", Field.State + " / " + Field.Placement);
					DrawInfoRow("Logical / stored bytes", std::format("{} / {}", Field.LogicalBytes, Field.StoredBytes));
					DrawInfoRow("Source identity", Field.Identity.IsZero() ? "Unavailable" : Field.Identity.ToString());
					DrawInfoRow("Workflow", Field.Diagnostic);
					ImGui::EndTable();
				}
				ImGui::PopID();
			}
			ImGui::SeparatorText("CPU residency and cooked loading");
			ImGui::TextWrapped("Decoded authored source: %s. Render CPU data: %s.",
				Inspection.bSourceResident ? "resident" : "not resident",
				Inspection.bCpuResident ? "resident" : "absent");
			ImGui::TextWrapped("Cooked load request: %s.", Inspection.bCookedLoadPending ? "pending" : "none");
			if (const auto Message = FormatCookedMeshLoadError(Inspection.CookedLoadError); !Message.empty())
				ImGui::TextWrapped("Last load error: %s", Message.c_str());
			if (Inspection.CookedLoadError.Code != ECookedMeshLoadError::None)
				ImGui::TextWrapped("Restore or recook invalid cooked data, then explicitly retry CPU loading.");
			if (Inspection.Gpu.Readiness == EStaticMeshRenderResourceReadiness::InvalidData)
				ImGui::TextWrapped("CPU geometry validation failed. Correct or rebuild the asset data.");
			ImGui::SeparatorText("Authored build operation");
			const auto& Operation = Inspection.Operation;
			if (!Operation.RequestId)
				ImGui::TextWrapped("Operation history unavailable (never observed or evicted). DDC origin, key, timings and persistence are unavailable.");
			else
			{
				const char* Phase = "Queued";
				if (Operation.Phase == EStaticMeshCompilationPhase::Building) Phase = "Building";
				else if (Operation.Phase == EStaticMeshCompilationPhase::Mailbox) Phase = "Awaiting publication";
				else if (Operation.Status)
				{
					switch (*Operation.Status)
					{
					case EStaticMeshCompilationStatus::Succeeded: Phase = "Succeeded"; break;
					case EStaticMeshCompilationStatus::Failed: Phase = "Failed"; break;
					case EStaticMeshCompilationStatus::Cancelled: Phase = "Cancelled"; break;
					}
				}
				ImGui::TextWrapped("Request %llu: %s. %s. This operation record does not establish current settings or payload coherence.",
					static_cast<unsigned long long>(Operation.RequestId), Phase,
					Inspection.bOperationSourceMatches ? "Source identity matches" : "Different source identity");
				const auto Message = FormatStaticMeshCompilationDiagnostic(Operation);
				if (!Message.empty()) ImGui::TextWrapped("%s", Message.c_str());
				ImGui::TextWrapped("Persistence is reported only by the operation diagnostic; a successful product does not prove a cache write. For build failure, restore/reimport source or explicitly rebuild disposable derived output. Cancelled or superseded work is not retried by inspection.");
			}
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
			? InspectStaticMeshCollision(*Mesh) : FStaticMeshCollisionInspection{};
		if (ImGui::BeginTable("StaticMeshCollisionInfo", 2, ImGuiTableFlags_SizingStretchProp))
		{
			DrawInfoRow("Mode", CollisionModeLabel(Collision.Mode));
			DrawInfoRow("Policy", CollisionPolicyLabel(Collision.Policy));
			DrawInfoRow("Resident derived geometry", Collision.bHasGeometry ? "Ready" : "Unavailable");
			DrawInfoRow("Source triangles", std::to_string(Collision.SourceTriangles));
			DrawInfoRow("Retained triangles", std::to_string(Collision.RetainedTriangles));
			DrawInfoRow("Triangle count difference", Collision.bTriangleCountsComparable
				? std::to_string(Collision.RemovedTriangles) : "Unavailable");
			DrawInfoRow("BVH nodes", std::to_string(Collision.Nodes));
			DrawInfoRow("Runtime bytes", std::to_string(Collision.RuntimeBytes));
			DrawInfoRow("Current cooker / schema", std::format("{} / {}", Collision.BuilderVersion, Collision.SchemaVersion));
			DrawInfoRow("Build revision", std::to_string(Collision.BuildRevision));
			DrawInfoRow("Revision coherence", Collision.bRevisionCoherent ? "Coherent" : "Unavailable");
			if (Collision.Bounds)
			{
				DrawInfoRow("Bounds center", FormatVector(Collision.Bounds->GetCenter()));
				DrawInfoRow("Bounds extent", FormatVector(Collision.Bounds->GetExtent()));
			}
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
			for (const FMeshMaterialSlotDefinition& Slot : Mesh->GetMaterialSlots())
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				MonaImGui::TextName(Slot.Name);
				ImGui::TableSetColumnIndex(1);
				const std::string MaterialPath = Slot.DefaultMaterial ? Slot.DefaultMaterial->GetObjectPath() : std::string("None");
				ImGui::TextUnformatted(MaterialPath.c_str());
			}
			ImGui::EndTable();
		}
		ImGui::EndChild();
	}
	auto FStaticMeshEditor::OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void
	{
		if (auto Node = MeshPreviews.extract(std::string(Source)); !Node.empty()) { Node.key() = Destination; MeshPreviews.insert(std::move(Node)); }
	}

}
