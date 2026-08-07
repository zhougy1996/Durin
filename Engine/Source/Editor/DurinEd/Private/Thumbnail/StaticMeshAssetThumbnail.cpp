#include "Thumbnail/StaticMeshAssetThumbnail.h"

#include "AssetSystem.h"
#include "Math/Operations.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	namespace
	{
		constexpr double MinimumNearClipDistance = 1.0e-6;
		constexpr double ClipPaddingFraction = 0.05;

		auto MakeStaticMeshThumbnailFingerprint(const Asset::FAssetData& Data)
			-> FAssetThumbnailPackageFingerprint
		{
			return {
				.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}

		auto FailStaticMeshThumbnailView(
			FStaticMeshAssetThumbnailView& OutView,
			std::string& OutError,
			std::string_view Error) -> bool
		{
			OutView = {};
			OutError = Error;
			return false;
		}
	} // namespace

	auto FStaticMeshAssetThumbnailProvider::GetRegistration() const
		-> FAssetThumbnailProviderRegistration
	{
		return {
			.AssetClassName = DStaticMesh::StaticClass()
				->GetQualifiedName().ToString(),
			.ProviderName =
				std::string(FStaticMeshAssetThumbnailContract::ProviderName),
			.GeneratorSchemaVersion =
				FStaticMeshAssetThumbnailContract::GeneratorSchemaVersion};
	}

	auto FStaticMeshAssetThumbnailProvider::CaptureGenerationRequest(
		const FAssetThumbnailRequest& Request,
		uint64 ProviderGeneration,
		FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError.clear();
		const FAssetThumbnailProviderRegistration Registration =
			GetRegistration();
		if (Request.Asset.AssetClassName != Registration.AssetClassName)
		{
			OutError =
				"The StaticMesh thumbnail provider received the wrong asset class.";
			return false;
		}

		const Asset::FAssetRegistry& Registry = Asset::GetAssetRegistry();
		const Asset::FAssetData* Root =
			Registry.FindAssetExact(Request.Asset.VirtualPath);
		if (Root == nullptr)
		{
			OutError = std::format(
				"StaticMesh thumbnail registry data is missing for {}.",
				Request.Asset.VirtualPath.ToString());
			return false;
		}
		if (MakeStaticMeshThumbnailFingerprint(*Root) != Request.Asset)
		{
			OutError = std::format(
				"StaticMesh thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.VirtualPath.ToString());
			return false;
		}

		std::vector<FAssetThumbnailDependencyNode> Nodes;
		Nodes.reserve(Registry.GetAssets().size());
		for (const auto& [Path, Data] : Registry.GetAssets())
		{
			Nodes.push_back({
				.Package = MakeStaticMeshThumbnailFingerprint(Data),
				.Dependencies = Data.Dependencies});
		}
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
		if (!BuildAssetThumbnailDependencyClosure(
				Request.Asset.VirtualPath, Nodes, Dependencies, OutError))
		{
			return false;
		}

		FRenderedAssetThumbnailVisualContract Visual;
		const FStaticMeshAssetThumbnailViewInput ViewContract;
		Visual.CameraDirectionX =
			static_cast<float>(ViewContract.CameraDirection.x);
		Visual.CameraDirectionY =
			static_cast<float>(ViewContract.CameraDirection.y);
		Visual.CameraDirectionZ =
			static_cast<float>(ViewContract.CameraDirection.z);
		Visual.VerticalFieldOfViewDegrees =
			static_cast<float>(ViewContract.VerticalFieldOfViewDegrees);
		Visual.bOutputOpaque =
			FStaticMeshAssetThumbnailContract::bOutputOpaque;
		OutRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(
				FStaticMeshAssetThumbnailContract::PreviewFixtureIdentity),
			.PreviewFixtureVersion =
				FStaticMeshAssetThumbnailContract::PreviewFixtureVersion,
			.ShaderContractVersion =
				FStaticMeshAssetThumbnailContract::ShaderContractVersion,
			.Dependencies = std::move(Dependencies)};
		OutRequest.Input =
			std::make_shared<FStaticMeshAssetThumbnailGenerationInput>(
				Request.Asset.VirtualPath, std::move(Visual));
		OutRequest.ProviderGeneration = ProviderGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		return true;
	}

	auto CalculateStaticMeshAssetThumbnailView(
		const FStaticMeshAssetThumbnailViewInput& Input,
		FStaticMeshAssetThumbnailView& OutView,
		std::string& OutError) -> bool
	{
		if (!Input.LocalBounds.bIsValid
			|| !Math::IsFinite(Input.LocalBounds.Min)
			|| !Math::IsFinite(Input.LocalBounds.Max))
		{
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail bounds must be finite and valid.");
		}

		const FVector3 BoundsSize = Input.LocalBounds.Max - Input.LocalBounds.Min;
		if (!Math::IsFinite(BoundsSize)
			|| BoundsSize.x <= 0.0
			|| BoundsSize.y <= 0.0
			|| BoundsSize.z <= 0.0)
		{
			return FailStaticMeshThumbnailView(
				OutView, OutError, "Static-mesh thumbnail bounds must have finite non-zero volume.");
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

		FStaticMeshAssetThumbnailView Result;
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
} // namespace Durin
