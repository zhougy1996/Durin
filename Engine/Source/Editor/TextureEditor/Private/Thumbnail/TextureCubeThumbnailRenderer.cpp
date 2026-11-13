#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "DObject/WeakObjectPtr.h"

#include "Asset/Asset.h"
#include "DObject/Package.h"
#include "Math/Operations.h"
#include "Texture/TextureCube.h"
#include "RenderResource.h"
#include "Engine/Engine.h"
#include "IRendererModule.h"

namespace Durin::TextureEditor
{
	namespace
	{
		constexpr uint32 TextureCubeThumbnailGeneratorSchema = 2;
		constexpr uint32 TextureCubeThumbnailShaderContract = 2;

		auto MakeFingerprint(const FPackageData& Data, const FAssetData& Asset)
			-> FAssetThumbnailPackageFingerprint
		{
			return {
				.AssetPath = Asset.AssetPath,
				.PackagePath = Data.PackagePath,
				.AssetClassName = Asset.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}

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

		class FTextureCubeThumbnailDrawState final
		{
		public:
			FTextureCubeThumbnailDrawState(DThumbnailRenderer& InRenderer, DObject* Object, FTopLevelAssetPath Path)
				: Renderer(InRenderer), AssetPath(std::move(Path))
			{
				TextureCube = Cast<DTextureCube>(Object);
				AssetRevision = Object->GetPackage() ? Object->GetPackage()->GetEditRevision() : 0;
				SourceIdentity = TextureCube.Get()->GetSource().GetIdentity();
			}

			~FTextureCubeThumbnailDrawState() { ResetPreview(); }

			auto Update() -> FThumbnailDrawResult
			{
				auto Readiness = CheckTextureCubeReadiness(TextureCube.Get());
				if (!Readiness)
					return {
						.State = EThumbnailDrawStatus::Failed,
						.Diagnostic = std::move(Readiness.error())};
				if (*Readiness && !bShadersRequested && GEngine && GEngine->GetRendererModule())
				{
					Shaders = GEngine->GetRendererModule()->PrepareSceneThumbnailShaders();
					bShadersRequested = true;
				}
				auto ShaderReady = Shaders.Poll();
				if (!ShaderReady) return {.Diagnostic = ShaderReady.error()};
				return {
					.State = *Readiness && *ShaderReady
						? EThumbnailDrawStatus::ReadyToRender
						: EThumbnailDrawStatus::WaitingForResources};
			}

			auto PreparePreview(
				FThumbnailDrawContext& Context) -> std::expected<void, std::string>
			{
				if (!TextureCube.IsValid())
				{
					return std::unexpected(std::format(
						"The rendered-thumbnail TextureCube {} is unavailable.",
						AssetPath.ToString()));
				}
				auto Readiness = CheckTextureCubeReadiness(TextureCube.Get());
				if (!Readiness) return std::unexpected(std::move(Readiness.error()));
				if (!*Readiness) return std::unexpected("The TextureCube render resource is not ready.");
				Snapshot = TextureCube.Get()->GetResource()->GetTextureRHI_GameThread();
				const FRHITextureReferenceRef TextureReference = Snapshot
					? FTextureReference(Snapshot).GetTextureReferenceRHI() : FRHITextureReferenceRef{};
				if (TextureReference == nullptr)
				{
					return std::unexpected(std::format(
						"The rendered-thumbnail TextureCube {} has no texture reference.",
						AssetPath.ToString()));
				}
				if (auto Bound = Context.SetScene(Renderer.GetPreviewScene()); !Bound) return Bound;
				Context.SetView(MakeTextureCubeThumbnailView());
				Context.SetViewEnvironment({.TextureReference = TextureReference});
				return {};
			}

			auto ValidatePreparedInput() const -> std::expected<void, std::string>
			{
				auto Readiness = CheckTextureCubeReadiness(TextureCube.Get());
				if (!Readiness) return std::unexpected(std::move(Readiness.error()));
				if (!*Readiness || !TextureCube.IsValid()
					|| (TextureCube.Get()->GetPackage() ? TextureCube.Get()->GetPackage()->GetEditRevision() : 0) != AssetRevision
					|| TextureCube.Get()->GetSource().GetIdentity() != SourceIdentity
					|| !Snapshot || TextureCube.Get()->GetResource()->GetTextureRHI_GameThread() != Snapshot)
				{
					return std::unexpected("The TextureCube changed while its thumbnail was being generated.");
				}
				return {};
			}

			auto ResetPreview() -> void
			{
				Snapshot = nullptr;
				TextureCube = nullptr;
				Shaders = {};
				bShadersRequested = false;
			}

		private:
			FShaderMapPreparation Shaders;
			bool bShadersRequested = false;
			FXxHash128 SourceIdentity{};
			FTextureRHIRef Snapshot;
			DThumbnailRenderer& Renderer;
			FTopLevelAssetPath AssetPath;
			TWeakObjectPtr<DTextureCube> TextureCube;
			uint64 AssetRevision = 0;
		};
	} // namespace

