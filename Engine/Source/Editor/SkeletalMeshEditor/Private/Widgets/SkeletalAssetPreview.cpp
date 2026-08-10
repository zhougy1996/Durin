#include "Widgets/SkeletalAssetPreview.h"

#include "Animation/AnimationClip.h"
#include "Client/ViewportClient.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Actor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "Mona/SceneViewport.h"
#include "MonaImGui.h"
#include "Preview/PreviewScene.h"
#include "SceneView.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "Widgets/MViewport.h"

namespace Durin
{
	namespace
	{
		constexpr double FieldOfViewDegrees = 42.0;
		constexpr double MinimumDistance = 0.05;
		constexpr double MaximumDistance = 1000000.0;

		auto RotationFromForward(const FVector3& Direction) -> FQuat
		{
			const FVector3 To = Math::Normalize(Direction);
			const double Dot = Math::Dot(FVectorConstants::Forward, To);
			if (Dot > 1.0 - 1.0e-8) return FQuatConstants::Identity;
			if (Dot < -1.0 + 1.0e-8)
				return Math::MakeQuaternionFromAxisAngleRadians(Math::Pi<double>(), FVectorConstants::Up);
			const FVector3 Cross = Math::Cross(FVectorConstants::Forward, To);
			return Math::Normalize(FQuat(1.0 + Dot, Cross.x, Cross.y, Cross.z));
		}
	}

	auto FSkeletalAssetPreviewController::FrameBounds(const FBox& Bounds) -> void
	{
		const FVector3 Extent = Bounds.GetExtent();
		const double MaxExtent = std::max({Extent.x, Extent.y, Extent.z});
		if (!Bounds.bIsValid || !std::isfinite(MaxExtent) || MaxExtent <= 0.0) return;
		FramedBounds = Bounds;
		Target = Bounds.GetCenter();
		Distance = std::clamp(MaxExtent * 1.35
			/ std::tan(Math::DegreesToRadians(FieldOfViewDegrees * 0.5)),
			MinimumDistance, MaximumDistance);
		YawDegrees = -45.0;
		PitchDegrees = 25.0;
	}

	auto FSkeletalAssetPreviewController::Orbit(float DeltaX, float DeltaY) -> void
	{
		YawDegrees = std::remainder(YawDegrees + static_cast<double>(DeltaX) * 0.25, 360.0);
		PitchDegrees = std::clamp(PitchDegrees + static_cast<double>(DeltaY) * 0.25, -85.0, 85.0);
	}

	auto FSkeletalAssetPreviewController::Pan(float DeltaX, float DeltaY) -> void
	{
		const double Yaw = Math::DegreesToRadians(YawDegrees);
		const FVector3 Right(-std::sin(Yaw), std::cos(Yaw), 0.0);
		Target += Right * (-static_cast<double>(DeltaX) * Distance * 0.0015);
		Target += FVectorConstants::Up * (static_cast<double>(DeltaY) * Distance * 0.0015);
	}

	auto FSkeletalAssetPreviewController::Zoom(float WheelDelta) -> void
	{
		Distance = std::clamp(Distance * std::pow(0.85, static_cast<double>(WheelDelta)),
			MinimumDistance, MaximumDistance);
	}

	auto FSkeletalAssetPreviewController::Reset() -> void
	{
		if (FramedBounds.bIsValid) FrameBounds(FramedBounds);
	}

