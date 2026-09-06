#include "Widgets/MStaticMeshInspector.h"

#include "Asset/Asset.h"
#include "Diagnostics/StaticMeshPayloadInspection.h"
#include "DObject/Object.h"
#include "Editor/WorkspaceManager.h"
#include "Editor/WorkspaceUI.h"
#include "Materials/MaterialInterface.h"
#include "MonaImGui.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
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

		FObjectPath AssetPath;
		std::string PathError;
		if (!FObjectPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			ErrorMessage = std::move(PathError);
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		DStaticMesh* Mesh = nullptr;
		const FAssetResult Result = LoadObject(AssetPath, Mesh);
		if (!Result || !Mesh)
		{
			ErrorMessage = Result ? "The selected asset is not a StaticMesh." : Result.Message;
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		FDocumentState State;
		State.Mesh = Mesh;
		State.PreviewId = Document.Id.Value;
		Documents.emplace(Document.ResourceId, std::move(State));
		return ::Durin::Editor::EDocumentOpenResult::Opened;
	}

	auto MStaticMeshInspector::ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void
	{
		FDocumentState* State = FindState(Document.ResourceId);
		DocumentModel.Activate(Document, State ? State->Mesh.Get() : nullptr);
	}

	auto MStaticMeshInspector::RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentCloseResult
	{
		if (FDocumentState* State = FindState(Document.ResourceId); State && State->Preview)
			State->Preview->SetVisible(false);
		Documents.erase(Document.ResourceId);
		DocumentModel.Close(Document.ResourceId);
		return ::Durin::Editor::EDocumentCloseResult::Closed;
	}

	auto MStaticMeshInspector::DrawWorkspace(bool bActive) -> bool
	{
		if (!bActive)
			for (auto& [ResourceId, State] : Documents)
				if (State.Preview) State.Preview->SetVisible(false);
		return DocumentModel.GetDocumentHost().DrawDocuments(
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

		if (DocumentModel.GetActiveResourceId() == Document.ResourceId && !ErrorMessage.empty())
			ImGui::TextWrapped("%s", ErrorMessage.c_str());
	}

	auto MStaticMeshInspector::DrawPreview(FDocumentState& State, float Height) -> void
	{
		DStaticMesh* Mesh = State.Mesh.Get();
		const FStaticMeshRenderResourceStatus Status = Mesh ? Mesh->GetRenderResourceStatus() : FStaticMeshRenderResourceStatus{};
		if (!State.Preview) State.Preview = std::make_unique<FStaticMeshPreview>(State.PreviewId);
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
			const char* CpuPhase = "Unloaded";
			switch (Inspection.CookedLoad.CpuPhase)
			{
			case ECookedMeshCpuPhase::Unloaded: break;
			case ECookedMeshCpuPhase::IoQueued: CpuPhase = "I/O queued"; break;
			case ECookedMeshCpuPhase::Reading: CpuPhase = "Reading"; break;
			case ECookedMeshCpuPhase::Decoding: CpuPhase = "Decoding"; break;
			case ECookedMeshCpuPhase::CpuReady: CpuPhase = "CPU ready"; break;
			case ECookedMeshCpuPhase::Failed: CpuPhase = "Failed; restore or recook cooked data before explicitly retrying loading"; break;
			case ECookedMeshCpuPhase::Cancelled: CpuPhase = "Cancelled; explicitly request loading when needed"; break;
			}
			ImGui::TextWrapped("Load phase: %s (generation %llu).", CpuPhase,
				static_cast<unsigned long long>(Inspection.CookedLoad.Generation));
			if (Inspection.Gpu.Readiness == EStaticMeshRenderResourceReadiness::Failed)
				ImGui::TextWrapped("GPU initialization failed. Retry resource initialization explicitly after correcting the resource failure.");
			ImGui::SeparatorText("Authored build operation");
			const auto& Operation = Inspection.Operation;
			if (!Operation.RequestId)
				ImGui::TextWrapped("Operation history unavailable (never observed or evicted). DDC origin, key, timings and persistence are unavailable.");
			else
			{
				const char* Phase = "Queued";
				if (Operation.Phase == EStaticMeshCompilationPhase::Building) Phase = "Building";
				else if (Operation.Phase == EStaticMeshCompilationPhase::Mailbox) Phase = "Awaiting publication";
				else if (Operation.Phase == EStaticMeshCompilationPhase::Terminal)
				{
					switch (Operation.Status)
					{
					case EStaticMeshCompilationStatus::Succeeded: Phase = "Succeeded"; break;
					case EStaticMeshCompilationStatus::Failed: Phase = "Failed"; break;
					case EStaticMeshCompilationStatus::Cancelled: Phase = "Cancelled"; break;
					case EStaticMeshCompilationStatus::Superseded: Phase = "Superseded"; break;
					}
				}
				ImGui::TextWrapped("Request %llu: %s. %s. This operation record does not establish current settings or payload coherence.",
					static_cast<unsigned long long>(Operation.RequestId), Phase,
					Inspection.bOperationSourceMatches ? "Source identity matches" : "Different source identity");
				ImGui::TextWrapped("Capture / worker / publication: %llu / %llu / %llu ns",
					static_cast<unsigned long long>(Operation.CaptureNanoseconds),
					static_cast<unsigned long long>(Operation.WorkerNanoseconds),
					static_cast<unsigned long long>(Operation.PublicationNanoseconds));
				for (const auto& [Name, Observation] : {std::pair{"Render", &Operation.Render}, std::pair{"Collision", &Operation.Collision}})
				{
					ImGui::TextWrapped("%s derived product: %s", Name, !*Observation ? "Observation unavailable" :
						(*Observation)->Origin == EStaticMeshBuildOrigin::CacheHit ? "Observed DDC hit" : "Observed rebuild");
					if (*Observation)
					{
						ImGui::TextWrapped("Key: %s", (*Observation)->DerivedDataKey.ToString().c_str());
						ImGui::TextWrapped("Payload %llu bytes; cache read / write %llu / %llu ns",
							static_cast<unsigned long long>((*Observation)->PayloadBytes),
							static_cast<unsigned long long>((*Observation)->CacheReadNanoseconds),
							static_cast<unsigned long long>((*Observation)->CacheWriteNanoseconds));
					}
				}
				if (!Operation.Message.empty()) ImGui::TextWrapped("%s", Operation.Message.c_str());
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
			DrawInfoRow("Builder / schema", std::format("{} / {}", Collision.BuilderVersion, Collision.SchemaVersion));
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
