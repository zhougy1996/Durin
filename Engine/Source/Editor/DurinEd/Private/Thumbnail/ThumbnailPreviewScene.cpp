#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Actors/SkyLightActor.h"
#include "Asset/Load.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Texture/TextureCube.h"
#include "DynamicRHI.h"
#include "Engine/Actor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "Preview/PreviewScene.h"
#include "RenderingThread.h"

namespace Durin
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
				return Math::MakeQuaternionFromAxisAngleRadians(
					Math::Pi<double>(), FVectorConstants::Up
				);
			const FVector3 Cross = Math::Cross(FVectorConstants::Forward, To);
			return Math::Normalize(FQuat(1.0 + Dot, Cross.x, Cross.y, Cross.z));
		}
	}

	struct FThumbnailPreviewScene::FImpl
	{
		std::unique_ptr<FPreviewScene> PreviewScene;
		TObjectPtr<AActor> Actor;
		std::string Error;
		explicit FImpl(const FThumbnailVisualContract& Contract)
		{
			checkf(IsInGameThread(), "Thumbnail scenes belong to the game thread.");
			if (GEngine == nullptr || GEngine->GetRendererModule() == nullptr
				|| GDynamicRHI == nullptr)
			{
				Error = "The renderer is not available.";
				return;
			}
			PreviewScene = std::make_unique<FPreviewScene>("RenderedAssetThumbnailPreview");
			if (!PreviewScene->IsAvailable())
			{
				Error = PreviewScene->GetDiagnostic();
				return;
			}
			FSoftObjectPath EnvironmentPath;
			if (!FSoftObjectPath::TryCreate("/Engine/Renderer/ThumbnailStudioCube.ThumbnailStudioCube", EnvironmentPath))
			{
				Error = "The thumbnail studio environment path is invalid.";
				return;
			}
			auto* EnvironmentCube = LoadObjectDetailed<DTextureCube>(EnvironmentPath).value_or(nullptr);
			auto* SkyLight = EnvironmentCube
				? PreviewScene->GetWorld()->SpawnActor<ASkyLightActor>("ThumbnailStudioSkyLight") : nullptr;
			if (!SkyLight)
			{
				Error = "The neutral thumbnail studio environment could not be loaded or created.";
				return;
			}
			SkyLight->GetSkyLightComponent()->SetSource(ESkyLightSourceMode::SpecifiedCube, EnvironmentCube);
			AActor* LightActor = PreviewScene->GetWorld()->SpawnActor<AActor>(
				"RenderedAssetThumbnailLightActor"
			);
			auto* Light = LightActor ? Cast<DDirectionalLightComponent>(LightActor->AddInstanceComponent(
									 DDirectionalLightComponent::StaticClass(), "PreviewLight"
								 )) :
								 nullptr;
			if (Light == nullptr)
			{
				Error = "The rendered-thumbnail preview light could not be created.";
				return;
			}
			Light->SetWorldRotation(RotationFromForward({Contract.KeyLightDirectionX, Contract.KeyLightDirectionY, Contract.KeyLightDirectionZ}));
			Light->SetIntensity(Contract.KeyLightIntensity);
			Light->SetAmbientIntensity(Contract.FillLightIntensity);
			Light->SetRimLightIntensity(0.16f);
		}
		~FImpl()
		{
			checkf(IsInGameThread(), "Thumbnail scenes belong to the game thread.");
			if (GRenderingThread) FlushRenderingCommands();
		}
	};

	FThumbnailPreviewScene::FThumbnailPreviewScene(FThumbnailVisualContract Contract)
		: Impl(std::make_unique<FImpl>(Contract)) {}
	FThumbnailPreviewScene::~FThumbnailPreviewScene()
	{
		checkf(!BindingOwner, "Reset thumbnail capture before destroying its borrowed preview scene.");
	}
	auto FThumbnailPreviewScene::IsAvailable() const -> bool
	{
		return Impl->Error.empty() && Impl->PreviewScene && Impl->PreviewScene->IsAvailable();
	}
	auto FThumbnailPreviewScene::GetDiagnostic() const -> std::string { return Impl->Error; }
	auto FThumbnailPreviewScene::GetWorld() -> DWorld*
	{
		return IsAvailable() ? Impl->PreviewScene->GetWorld() : nullptr;
	}
	auto FThumbnailPreviewScene::GetRenderScene() -> FSceneInterface*
	{
		return IsAvailable() ? Impl->PreviewScene->GetRenderScene() : nullptr;
	}
	auto FThumbnailPreviewScene::GetPreviewActor() -> AActor*
	{
		auto* World = GetWorld();
		if (World && !Impl->Actor) Impl->Actor = World->SpawnActor<AActor>("ThumbnailPreviewActor");
		return Impl->Actor.Get();
	}
	auto FThumbnailPreviewScene::ResetPreview() -> void
	{
		if (auto* Actor = Impl->Actor.Get())
			if (auto* Component = Actor->FindComponentByExactClass<DStaticMeshComponent>())
			{
				Component->ClearMaterialOverrides();
				Component->SetStaticMesh(nullptr);
			}
	}
} // namespace Durin
