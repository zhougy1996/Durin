#include "Renderers/DirectionalShadowView.h"

#include "Math/Operations.h"
#include "Scene.h"

namespace Durin
{
	auto PrepareDirectionalShadowFilter(
		EDirectionalShadowFilterQuality Quality) -> FDirectionalShadowFilter
	{
		FDirectionalShadowFilter Result;
		switch (Quality)
		{
		case EDirectionalShadowFilterQuality::Low:
			break;
		case EDirectionalShadowFilterQuality::Medium:
			Result.Quality = Quality;
			Result.ComparisonOperations = 9;
			Result.GuardTexels = DirectionalShadowMediumGuardTexels;
			Result.FootprintRadiusTexels = 1.5f;
			break;
		case EDirectionalShadowFilterQuality::High:
			Result.Quality = Quality;
			Result.ComparisonOperations = 25;
			Result.GuardTexels = DirectionalShadowHighGuardTexels;
			Result.FootprintRadiusTexels = 2.5f;
			break;
		default:
			Result.bUsedInvalidQualityFallback = true;
			break;
		}
		return Result;
	}

	auto CalculateDirectionalShadowBias(
		const FVector2& TexelWorldSize,
		double SurfaceLightCosine) -> FDirectionalShadowBias
	{
		FDirectionalShadowBias Result;
		const double Texel = std::max(TexelWorldSize.x, TexelWorldSize.y);
		if (!std::isfinite(Texel) || Texel <= 0.0
			|| !std::isfinite(SurfaceLightCosine))
		{
			Result.bUsedFallback = true;
			return Result;
		}
		const double Grazing = 1.0 - std::clamp(std::abs(SurfaceLightCosine), 0.0, 1.0);
		Result.RasterConstant = static_cast<float>(
			std::clamp(1.0 + 2.0 * Texel, 1.0, 1.5));
		Result.RasterSlope = static_cast<float>(
			std::clamp(1.25 + Texel, 1.25, 2.0));
		Result.RasterClamp = static_cast<float>(
			std::clamp(2.0 + 8.0 * Texel, 2.0, 4.0));
		Result.ReceiverWorld = static_cast<float>(std::clamp(
			Texel * (0.05 + 0.10 * Grazing), 0.0005,
			static_cast<double>(DirectionalShadowMaximumReceiverWorldBias)));
		Result.NormalWorld = static_cast<float>(std::clamp(
			Texel * (0.20 + 0.55 * Grazing), 0.0,
			static_cast<double>(DirectionalShadowMaximumNormalOffset)));
		const float MaximumTotal = static_cast<float>(std::min(
			0.75 * Texel,
			static_cast<double>(DirectionalShadowMaximumTotalWorldBias)));
		if (Result.ReceiverWorld + Result.NormalWorld > MaximumTotal)
		{
			Result.NormalWorld = std::max(
				0.0f, MaximumTotal - Result.ReceiverWorld);
			Result.bTotalClamped = true;
		}
		Result.NormalizedRasterSeparation = std::clamp(
			(Result.RasterConstant + Result.RasterSlope) / 8.0f, 0.0f, 1.0f);
		return Result;
	}

	namespace
	{
		inline constexpr double MatrixEpsilon = 1.0e-8;
		inline constexpr double ParallelAxisEpsilon = 1.0e-4;
		inline constexpr double BoundsContactEpsilon = 1.0e-9;

		auto SetRow(FMatrix& Matrix, uint32 Row, const FVector4& Value) -> void
		{
			Matrix[0][Row] = Value.x;
			Matrix[1][Row] = Value.y;
			Matrix[2][Row] = Value.z;
			Matrix[3][Row] = Value.w;
		}

		auto IsPerspectiveProjection(const FMatrix& Projection) -> bool
		{
			return Math::IsFinite(Projection)
				&& std::abs(Projection[0][3]) > MatrixEpsilon
				&& std::abs(Projection[1][3]) <= MatrixEpsilon
				&& std::abs(Projection[2][3]) <= MatrixEpsilon
				&& std::abs(Projection[3][3]) <= MatrixEpsilon;
		}