	class FSkeletalPreviewViewportClient final : public FViewportClient
	{
	public:
		auto SetEnabled(bool bInEnabled) -> void { bEnabled = bInEnabled; }
		auto GetController() -> FSkeletalAssetPreviewController& { return Controller; }
		auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override
		{
			if (!bEnabled || Width == 0 || Height == 0) return false;
			const double Yaw = Math::DegreesToRadians(Controller.GetYawDegrees());
			const double Pitch = Math::DegreesToRadians(Controller.GetPitchDegrees());
			const double CosPitch = std::cos(Pitch);
			const FVector3 Eye = Controller.GetTarget() + FVector3(
				CosPitch * std::cos(Yaw), CosPitch * std::sin(Yaw), std::sin(Pitch))
				* Controller.GetDistance();
			const FVector3 Forward = Math::Normalize(Controller.GetTarget() - Eye);
			const FVector3 Right = Math::Normalize(Math::Cross(FVectorConstants::Up, Forward));
			const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));
			OutView = {};
			OutView.ViewportWidth = Width; OutView.ViewportHeight = Height; OutView.ViewLocation = Eye;
			OutView.ViewMatrix[0][0] = Forward.x; OutView.ViewMatrix[1][0] = Forward.y;
			OutView.ViewMatrix[2][0] = Forward.z; OutView.ViewMatrix[3][0] = -Math::Dot(Forward, Eye);
			OutView.ViewMatrix[0][1] = Right.x; OutView.ViewMatrix[1][1] = Right.y;
			OutView.ViewMatrix[2][1] = Right.z; OutView.ViewMatrix[3][1] = -Math::Dot(Right, Eye);
			OutView.ViewMatrix[0][2] = Up.x; OutView.ViewMatrix[1][2] = Up.y;
			OutView.ViewMatrix[2][2] = Up.z; OutView.ViewMatrix[3][2] = -Math::Dot(Up, Eye);
			const float NearClip = static_cast<float>(std::max(0.001, Controller.GetDistance() * 0.001));
			const float FarClip = static_cast<float>(std::max(100.0, Controller.GetDistance() * 20.0));
			const float YScale = 1.0f / std::tan(static_cast<float>(Math::DegreesToRadians(FieldOfViewDegrees * 0.5)));
			const float XScale = YScale / std::max(static_cast<float>(Width) / static_cast<float>(Height), 0.001f);
			const float DepthScale = FarClip / (FarClip - NearClip);
			OutView.ProjectionMatrix = FMatrix(0.0f);
			OutView.ProjectionMatrix[1][0] = XScale; OutView.ProjectionMatrix[2][1] = -YScale;
			OutView.ProjectionMatrix[0][2] = DepthScale;
			OutView.ProjectionMatrix[3][2] = -NearClip * DepthScale;
			OutView.ProjectionMatrix[0][3] = 1.0f;
			OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
			return true;
		}
	private:
		FSkeletalAssetPreviewController Controller;
		bool bEnabled = false;
	};

	class FSkeletalAssetPreview::FImpl
	{
	public:
		explicit FImpl(uint64 PreviewId)
		{
			PreviewScene = std::make_unique<FPreviewScene>(FName(std::format("SkeletalAssetPreview_{}", PreviewId)));
			if (!PreviewScene->IsAvailable()) { Error = PreviewScene->GetDiagnostic(); return; }
			AActor* Actor = PreviewScene->GetWorld()->SpawnActor<AActor>(FName(std::format("SkeletalAssetPreviewActor_{}", PreviewId)));
			if (Actor) Actor->SetActorTickEnabled(true);
			Component = Actor ? Cast<DSkeletalMeshComponent>(Actor->AddInstanceComponent(
				DSkeletalMeshComponent::StaticClass(), "PreviewMesh")) : nullptr;
			AActor* LightActor = PreviewScene->GetWorld()->SpawnActor<AActor>(FName(std::format("SkeletalAssetPreviewLightActor_{}", PreviewId)));
			Light = LightActor ? Cast<DDirectionalLightComponent>(LightActor->AddInstanceComponent(
				DDirectionalLightComponent::StaticClass(), "PreviewLight")) : nullptr;
			if (!Component || !Light) { Error = "The skeletal preview components could not be created."; return; }
			Light->SetWorldRotation(RotationFromForward(FVector3(-2.6, 2.6, -2.4)));
			ViewportClient = std::make_unique<FSkeletalPreviewViewportClient>();
			ViewportWidget = std::make_shared<MViewport>();
			SceneViewport = std::make_shared<FSceneViewport>(ViewportClient.get(), ViewportWidget, PreviewScene->GetRenderScene());
			ViewportWidget->SetViewportInterface(SceneViewport);
			GEngine->RegisterAuxiliarySceneViewport(SceneViewport);
			PreviewScene->BeginPlay();
		}

		~FImpl()
		{
			if (Component) { std::string Ignored; Component->SetAnimationClip(nullptr, Ignored); Component->SetSkeletalMesh(nullptr, Ignored); }
			if (PreviewScene) PreviewScene->EndPlay();
			if (GEngine) GEngine->UnregisterAuxiliarySceneViewport(SceneViewport.get());
			SceneViewport.reset(); ViewportWidget.reset(); ViewportClient.reset(); PreviewScene.reset();
		}

		auto Bind(DSkeletalMesh* Mesh, DAnimationClip* Clip) -> bool
		{
			if (Mesh == CurrentMesh && Clip == CurrentClip) return Mesh != nullptr;
			std::string BindError;
			if (Component) Component->SetAnimationClip(nullptr, BindError);
			if (!Component || !Component->SetSkeletalMesh(Mesh, BindError)
				|| !Component->SetAnimationClip(Clip, BindError))
			{
				if (Component)
				{
					std::string Ignored;
					Component->SetAnimationClip(nullptr, Ignored);
					Component->SetSkeletalMesh(nullptr, Ignored);
				}
				Error = BindError.empty() ? "The mesh and clip are incompatible." : BindError;
				CurrentMesh = nullptr; CurrentClip = nullptr; return false;
			}
			CurrentMesh = Mesh; CurrentClip = Clip; Error.clear();
			if (Mesh && Mesh->GetSummary().LocalBounds.bIsValid)
				ViewportClient->GetController().FrameBounds(Mesh->GetSummary().LocalBounds.ToBox());
			return Mesh != nullptr;
		}

		auto Draw(DSkeletalMesh* Mesh, DAnimationClip* Clip, float Height) -> void
		{
			if (!ImGui::BeginChild("SkeletalPreview", ImVec2(0.0f, Height), ImGuiChildFlags_Borders)) { ImGui::EndChild(); return; }
			if (!Bind(Mesh, Clip))
			{
				SetVisible(false); ImGui::TextWrapped("Preview unavailable: %s", Error.c_str()); ImGui::EndChild(); return;
			}
			Mesh->InitResources();
			PreviewScene->Tick(std::max(0.0f, ImGui::GetIO().DeltaTime));
			SetVisible(true);
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			ViewportWidget->SetDesiredSize({std::max(8.0f, Available.x), std::max(8.0f, Available.y)});
			ViewportWidget->Draw();
			if (ViewportWidget->WasTextureDrawn())
			{
				const bool Hovered = ImGui::IsItemHovered(); const ImGuiIO& IO = ImGui::GetIO();
				if (Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) bOrbiting = true;
				if (Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) bPanning = true;
				if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) bOrbiting = false;
				if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) bPanning = false;
				if (bOrbiting) ViewportClient->GetController().Orbit(IO.MouseDelta.x, IO.MouseDelta.y);
				if (bPanning) ViewportClient->GetController().Pan(IO.MouseDelta.x, IO.MouseDelta.y);
				if (Hovered && IO.MouseWheel != 0.0f) ViewportClient->GetController().Zoom(IO.MouseWheel);
			}
			ImGui::EndChild();
		}

		auto SetVisible(bool Visible) -> void { if (ViewportClient) ViewportClient->SetEnabled(Visible); }
		auto ComponentOrNull() const -> DSkeletalMeshComponent* { return Component; }
		std::unique_ptr<FPreviewScene> PreviewScene;
		std::unique_ptr<FSkeletalPreviewViewportClient> ViewportClient;
		std::shared_ptr<MViewport> ViewportWidget; std::shared_ptr<FSceneViewport> SceneViewport;
		TObjectPtr<DSkeletalMeshComponent> Component; TObjectPtr<DDirectionalLightComponent> Light;
		DSkeletalMesh* CurrentMesh = nullptr; DAnimationClip* CurrentClip = nullptr;
		bool bOrbiting = false; bool bPanning = false; std::string Error;
	};

	FSkeletalAssetPreview::FSkeletalAssetPreview(uint64 Id) : Impl(std::make_unique<FImpl>(Id)) {}
	FSkeletalAssetPreview::~FSkeletalAssetPreview() = default;
	auto FSkeletalAssetPreview::Draw(DSkeletalMesh* Mesh, DAnimationClip* Clip, float Height) -> void { Impl->Draw(Mesh, Clip, Height); }
	auto FSkeletalAssetPreview::SetVisible(bool Visible) -> void { Impl->SetVisible(Visible); }
	auto FSkeletalAssetPreview::ResetView() -> void { if (Impl->ViewportClient) Impl->ViewportClient->GetController().Reset(); }
	auto FSkeletalAssetPreview::SetWireframe(bool Wireframe) -> void { if (Impl->ViewportClient) { auto Settings = Impl->ViewportClient->GetViewSettings(); Settings.RasterMode = Wireframe ? ERasterMode::Wireframe : ERasterMode::Solid; Impl->ViewportClient->SetViewSettings(Settings); } }
	auto FSkeletalAssetPreview::IsWireframe() const -> bool { return Impl->ViewportClient && Impl->ViewportClient->GetViewSettings().RasterMode == ERasterMode::Wireframe; }
	auto FSkeletalAssetPreview::SetLit(bool Lit) -> void { if (Impl->ViewportClient) { auto Settings = Impl->ViewportClient->GetViewSettings(); Settings.RenderMode = Lit ? ERenderMode::Lit : ERenderMode::Unlit; Impl->ViewportClient->SetViewSettings(Settings); } }
	auto FSkeletalAssetPreview::IsLit() const -> bool { return !Impl->ViewportClient || Impl->ViewportClient->GetViewSettings().RenderMode == ERenderMode::Lit; }
	auto FSkeletalAssetPreview::Play(std::string& Error) -> bool { return Impl->ComponentOrNull() && Impl->ComponentOrNull()->Play(Error); }
	auto FSkeletalAssetPreview::Pause() -> void { if (Impl->ComponentOrNull()) Impl->ComponentOrNull()->Pause(); }
	auto FSkeletalAssetPreview::ResetPlayback(std::string& Error) -> bool { return Impl->ComponentOrNull() && Impl->ComponentOrNull()->Stop(Error); }
	auto FSkeletalAssetPreview::Seek(float Time, std::string& Error) -> bool { return Impl->ComponentOrNull() && Impl->ComponentOrNull()->Seek(Time, Error); }
	auto FSkeletalAssetPreview::SetLooping(bool Looping) -> void { if (Impl->ComponentOrNull()) Impl->ComponentOrNull()->SetLooping(Looping); }
	auto FSkeletalAssetPreview::SetPlayRate(float Rate, std::string& Error) -> bool { return Impl->ComponentOrNull() && Impl->ComponentOrNull()->SetPlayRate(Rate, Error); }
	auto FSkeletalAssetPreview::IsPlaying() const -> bool { return Impl->ComponentOrNull() && Impl->ComponentOrNull()->IsPlaying(); }
	auto FSkeletalAssetPreview::IsLooping() const -> bool { return Impl->ComponentOrNull() && Impl->ComponentOrNull()->IsLooping(); }
	auto FSkeletalAssetPreview::GetPlayRate() const -> float { return Impl->ComponentOrNull() ? Impl->ComponentOrNull()->GetPlayRate() : 1.0f; }
	auto FSkeletalAssetPreview::GetPlaybackTime() const -> float { return Impl->ComponentOrNull() ? Impl->ComponentOrNull()->GetPlaybackTimeSeconds() : 0.0f; }
}
