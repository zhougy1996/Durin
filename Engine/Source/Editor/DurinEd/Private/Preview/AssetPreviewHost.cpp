#include "Preview/StudioLighting.h"
#include "Preview/AssetPreviewHost.h"

#include "Client/SceneViewport.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/Actor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "MonaImGui.h"
#include "Preview/PreviewScene.h"
#include "Rendering/SceneInterface.h"
#include "Widgets/MViewport.h"

namespace Durin::Editor
{
	namespace
	{
		constexpr double RotationTolerance = 1.0e-8;

		auto RotationFromForward(const FVector3& Direction) -> FQuat
		{
			const FVector3 To = Math::Normalize(Direction);
			const double Dot = Math::Dot(FVectorConstants::Forward, To);
			if (Dot > 1.0 - RotationTolerance) return FQuatConstants::Identity;
			if (Dot < -1.0 + RotationTolerance)
				return Math::MakeQuaternionFromAxisAngleRadians(Math::Pi<double>(), FVectorConstants::Up);
			const FVector3 Cross = Math::Cross(FVectorConstants::Forward, To);
			return Math::Normalize(FQuat(1.0 + Dot, Cross.x, Cross.y, Cross.z));
		}
	}

	class FAssetPreviewHost::FImpl
	{
	public:
		FImpl(FAssetPreviewHostConfig InConfig,
			std::unique_ptr<FAssetPreviewViewportClient> InViewportClient)
			: Config(std::move(InConfig)), ViewportClient(std::move(InViewportClient))
		{
			if (!ViewportClient)
			{
				Error = "The asset preview viewport client is unavailable.";
				return;
			}
			static std::atomic<uint64> SceneSerial = 0;
			const auto Name = Config.FeatureLevel ? FName(std::format("{}_Feature{}_{}", Config.SceneName.ToString(),
				static_cast<uint8>(*Config.FeatureLevel), ++SceneSerial)) : Config.SceneName;
			PreviewScene = std::make_unique<FPreviewScene>(Name, Config.FeatureLevel);
			if (!PreviewScene->IsAvailable())
			{
				Error = PreviewScene->GetDiagnostic();
				return;
			}

			(void)Editor::AddStudioSkyLight(*PreviewScene->GetWorld()->GetCurrentLevel());
			ContentActor = PreviewScene->GetWorld()->SpawnActor<AActor>(Config.ContentActorName);
			AActor* LightActor = PreviewScene->GetWorld()->SpawnActor<AActor>(Config.LightActorName);
			Light = LightActor
				? Cast<DDirectionalLightComponent>(LightActor->AddInstanceComponent(
					DDirectionalLightComponent::StaticClass(), Config.LightComponentName))
				: nullptr;
			if (!ContentActor || !Light)
			{
				Error = "The asset preview actors could not be created.";
				return;
			}
			Light->SetWorldRotation(RotationFromForward(FVector3(-2.6, 2.6, -2.4)));

			ViewportWidget = std::make_shared<MViewport>();
			SceneViewport = FSceneViewport::CreateOffscreen(
				ViewportClient.get(), PreviewScene->GetRenderScene());
			if (!SceneViewport)
			{
				Error = "The asset preview viewport could not be created.";
				return;
			}
			ViewportWidget->SetDisplaySource(SceneViewport);
			GEngine->RegisterAuxiliarySceneViewport(SceneViewport);
			bViewportRegistered = true;
			if (Config.bBeginPlay)
			{
				PreviewScene->BeginPlay();
				bPlayBegun = true;
			}
		}

		~FImpl()
		{
			if (bPlayBegun && PreviewScene) PreviewScene->EndPlay();
			if (bViewportRegistered && GEngine)
				GEngine->UnregisterAuxiliarySceneViewport(SceneViewport.get());
			SceneViewport.reset();
			ViewportWidget.reset();
			ViewportClient.reset();
			PreviewScene.reset();
		}