	DTextureCubeThumbnailRenderer::DTextureCubeThumbnailRenderer(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DTextureCubeThumbnailRenderer::GetRegistration() const
		-> FThumbnailRenderingInfo
	{
		return {
			.AssetClassName = DTextureCube::StaticClass()->GetQualifiedName().ToString(),
			.RendererName = "TextureCubeRenderedThumbnail",
			.GeneratorSchemaVersion = TextureCubeThumbnailGeneratorSchema};
	}

	auto DTextureCubeThumbnailRenderer::DescribeThumbnail(const FAssetThumbnailPackageFingerprint& Asset) -> std::expected<FThumbnailCacheDescription, std::string>
	{
		FThumbnailCacheDescription Result;

		const FThumbnailRenderingInfo Registration = GetRegistration();
		if (Asset.AssetClassName != Registration.AssetClassName)
		{
			return std::unexpected("The TextureCube thumbnail renderer received the wrong asset class.");
		}
		const FAssetCatalogEntry Entry = FindAssetExact(Asset.AssetPath, EAssetQuerySource::OnDisk);
		if (!Entry || Entry.Package->PackagePath != Asset.PackagePath)
		{
			return std::unexpected(std::format(
				"TextureCube thumbnail registry data is missing for {}.",
				Asset.AssetPath.ToString()));
		}
		if (MakeFingerprint(*Entry.Package, *Entry.Asset) != Asset)
		{
			return std::unexpected(std::format(
				"TextureCube thumbnail registry data changed for {}; refresh the request snapshot.",
				Asset.AssetPath.ToString()));
		}

		const FThumbnailVisualContract Visual;
		Result = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity =
				std::string(
					FThumbnailVisualContract::TextureCubeEnvironmentViewIdentity),
			.PreviewFixtureVersion =
				FThumbnailVisualContract::TextureCubeEnvironmentViewVersion,
			.ShaderContractVersion = TextureCubeThumbnailShaderContract};

		Result.bHasTransparency = false;
		return Result;
	}

	auto DTextureCubeThumbnailRenderer::CanVisualizeAsset(DObject* Object) const -> bool
	{
		return Cast<DTextureCube>(Object) != nullptr;
	}

	auto DTextureCubeThumbnailRenderer::Draw(DObject* Object, FThumbnailDrawContext& Context) -> void
	{
		if (!CanVisualizeAsset(Object)) { Context.SetResult({.Diagnostic = "The renderer cannot visualize this asset."}); return; }
		auto& State = Context.GetState<FTextureCubeThumbnailDrawState>(*this, Object, Context.GetAssetPath());
		auto Result = State.Update();
		Context.SetResult(Result);
		if (Result.State == EThumbnailDrawStatus::ReadyToRender)
		{
			if (auto Prepared = State.PreparePreview(Context); !Prepared)
			{
				if (Context.GetResult().State == EThumbnailDrawStatus::ReadyToRender)
					Context.SetResult({.Diagnostic = Prepared.error()});
			}
			else Context.SetValidation([&State] { return State.ValidatePreparedInput(); });
		}
	}

} // namespace Durin::TextureEditor
