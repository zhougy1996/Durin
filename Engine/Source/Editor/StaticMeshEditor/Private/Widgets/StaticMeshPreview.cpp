#include "Widgets/StaticMeshPreview.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "Math/Operations.h"
#include "MonaImGui.h"
#include "Preview/AssetPreviewHost.h"
#include "SceneView.h"
#include "SceneViewProjection.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::StaticMesh
{
	namespace
	{
		constexpr double FieldOfViewDegrees = 42.0;
		constexpr double MinimumDistance = 0.05;
		constexpr double MaximumDistance = 1000000.0;
		constexpr double RotationSensitivity = 0.25;
		constexpr double PanSensitivity = 0.0015;
		constexpr double ZoomScale = 0.85;

		auto MaxExtent(const FBox& Bounds) -> double
		{
			const FVector3 Extent = Bounds.GetExtent();
			return std::max({Extent.x, Extent.y, Extent.z});
		}
	}

	auto FStaticMeshPreviewController::FrameBounds(const FBox& Bounds) -> void
	{
		if (!Bounds.bIsValid || !std::isfinite(MaxExtent(Bounds)) || MaxExtent(Bounds) <= 0.0) return;
		FramedBounds = Bounds;
		Target = Bounds.GetCenter();
		const double HalfFov = Math::DegreesToRadians(FieldOfViewDegrees * 0.5);
		Distance = std::clamp(MaxExtent(Bounds) * 1.35 / std::tan(HalfFov), MinimumDistance, MaximumDistance);
		YawDegrees = -45.0;
		PitchDegrees = 25.0;
	}

	auto FStaticMeshPreviewController::Orbit(float DeltaX, float DeltaY) -> void
	{
		YawDegrees = std::remainder(YawDegrees + static_cast<double>(DeltaX) * RotationSensitivity, 360.0);
		PitchDegrees = std::clamp(PitchDegrees + static_cast<double>(DeltaY) * RotationSensitivity, -85.0, 85.0);
	}

	auto FStaticMeshPreviewController::Pan(float DeltaX, float DeltaY) -> void
	{
		const double Yaw = Math::DegreesToRadians(YawDegrees);
		const FVector3 Right(-std::sin(Yaw), std::cos(Yaw), 0.0);
		const FVector3 Up = FVectorConstants::Up;
		const double Scale = Distance * PanSensitivity;
		Target += Right * (-static_cast<double>(DeltaX) * Scale);
		Target += Up * (static_cast<double>(DeltaY) * Scale);
	}

	auto FStaticMeshPreviewController::Zoom(float WheelDelta) -> void
	{
		Distance = std::clamp(
			Distance * std::pow(ZoomScale, static_cast<double>(WheelDelta)),
			MinimumDistance, MaximumDistance);
	}

	auto FStaticMeshPreviewController::Reset() -> void
	{
		if (FramedBounds.bIsValid) FrameBounds(FramedBounds);
	}

	class FStaticMeshPreviewViewportClient final : public ::Durin::Editor::FAssetPreviewViewportClient
	{
	public:
		auto GetController() -> FStaticMeshPreviewController& { return Controller; }

		auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override
		{
			if (!IsPreviewEnabled() || Width == 0 || Height == 0) return false;

			const double Yaw = Math::DegreesToRadians(Controller.GetYawDegrees());
			const double Pitch = Math::DegreesToRadians(Controller.GetPitchDegrees());
			const double CosPitch = std::cos(Pitch);
			const FVector3 Offset(
				CosPitch * std::cos(Yaw),
				CosPitch * std::sin(Yaw),
				std::sin(Pitch));
			const FVector3 Eye = Controller.GetTarget() + Offset * Controller.GetDistance();
			const FVector3 Forward = Math::Normalize(Controller.GetTarget() - Eye);
			const FVector3 Right = Math::Normalize(Math::Cross(FVectorConstants::Up, Forward));
			const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));

			OutView = {};
			OutView.ViewportWidth = Width;
			OutView.ViewportHeight = Height;
			OutView.ViewLocation = Eye;
			OutView.ViewMatrix[0][0] = Forward.x;
			OutView.ViewMatrix[1][0] = Forward.y;
			OutView.ViewMatrix[2][0] = Forward.z;
			OutView.ViewMatrix[3][0] = -Math::Dot(Forward, Eye);
			OutView.ViewMatrix[0][1] = Right.x;
			OutView.ViewMatrix[1][1] = Right.y;
			OutView.ViewMatrix[2][1] = Right.z;
			OutView.ViewMatrix[3][1] = -Math::Dot(Right, Eye);
			OutView.ViewMatrix[0][2] = Up.x;
			OutView.ViewMatrix[1][2] = Up.y;
			OutView.ViewMatrix[2][2] = Up.z;
			OutView.ViewMatrix[3][2] = -Math::Dot(Up, Eye);

			const float NearClip = static_cast<float>(std::max(0.001, Controller.GetDistance() * 0.001));
			const float FarClip = static_cast<float>(std::max(100.0, Controller.GetDistance() * 20.0));
			const float AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
			if (!SceneViewProjection::BuildPerspectiveProjection(FieldOfViewDegrees,
				AspectRatio, NearClip, FarClip, ESceneDepthConvention::ReversedZ,
				OutView.ProjectionMatrix)) return false;
			OutView.NearClipDistance = NearClip;
			OutView.FarClipDistance = FarClip;
			OutView.DepthConvention = ESceneDepthConvention::ReversedZ;
			OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
			return true;
		}

	private:
		FStaticMeshPreviewController Controller;
	};

	class FStaticMeshPreview::FImpl
	{
	public:
		explicit FImpl(uint64 PreviewId)
		{
			auto Client = std::make_unique<FStaticMeshPreviewViewportClient>();
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
			FSceneViewSettings Settings = ViewportClient->GetViewSettings();
			Settings.Mode.RasterMode = bWireframe ? ERasterMode::Wireframe : ERasterMode::Solid;
			ViewportClient->SetViewSettings(Settings);
		}

		auto IsWireframe() const -> bool
		{
			return ViewportClient != nullptr && ViewportClient->GetViewSettings().Mode.RasterMode == ERasterMode::Wireframe;
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
			if (Host->DrawViewport(Available.x, Available.y)) UpdateInput();
			ImGui::EndChild();
		}

	private:
		auto UpdateInput() -> void
		{
			const bool bHovered = ImGui::IsItemHovered();
			const ImGuiIO& IO = ImGui::GetIO();
			if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) bOrbiting = true;
			if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) bPanning = true;
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) bOrbiting = false;
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) bPanning = false;
			if (bOrbiting) ViewportClient->GetController().Orbit(IO.MouseDelta.x, IO.MouseDelta.y);
			if (bPanning) ViewportClient->GetController().Pan(IO.MouseDelta.x, IO.MouseDelta.y);
			if (bHovered && IO.MouseWheel != 0.0f) ViewportClient->GetController().Zoom(IO.MouseWheel);
		}

		std::unique_ptr<::Durin::Editor::FAssetPreviewHost> Host;
		FStaticMeshPreviewViewportClient* ViewportClient = nullptr;
		TObjectPtr<DStaticMeshComponent> PreviewMesh;
		DStaticMesh* CurrentMesh = nullptr;
		uint64 CurrentRevision = 0;
		bool bOrbiting = false;
		bool bPanning = false;
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
