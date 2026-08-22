#include "Widgets/SkeletalAssetPreview.h"

#include "Animation/AnimationClip.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Actor.h"
#include "MonaImGui.h"
#include "Preview/AssetPreviewHost.h"
#include "Preview/OrbitAssetPreview.h"
#include "SkeletalMesh/SkeletalMesh.h"

namespace Durin::Editor::SkeletalMesh
{
	class FSkeletalAssetPreview::FImpl
	{
	public:
		explicit FImpl(uint64 PreviewId)
		{
			auto Client = std::make_unique<::Durin::Editor::FOrbitAssetPreviewViewportClient>();
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
			::Durin::Editor::FAssetPreviewViewportInput Input;
			if (Host->DrawViewport(Available.x, Available.y, &Input))
				ViewportClient->GetController().ApplyInput(Input);
			ImGui::EndChild();
		}

		auto SetVisible(bool Visible) -> void { if (Host) Host->SetVisible(Visible); }
		auto ComponentOrNull() const -> DSkeletalMeshComponent* { return Component; }
		std::unique_ptr<::Durin::Editor::FAssetPreviewHost> Host;
		::Durin::Editor::FOrbitAssetPreviewViewportClient* ViewportClient = nullptr;
		TObjectPtr<DSkeletalMeshComponent> Component;
		DSkeletalMesh* CurrentMesh = nullptr; DAnimationClip* CurrentClip = nullptr;
		std::string Error;
	};

	FSkeletalAssetPreview::FSkeletalAssetPreview(uint64 Id) : Impl(std::make_unique<FImpl>(Id)) {}
	FSkeletalAssetPreview::~FSkeletalAssetPreview() = default;
	auto FSkeletalAssetPreview::Draw(DSkeletalMesh* Mesh, DAnimationClip* Clip, float Height) -> void { Impl->Draw(Mesh, Clip, Height); }
	auto FSkeletalAssetPreview::SetVisible(bool Visible) -> void { Impl->SetVisible(Visible); }
	auto FSkeletalAssetPreview::ResetView() -> void { if (Impl->ViewportClient) Impl->ViewportClient->GetController().Reset(); }
	auto FSkeletalAssetPreview::SetWireframe(bool Wireframe) -> void { if (Impl->ViewportClient) Impl->ViewportClient->SetWireframe(Wireframe); }
	auto FSkeletalAssetPreview::IsWireframe() const -> bool { return Impl->ViewportClient && Impl->ViewportClient->IsWireframe(); }
	auto FSkeletalAssetPreview::SetLit(bool Lit) -> void { if (Impl->ViewportClient) Impl->ViewportClient->SetLit(Lit); }
	auto FSkeletalAssetPreview::IsLit() const -> bool { return !Impl->ViewportClient || Impl->ViewportClient->IsLit(); }
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