		FAssetPreviewHostConfig Config;
		std::unique_ptr<FPreviewScene> PreviewScene;
		std::unique_ptr<FAssetPreviewViewportClient> ViewportClient;
		std::shared_ptr<MViewport> ViewportWidget;
		std::shared_ptr<FSceneViewport> SceneViewport;
		TObjectPtr<AActor> ContentActor;
		TObjectPtr<DDirectionalLightComponent> Light;
		std::string Error;
		bool bViewportRegistered = false;
		bool bPlayBegun = false;
		bool bLeftDragging = false;
		bool bMiddleDragging = false;
	};

	FAssetPreviewHost::FAssetPreviewHost(
		FAssetPreviewHostConfig Config,
		std::unique_ptr<FAssetPreviewViewportClient> ViewportClient)
		: Impl(std::make_unique<FImpl>(std::move(Config), std::move(ViewportClient)))
	{
	}

	FAssetPreviewHost::~FAssetPreviewHost() = default;

	auto FAssetPreviewHost::IsAvailable() const -> bool
	{
		return Impl->Error.empty() && Impl->PreviewScene && Impl->ViewportClient
			&& Impl->ViewportWidget && Impl->SceneViewport && Impl->ContentActor && Impl->Light;
	}

	auto FAssetPreviewHost::GetDiagnostic() const -> const std::string& { return Impl->Error; }
	auto FAssetPreviewHost::GetContentActor() const -> AActor* { return Impl->ContentActor.Get(); }
	auto FAssetPreviewHost::GetFeatureLevel() const -> ERHIFeatureLevel
	{
		return Impl->PreviewScene && Impl->PreviewScene->GetRenderScene()
			? Impl->PreviewScene->GetRenderScene()->GetFeatureLevel() : FSceneInterface::GetDefaultFeatureLevel();
	}

	auto FAssetPreviewHost::SetFeatureLevel(ERHIFeatureLevel FeatureLevel) -> bool
	{
		if (!IsAvailable() || FeatureLevel > FSceneInterface::GetDefaultFeatureLevel()) return false;
		if (GetFeatureLevel() == FeatureLevel) return true;
		auto Config = Impl->Config;
		Config.FeatureLevel = FeatureLevel;
		auto Client = std::move(Impl->ViewportClient);
		Impl.reset();
		Impl = std::make_unique<FImpl>(std::move(Config), std::move(Client));
		return IsAvailable();
	}

	auto FAssetPreviewHost::SetVisible(bool bVisible) -> void
	{
		if (Impl->ViewportClient) Impl->ViewportClient->SetPreviewEnabled(bVisible);
		if (!bVisible)
		{
			Impl->bLeftDragging = false;
			Impl->bMiddleDragging = false;
		}
	}
	auto FAssetPreviewHost::Tick(float DeltaSeconds) -> void
	{
		if (Impl->PreviewScene) Impl->PreviewScene->Tick(std::max(0.0f, DeltaSeconds));
	}
	auto FAssetPreviewHost::DrawViewport(float Width, float Height,
		FAssetPreviewViewportInput* OutInput) -> bool
	{
		if (OutInput) *OutInput = {};
		if (!IsAvailable()) return false;
		Impl->ViewportWidget->SetDesiredSize({std::max(8.0f, Width), std::max(8.0f, Height)});
		Impl->ViewportWidget->Draw();
		if (!Impl->ViewportWidget->WasTextureDrawn()) return false;
		if (OutInput)
		{
			OutInput->bHovered = ImGui::IsItemHovered();
			if (OutInput->bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				Impl->bLeftDragging = true;
			if (OutInput->bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
				Impl->bMiddleDragging = true;
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) Impl->bLeftDragging = false;
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) Impl->bMiddleDragging = false;

			const ImGuiIO& IO = ImGui::GetIO();
			OutInput->MouseDeltaX = IO.MouseDelta.x;
			OutInput->MouseDeltaY = IO.MouseDelta.y;
			OutInput->MouseWheel = OutInput->bHovered ? IO.MouseWheel : 0.0f;
			OutInput->bLeftDragging = Impl->bLeftDragging;
			OutInput->bMiddleDragging = Impl->bMiddleDragging;
		}
		return true;
	}
}
