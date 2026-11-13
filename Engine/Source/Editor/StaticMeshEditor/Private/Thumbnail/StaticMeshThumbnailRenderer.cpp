#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
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

		class FStaticMeshThumbnailDrawState final
		{
		public:
			FStaticMeshThumbnailDrawState(DThumbnailRenderer& InRenderer, DObject* Object, FTopLevelAssetPath Path)
				: Renderer(InRenderer), AssetPath(std::move(Path))
			{
				StaticMesh = Cast<DStaticMesh>(Object);
				AssetRevision = Object->GetPackage() ? Object->GetPackage()->GetEditRevision() : 0;

				const FStaticMeshThumbnailRendererViewInput ViewContract;
				VisualContract.CameraDirectionX =
					static_cast<float>(ViewContract.CameraDirection.x);
				VisualContract.CameraDirectionY =
					static_cast<float>(ViewContract.CameraDirection.y);
				VisualContract.CameraDirectionZ =
					static_cast<float>(ViewContract.CameraDirection.z);
				VisualContract.VerticalFieldOfViewDegrees =
					static_cast<float>(ViewContract.VerticalFieldOfViewDegrees);
				VisualContract.bOutputOpaque =
					FStaticMeshThumbnailRendererContract::bOutputOpaque;
			}

			~FStaticMeshThumbnailDrawState()
			{
				ResetPreview();
			}

			auto Update() -> FThumbnailDrawResult
			{
				return PollStaticMeshReadiness();
			}

		private:
			FShaderMapPreparation Shaders;
			bool bShadersRequested = false;
			auto PollMaterialReadiness() const
				-> FThumbnailDrawResult
			{
				for (const auto& Slot : StaticMesh->GetMaterialSlots())
				{
					const auto* Material = Slot.DefaultMaterial.Get();
					if (!Material) continue;
					const auto& Status = Material->GetMaterialCompileStatus();
					if (Status.IsCurrent() && Material->GetAcceptedCompiledProgram()) continue;
					if (Material->IsCompiling() || Status.HasScheduledEdits())
						return {.State = EThumbnailDrawStatus::WaitingForResources};
					const auto Diagnostics = Material->GetMaterialCompileDiagnostics();
					return {.Diagnostic = QualifyDiagnostic(AssetPath, std::format(
						"Material '{}' has no current compiled program. {}", Material->GetObjectPath(),
						Diagnostics.empty() ? std::string{} : Diagnostics.front().Message))};
				}
				return {.State = EThumbnailDrawStatus::ReadyToRender};
			}
			auto PollStaticMeshReadiness()
				-> FThumbnailDrawResult
			{
				if (StaticMesh->IsCompiling())
					return {
						.State = EThumbnailDrawStatus::WaitingForResources};
				FStaticMeshRenderResourceStatus Status =
					StaticMesh->GetRenderResourceStatus();
				if (Status.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
				{
					StaticMesh->RequestRenderDataAndResources();
					if (StaticMesh->GetRenderDataLoadError().Code != ECookedMeshLoadError::None)
						return {.Diagnostic = QualifyDiagnostic(AssetPath, "The mesh render data could not be loaded.")};
					if (!StaticMesh->GetRenderData())
						return {.State = EThumbnailDrawStatus::WaitingForResources};
					Status = StaticMesh->GetRenderResourceStatus();
				}
				if (!StaticMesh->GetLOD0LocalBounds())
				{
					return {
						.State = EThumbnailDrawStatus::Failed,
						.Diagnostic = std::format(
							"StaticMesh '{}' has no finite, valid LOD 0 bounds.",
							AssetPath.ToString())};
				}
				switch (Status.Readiness)
				{
				case EStaticMeshRenderResourceReadiness::Ready:
				{
					const auto Materials = PollMaterialReadiness();
					if (Materials.State != EThumbnailDrawStatus::ReadyToRender)
						return Materials;
					if (!bShadersRequested && GEngine && GEngine->GetRendererModule())
					{
						Shaders = GEngine->GetRendererModule()->PrepareSceneThumbnailShaders();
						bShadersRequested = true;
					}
					auto ShaderReady = Shaders.Poll();
					if (!ShaderReady) return {.Diagnostic = ShaderReady.error()};
					return {
						.State = *ShaderReady ? EThumbnailDrawStatus::ReadyToRender
							: EThumbnailDrawStatus::WaitingForResources};
				}
				case EStaticMeshRenderResourceReadiness::Queued:
					return {
						.State = EThumbnailDrawStatus::WaitingForResources};
				case EStaticMeshRenderResourceReadiness::InvalidData:
					return {
						.State = EThumbnailDrawStatus::Failed,
						.Diagnostic = QualifyDiagnostic(
							AssetPath, "The render resource failed.")};
				case EStaticMeshRenderResourceReadiness::Unavailable:
				default:
					return {
						.State = EThumbnailDrawStatus::Failed,
						.Diagnostic = QualifyDiagnostic(
							AssetPath, "The render resource is unavailable.")};
				}
			}

		public:
			auto PreparePreview(
				FThumbnailDrawContext& Context) -> std::expected<void, std::string>
			{
				ResetScenePreview();
				if (!StaticMesh) return std::unexpected(QualifyDiagnostic(AssetPath, "The StaticMesh asset is unavailable."));
				if (const auto Materials = PollMaterialReadiness();
					Materials.State != EThumbnailDrawStatus::ReadyToRender)
					return std::unexpected(QualifyDiagnostic(AssetPath,
						Materials.Diagnostic.empty() ? "A material is still compiling." : Materials.Diagnostic));
				for (const auto& Slot : StaticMesh->GetMaterialSlots())
					if (auto* Material = Slot.DefaultMaterial.Get())
						PreparedMaterials.emplace_back(FWeakObjectPtr(Material), Material->GetMaterialResource());
				PreparedResourceRevision = StaticMesh ? StaticMesh->GetRenderResourceStatus().Revision : 0;
				const std::optional<FBox> Bounds = StaticMesh
					? StaticMesh->GetLOD0LocalBounds()
					: std::nullopt;
				if (!Bounds) return std::unexpected(QualifyDiagnostic(AssetPath, "The mesh bounds are unavailable."));
				auto ViewResult = CalculateStaticMeshThumbnailRendererView({
						.LocalBounds = *Bounds,
						.OutputAspectRatio =
							static_cast<double>(VisualContract.Output.Width)
							/ static_cast<double>(VisualContract.Output.Height),
						.VerticalFieldOfViewDegrees =
							VisualContract.VerticalFieldOfViewDegrees,
						.CameraDirection = FVector3(
							VisualContract.CameraDirectionX,
							VisualContract.CameraDirectionY,
							VisualContract.CameraDirectionZ)});
				if (!ViewResult)
				{
					return std::unexpected(QualifyDiagnostic(AssetPath, ViewResult.error()));
				}
				const auto& ThumbnailView = *ViewResult;
				auto& Scene = Renderer.GetPreviewScene();
				if (auto Bound = Context.SetScene(Scene); !Bound) return Bound;
				AActor* Actor = Scene.GetPreviewActor();
				Component = Actor ? Actor->FindComponentByExactClass<DStaticMeshComponent>() : nullptr;
				if (Actor && !Component)
					Component = Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
						DStaticMeshComponent::StaticClass(), "StaticMeshPreview"));
				if (Component == nullptr || StaticMesh == nullptr)
				{
					ResetPreview();
					return std::unexpected(QualifyDiagnostic(
						AssetPath, "The preview component is unavailable."));
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
						VisualContract.VerticalFieldOfViewDegrees,
					.NearClipDistance = ThumbnailView.NearClipDistance,
					.FarClipDistance = ThumbnailView.FarClipDistance,
					.ClearRed = VisualContract.bOutputOpaque
						? VisualContract.BackgroundRed
						: 0.0f,
					.ClearGreen = VisualContract.bOutputOpaque
						? VisualContract.BackgroundGreen
						: 0.0f,
					.ClearBlue = VisualContract.bOutputOpaque
						? VisualContract.BackgroundBlue
						: 0.0f,
					.ClearAlpha = VisualContract.bOutputOpaque
						? 1.0f
						: 0.0f};
				Context.SetView(View);
				return {};
			}

			auto ValidatePreparedInput() const -> std::expected<void, std::string>
			{
				if (StaticMesh == nullptr || Component == nullptr)
				{
					return std::unexpected(QualifyDiagnostic(
						AssetPath, "The StaticMesh asset is unavailable."));
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
						AssetPath.ToString()));
				}
				for (const auto& [Owner, Snapshot] : PreparedMaterials)
				{
					const auto* Material = Cast<DMaterialInterface>(Owner.Get());
					if (!Material || !Material->GetMaterialCompileStatus().IsCurrent()
						|| !Material->GetAcceptedCompiledProgram() || Material->GetMaterialResource() != Snapshot)
						return std::unexpected(QualifyDiagnostic(AssetPath,
							"A material changed while its thumbnail was being generated."));
				}
				return {};
			}

			auto ResetPreview() -> void
			{
				ResetScenePreview();
				StaticMesh = nullptr;
				Shaders = {};
				bShadersRequested = false;
			}

			auto ResetScenePreview() -> void
			{
				PreparedMaterials.clear();
				if (Component)
				{
					Component->ClearMaterialOverrides();
					Component->SetStaticMesh(nullptr);
				}
				Component = nullptr;
			}

			FThumbnailVisualContract VisualContract;
			FTopLevelAssetPath AssetPath;
			DStaticMesh* StaticMesh = nullptr;
			uint64 AssetRevision = 0;
			uint64 PreparedResourceRevision = 0;
			std::vector<std::pair<FWeakObjectPtr, FMaterialResource>> PreparedMaterials;
			DThumbnailRenderer& Renderer;
			DStaticMeshComponent* Component = nullptr;
		};
	} // namespace

	DStaticMeshThumbnailRenderer::DStaticMeshThumbnailRenderer(const FObjectInitializer& Initializer) : Super(Initializer) {}

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

	auto DStaticMeshThumbnailRenderer::DescribeThumbnail(const FAssetThumbnailPackageFingerprint& Asset) -> std::expected<FThumbnailCacheDescription, std::string>
	{
		FThumbnailCacheDescription Result;

		const FThumbnailRenderingInfo Registration =
			GetRegistration();
		if (Asset.AssetClassName != Registration.AssetClassName)
		{
			return std::unexpected("The StaticMesh thumbnail renderer received the wrong asset class.");
		}

		const FAssetCatalogSnapshot Catalog =
			CaptureAssetCatalogSnapshot();
		const FPackageData* Root =
			Catalog.FindExact(Asset.PackagePath);
		if (Root == nullptr)
		{
			return std::unexpected(std::format(
				"StaticMesh thumbnail registry data is missing for {}.",
				Asset.AssetPath.ToString()));
		}
		if (MakeStaticMeshThumbnailFingerprint(*Root, Asset.AssetPath) != Asset)
		{
			return std::unexpected(std::format(
				"StaticMesh thumbnail registry data changed for {}; refresh the request snapshot.",
				Asset.AssetPath.ToString()));
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
			Asset.PackagePath, Nodes);
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
		Result = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(
				FStaticMeshThumbnailRendererContract::PreviewFixtureIdentity),
			.PreviewFixtureVersion =
				FStaticMeshThumbnailRendererContract::PreviewFixtureVersion,
			.ShaderContractVersion =
				FStaticMeshThumbnailRendererContract::ShaderContractVersion,
			.Dependencies = std::move(*Dependencies)};

		return Result;
	}

	auto DStaticMeshThumbnailRenderer::CanVisualizeAsset(DObject* Object) const -> bool
	{
		return Cast<DStaticMesh>(Object) != nullptr;
	}

	auto DStaticMeshThumbnailRenderer::Draw(DObject* Object, FThumbnailDrawContext& Context) -> void
	{
		if (!CanVisualizeAsset(Object)) { Context.SetResult({.Diagnostic = "The renderer cannot visualize this asset."}); return; }
		auto& State = Context.GetState<FStaticMeshThumbnailDrawState>(*this, Object, Context.GetAssetPath());
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
