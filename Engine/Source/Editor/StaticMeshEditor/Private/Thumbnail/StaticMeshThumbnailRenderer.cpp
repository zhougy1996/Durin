#include "Thumbnail/StaticMeshThumbnailRenderer.h"

// StaticMeshEditor owns the complete StaticMesh thumbnail extension.

#include "Asset/Asset.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"

namespace Durin::Editor::StaticMesh
{
	namespace
	{
		constexpr double MinimumNearClipDistance = 1.0e-6;
		constexpr double ClipPaddingFraction = 0.05;

		auto MakeStaticMeshThumbnailFingerprint(const FAssetData& Data,
			FTopLevelAssetPath AssetPath = {})
			-> ::Durin::Editor::FAssetThumbnailPackageFingerprint
		{
			return {
				.AssetPath = std::move(AssetPath),
				.PackagePath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}

		auto FailStaticMeshThumbnailView(
			FStaticMeshThumbnailRendererView& OutView,
			std::string& OutError,
			std::string_view Error) -> bool
		{
			OutView = {};
			OutError = Error;
			return false;
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
			: public ::Durin::Editor::IThumbnailRendererSession
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

			auto Load() -> ::Durin::Editor::FThumbnailRendererSessionUpdate override
			{
				DObject* Loaded = nullptr;
				const FAssetResult Result = LoadObject(Input.AssetPath, Loaded);
				StaticMesh = Result ? Cast<DStaticMesh>(Loaded) : nullptr;
				if (!Result || StaticMesh == nullptr
					|| StaticMesh->GetClass() != DStaticMesh::StaticClass())
				{
					StaticMesh = nullptr;
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = Result.Message.empty()
							? std::format(
								"The requested asset '{}' is not an exact DStaticMesh.",
								Input.AssetPath.ToString())
							: Result.Message};
				}
				return PollStaticMeshReadiness();
			}

			auto PollResources() -> ::Durin::Editor::FThumbnailRendererSessionUpdate override
			{
				if (StaticMesh == nullptr)
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = QualifyDiagnostic(
							Input.AssetPath, "The StaticMesh asset is unavailable.")};
				return PollStaticMeshReadiness();
			}

