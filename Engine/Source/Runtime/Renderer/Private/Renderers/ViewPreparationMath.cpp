#include "Renderers/ViewPreparationMath.h"

#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		inline constexpr double PlaneTouchRelativeEpsilon = 1.0e-9;
		inline constexpr double ProjectionKindEpsilon = 1.0e-12;

		auto GetRow(const FMatrix& Matrix, uint32 Row) -> FVector4
		{
			return {
				Matrix[0][Row],
				Matrix[1][Row],
				Matrix[2][Row],
				Matrix[3][Row]};
		}

		auto IsFinite(const FVector4& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				&& std::isfinite(Value.z) && std::isfinite(Value.w);
		}

		auto IsFiniteBounds(const FBox& Bounds) -> bool
		{
			return Bounds.bIsValid && Math::IsFinite(Bounds.Min)
				&& Math::IsFinite(Bounds.Max)
				&& Bounds.Min.x <= Bounds.Max.x
				&& Bounds.Min.y <= Bounds.Max.y
				&& Bounds.Min.z <= Bounds.Max.z;
		}

		auto GetBoundsScale(const FBox& Bounds) -> double
		{
			const FVector3 AbsoluteMin = Math::Abs(Bounds.Min);
			const FVector3 AbsoluteMax = Math::Abs(Bounds.Max);
			return std::max({
				1.0,
				AbsoluteMin.x,
				AbsoluteMin.y,
				AbsoluteMin.z,
				AbsoluteMax.x,
				AbsoluteMax.y,
				AbsoluteMax.z});
		}

		auto IsSupportedProjection(const FMatrix& ProjectionMatrix) -> bool
		{
			if (!Math::IsFinite(ProjectionMatrix))
			{
				return false;
			}
			const FVector4 WRow = GetRow(ProjectionMatrix, 3);
			const bool bPerspective =
				std::abs(WRow.x) > ProjectionKindEpsilon
				&& std::abs(WRow.y) <= ProjectionKindEpsilon
				&& std::abs(WRow.z) <= ProjectionKindEpsilon
				&& std::abs(WRow.w) <= ProjectionKindEpsilon;
			const bool bOrthographic =
				std::abs(WRow.x) <= ProjectionKindEpsilon
				&& std::abs(WRow.y) <= ProjectionKindEpsilon
				&& std::abs(WRow.z) <= ProjectionKindEpsilon
				&& std::abs(WRow.w) > ProjectionKindEpsilon;
			return bPerspective || bOrthographic;
		}
	} // namespace

	auto TryBuildViewFrustum(
		const FMatrix& ViewProjectionMatrix,
		FViewFrustum& OutFrustum) -> bool
	{
		if (!Math::IsFinite(ViewProjectionMatrix))
		{
			return false;
		}
		const FVector4 Row0 = GetRow(ViewProjectionMatrix, 0);
		const FVector4 Row1 = GetRow(ViewProjectionMatrix, 1);
		const FVector4 Row2 = GetRow(ViewProjectionMatrix, 2);
		const FVector4 Row3 = GetRow(ViewProjectionMatrix, 3);
		FViewFrustum Candidate{{
			Row3 + Row0,
			Row3 - Row0,
			Row3 + Row1,
			Row3 - Row1,
			Row2,
			Row3 - Row2}};
		for (FVector4& Plane : Candidate.Planes)
		{
			const double NormalLength = Math::Length(FVector3(Plane));
			if (!IsFinite(Plane) || !std::isfinite(NormalLength)
				|| NormalLength <= ProjectionKindEpsilon)
			{
				return false;
			}
			Plane /= NormalLength;
		}
		OutFrustum = Candidate;
		return true;
	}

	auto TryBuildViewFrustum(
		const FSceneView& View,
		FViewFrustum& OutFrustum) -> bool
	{
		return IsSupportedProjection(View.ProjectionMatrix)
			&& TryBuildViewFrustum(View.ViewProjectionMatrix, OutFrustum);
	}

	auto ClassifyWorldBounds(
		const FViewFrustum& Frustum,
		const FBox& WorldBounds) -> EViewBoundsClassification
	{
		if (!IsFiniteBounds(WorldBounds))
		{
			return EViewBoundsClassification::InvalidBounds;
		}
		const FVector3 Center = WorldBounds.GetCenter();
		const FVector3 Extent = WorldBounds.GetExtent();
		const double Epsilon =
			PlaneTouchRelativeEpsilon * GetBoundsScale(WorldBounds);
		bool bIntersects = false;
		for (const FVector4& Plane : Frustum.Planes)
		{
			if (!IsFinite(Plane))
			{
				return EViewBoundsClassification::InvalidBounds;
			}
			const FVector3 Normal = FVector3(Plane);
			const double CenterDistance = Math::Dot(Normal, Center) + Plane.w;
			const double Radius = Math::Dot(Math::Abs(Normal), Extent);
			if (!std::isfinite(CenterDistance) || !std::isfinite(Radius))
			{
				return EViewBoundsClassification::InvalidBounds;
			}
			if (CenterDistance + Radius < -Epsilon)
			{
				return EViewBoundsClassification::Outside;
			}
			bIntersects |= CenterDistance - Radius <= Epsilon;
		}
		return bIntersects ? EViewBoundsClassification::Intersecting
						   : EViewBoundsClassification::Inside;
	}

	auto ComputeProjectedScreenSize(
		const FSceneView& View,
		const FBox& WorldBounds) -> FProjectedScreenSizeResult
	{
		if (!IsFiniteBounds(WorldBounds))
		{
			return {1.0f, EProjectedScreenSizeStatus::InvalidBounds};
		}
		if (View.ViewportWidth == 0 || View.ViewportHeight == 0
			|| !Math::IsFinite(View.ViewProjectionMatrix)
			|| !IsSupportedProjection(View.ProjectionMatrix))
		{
			return {1.0f, EProjectedScreenSizeStatus::InvalidView};
		}

		double MinimumX = std::numeric_limits<double>::max();
		double MinimumY = std::numeric_limits<double>::max();
		double MaximumX = std::numeric_limits<double>::lowest();
		double MaximumY = std::numeric_limits<double>::lowest();
		for (uint32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			const FVector3 Corner{
				(CornerIndex & 1u) != 0 ? WorldBounds.Max.x : WorldBounds.Min.x,
				(CornerIndex & 2u) != 0 ? WorldBounds.Max.y : WorldBounds.Min.y,
				(CornerIndex & 4u) != 0 ? WorldBounds.Max.z : WorldBounds.Min.z};
			const FVector4 Clip =
				View.ViewProjectionMatrix * FVector4(Corner, 1.0);
			const double CrossingEpsilon =
				PlaneTouchRelativeEpsilon * std::max(1.0, std::abs(Clip.w));
			if (!IsFinite(Clip))
			{
				return {1.0f, EProjectedScreenSizeStatus::InvalidView};
			}
			const double NearPlaneDistance =
				View.DepthConvention == ESceneDepthConvention::ReversedZ
					? Clip.w - Clip.z : Clip.z;
			if (Clip.w <= CrossingEpsilon || NearPlaneDistance <= CrossingEpsilon)
			{
				return {
					1.0f,
					EProjectedScreenSizeStatus::NearPlaneOrCameraCrossing};
			}
			const double NdcX = Clip.x / Clip.w;
			const double NdcY = Clip.y / Clip.w;
			if (!std::isfinite(NdcX) || !std::isfinite(NdcY))
			{
				return {1.0f, EProjectedScreenSizeStatus::InvalidView};
			}
			MinimumX = std::min(MinimumX, NdcX);
			MinimumY = std::min(MinimumY, NdcY);
			MaximumX = std::max(MaximumX, NdcX);
			MaximumY = std::max(MaximumY, NdcY);
		}

		const double MinimumDimension = static_cast<double>(
			std::min(View.ViewportWidth, View.ViewportHeight));
		const double DiameterPixels = 0.5 * std::max(
			(MaximumX - MinimumX) * View.ViewportWidth,
			(MaximumY - MinimumY) * View.ViewportHeight);
		const double NormalizedSize = DiameterPixels / MinimumDimension;
		if (!std::isfinite(NormalizedSize) || NormalizedSize < 0.0)
		{
			return {1.0f, EProjectedScreenSizeStatus::InvalidView};
		}
		return {
			static_cast<float>(std::clamp(NormalizedSize, 0.0, 1.0)),
			EProjectedScreenSizeStatus::Valid};
	}

	auto MakeDefaultStaticMeshLODScreenSizes(uint32 NumLODs)
		-> std::vector<float>
	{
		std::vector<float> Result(NumLODs, 0.0f);
		for (uint32 LODIndex = 0; LODIndex + 1 < NumLODs; ++LODIndex)
		{
			Result[LODIndex] =
				std::ldexp(1.0f, -static_cast<int>(LODIndex + 1));
		}
		return Result;
	}

	auto ValidateStaticMeshLODScreenSizes(
		std::span<const float> ScreenSizes) -> bool
	{
		if (ScreenSizes.empty() || ScreenSizes.back() != 0.0f)
		{
			return false;
		}
		for (size_t Index = 0; Index < ScreenSizes.size(); ++Index)
		{
			if (!std::isfinite(ScreenSizes[Index]) || ScreenSizes[Index] < 0.0f
				|| ScreenSizes[Index] > 1.0f)
			{
				return false;
			}
			if (Index > 0 && ScreenSizes[Index - 1] <= ScreenSizes[Index])
			{
				return false;
			}
		}
		return true;
	}

	auto SelectStaticMeshLOD(
		float NormalizedScreenSize,
		std::span<const float> ScreenSizes) -> uint32
	{
		if (!std::isfinite(NormalizedScreenSize)
			|| NormalizedScreenSize < 0.0f || NormalizedScreenSize > 1.0f
			|| !ValidateStaticMeshLODScreenSizes(ScreenSizes))
		{
			return 0;
		}
		for (uint32 LODIndex = 0;
			 LODIndex < static_cast<uint32>(ScreenSizes.size());
			 ++LODIndex)
		{
			if (NormalizedScreenSize >= ScreenSizes[LODIndex])
			{
				return LODIndex;
			}
		}
		return static_cast<uint32>(ScreenSizes.size() - 1);
	}

	auto ResolveAvailableStaticMeshLOD(
		uint32 RequestedLOD,
		std::span<const uint8> ReadyLODs) -> uint32
	{
		if (RequestedLOD >= ReadyLODs.size())
		{
			return InvalidStaticMeshLODIndex;
		}
		for (uint32 LODIndex = RequestedLOD;
			 LODIndex < static_cast<uint32>(ReadyLODs.size());
			 ++LODIndex)
		{
			if (ReadyLODs[LODIndex] != 0)
			{
				return LODIndex;
			}
		}
		for (uint32 LODIndex = RequestedLOD; LODIndex > 0; --LODIndex)
		{
			if (ReadyLODs[LODIndex - 1] != 0)
			{
				return LODIndex - 1;
			}
		}
		return InvalidStaticMeshLODIndex;
	}
} // namespace Durin
