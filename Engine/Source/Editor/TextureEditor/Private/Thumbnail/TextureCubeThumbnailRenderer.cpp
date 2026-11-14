#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "DObject/WeakObjectPtr.h"

#include "Asset/Asset.h"
#include "Math/Operations.h"
#include "Texture/TextureCube.h"
#include "RenderResource.h"
#include "Engine/Engine.h"
#include "IRendererModule.h"

namespace Durin::TextureEditor
{
	namespace
	{

		auto CheckTextureCubeReadiness(
			DTextureCube* TextureCube) -> std::expected<bool, std::string>
		{
			if (TextureCube == nullptr || TextureCube->IsPendingKill())
			{
				return std::unexpected("The TextureCube asset is unavailable.");
			}
			if (!TextureCube->IsAsyncCacheComplete()) return false;
			if (!TextureCube->HasPlatformData())
			{
				return std::unexpected("The TextureCube has no installed platform data.");
			}
			if (TextureCube->GetTextureReferenceRHI() == nullptr)
			{
				return std::unexpected("The TextureCube has no texture reference.");
			}
			if (TextureCube->HasPendingRenderResourceInitialization()) return false;
			const auto* Resource = TextureCube->GetResource();
			if (!Resource || !Resource->GetTextureRHI_GameThread())
			{
				return std::unexpected("The TextureCube render resource is unavailable.");
			}
			return true;
		}

		auto MakeTextureCubeThumbnailView() -> FThumbnailPreviewView
		{
			const FThumbnailVisualContract Contract;
			const float EnvironmentYScale = 1.0f / std::tan(
				Math::DegreesToRadians(
					FTextureCubeThumbnailRendererVisualContract::VerticalFieldOfViewDegrees)
				* 0.5f);
			const double CompatibleVerticalFieldOfViewDegrees =
				Math::RadiansToDegrees(
					2.0 * std::atan(1.0 / static_cast<double>(EnvironmentYScale)));
			const FVector3 Eye = Math::Normalize(FVector3(
				Contract.CameraDirectionX,
				Contract.CameraDirectionY,
				Contract.CameraDirectionZ)) * static_cast<double>(Contract.CameraDistance);
			const FVector3 Forward = Math::Normalize(-Eye);
			const FVector3 Right = Math::Normalize(
				Math::Cross(FVectorConstants::Up, Forward));
			const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));
			return {
				.CameraPosition = {Eye.x, Eye.y, Eye.z},
				.CameraForward = {Forward.x, Forward.y, Forward.z},
				.CameraRight = {Right.x, Right.y, Right.z},
				.CameraUp = {Up.x, Up.y, Up.z},
				// Reconstruct the legacy float-quantized 100-degree projection exactly.
				.VerticalFieldOfViewDegrees = CompatibleVerticalFieldOfViewDegrees,
				.NearClipDistance = Contract.NearClipDistance,
				.FarClipDistance = Contract.FarClipDistance,
				.ClearRed = Contract.BackgroundRed,
				.ClearGreen = Contract.BackgroundGreen,
				.ClearBlue = Contract.BackgroundBlue,
				.ClearAlpha = 1.0f};
		}

	} // namespace

	DTextureCubeThumbnailRenderer::DTextureCubeThumbnailRenderer(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DTextureCubeThumbnailRenderer::CanVisualizeAsset(DObject* Object) const -> bool
	{
		return Cast<DTextureCube>(Object) != nullptr;
	}

	auto DTextureCubeThumbnailRenderer::Draw(DObject* Object, FThumbnailDrawContext& Context) -> void
	{
		auto* Texture = Cast<DTextureCube>(Object);
		if (!Texture) return Context.Fail("The renderer cannot visualize this asset.");
		auto Readiness = CheckTextureCubeReadiness(Texture);
		if (!Readiness) return Context.Fail(Readiness.error());
		if (!*Readiness) return;
		if (!Shaders && GEngine && GEngine->GetRendererModule())
			Shaders = GEngine->GetRendererModule()->PrepareSceneThumbnailShaders();
		if (!Shaders) return;
		auto Ready = Shaders->Poll();
		if (!Ready) return Context.Fail(Ready.error());
		if (!*Ready) return;
		auto Snapshot = Texture->GetResource()->GetTextureRHI_GameThread();
		const auto Reference = FTextureReference(Snapshot).GetTextureReferenceRHI();
		if (!Reference) return Context.Fail("The TextureCube has no texture reference.");
		if (!Context.TrySetScene(GetPreviewScene())) return;
		Context.SetView(MakeTextureCubeThumbnailView());
		Context.SetViewEnvironment({.TextureReference = Reference});
		Context.SetValidation([Texture = TWeakObjectPtr<DTextureCube>(Texture), Snapshot, Identity = Texture->GetSource().GetIdentity()] {
			auto Ready = CheckTextureCubeReadiness(Texture.Get());
			return Ready && *Ready && Texture.Get()->GetSource().GetIdentity() == Identity
				&& Texture.Get()->GetResource()->GetTextureRHI_GameThread() == Snapshot;
		});
		Context.MarkReady();
	}

} // namespace Durin::TextureEditor
