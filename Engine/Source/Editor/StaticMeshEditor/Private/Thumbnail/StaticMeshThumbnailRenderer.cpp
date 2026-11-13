#include "Engine/StreamableManager.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"

// StaticMeshEditor owns the complete StaticMesh thumbnail extension.

#include "Asset/Asset.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"
#include "Engine/Actor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "IRendererModule.h"
#include "Math/Operations.h"
#include "Materials/MaterialInterface.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"

namespace Durin::StaticMeshEditor
{
	namespace
	{
		constexpr double MinimumNearClipDistance = 1.0e-6;
		constexpr double ClipPaddingFraction = 0.05;

		auto MakeStaticMeshThumbnailFingerprint(const FPackageData& Data,
			FTopLevelAssetPath AssetPath = {})
			-> FAssetThumbnailPackageFingerprint
		{
			const auto Asset = std::ranges::find(Data.TopLevelAssets, AssetPath, &FAssetData::AssetPath);
			return {
				.AssetPath = std::move(AssetPath),
				.PackagePath = Data.PackagePath,
				.AssetClassName = Asset == Data.TopLevelAssets.end() ? std::string{} : Asset->AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}

		auto QualifyDiagnostic(const FTopLevelAssetPath& AssetPath, std::string_view Detail)
			-> std::string
		{
			return std::format(
				"StaticMesh '{}' thumbnail generation failed: {}",
				AssetPath.ToString(),
				Detail.empty() ? "unknown preview error" : Detail);
		}

		class FStaticMeshThumbnailGenerationSession final
			: public IThumbnailRendererSession
		{
		public:
			explicit FStaticMeshThumbnailGenerationSession(
				FStaticMeshThumbnailRendererGenerationInput InInput)
				: Input(std::move(InInput))
			{
			}

			~FStaticMeshThumbnailGenerationSession() override
			{
				ResetPreview();
			}

			auto Load() -> FThumbnailRendererSessionUpdate override
			{
				ResetPreview();
				AssetLoad = StreamableManager.RequestAsyncLoad(Input.AssetPath, {}, DStaticMesh::StaticClass());
				return {.State = EThumbnailRendererSessionState::WaitingForResources};
			}

			auto FinishLoad() -> FThumbnailRendererSessionUpdate
			{
				const bool Result = !AssetLoad->HasError();
				StaticMesh = Result ? Cast<DStaticMesh>(AssetLoad->GetLoadedAsset()) : nullptr;
				if (!Result || StaticMesh == nullptr
					|| StaticMesh->GetClass() != DStaticMesh::StaticClass())
				{
					StaticMesh = nullptr;
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = AssetLoad->GetError().Message.empty()
							? std::format(
								"The requested asset '{}' is not an exact DStaticMesh.",
								Input.AssetPath.ToString())
							: AssetLoad->GetError().Message};
				}
				const DPackage* Package = StaticMesh->GetPackage();
				if (Package == nullptr)
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = QualifyDiagnostic(Input.AssetPath, "The asset package is unavailable.")};
				AssetRevision = Package->GetEditRevision();
				return PollStaticMeshReadiness();
			}

			auto PollResources() -> FThumbnailRendererSessionUpdate override
			{
				if (!AssetLoad) return {.Diagnostic = "The thumbnail load was reset."};
				if (!AssetLoad->IsComplete())
					return {.State = EThumbnailRendererSessionState::WaitingForResources};
				if (StaticMesh == nullptr) return FinishLoad();
				return PollStaticMeshReadiness();
			}

