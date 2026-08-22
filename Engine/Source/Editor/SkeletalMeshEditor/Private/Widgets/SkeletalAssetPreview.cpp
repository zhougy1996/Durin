#include "Widgets/SkeletalAssetPreview.h"

#include "Animation/AnimationClip.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Actor.h"
#include "Math/Operations.h"
#include "MonaImGui.h"
#include "Preview/AssetPreviewHost.h"
#include "SceneView.h"
#include "SceneViewProjection.h"
#include "SkeletalMesh/SkeletalMesh.h"

namespace Durin::Editor::SkeletalMesh
{
	namespace
	{
		constexpr double FieldOfViewDegrees = 42.0;
		constexpr double MinimumDistance = 0.05;
		constexpr double MaximumDistance = 1000000.0;

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

	class FSkeletalPreviewViewportClient final : public ::Durin::Editor::FAssetPreviewViewportClient
	{
	public:
		auto GetController() -> FSkeletalAssetPreviewController& { return Controller; }
		auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override
		{
			if (!IsPreviewEnabled() || Width == 0 || Height == 0) return false;
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
			if (!SceneViewProjection::BuildPerspectiveProjection(FieldOfViewDegrees,
				static_cast<double>(Width) / Height, NearClip, FarClip,
				ESceneDepthConvention::ReversedZ, OutView.ProjectionMatrix)) return false;
			OutView.NearClipDistance = NearClip;
			OutView.FarClipDistance = FarClip;
			OutView.DepthConvention = ESceneDepthConvention::ReversedZ;
			OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
			return true;
		}
	private:
		FSkeletalAssetPreviewController Controller;
	};

	class FSkeletalAssetPreview::FImpl
	{
	public:
		explicit FImpl(uint64 PreviewId)
		{
			auto Client = std::make_unique<FSkeletalPreviewViewportClient>();
			ViewportClient = Client.get();
			Host = std::make_unique<::Durin::Editor::FAssetPreviewHost>(
				::Durin::Editor::FAssetPreviewHostConfig{
					.SceneName = FName(std::format("SkeletalAssetPreview_{}", PreviewId)),
					.ContentActorName = FName(std::format("SkeletalAssetPreviewActor_{}", PreviewId)),
					.LightActorName = FName(std::format("SkeletalAssetPreviewLightActor_{}", PreviewId)),
					.bBeginPlay = true},
				std::move(Client));
			if (!Host->IsAvailable()) { Error = Host->GetDiagnostic(); return; }
			AActor* Actor = Host->GetContentActor();
			if (Actor) Actor->SetActorTickEnabled(true);
			Component = Actor ? Cast<DSkeletalMeshComponent>(Actor->AddInstanceComponent(
				DSkeletalMeshComponent::StaticClass(), "PreviewMesh")) : nullptr;
			if (!Component) { Error = "The skeletal preview component could not be created."; return; }
		}

		~FImpl()
		{
			if (Component) { std::string Ignored; Component->SetAnimationClip(nullptr, Ignored); Component->SetSkeletalMesh(nullptr, Ignored); }
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
			Host->Tick(ImGui::GetIO().DeltaTime);
			SetVisible(true);
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			if (Host->DrawViewport(Available.x, Available.y))
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

		auto SetVisible(bool Visible) -> void { if (Host) Host->SetVisible(Visible); }
		auto ComponentOrNull() const -> DSkeletalMeshComponent* { return Component; }
		std::unique_ptr<::Durin::Editor::FAssetPreviewHost> Host;
		FSkeletalPreviewViewportClient* ViewportClient = nullptr;
		TObjectPtr<DSkeletalMeshComponent> Component;
		DSkeletalMesh* CurrentMesh = nullptr; DAnimationClip* CurrentClip = nullptr;
		bool bOrbiting = false; bool bPanning = false; std::string Error;
	};

	FSkeletalAssetPreview::FSkeletalAssetPreview(uint64 Id) : Impl(std::make_unique<FImpl>(Id)) {}
	FSkeletalAssetPreview::~FSkeletalAssetPreview() = default;
	auto FSkeletalAssetPreview::Draw(DSkeletalMesh* Mesh, DAnimationClip* Clip, float Height) -> void { Impl->Draw(Mesh, Clip, Height); }
	auto FSkeletalAssetPreview::SetVisible(bool Visible) -> void { Impl->SetVisible(Visible); }
	auto FSkeletalAssetPreview::ResetView() -> void { if (Impl->ViewportClient) Impl->ViewportClient->GetController().Reset(); }
	auto FSkeletalAssetPreview::SetWireframe(bool Wireframe) -> void { if (Impl->ViewportClient) { auto Settings = Impl->ViewportClient->GetViewSettings(); Settings.Mode.RasterMode = Wireframe ? ERasterMode::Wireframe : ERasterMode::Solid; Impl->ViewportClient->SetViewSettings(Settings); } }
	auto FSkeletalAssetPreview::IsWireframe() const -> bool { return Impl->ViewportClient && Impl->ViewportClient->GetViewSettings().Mode.RasterMode == ERasterMode::Wireframe; }
	auto FSkeletalAssetPreview::SetLit(bool Lit) -> void { if (Impl->ViewportClient) { auto Settings = Impl->ViewportClient->GetViewSettings(); Settings.Mode.RenderMode = Lit ? ERenderMode::Lit : ERenderMode::Unlit; Impl->ViewportClient->SetViewSettings(Settings); } }
	auto FSkeletalAssetPreview::IsLit() const -> bool { return !Impl->ViewportClient || Impl->ViewportClient->GetViewSettings().Mode.RenderMode == ERenderMode::Lit; }
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