		auto IsOrthographicProjection(const FMatrix& Projection) -> bool
		{
			return Math::IsFinite(Projection)
				&& std::abs(Projection[0][3]) <= MatrixEpsilon
				&& std::abs(Projection[1][3]) <= MatrixEpsilon
				&& std::abs(Projection[2][3]) <= MatrixEpsilon
				&& std::abs(Projection[3][3]) > MatrixEpsilon;
		}

		auto TryTransformPoint(
			const FMatrix& Matrix, const FVector4& Point, FVector3& Out) -> bool
		{
			FVector4 Value = Matrix * Point;
			if (!Math::IsFinite(Value) || std::abs(Value.w) <= MatrixEpsilon)
				return false;
			Value /= Value.w;
			Out = FVector3(Value);
			return Math::IsFinite(Out);
		}

		auto TryBuildReceiverCorners(
			const FSceneView& View, std::array<FVector3, 8>& OutCorners) -> bool
		{
			const bool bPerspective = IsPerspectiveProjection(View.ProjectionMatrix);
			if (!bPerspective && !IsOrthographicProjection(View.ProjectionMatrix))
				return false;
			FMatrix ClipToWorld;
			if (!Math::TryInverse(
					View.ViewProjectionMatrix, ClipToWorld, MatrixEpsilon))
				return false;
			for (uint32 Corner = 0; Corner < 4; ++Corner)
			{
				const double X = (Corner & 1u) != 0 ? 1.0 : -1.0;
				const double Y = (Corner & 2u) != 0 ? 1.0 : -1.0;
				if (!TryTransformPoint(
						ClipToWorld, FVector4(X, Y, 0.0, 1.0),
						OutCorners[Corner])
					|| !TryTransformPoint(
						ClipToWorld, FVector4(X, Y, 1.0, 1.0),
						OutCorners[Corner + 4]))
					return false;

				const FVector3 Origin = bPerspective
					? View.ViewLocation : OutCorners[Corner];
				const FVector3 Delta = OutCorners[Corner + 4] - Origin;
				const double Length = Math::Length(Delta);
				if (!Math::IsFinite(Origin) || !Math::IsFinite(Delta)
					|| !std::isfinite(Length) || Length <= MatrixEpsilon)
					return false;
				const double ClampedLength = std::min(
					Length, DirectionalShadowDistance);
				OutCorners[Corner + 4] = Origin + Delta * (ClampedLength / Length);
				if (Math::LengthSquared(
						OutCorners[Corner + 4] - OutCorners[Corner])
					<= MatrixEpsilon * MatrixEpsilon)
					return false;
			}
			return true;
		}

		auto BuildWorldToLight(
			const FVector3& Right, const FVector3& Up,
			const FVector3& Forward) -> FMatrix
		{
			FMatrix Result(1.0);
			SetRow(Result, 0, FVector4(Right, 0.0));
			SetRow(Result, 1, FVector4(Up, 0.0));
			SetRow(Result, 2, FVector4(Forward, 0.0));
			SetRow(Result, 3, FVector4(0.0, 0.0, 0.0, 1.0));
			return Result;
		}

		auto BuildLightProjection(
			const FVector3& Minimum, const FVector3& Maximum) -> FMatrix
		{
			const double HalfX = (Maximum.x - Minimum.x) * 0.5;
			const double HalfY = (Maximum.y - Minimum.y) * 0.5;
			const double CenterX = (Minimum.x + Maximum.x) * 0.5;
			const double CenterY = (Minimum.y + Maximum.y) * 0.5;
			const double Depth = Maximum.z - Minimum.z;
			FMatrix Result(0.0);
			SetRow(Result, 0, {1.0 / HalfX, 0.0, 0.0, -CenterX / HalfX});
			SetRow(Result, 1, {0.0, 1.0 / HalfY, 0.0, -CenterY / HalfY});
			SetRow(Result, 2, {0.0, 0.0, 1.0 / Depth, -Minimum.z / Depth});
			SetRow(Result, 3, {0.0, 0.0, 0.0, 1.0});
			return Result;
		}