		private:
			FShaderMapPreparation Shaders;
			bool bShadersRequested = false;
			auto PollMaterialReadiness() const
				-> FThumbnailRendererSessionUpdate
			{
				for (const auto& Slot : StaticMesh->GetMaterialSlots())
				{
					const auto* Material = Slot.DefaultMaterial.Get();
					if (!Material) continue;
					const auto& Status = Material->GetMaterialCompileStatus();
					if (Status.IsCurrent() && Material->GetAcceptedCompiledProgram()) continue;
					if (Material->IsCompiling() || Status.HasScheduledEdits())
						return {.State = EThumbnailRendererSessionState::WaitingForResources};
					const auto Diagnostics = Material->GetMaterialCompileDiagnostics();
					return {.Diagnostic = QualifyDiagnostic(Input.AssetPath, std::format(
						"Material '{}' has no current compiled program. {}", Material->GetObjectPath(),
						Diagnostics.empty() ? std::string{} : Diagnostics.front().Message))};
				}
				return {.State = EThumbnailRendererSessionState::ReadyToRender};
			}
			auto PollStaticMeshReadiness()
				-> FThumbnailRendererSessionUpdate
			{
				if (StaticMesh->IsCompiling())
					return {
						.State = EThumbnailRendererSessionState::WaitingForResources};
				FStaticMeshRenderResourceStatus Status =
					StaticMesh->GetRenderResourceStatus();
				if (Status.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
				{
					StaticMesh->RequestRenderDataAndResources();
					if (StaticMesh->GetRenderDataLoadError().Code != ECookedMeshLoadError::None)
						return {.Diagnostic = QualifyDiagnostic(Input.AssetPath, "The mesh render data could not be loaded.")};
					if (!StaticMesh->GetRenderData())
						return {.State = EThumbnailRendererSessionState::WaitingForResources};
					Status = StaticMesh->GetRenderResourceStatus();
				}
				if (!StaticMesh->GetLOD0LocalBounds())
				{
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = std::format(
							"StaticMesh '{}' has no finite, valid LOD 0 bounds.",
							Input.AssetPath.ToString())};
				}
				switch (Status.Readiness)
				{
				case EStaticMeshRenderResourceReadiness::Ready:
				{
					const auto Materials = PollMaterialReadiness();
					if (Materials.State != EThumbnailRendererSessionState::ReadyToRender)
						return Materials;
					if (!bShadersRequested && GEngine && GEngine->GetRendererModule())
					{
						Shaders = GEngine->GetRendererModule()->PrepareSceneThumbnailShaders();
						bShadersRequested = true;
					}
					auto ShaderReady = Shaders.Poll();
					if (!ShaderReady) return {.Diagnostic = ShaderReady.error()};
					return {
						.State = *ShaderReady ? EThumbnailRendererSessionState::ReadyToRender
							: EThumbnailRendererSessionState::WaitingForResources};
				}
				case EStaticMeshRenderResourceReadiness::Queued:
					return {
						.State = EThumbnailRendererSessionState::WaitingForResources};
				case EStaticMeshRenderResourceReadiness::InvalidData:
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = QualifyDiagnostic(
							Input.AssetPath, "The render resource failed.")};
				case EStaticMeshRenderResourceReadiness::Unavailable:
				default:
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = QualifyDiagnostic(
							Input.AssetPath, "The render resource is unavailable.")};
				}
			}

			auto PreparePreview(
				IThumbnailPreviewScene& PreviewScene) -> std::expected<void, std::string> override
			{
				ResetScenePreview();
				if (!StaticMesh) return std::unexpected(QualifyDiagnostic(Input.AssetPath, "The StaticMesh asset is unavailable."));
				if (const auto Materials = PollMaterialReadiness();
					Materials.State != EThumbnailRendererSessionState::ReadyToRender)
					return std::unexpected(QualifyDiagnostic(Input.AssetPath,
						Materials.Diagnostic.empty() ? "A material is still compiling." : Materials.Diagnostic));
				for (const auto& Slot : StaticMesh->GetMaterialSlots())
					if (auto* Material = Slot.DefaultMaterial.Get())
						PreparedMaterials.emplace_back(FWeakObjectPtr(Material), Material->GetMaterialResource());
				PreparedResourceRevision = StaticMesh ? StaticMesh->GetRenderResourceStatus().Revision : 0;
				const std::optional<FBox> Bounds = StaticMesh
					? StaticMesh->GetLOD0LocalBounds()
					: std::nullopt;
				if (!Bounds) return std::unexpected(QualifyDiagnostic(Input.AssetPath, "The mesh bounds are unavailable."));
				auto ViewResult = CalculateStaticMeshThumbnailRendererView({
						.LocalBounds = *Bounds,
						.OutputAspectRatio =
							static_cast<double>(Input.VisualContract.Output.Width)
							/ static_cast<double>(Input.VisualContract.Output.Height),
						.VerticalFieldOfViewDegrees =
							Input.VisualContract.VerticalFieldOfViewDegrees,
						.CameraDirection = FVector3(
							Input.VisualContract.CameraDirectionX,
							Input.VisualContract.CameraDirectionY,
							Input.VisualContract.CameraDirectionZ)});
				if (!ViewResult)
				{
					return std::unexpected(QualifyDiagnostic(Input.AssetPath, ViewResult.error()));
				}
				const auto& ThumbnailView = *ViewResult;
				World = PreviewScene.GetWorld();
				Actor = World
					? World->SpawnActor<AActor>("StaticMeshThumbnailPreviewActor")
					: nullptr;
				Component = Actor
					? Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
						DStaticMeshComponent::StaticClass(), "StaticMeshPreview"))
					: nullptr;
				if (Component == nullptr || StaticMesh == nullptr)
				{
					ResetPreview();
					return std::unexpected(QualifyDiagnostic(
						Input.AssetPath, "The preview component is unavailable."));
				}
				Component->ClearMaterialOverrides();
				Component->SetStaticMesh(StaticMesh);
				Component->SetWorldTransform(ThumbnailView.MeshTransform);
				const FThumbnailPreviewView View{
					.CameraPosition = {
						ThumbnailView.CameraPosition.x,
						ThumbnailView.CameraPosition.y,
						ThumbnailView.CameraPosition.z},
					.CameraForward = {
						ThumbnailView.CameraForward.x,
						ThumbnailView.CameraForward.y,
						ThumbnailView.CameraForward.z},
					.CameraRight = {
						ThumbnailView.CameraRight.x,
						ThumbnailView.CameraRight.y,
						ThumbnailView.CameraRight.z},
					.CameraUp = {
						ThumbnailView.CameraUp.x,
						ThumbnailView.CameraUp.y,
						ThumbnailView.CameraUp.z},
					.VerticalFieldOfViewDegrees =
						Input.VisualContract.VerticalFieldOfViewDegrees,
					.NearClipDistance = ThumbnailView.NearClipDistance,
					.FarClipDistance = ThumbnailView.FarClipDistance,
					.ClearRed = Input.VisualContract.bOutputOpaque
						? Input.VisualContract.BackgroundRed
						: 0.0f,
					.ClearGreen = Input.VisualContract.bOutputOpaque
						? Input.VisualContract.BackgroundGreen
						: 0.0f,
					.ClearBlue = Input.VisualContract.bOutputOpaque
						? Input.VisualContract.BackgroundBlue
						: 0.0f,
					.ClearAlpha = Input.VisualContract.bOutputOpaque
						? 1.0f
						: 0.0f};
				if (auto ViewResult = PreviewScene.SetView(View); !ViewResult)
				{
					ResetPreview();
					return std::unexpected(QualifyDiagnostic(Input.AssetPath, ViewResult.error()));
				}
				return {};
			}

			auto ValidatePreparedInput() const -> std::expected<void, std::string> override
			{
				if (StaticMesh == nullptr || Component == nullptr)
				{
					return std::unexpected(QualifyDiagnostic(
						Input.AssetPath, "The StaticMesh asset is unavailable."));
				}
				const FStaticMeshRenderResourceStatus Status =
					StaticMesh->GetRenderResourceStatus();
				if (Status.Readiness != EStaticMeshRenderResourceReadiness::Ready
					|| StaticMesh->GetPackage() == nullptr
					|| StaticMesh->GetPackage()->GetEditRevision() != AssetRevision
					|| Status.Revision != PreparedResourceRevision)
				{
					return std::unexpected(std::format(
						"StaticMesh '{}' changed while its thumbnail was being generated.",
						Input.AssetPath.ToString()));
				}
				for (const auto& [Owner, Snapshot] : PreparedMaterials)
				{
					const auto* Material = Cast<DMaterialInterface>(Owner.Get());
					if (!Material || !Material->GetMaterialCompileStatus().IsCurrent()
						|| !Material->GetAcceptedCompiledProgram() || Material->GetMaterialResource() != Snapshot)
						return std::unexpected(QualifyDiagnostic(Input.AssetPath,
							"A material changed while its thumbnail was being generated."));
				}
				return {};
			}

			auto ResetPreview() -> void override
			{
				ResetScenePreview();
				StaticMesh = nullptr;
				if (AssetLoad) AssetLoad->CancelHandle();
				AssetLoad.reset();
				Shaders = {};
				bShadersRequested = false;
			}

			auto ResetScenePreview() -> void
			{
				PreparedMaterials.clear();
				if (World != nullptr && Actor != nullptr) World->DestroyActor(Actor);
				Component = nullptr;
				Actor = nullptr;
				World = nullptr;
			}

			FStaticMeshThumbnailRendererGenerationInput Input;
			DStaticMesh* StaticMesh = nullptr;
			FStreamableManager StreamableManager;
			std::shared_ptr<FStreamableHandle> AssetLoad;
			uint64 AssetRevision = 0;
			uint64 PreparedResourceRevision = 0;
			std::vector<std::pair<FWeakObjectPtr, FMaterialResource>> PreparedMaterials;
			DWorld* World = nullptr;
			AActor* Actor = nullptr;
			DStaticMeshComponent* Component = nullptr;
		};
	} // namespace

	auto DStaticMeshThumbnailRenderer::GetRegistration() const
		-> FThumbnailRenderingInfo
	{
		return {
			.AssetClassName = DStaticMesh::StaticClass()
				->GetQualifiedName().ToString(),
			.RendererName =
				std::string(FStaticMeshThumbnailRendererContract::RendererName),
			.GeneratorSchemaVersion =
				FStaticMeshThumbnailRendererContract::GeneratorSchemaVersion};
	}

	auto DStaticMeshThumbnailRenderer::CaptureGenerationRequest(
		const FAssetThumbnailRequest& Request,
		uint64 RendererGeneration) -> std::expected<FAssetThumbnailGenerationRequest, std::string>
	{
		FAssetThumbnailGenerationRequest GenerationRequest;

		const FThumbnailRenderingInfo Registration =
			GetRegistration();
		if (Request.Asset.AssetClassName != Registration.AssetClassName)
		{
			return std::unexpected("The StaticMesh thumbnail renderer received the wrong asset class.");
		}

		const FAssetCatalogSnapshot Catalog =
			CaptureAssetCatalogSnapshot();
		const FPackageData* Root =
			Catalog.FindExact(Request.Asset.PackagePath);
		if (Root == nullptr)
		{
			return std::unexpected(std::format(
				"StaticMesh thumbnail registry data is missing for {}.",
				Request.Asset.AssetPath.ToString()));
		}
		if (MakeStaticMeshThumbnailFingerprint(*Root, Request.Asset.AssetPath) != Request.Asset)
		{
			return std::unexpected(std::format(
				"StaticMesh thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.AssetPath.ToString()));
		}

		std::vector<FAssetThumbnailDependencyNode> Nodes;
		Nodes.reserve(Catalog.Assets.size());
		for (const auto& [Path, Data] : Catalog.Assets)
		{
			Nodes.push_back({
				.Package = MakeStaticMeshThumbnailFingerprint(Data),
				.Dependencies = Data.Dependencies});
		}
		auto Dependencies = BuildAssetThumbnailDependencyClosure(
			Request.Asset.PackagePath, Nodes);
		if (!Dependencies) return std::unexpected(std::move(Dependencies.error()));

		FThumbnailVisualContract Visual;
		const FStaticMeshThumbnailRendererViewInput ViewContract;
		Visual.CameraDirectionX =
			static_cast<float>(ViewContract.CameraDirection.x);
		Visual.CameraDirectionY =
			static_cast<float>(ViewContract.CameraDirection.y);
		Visual.CameraDirectionZ =
			static_cast<float>(ViewContract.CameraDirection.z);
		Visual.VerticalFieldOfViewDegrees =
			static_cast<float>(ViewContract.VerticalFieldOfViewDegrees);
		Visual.bOutputOpaque =
			FStaticMeshThumbnailRendererContract::bOutputOpaque;
		GenerationRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(
				FStaticMeshThumbnailRendererContract::PreviewFixtureIdentity),
			.PreviewFixtureVersion =
				FStaticMeshThumbnailRendererContract::PreviewFixtureVersion,
			.ShaderContractVersion =
				FStaticMeshThumbnailRendererContract::ShaderContractVersion,
			.Dependencies = std::move(*Dependencies)};
		GenerationRequest.Input =
			std::make_shared<FStaticMeshThumbnailRendererGenerationInput>(
				Request.Asset.AssetPath, std::move(Visual));
		GenerationRequest.RendererGeneration = RendererGeneration;
		GenerationRequest.RequestSerial = Request.RequestSerial;
		return GenerationRequest;
	}

	auto DStaticMeshThumbnailRenderer::CreateGenerationSession(
		const FAssetThumbnailGenerationRequest&,
		const IAssetThumbnailGenerationInput& Input)
		-> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string>
	{
		const auto* StaticMeshInput =
			dynamic_cast<const FStaticMeshThumbnailRendererGenerationInput*>(&Input);
		if (StaticMeshInput == nullptr)
		{
			return std::unexpected("The StaticMesh thumbnail generation input is invalid.");
		}
		return std::make_unique<FStaticMeshThumbnailGenerationSession>(*StaticMeshInput);
	}

	auto CalculateStaticMeshThumbnailRendererView(
		const FStaticMeshThumbnailRendererViewInput& Input)
		-> std::expected<FStaticMeshThumbnailRendererView, std::string>
	{
		if (!Input.LocalBounds.bIsValid
			|| !Math::IsFinite(Input.LocalBounds.Min)
			|| !Math::IsFinite(Input.LocalBounds.Max))
		{
			return std::unexpected("Static-mesh thumbnail bounds must be finite and valid.");
		}

		if (!std::isfinite(Input.OutputAspectRatio) || Input.OutputAspectRatio <= 0.0)
		{
			return std::unexpected("Static-mesh thumbnail output aspect ratio must be positive and finite.");
		}
		if (!std::isfinite(Input.VerticalFieldOfViewDegrees)
			|| Input.VerticalFieldOfViewDegrees <= 0.0
			|| Input.VerticalFieldOfViewDegrees >= 180.0)
		{
			return std::unexpected("Static-mesh thumbnail field of view must be between zero and 180 degrees.");
		}
		if (!std::isfinite(Input.ImageMargin)
			|| Input.ImageMargin < 0.0
			|| Input.ImageMargin >= 1.0)
		{
			return std::unexpected("Static-mesh thumbnail image margin must be in [0, 1).");
		}

		FVector3 CameraDirection;
		if (!Math::TryNormalize(Input.CameraDirection, CameraDirection))
		{
			return std::unexpected("Static-mesh thumbnail camera direction must be finite and non-zero.");
		}
		const FVector3 CameraForward = -CameraDirection;
		FVector3 CameraRight;
		if (!Math::TryNormalize(
				Math::Cross(FVectorConstants::Up, CameraForward), CameraRight))
		{
			return std::unexpected("Static-mesh thumbnail camera direction cannot be parallel to world up.");
		}
		FVector3 CameraUp;
		if (!Math::TryNormalize(Math::Cross(CameraForward, CameraRight), CameraUp))
		{
			return std::unexpected("Static-mesh thumbnail camera basis is invalid.");
		}

		const double HalfVerticalFieldOfViewRadians =
			Math::DegreesToRadians(Input.VerticalFieldOfViewDegrees) * 0.5;
		const double UsableVerticalTangent =
			std::tan(HalfVerticalFieldOfViewRadians) * (1.0 - Input.ImageMargin);
		const double UsableHorizontalTangent =
			UsableVerticalTangent * Input.OutputAspectRatio;
		if (!std::isfinite(UsableVerticalTangent)
			|| !std::isfinite(UsableHorizontalTangent)
			|| UsableVerticalTangent <= 0.0
			|| UsableHorizontalTangent <= 0.0)
		{
			return std::unexpected("Static-mesh thumbnail projection is invalid.");
		}

		const FVector3 BoundsCenter = Input.LocalBounds.GetCenter();
		const FVector3 BoundsExtent = Input.LocalBounds.GetExtent();
		double CameraDistance = 0.0;
		double MinimumDepthOffset = std::numeric_limits<double>::max();
		for (uint32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			const FVector3 Corner(
				(CornerIndex & 1u) != 0 ? BoundsExtent.x : -BoundsExtent.x,
				(CornerIndex & 2u) != 0 ? BoundsExtent.y : -BoundsExtent.y,
				(CornerIndex & 4u) != 0 ? BoundsExtent.z : -BoundsExtent.z);
			const double DepthOffset = Math::Dot(CameraForward, Corner);
			MinimumDepthOffset = std::min(MinimumDepthOffset, DepthOffset);
			CameraDistance = std::max({
				CameraDistance,
				std::abs(Math::Dot(CameraRight, Corner)) / UsableHorizontalTangent - DepthOffset,
				std::abs(Math::Dot(CameraUp, Corner)) / UsableVerticalTangent - DepthOffset});
		}

		const double MinimumRequiredDepth = MinimumNearClipDistance * 2.0;
		CameraDistance = std::max(
			CameraDistance, MinimumRequiredDepth - MinimumDepthOffset);
		if (!std::isfinite(CameraDistance) || CameraDistance <= 0.0)
		{
			return std::unexpected("Static-mesh thumbnail camera distance is invalid.");
		}

		double MinimumDepth = std::numeric_limits<double>::max();
		double MaximumDepth = std::numeric_limits<double>::lowest();
		for (uint32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			const FVector3 Corner(
				(CornerIndex & 1u) != 0 ? BoundsExtent.x : -BoundsExtent.x,
				(CornerIndex & 2u) != 0 ? BoundsExtent.y : -BoundsExtent.y,
				(CornerIndex & 4u) != 0 ? BoundsExtent.z : -BoundsExtent.z);
			const double Depth = CameraDistance + Math::Dot(CameraForward, Corner);
			MinimumDepth = std::min(MinimumDepth, Depth);
			MaximumDepth = std::max(MaximumDepth, Depth);
		}

		const double ClipPadding = std::max(
			(MaximumDepth - MinimumDepth) * ClipPaddingFraction,
			MinimumNearClipDistance);
		const double NearClipDistance = std::max(
			MinimumNearClipDistance, MinimumDepth - ClipPadding);
		const double FarClipDistance = std::max(
			MaximumDepth + ClipPadding, NearClipDistance + MinimumNearClipDistance);
		if (!std::isfinite(NearClipDistance)
			|| !std::isfinite(FarClipDistance)
			|| NearClipDistance <= 0.0
			|| FarClipDistance <= NearClipDistance)
		{
			return std::unexpected("Static-mesh thumbnail clip planes are invalid.");
		}

		FStaticMeshThumbnailRendererView Result;
		Result.MeshTransform.Translation = -BoundsCenter;
		Result.CameraPosition = CameraDirection * CameraDistance;
		Result.CameraTarget = FVector3(0.0);
		Result.CameraForward = CameraForward;
		Result.CameraRight = CameraRight;
		Result.CameraUp = CameraUp;
		Result.CameraDistance = CameraDistance;
		Result.NearClipDistance = NearClipDistance;
		Result.FarClipDistance = FarClipDistance;
		return Result;
	}
} // namespace Durin::StaticMeshEditor