		private:
			auto PollStaticMeshReadiness()
				-> ::Durin::Editor::FThumbnailRendererSessionUpdate
			{
				if (HasPendingStaticMeshCompilation(*StaticMesh))
					return {.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
				FStaticMeshRenderResourceStatus Status =
					StaticMesh->GetRenderResourceStatus();
				if (Status.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
				{
					if (StaticMesh->EnsureRenderDataLoadedBlocking()) StaticMesh->InitResources();
					Status = StaticMesh->GetRenderResourceStatus();
				}
				if (!StaticMesh->GetLOD0LocalBounds())
				{
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.AssetRevision = Status.Revision,
						.ResourceRevision = Status.Revision,
						.Diagnostic = std::format(
							"StaticMesh '{}' has no finite, valid LOD 0 bounds.",
							Input.AssetPath.ToString())};
				}
				if (!bCapturedAssetRevision)
				{
					AssetRevision = Status.Revision;
					bCapturedAssetRevision = true;
				}
				switch (Status.Readiness)
				{
				case EStaticMeshRenderResourceReadiness::Ready:
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::ReadyToRender,
						.AssetRevision = AssetRevision,
						.ResourceRevision = Status.Revision};
				case EStaticMeshRenderResourceReadiness::Queued:
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources,
						.AssetRevision = AssetRevision,
						.ResourceRevision = Status.Revision};
				case EStaticMeshRenderResourceReadiness::Failed:
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.AssetRevision = AssetRevision,
						.ResourceRevision = Status.Revision,
						.Diagnostic = QualifyDiagnostic(
							Input.AssetPath, "The render resource failed.")};
				case EStaticMeshRenderResourceReadiness::Unavailable:
				default:
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.AssetRevision = AssetRevision,
						.ResourceRevision = Status.Revision,
						.Diagnostic = QualifyDiagnostic(
							Input.AssetPath, "The render resource is unavailable.")};
				}
			}

			auto PreparePreview(
				::Durin::Editor::IThumbnailPreviewScene& PreviewScene,
				std::string& OutError) -> bool override
			{
				ResetPreview();
				const std::optional<FBox> Bounds = StaticMesh
					? StaticMesh->GetLOD0LocalBounds()
					: std::nullopt;
				FStaticMeshThumbnailRendererView ThumbnailView;
				if (!Bounds || !CalculateStaticMeshThumbnailRendererView({
						.LocalBounds = Bounds.value_or(FBox()),
						.OutputAspectRatio =
							static_cast<double>(Input.VisualContract.Output.Width)
							/ static_cast<double>(Input.VisualContract.Output.Height),
						.VerticalFieldOfViewDegrees =
							Input.VisualContract.VerticalFieldOfViewDegrees,
						.CameraDirection = FVector3(
							Input.VisualContract.CameraDirectionX,
							Input.VisualContract.CameraDirectionY,
							Input.VisualContract.CameraDirectionZ)},
					ThumbnailView,
					OutError))
				{
					OutError = QualifyDiagnostic(Input.AssetPath, OutError);
					return false;
				}
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
					OutError = QualifyDiagnostic(
						Input.AssetPath, "The preview component is unavailable.");
					ResetPreview();
					return false;
				}
				Component->ClearMaterialOverrides();
				Component->SetStaticMesh(StaticMesh);
				Component->SetWorldTransform(ThumbnailView.MeshTransform);
				const ::Durin::Editor::FThumbnailPreviewView View{
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
				if (!PreviewScene.SetView(View, OutError))
				{
					OutError = QualifyDiagnostic(Input.AssetPath, OutError);
					ResetPreview();
					return false;
				}
				return true;
			}

			auto ValidateRevisions(
				uint64 ExpectedAssetRevision,
				uint64 ExpectedResourceRevision,
				std::string& OutError) const -> bool override
			{
				if (StaticMesh == nullptr)
				{
					OutError = QualifyDiagnostic(
						Input.AssetPath, "The StaticMesh asset is unavailable.");
					return false;
				}
				const FStaticMeshRenderResourceStatus Status =
					StaticMesh->GetRenderResourceStatus();
				if (Status.Readiness != EStaticMeshRenderResourceReadiness::Ready
					|| AssetRevision != ExpectedAssetRevision
					|| Status.Revision != ExpectedResourceRevision)
				{
					OutError = std::format(
						"StaticMesh '{}' changed while its thumbnail was being generated.",
						Input.AssetPath.ToString());
					return false;
				}
				OutError.clear();
				return true;
			}

			auto ResetPreview() -> void override
			{
				if (World != nullptr && Actor != nullptr) World->DestroyActor(Actor);
				Component = nullptr;
				Actor = nullptr;
				World = nullptr;
			}

			FStaticMeshThumbnailRendererGenerationInput Input;
			DStaticMesh* StaticMesh = nullptr;
			uint64 AssetRevision = 0;
			bool bCapturedAssetRevision = false;
			DWorld* World = nullptr;
			AActor* Actor = nullptr;
			DStaticMeshComponent* Component = nullptr;
		};
	} // namespace

	auto DStaticMeshThumbnailRenderer::GetRegistration() const
		-> ::Durin::Editor::FThumbnailRenderingInfo
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
		const ::Durin::Editor::FAssetThumbnailRequest& Request,
		uint64 RendererGeneration,
		::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError.clear();
		const ::Durin::Editor::FThumbnailRenderingInfo Registration =
			GetRegistration();
		if (Request.Asset.AssetClassName != Registration.AssetClassName)
		{
			OutError =
				"The StaticMesh thumbnail renderer received the wrong asset class.";
			return false;
		}

		const FAssetCatalogSnapshot Catalog =
			CaptureAssetCatalogSnapshot();
		const FAssetData* Root =
			Catalog.FindExact(Request.Asset.PackagePath);
		if (Root == nullptr)
		{
			OutError = std::format(
				"StaticMesh thumbnail registry data is missing for {}.",
				Request.Asset.AssetPath.ToString());
			return false;
		}
		if (MakeStaticMeshThumbnailFingerprint(*Root, Request.Asset.AssetPath) != Request.Asset)
		{
			OutError = std::format(
				"StaticMesh thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.AssetPath.ToString());
			return false;
		}

		std::vector<::Durin::Editor::FAssetThumbnailDependencyNode> Nodes;
		Nodes.reserve(Catalog.Assets.size());
		for (const auto& [Path, Data] : Catalog.Assets)
		{
			Nodes.push_back({
				.Package = MakeStaticMeshThumbnailFingerprint(Data),
				.Dependencies = Data.Dependencies});
		}
		std::vector<::Durin::Editor::FAssetThumbnailPackageFingerprint> Dependencies;
		if (!::Durin::Editor::BuildAssetThumbnailDependencyClosure(
				Request.Asset.PackagePath, Nodes, Dependencies, OutError))
		{
			return false;
		}

		::Durin::Editor::FThumbnailVisualContract Visual;
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
		OutRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(
				FStaticMeshThumbnailRendererContract::PreviewFixtureIdentity),
			.PreviewFixtureVersion =
				FStaticMeshThumbnailRendererContract::PreviewFixtureVersion,
			.ShaderContractVersion =
				FStaticMeshThumbnailRendererContract::ShaderContractVersion,
			.Dependencies = std::move(Dependencies)};
		OutRequest.Input =
			std::make_shared<FStaticMeshThumbnailRendererGenerationInput>(
				Request.Asset.AssetPath, std::move(Visual));
		OutRequest.RendererGeneration = RendererGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		return true;
	}

	auto DStaticMeshThumbnailRenderer::CreateGenerationSession(
		const ::Durin::Editor::FAssetThumbnailGenerationRequest&,
		const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
		std::string& OutError)
		-> std::unique_ptr<::Durin::Editor::IThumbnailRendererSession>
	{
		const auto* StaticMeshInput =
			dynamic_cast<const FStaticMeshThumbnailRendererGenerationInput*>(&Input);
		if (StaticMeshInput == nullptr)
		{
			OutError = "The StaticMesh thumbnail generation input is invalid.";
			return nullptr;
		}
		OutError.clear();
		return std::make_unique<FStaticMeshThumbnailGenerationSession>(*StaticMeshInput);
	}

	auto CalculateStaticMeshThumbnailRendererView(
		const FStaticMeshThumbnailRendererViewInput& Input,
		FStaticMeshThumbnailRendererView& OutView,
		std::string& OutError) -> bool
	{
		if (!Input.LocalBounds.bIsValid
			|| !Math::IsFinite(Input.LocalBounds.Min)
			|| !Math::IsFinite(Input.LocalBounds.Max))
		{
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail bounds must be finite and valid.");
		}

		if (!std::isfinite(Input.OutputAspectRatio) || Input.OutputAspectRatio <= 0.0)
		{
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail output aspect ratio must be positive and finite.");
		}
		if (!std::isfinite(Input.VerticalFieldOfViewDegrees)
			|| Input.VerticalFieldOfViewDegrees <= 0.0
			|| Input.VerticalFieldOfViewDegrees >= 180.0)
		{
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail field of view must be between zero and 180 degrees.");
		}
		if (!std::isfinite(Input.ImageMargin)
			|| Input.ImageMargin < 0.0
			|| Input.ImageMargin >= 1.0)
		{
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail image margin must be in [0, 1).");
		}

		FVector3 CameraDirection;
		if (!Math::TryNormalize(Input.CameraDirection, CameraDirection))
		{
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail camera direction must be finite and non-zero.");
		}
		const FVector3 CameraForward = -CameraDirection;
		FVector3 CameraRight;
		if (!Math::TryNormalize(
				Math::Cross(FVectorConstants::Up, CameraForward), CameraRight))
		{
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail camera direction cannot be parallel to world up.");
		}
		FVector3 CameraUp;
		if (!Math::TryNormalize(Math::Cross(CameraForward, CameraRight), CameraUp))
		{
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail camera basis is invalid.");
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
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail projection is invalid.");
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
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail camera distance is invalid.");
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
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail clip planes are invalid.");
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
		OutView = Result;
		OutError.clear();
		return true;
	}
} // namespace Durin::Editor::StaticMesh