		auto BuildClipToTexture() -> FMatrix
		{
			FMatrix Result(1.0);
			SetRow(Result, 0, {0.5, 0.0, 0.0, 0.5});
			SetRow(Result, 1, {0.0, 0.5, 0.0, 0.5});
			return Result;
		}
	}

	auto TryPrepareDirectionalShadowView(
		const FSceneView& View,
		FLightSceneId LightId,
		const FDirectionalLightSceneData& Light,
		FPreparedDirectionalShadowView& OutShadow) -> bool
	{
		FPreparedDirectionalShadowView Candidate;
		if (!Light.bCastShadows || LightId == InvalidLightSceneId
			|| !Math::IsFinite(Light.Direction)
			|| Math::LengthSquared(Light.Direction) <= MatrixEpsilon * MatrixEpsilon
			|| !TryBuildReceiverCorners(View, Candidate.ReceiverCorners))
			return false;

		Candidate.LightId = LightId;
		Candidate.Filter = PrepareDirectionalShadowFilter(
			View.Settings.DirectionalShadowFilterQuality);
		Candidate.CasterVolume.Forward = Math::Normalize(Light.Direction);
		const FVector3 PreferredUp{0.0, 0.0, 1.0};
		const FVector3 FallbackUp{0.0, 1.0, 0.0};
		const FVector3 ReferenceUp = std::abs(glm::dot(
			Candidate.CasterVolume.Forward, PreferredUp))
			> 1.0 - ParallelAxisEpsilon ? FallbackUp : PreferredUp;
		Candidate.CasterVolume.Right = Math::Normalize(glm::cross(
			ReferenceUp, Candidate.CasterVolume.Forward));
		Candidate.CasterVolume.Up = Math::Normalize(glm::cross(
			Candidate.CasterVolume.Forward, Candidate.CasterVolume.Right));
		if (!Math::IsFinite(Candidate.CasterVolume.Right)
			|| !Math::IsFinite(Candidate.CasterVolume.Up))
			return false;

		FVector3 Minimum(std::numeric_limits<double>::max());
		FVector3 Maximum(std::numeric_limits<double>::lowest());
		for (const FVector3& Corner : Candidate.ReceiverCorners)
		{
			const FVector3 LightSpace{
				glm::dot(Candidate.CasterVolume.Right, Corner),
				glm::dot(Candidate.CasterVolume.Up, Corner),
				glm::dot(Candidate.CasterVolume.Forward, Corner)};
			Minimum = glm::min(Minimum, LightSpace);
			Maximum = glm::max(Maximum, LightSpace);
		}
		const double RawWidth = Maximum.x - Minimum.x;
		const double RawHeight = Maximum.y - Minimum.y;
		if (!Math::IsFinite(Minimum) || !Math::IsFinite(Maximum)
			|| RawWidth <= MatrixEpsilon || RawHeight <= MatrixEpsilon
			|| Maximum.z - Minimum.z <= MatrixEpsilon)
			return false;
		const double InnerResolution = static_cast<double>(
			DirectionalShadowResolution - 2 * Candidate.Filter.GuardTexels);
		double HalfX = RawWidth * 0.5 * DirectionalShadowResolution
			/ InnerResolution;
		double HalfY = RawHeight * 0.5 * DirectionalShadowResolution
			/ InnerResolution;
		Candidate.TexelWorldSize = {
			2.0 * HalfX / DirectionalShadowResolution,
			2.0 * HalfY / DirectionalShadowResolution};
		Candidate.Bias = CalculateDirectionalShadowBias(
			Candidate.TexelWorldSize);
		Candidate.LightDirection = Light.Direction;
		Candidate.DiagnosticMode = static_cast<size_t>(
			View.Settings.DirectionalShadowDiagnosticMode)
			< static_cast<size_t>(EDirectionalShadowDiagnosticMode::Count)
			? View.Settings.DirectionalShadowDiagnosticMode
			: EDirectionalShadowDiagnosticMode::Lit;
		double CenterX = (Minimum.x + Maximum.x) * 0.5;
		double CenterY = (Minimum.y + Maximum.y) * 0.5;
		CenterX = std::round(CenterX / Candidate.TexelWorldSize.x)
			* Candidate.TexelWorldSize.x;
		CenterY = std::round(CenterY / Candidate.TexelWorldSize.y)
			* Candidate.TexelWorldSize.y;
		Minimum.x = CenterX - HalfX;
		Maximum.x = CenterX + HalfX;
		Minimum.y = CenterY - HalfY;
		Maximum.y = CenterY + HalfY;
		Minimum.z -= DirectionalShadowCasterExtrusion;
		Candidate.CasterVolume.Minimum = Minimum;
		Candidate.CasterVolume.Maximum = Maximum;
		Candidate.LightViewMatrix = BuildWorldToLight(
			Candidate.CasterVolume.Right, Candidate.CasterVolume.Up,
			Candidate.CasterVolume.Forward);
		Candidate.LightProjectionMatrix = BuildLightProjection(Minimum, Maximum);
		Candidate.LightViewProjectionMatrix =
			Candidate.LightProjectionMatrix * Candidate.LightViewMatrix;
		Candidate.WorldToShadowMatrix = BuildClipToTexture()
			* Candidate.LightViewProjectionMatrix;
		if (!Math::IsFinite(Candidate.LightViewProjectionMatrix)
			|| !Math::IsFinite(Candidate.WorldToShadowMatrix))
			return false;

		Candidate.CasterView.ViewMatrix = Candidate.LightViewMatrix;
		Candidate.CasterView.ProjectionMatrix = Candidate.LightProjectionMatrix;
		Candidate.CasterView.ViewProjectionMatrix =
			Candidate.LightViewProjectionMatrix;
		Candidate.CasterView.ViewportWidth = DirectionalShadowResolution;
		Candidate.CasterView.ViewportHeight = DirectionalShadowResolution;
		Candidate.CasterView.Settings.RasterMode = ERasterMode::Solid;
		Candidate.bEnabled = true;
		OutShadow = Candidate;
		return true;
	}

