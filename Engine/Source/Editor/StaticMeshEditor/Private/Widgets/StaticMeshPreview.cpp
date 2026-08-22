#include "Widgets/StaticMeshPreview.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "MonaImGui.h"
#include "Preview/AssetPreviewHost.h"
#include "Preview/OrbitAssetPreview.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::StaticMesh
{
	class FStaticMeshPreview::FImpl
	{
	public:
		explicit FImpl(uint64 PreviewId)
		{
			auto Client = std::make_unique<::Durin::Editor::FOrbitAssetPreviewViewportClient>();
			ViewportClient = Client.get();
			Host = std::make_unique<::Durin::Editor::FAssetPreviewHost>(
				::Durin::Editor::FAssetPreviewHostConfig{
					.SceneName = FName(std::format("StaticMeshPreview_{}", PreviewId)),
					.ContentActorName = FName(std::format("StaticMeshPreviewActor_{}", PreviewId)),
					.LightActorName = FName(std::format("StaticMeshPreviewLightActor_{}", PreviewId))},
				std::move(Client));
			if (!Host->IsAvailable())
			{
				Error = Host->GetDiagnostic();
				return;
			}

			PreviewMesh = Host->GetContentActor()
				? Cast<DStaticMeshComponent>(Host->GetContentActor()->AddInstanceComponent(
					DStaticMeshComponent::StaticClass(), "PreviewMesh"))
				: nullptr;
			if (PreviewMesh == nullptr)
			{
				Error = "The StaticMesh preview component could not be created.";
				return;
			}
		}

		~FImpl()
		{
			if (PreviewMesh != nullptr) PreviewMesh->SetStaticMesh(nullptr);
		}

		auto SetVisible(bool bVisible) -> void
		{
			if (Host) Host->SetVisible(bVisible);
		}

		auto SetWireframe(bool bWireframe) -> void
		{
			if (ViewportClient == nullptr) return;
			ViewportClient->SetWireframe(bWireframe);
		}

		auto IsWireframe() const -> bool
		{
			return ViewportClient != nullptr && ViewportClient->IsWireframe();
		}

		auto ResetView() -> void
		{
			if (ViewportClient != nullptr) ViewportClient->GetController().Reset();
		}

		auto Draw(DStaticMesh* Mesh, uint64 Revision, float PanelHeight) -> void
		{
			if (!ImGui::BeginChild("StaticMeshPreview", ImVec2(0.0f, PanelHeight), ImGuiChildFlags_Borders))
			{
				ImGui::EndChild();
				return;
			}
			if (!Error.empty())
			{
				ImGui::TextWrapped("StaticMesh preview unavailable: %s", Error.c_str());
				ImGui::EndChild();
				return;
			}

			FStaticMeshRenderResourceStatus Status = Mesh ? Mesh->GetRenderResourceStatus() : FStaticMeshRenderResourceStatus{};
			const std::optional<FBox> Bounds = Mesh ? Mesh->GetLOD0LocalBounds() : std::nullopt;
			if (Mesh && Bounds && Status.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
			{
				Mesh->InitResources();
				Status = Mesh->GetRenderResourceStatus();
			}
			if (!Mesh || !Status.IsReady() || !Bounds)
			{
				if (PreviewMesh != nullptr && CurrentMesh != nullptr) PreviewMesh->SetStaticMesh(nullptr);
				CurrentMesh = nullptr;
				CurrentRevision = 0;
				SetVisible(false);
				const char* State = Status.Readiness == EStaticMeshRenderResourceReadiness::Queued ? "resources are still loading"
					: Status.Readiness == EStaticMeshRenderResourceReadiness::Failed ? "resource initialization failed"
					: "render data is unavailable";
				ImGui::TextWrapped("Preview unavailable: %s.", State);
				ImGui::EndChild();
				return;
			}

			if (CurrentMesh != Mesh || CurrentRevision != Revision)
			{
				PreviewMesh->SetStaticMesh(Mesh);
				ViewportClient->GetController().FrameBounds(*Bounds);
				CurrentMesh = Mesh;
				CurrentRevision = Revision;
			}
			SetVisible(true);
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			::Durin::Editor::FAssetPreviewViewportInput Input;
			if (Host->DrawViewport(Available.x, Available.y, &Input))
				ViewportClient->GetController().ApplyInput(Input);
			ImGui::EndChild();
		}

	private:
		std::unique_ptr<::Durin::Editor::FAssetPreviewHost> Host;
		::Durin::Editor::FOrbitAssetPreviewViewportClient* ViewportClient = nullptr;
		TObjectPtr<DStaticMeshComponent> PreviewMesh;
		DStaticMesh* CurrentMesh = nullptr;
		uint64 CurrentRevision = 0;
		std::string Error;
	};

	FStaticMeshPreview::FStaticMeshPreview(uint64 PreviewId)
		: Impl(std::make_unique<FImpl>(PreviewId))
	{
	}

	FStaticMeshPreview::~FStaticMeshPreview() = default;
	auto FStaticMeshPreview::SetVisible(bool bVisible) -> void { Impl->SetVisible(bVisible); }
	auto FStaticMeshPreview::Draw(DStaticMesh* Mesh, uint64 Revision, float PanelHeight) -> void { Impl->Draw(Mesh, Revision, PanelHeight); }
	auto FStaticMeshPreview::ResetView() -> void { Impl->ResetView(); }
	auto FStaticMeshPreview::SetWireframe(bool bWireframe) -> void { Impl->SetWireframe(bWireframe); }
	auto FStaticMeshPreview::IsWireframe() const -> bool { return Impl->IsWireframe(); }
}
