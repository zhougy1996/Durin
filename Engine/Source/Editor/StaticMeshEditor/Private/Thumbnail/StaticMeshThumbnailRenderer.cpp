#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"

// StaticMeshEditor owns the complete StaticMesh thumbnail extension.

#include "Asset/Asset.h"
#include "Components/StaticMeshComponent.h"
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

		auto PollMaterialReadiness(DStaticMesh* StaticMesh, FThumbnailDrawContext& Context) -> bool
		{
			for (const auto& Slot : StaticMesh->GetMaterialSlots())
			{
				const auto* Material = Slot.DefaultMaterial.Get();
				if (!Material) continue;
				const auto& Status = Material->GetMaterialCompileStatus();
				if (Status.IsCurrent() && Material->GetAcceptedCompiledProgram()) continue;
				if (Material->IsCompiling() || Status.HasScheduledEdits()) return false;
				const auto Diagnostics = Material->GetMaterialCompileDiagnostics();
				Context.Fail(std::format(
					"Material '{}' has no current compiled program. {}", Material->GetObjectPath(),
					Diagnostics.empty() ? std::string{} : Diagnostics.front().Message));
				return false;
			}
			return true;
		}
		auto PollStaticMeshReadiness(DStaticMesh* StaticMesh, FThumbnailDrawContext& Context) -> bool
		{
			if (!StaticMesh || StaticMesh->IsCompiling()) return false;
			auto Status = StaticMesh->GetRenderResourceStatus();
			if (Status.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
			{
				StaticMesh->RequestRenderDataAndResources();
				if (StaticMesh->GetRenderDataLoadError().Code != ECookedMeshLoadError::None)
				{
					Context.Fail("The mesh render data could not be loaded.");
					return false;
				}
				if (!StaticMesh->GetRenderData()) return false;
				Status = StaticMesh->GetRenderResourceStatus();
			}
			if (!StaticMesh->GetLOD0LocalBounds())
			{
				Context.Fail(std::format("StaticMesh '{}' has no finite, valid LOD 0 bounds.", StaticMesh->GetObjectPath()));
				return false;
			}
			switch (Status.Readiness)
			{
			case EStaticMeshRenderResourceReadiness::Ready:
			{
				return PollMaterialReadiness(StaticMesh, Context);
			}
			case EStaticMeshRenderResourceReadiness::Queued:
				return false;
			case EStaticMeshRenderResourceReadiness::InvalidData:
				Context.Fail("The render resource failed.");
				return false;
			default:
				Context.Fail("The render resource is unavailable.");
				return false;
			}
		}
	} // namespace

	DStaticMeshThumbnailRenderer::DStaticMeshThumbnailRenderer(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DStaticMeshThumbnailRenderer::CanVisualizeAsset(DObject* Object) const -> bool
	{
		return Cast<DStaticMesh>(Object) != nullptr;
	}

	auto DStaticMeshThumbnailRenderer::Draw(DObject* Object, FThumbnailDrawContext& Context) -> void
	{
		auto* StaticMesh = Cast<DStaticMesh>(Object);
		if (!StaticMesh) return Context.Fail("The renderer cannot visualize this asset.");
		FThumbnailVisualContract VisualContract;
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
		if (!PollStaticMeshReadiness(StaticMesh, Context)) return;
		if (!Shaders && GEngine && GEngine->GetRendererModule()) Shaders = GEngine->GetRendererModule()->PrepareSceneThumbnailShaders();
		if (!Shaders) return;
		auto Ready = Shaders->Poll();
		if (!Ready) return Context.Fail(Ready.error());
		if (!*Ready) return;
		std::vector<std::tuple<FWeakObjectPtr, uint64, std::shared_ptr<const FMaterialProgram>, FMaterialStaticProperties>> PreparedMaterials;
		for (const auto& Slot : StaticMesh->GetMaterialSlots())
			if (auto* Material = Slot.DefaultMaterial.Get())
				PreparedMaterials.emplace_back(FWeakObjectPtr(Material), Material->GetRenderChangeCount(),
					Material->GetAcceptedCompiledProgram(), Material->GetMaterialResourceProperties());
		const auto PreparedResourceRevision = StaticMesh->GetRenderResourceStatus().Revision;
		const auto Bounds = StaticMesh->GetLOD0LocalBounds();
		if (!Bounds) return Context.Fail("The mesh bounds are unavailable.");
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
			return Context.Fail(ViewResult.error());
		}
		const auto& ThumbnailView = *ViewResult;
		auto& Scene = GetPreviewScene();
		if (!Context.TrySetScene(Scene)) return;
		AActor* Actor = Scene.GetPreviewActor();
		auto* Component = Actor ? Actor->FindComponentByExactClass<DStaticMeshComponent>() : nullptr;
		if (Actor && !Component)
			Component = Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
				DStaticMeshComponent::StaticClass(), "StaticMeshPreview"));
		if (Component == nullptr)
		{
			return Context.Fail("The preview component is unavailable.");
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
		Context.SetValidation([Mesh = TWeakObjectPtr<DStaticMesh>(StaticMesh), PreparedResourceRevision, Materials = std::move(PreparedMaterials)] {
			const auto* Current = Mesh.Get();
			if (!Current) return false;
			const auto Status = Current->GetRenderResourceStatus();
			return Status.Readiness == EStaticMeshRenderResourceReadiness::Ready && Status.Revision == PreparedResourceRevision
				&& std::ranges::all_of(Materials, [](const auto& Item) {
					const auto& [Owner, Revision, Program, Properties] = Item;
					const auto* Material = Cast<DMaterialInterface>(Owner.Get());
					return Material && Material->GetMaterialCompileStatus().IsCurrent() && Material->GetAcceptedCompiledProgram()
						&& Material->GetRenderChangeCount() == Revision
						&& Material->GetAcceptedCompiledProgram() == Program
						&& Material->GetMaterialResourceProperties() == Properties;
				});
		});
		Context.MarkReady();
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