	auto ClassifyDirectionalShadowCasterBounds(
		const FPreparedDirectionalShadowView& Shadow,
		const FBox& WorldBounds) -> EDirectionalShadowBoundsClassification
	{
		if (!Shadow.bEnabled || !WorldBounds.bIsValid
			|| !Math::IsFinite(WorldBounds.Min) || !Math::IsFinite(WorldBounds.Max)
			|| glm::any(glm::greaterThan(WorldBounds.Min, WorldBounds.Max)))
			return EDirectionalShadowBoundsClassification::InvalidBoundsFallback;
		FVector3 Minimum(std::numeric_limits<double>::max());
		FVector3 Maximum(std::numeric_limits<double>::lowest());
		for (uint32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector3 World{
				(Corner & 1u) != 0 ? WorldBounds.Max.x : WorldBounds.Min.x,
				(Corner & 2u) != 0 ? WorldBounds.Max.y : WorldBounds.Min.y,
				(Corner & 4u) != 0 ? WorldBounds.Max.z : WorldBounds.Min.z};
			const FVector3 LightSpace{
				glm::dot(Shadow.CasterVolume.Right, World),
				glm::dot(Shadow.CasterVolume.Up, World),
				glm::dot(Shadow.CasterVolume.Forward, World)};
			Minimum = glm::min(Minimum, LightSpace);
			Maximum = glm::max(Maximum, LightSpace);
		}
		const FVector3 AbsoluteMinimum = glm::abs(Minimum);
		const FVector3 AbsoluteMaximum = glm::abs(Maximum);
		const double Scale = std::max({
			1.0,
			AbsoluteMinimum.x,
			AbsoluteMinimum.y,
			AbsoluteMinimum.z,
			AbsoluteMaximum.x,
			AbsoluteMaximum.y,
			AbsoluteMaximum.z});
		const double Epsilon = BoundsContactEpsilon * Scale;
		const bool bOutside = Maximum.x < Shadow.CasterVolume.Minimum.x - Epsilon
			|| Minimum.x > Shadow.CasterVolume.Maximum.x + Epsilon
			|| Maximum.y < Shadow.CasterVolume.Minimum.y - Epsilon
			|| Minimum.y > Shadow.CasterVolume.Maximum.y + Epsilon
			|| Maximum.z < Shadow.CasterVolume.Minimum.z - Epsilon
			|| Minimum.z > Shadow.CasterVolume.Maximum.z + Epsilon;
		return bOutside ? EDirectionalShadowBoundsClassification::Outside
			: EDirectionalShadowBoundsClassification::InsideOrIntersecting;
	}

	auto MakeDirectionalShadowSamplerDesc() -> FRHISamplerDesc
	{
		FRHISamplerDesc Result = FRHISamplerDesc::LinearClamp();
		Result.AddressU = ESamplerAddressMode::ClampToBorder;
		Result.AddressV = ESamplerAddressMode::ClampToBorder;
		Result.AddressW = ESamplerAddressMode::ClampToBorder;
		Result.bEnableCompare = true;
		Result.CompareOp = ESamplerCompareOp::LessOrEqual;
		Result.MinLod = 0.0f;
		Result.MaxLod = 0.0f;
		Result.BorderColor = ESamplerBorderColor::FloatOpaqueWhite;
		return Result;
	}

	auto PrepareDirectionalShadowCasterCandidates(
		const FScene& Scene,
		const FPreparedDirectionalShadowView& Shadow,
		bool bDisableCulling) -> FDirectionalShadowCasterCandidates
	{
		FDirectionalShadowCasterCandidates Result;
		for (const FPrimitiveSceneInfo* Info : Scene.GetPrimitiveSceneInfos())
		{
			check(Info != nullptr);
			if (Info == nullptr) continue;
			++Result.Submitted;
			if (!Info->IsVisible())
			{
				++Result.Hidden;
				continue;
			}
			const EDirectionalShadowBoundsClassification Classification =
				bDisableCulling
					? EDirectionalShadowBoundsClassification::InsideOrIntersecting
					: ClassifyDirectionalShadowCasterBounds(
						Shadow, Info->GetWorldBounds());
			if (Classification == EDirectionalShadowBoundsClassification::Outside)
			{
				++Result.Culled;
				continue;
			}
			if (Classification
				== EDirectionalShadowBoundsClassification::InvalidBoundsFallback)
				++Result.InvalidBoundsFallbacks;
			switch (Info->GetKind())
			{
			case EPrimitiveSceneProxyKind::StaticMesh:
				Result.StaticMeshes.push_back(Info);
				break;
			case EPrimitiveSceneProxyKind::SplineMesh:
				Result.SplineMeshes.push_back(Info);
				break;
			case EPrimitiveSceneProxyKind::SkeletalMesh:
				Result.SkeletalMeshes.push_back(Info);
				break;
			case EPrimitiveSceneProxyKind::Terrain:
				Result.Terrains.push_back(Info);
				break;
			}
		}
		check(Result.Submitted == Result.Hidden + Result.Culled
			+ Result.StaticMeshes.size() + Result.SplineMeshes.size()
			+ Result.SkeletalMeshes.size() + Result.Terrains.size());
		return Result;
	}
}
