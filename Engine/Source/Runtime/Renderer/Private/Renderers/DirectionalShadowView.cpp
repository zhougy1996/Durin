#include "Renderers/DirectionalShadowView.h"

#include "SceneViewProjection.h"

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
		Result.RasterConstant = static_cast<float>(
			std::clamp(1.0 + 2.0 * Texel, 1.0, 1.5));
		Result.RasterSlope = static_cast<float>(
			std::clamp(1.25 + Texel, 1.25, 2.0));
		Result.RasterClamp = static_cast<float>(
			std::clamp(2.0 + 8.0 * Texel, 2.0, 4.0));
		Result.ReceiverWorld = 0.0f;
		Result.NormalWorld = 0.0f;
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

		struct FReceiverFrustum
		{
			bool bPerspective = false;
			double NearDepth = 0.0;
			double FarDepth = 0.0;
			double FittedFarDepth = 0.0;
			FVector4 DepthTransform{0.0};
			std::array<FVector3, 8> Corners{};
		};

		auto GetRow(const FMatrix& Matrix, uint32 Row) -> FVector4
		{
			return {Matrix[0][Row], Matrix[1][Row], Matrix[2][Row], Matrix[3][Row]};
		}

		auto TryBuildReceiverFrustum(
			const FSceneView& View, FReceiverFrustum& OutFrustum) -> bool
		{
			const bool bPerspective = IsPerspectiveProjection(View.ProjectionMatrix);
			if (!bPerspective && !IsOrthographicProjection(View.ProjectionMatrix))
				return false;
			if (!Math::IsFinite(View.ViewMatrix)) return false;
			FMatrix ClipToWorld;
			if (!Math::TryInverse(
					View.ViewProjectionMatrix, ClipToWorld, MatrixEpsilon))
				return false;
			OutFrustum.bPerspective = bPerspective;
			OutFrustum.DepthTransform = GetRow(View.ViewMatrix, 0);
			const double NearDeviceDepth =
				SceneViewProjection::GetNearDeviceDepth(View.DepthConvention);
			const double FarDeviceDepth =
				SceneViewProjection::GetFarDeviceDepth(View.DepthConvention);
			double NearMinimum = std::numeric_limits<double>::max();
			double NearMaximum = std::numeric_limits<double>::lowest();
			double FarMinimum = std::numeric_limits<double>::max();
			double FarMaximum = std::numeric_limits<double>::lowest();
			for (uint32 Corner = 0; Corner < 4; ++Corner)
			{
				const double X = (Corner & 1u) != 0 ? 1.0 : -1.0;
				const double Y = (Corner & 2u) != 0 ? 1.0 : -1.0;
				if (!TryTransformPoint(
						ClipToWorld, FVector4(X, Y, NearDeviceDepth, 1.0),
						OutFrustum.Corners[Corner])
					|| !TryTransformPoint(
						ClipToWorld, FVector4(X, Y, FarDeviceDepth, 1.0),
						OutFrustum.Corners[Corner + 4]))
					return false;
				const FVector4 NearView = View.ViewMatrix
					* FVector4(OutFrustum.Corners[Corner], 1.0);
				const FVector4 FarView = View.ViewMatrix
					* FVector4(OutFrustum.Corners[Corner + 4], 1.0);
				if (!Math::IsFinite(NearView) || !Math::IsFinite(FarView)) return false;
				NearMinimum = std::min(NearMinimum, NearView.x);
				NearMaximum = std::max(NearMaximum, NearView.x);
				FarMinimum = std::min(FarMinimum, FarView.x);
				FarMaximum = std::max(FarMaximum, FarView.x);
			}
			const double Scale = std::max({1.0, std::abs(NearMinimum),
				std::abs(NearMaximum), std::abs(FarMinimum), std::abs(FarMaximum)});
			if (!std::isfinite(NearMinimum) || !std::isfinite(FarMinimum)
				|| NearMinimum <= MatrixEpsilon
				|| FarMinimum <= NearMaximum + MatrixEpsilon
				|| NearMaximum - NearMinimum > Scale * 1.0e-6
				|| FarMaximum - FarMinimum > Scale * 1.0e-6)
				return false;
			OutFrustum.NearDepth = (NearMinimum + NearMaximum) * 0.5;
			OutFrustum.FittedFarDepth = (FarMinimum + FarMaximum) * 0.5;
			OutFrustum.FarDepth = std::min(
				OutFrustum.FittedFarDepth, DirectionalShadowDistance);
			if (OutFrustum.FarDepth <= OutFrustum.NearDepth + MatrixEpsilon)
				return false;
			return true;
		}

		auto TryBuildLegacyReceiverCorners(
			const FSceneView& View,
			std::array<FVector3, 8>& OutCorners) -> bool
		{
			const bool bPerspective = IsPerspectiveProjection(View.ProjectionMatrix);
			if (!bPerspective && !IsOrthographicProjection(View.ProjectionMatrix))
				return false;
			FMatrix ClipToWorld;
			if (!Math::TryInverse(
					View.ViewProjectionMatrix, ClipToWorld, MatrixEpsilon)) return false;
			const double NearDeviceDepth =
				SceneViewProjection::GetNearDeviceDepth(View.DepthConvention);
			const double FarDeviceDepth =
				SceneViewProjection::GetFarDeviceDepth(View.DepthConvention);
			for (uint32 Corner = 0; Corner < 4; ++Corner)
			{
				const double X = (Corner & 1u) != 0 ? 1.0 : -1.0;
				const double Y = (Corner & 2u) != 0 ? 1.0 : -1.0;
				if (!TryTransformPoint(ClipToWorld,
						FVector4(X, Y, NearDeviceDepth, 1.0), OutCorners[Corner])
					|| !TryTransformPoint(ClipToWorld,
						FVector4(X, Y, FarDeviceDepth, 1.0), OutCorners[Corner + 4]))
					return false;
				const FVector3 Origin = bPerspective
					? View.ViewLocation : OutCorners[Corner];
				const FVector3 Delta = OutCorners[Corner + 4] - Origin;
				const double Length = Math::Length(Delta);
				if (!Math::IsFinite(Origin) || !Math::IsFinite(Delta)
					|| !std::isfinite(Length) || Length <= MatrixEpsilon) return false;
				const double ClampedLength = std::min(
					Length, DirectionalShadowDistance);
				OutCorners[Corner + 4] = Origin
					+ Delta * (ClampedLength / Length);
			}
			return true;
		}

		auto BuildReceiverSlice(
			const FReceiverFrustum& Frustum,
			double NearDepth,
			double FarDepth,
			std::array<FVector3, 8>& OutCorners) -> bool
		{
			for (uint32 Corner = 0; Corner < 4; ++Corner)
			{
				const FVector3& FullNear = Frustum.Corners[Corner];
				const FVector3& FullFar = Frustum.Corners[Corner + 4];
				const double Denominator =
					Frustum.FittedFarDepth - Frustum.NearDepth;
				if (!std::isfinite(Denominator) || Denominator <= MatrixEpsilon)
					return false;
				const double NearAlpha = (NearDepth - Frustum.NearDepth) / Denominator;
				const double FarAlpha = (FarDepth - Frustum.NearDepth) / Denominator;
				OutCorners[Corner] = FullNear + (FullFar - FullNear) * NearAlpha;
				OutCorners[Corner + 4] = FullNear + (FullFar - FullNear) * FarAlpha;
				if (!Math::IsFinite(OutCorners[Corner])
					|| !Math::IsFinite(OutCorners[Corner + 4])) return false;
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

		auto TryFitCascade(
			const std::array<FVector3, 8>& ReceiverCorners,
			const FDirectionalShadowFilter& Filter,
			const FVector3& Right,
			const FVector3& Up,
			const FVector3& Forward,
			uint32 Layer,
			double NearDepth,
			double FarDepth,
			double TransitionStartDepth,
			FPreparedDirectionalShadowCascade& OutCascade) -> bool
		{
			FPreparedDirectionalShadowCascade Candidate;
			Candidate.Layer = Layer;
			Candidate.NearDepth = NearDepth;
			Candidate.FarDepth = FarDepth;
			Candidate.TransitionStartDepth = TransitionStartDepth;
			Candidate.Filter = Filter;
			Candidate.ReceiverCorners = ReceiverCorners;
			Candidate.CasterVolume.Right = Right;
			Candidate.CasterVolume.Up = Up;
			Candidate.CasterVolume.Forward = Forward;
			FVector3 Minimum(std::numeric_limits<double>::max());
			FVector3 Maximum(std::numeric_limits<double>::lowest());
			for (const FVector3& Corner : Candidate.ReceiverCorners)
			{
				const FVector3 LightSpace{
					Math::Dot(Right, Corner), Math::Dot(Up, Corner),
					Math::Dot(Forward, Corner)};
				Minimum = Math::Min(Minimum, LightSpace);
				Maximum = Math::Max(Maximum, LightSpace);
			}
			const double RawWidth = Maximum.x - Minimum.x;
			const double RawHeight = Maximum.y - Minimum.y;
			if (!Math::IsFinite(Minimum) || !Math::IsFinite(Maximum)
				|| RawWidth <= MatrixEpsilon || RawHeight <= MatrixEpsilon
				|| Maximum.z - Minimum.z <= MatrixEpsilon)
				return false;
			const double InnerResolution = static_cast<double>(
				DirectionalShadowResolution - 2 * Filter.GuardTexels);
			double HalfX = RawWidth * 0.5 * DirectionalShadowResolution
				/ InnerResolution;
			double HalfY = RawHeight * 0.5 * DirectionalShadowResolution
				/ InnerResolution;
			Candidate.TexelWorldSize = {
				2.0 * HalfX / DirectionalShadowResolution,
				2.0 * HalfY / DirectionalShadowResolution};
			Candidate.Bias = CalculateDirectionalShadowBias(
				Candidate.TexelWorldSize);
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
			Candidate.LightViewMatrix = BuildWorldToLight(Right, Up, Forward);
			Candidate.LightProjectionMatrix = BuildLightProjection(Minimum, Maximum);
			Candidate.LightViewProjectionMatrix =
				Candidate.LightProjectionMatrix * Candidate.LightViewMatrix;
			Candidate.WorldToShadowMatrix = BuildClipToTexture()
				* Candidate.LightViewProjectionMatrix;
			if (!Math::IsFinite(Candidate.LightViewProjectionMatrix)
				|| !Math::IsFinite(Candidate.WorldToShadowMatrix)) return false;
			Candidate.CasterView.ViewMatrix = Candidate.LightViewMatrix;
			Candidate.CasterView.ProjectionMatrix = Candidate.LightProjectionMatrix;
			Candidate.CasterView.ViewProjectionMatrix =
				Candidate.LightViewProjectionMatrix;
			Candidate.CasterView.ViewportWidth = DirectionalShadowResolution;
			Candidate.CasterView.ViewportHeight = DirectionalShadowResolution;
			Candidate.CasterView.DepthConvention = ESceneDepthConvention::ForwardZ;
			Candidate.CasterView.Settings.Mode.RasterMode = ERasterMode::Solid;
			Candidate.bEnabled = true;
			OutCascade = Candidate;
			return true;
		}
	}

	auto TryPrepareDirectionalShadowView(
		const FSceneView& View,
		FLightSceneId LightId,
		const FDirectionalLightSceneData& Light,
		FPreparedDirectionalShadowView& OutShadow) -> bool
	{
		FPreparedDirectionalShadowView Candidate;
		FReceiverFrustum ReceiverFrustum;
		const EDirectionalShadowCandidate RequestedCandidate =
			View.Settings.DirectionalShadow.Candidate
			== EDirectionalShadowCandidate::ThreeCascades
			? EDirectionalShadowCandidate::ThreeCascades
			: EDirectionalShadowCandidate::SingleMap;
		if (!Light.bCastShadows || LightId == InvalidLightSceneId
			|| !Math::IsFinite(Light.Direction)
			|| Math::LengthSquared(Light.Direction) <= MatrixEpsilon * MatrixEpsilon)
			return false;
		std::array<FVector3, 8> LegacyCorners;
		if (RequestedCandidate == EDirectionalShadowCandidate::ThreeCascades)
		{
			if (!TryBuildReceiverFrustum(View, ReceiverFrustum)) return false;
		}
		else if (!TryBuildLegacyReceiverCorners(View, LegacyCorners)) return false;

		Candidate.LightId = LightId;
		Candidate.Candidate = RequestedCandidate;
		Candidate.CascadeCount = Candidate.Candidate
			== EDirectionalShadowCandidate::ThreeCascades
			? DirectionalShadowCascadeCount : 1u;
		Candidate.ViewDepthTransform = RequestedCandidate
			== EDirectionalShadowCandidate::ThreeCascades
			? ReceiverFrustum.DepthTransform : FVector4{0.0};
		const FDirectionalShadowFilter Filter = PrepareDirectionalShadowFilter(
			View.Settings.DirectionalShadow.FilterQuality);
		const FVector3 Forward = Math::Normalize(Light.Direction);
		const FVector3 PreferredUp{0.0, 0.0, 1.0};
		const FVector3 FallbackUp{0.0, 1.0, 0.0};
		const FVector3 ReferenceUp = std::abs(Math::Dot(
			Forward, PreferredUp))
			> 1.0 - ParallelAxisEpsilon ? FallbackUp : PreferredUp;
		const FVector3 Right = Math::Normalize(Math::Cross(ReferenceUp, Forward));
		const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));
		if (!Math::IsFinite(Right) || !Math::IsFinite(Up))
			return false;
		Candidate.LightDirection = Light.Direction;
		Candidate.DiagnosticMode = static_cast<size_t>(
			View.Settings.DirectionalShadow.DiagnosticMode)
			< static_cast<size_t>(EDirectionalShadowDiagnosticMode::Count)
			? View.Settings.DirectionalShadow.DiagnosticMode
			: EDirectionalShadowDiagnosticMode::Lit;
		Candidate.SplitDepths[0] = RequestedCandidate
			== EDirectionalShadowCandidate::ThreeCascades
			? ReceiverFrustum.NearDepth : 0.0;
		Candidate.SplitDepths[Candidate.CascadeCount] = RequestedCandidate
			== EDirectionalShadowCandidate::ThreeCascades
			? ReceiverFrustum.FarDepth : DirectionalShadowDistance;
		for (uint32 Boundary = 1; Boundary < Candidate.CascadeCount; ++Boundary)
		{
			const double P = static_cast<double>(Boundary)
				/ static_cast<double>(Candidate.CascadeCount);
			const double Uniform = ReceiverFrustum.NearDepth
				+ (ReceiverFrustum.FarDepth - ReceiverFrustum.NearDepth) * P;
			const double Logarithmic = ReceiverFrustum.bPerspective
				? ReceiverFrustum.NearDepth * std::pow(
					ReceiverFrustum.FarDepth / ReceiverFrustum.NearDepth, P)
				: Uniform;
			Candidate.SplitDepths[Boundary] = ReceiverFrustum.bPerspective
				? DirectionalShadowSplitLambda * Logarithmic
					+ (1.0 - DirectionalShadowSplitLambda) * Uniform
				: Uniform;
		}
		for (uint32 CascadeIndex = 0;
			CascadeIndex < Candidate.CascadeCount; ++CascadeIndex)
		{
			const double NearDepth = Candidate.SplitDepths[CascadeIndex];
			const double FarDepth = Candidate.SplitDepths[CascadeIndex + 1];
			if (!std::isfinite(NearDepth) || !std::isfinite(FarDepth)
				|| FarDepth <= NearDepth + MatrixEpsilon) return false;
			std::array<FVector3, 8> SliceCorners{};
			if (RequestedCandidate == EDirectionalShadowCandidate::ThreeCascades)
			{
				if (!BuildReceiverSlice(
						ReceiverFrustum, NearDepth, FarDepth, SliceCorners)) return false;
			}
			else
			{
				SliceCorners = LegacyCorners;
			}
			const double TransitionStart = CascadeIndex == 0 ? NearDepth
				: NearDepth - DirectionalShadowTransitionFraction
					* (FarDepth - NearDepth);
			if (!TryFitCascade(SliceCorners, Filter, Right, Up, Forward,
					CascadeIndex, NearDepth, FarDepth, TransitionStart,
					Candidate.Cascades[CascadeIndex])) return false;
		}
		Candidate.bEnabled = true;
		OutShadow = Candidate;
		return true;
	}

	auto ClassifyDirectionalShadowCasterBounds(
		const FPreparedDirectionalShadowCascade& Cascade,
		const FBox& WorldBounds) -> EDirectionalShadowBoundsClassification
	{
		if (!Cascade.bEnabled || !WorldBounds.bIsValid
			|| !Math::IsFinite(WorldBounds.Min) || !Math::IsFinite(WorldBounds.Max)
			|| Math::AnyGreaterThan(WorldBounds.Min, WorldBounds.Max))
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
				Math::Dot(Cascade.CasterVolume.Right, World),
				Math::Dot(Cascade.CasterVolume.Up, World),
				Math::Dot(Cascade.CasterVolume.Forward, World)};
			Minimum = Math::Min(Minimum, LightSpace);
			Maximum = Math::Max(Maximum, LightSpace);
		}
		const FVector3 AbsoluteMinimum = Math::Abs(Minimum);
		const FVector3 AbsoluteMaximum = Math::Abs(Maximum);
		const double Scale = std::max({
			1.0,
			AbsoluteMinimum.x,
			AbsoluteMinimum.y,
			AbsoluteMinimum.z,
			AbsoluteMaximum.x,
			AbsoluteMaximum.y,
			AbsoluteMaximum.z});
		const double Epsilon = BoundsContactEpsilon * Scale;
		const bool bOutside = Maximum.x < Cascade.CasterVolume.Minimum.x - Epsilon
			|| Minimum.x > Cascade.CasterVolume.Maximum.x + Epsilon
			|| Maximum.y < Cascade.CasterVolume.Minimum.y - Epsilon
			|| Minimum.y > Cascade.CasterVolume.Maximum.y + Epsilon
			|| Maximum.z < Cascade.CasterVolume.Minimum.z - Epsilon
			|| Minimum.z > Cascade.CasterVolume.Maximum.z + Epsilon;
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

	auto PrepareDirectionalShadowCasterTable(
		const FScene& Scene,
		const FPreparedDirectionalShadowView& Shadow,
		bool bDisableCulling) -> FDirectionalShadowCasterTable
	{
		static_assert(DirectionalShadowCascadeCount <= 8);
		FDirectionalShadowCasterTable Result;
		check(Shadow.CascadeCount <= DirectionalShadowCascadeCount);
		if (Shadow.CascadeCount > DirectionalShadowCascadeCount) return Result;
		Result.SceneTraversals = 1;
		Result.Records.reserve(Scene.GetPrimitiveSceneInfos().size());
		for (const FPrimitiveSceneInfo* Info : Scene.GetPrimitiveSceneInfos())
		{
			check(Info != nullptr);
			if (Info == nullptr) continue;
			++Result.UniqueSubmitted;
			if (!Info->IsVisible())
			{
				++Result.UniqueHidden;
				continue;
			}
			FDirectionalShadowCasterRecord Record;
			Record.SceneInfo = Info;
			switch (Info->GetKind())
			{
			case EPrimitiveSceneProxyKind::StaticMesh:
				Record.Kind = EDirectionalShadowCasterKind::StaticMesh;
				++Result.UniqueEligibleStaticMeshes;
				break;
			case EPrimitiveSceneProxyKind::SplineMesh:
				Record.Kind = EDirectionalShadowCasterKind::SplineMesh;
				++Result.UniqueEligibleSplineMeshes;
				break;
			case EPrimitiveSceneProxyKind::SkeletalMesh:
				Record.Kind = EDirectionalShadowCasterKind::SkeletalMesh;
				++Result.UniqueEligibleSkeletalMeshes;
				break;
			case EPrimitiveSceneProxyKind::Terrain:
				Record.Kind = EDirectionalShadowCasterKind::Terrain;
				++Result.UniqueEligibleTerrains;
				break;
			}
			for (uint32 CascadeIndex = 0;
				 CascadeIndex < Shadow.CascadeCount; ++CascadeIndex)
			{
				const auto& Cascade = Shadow.Cascades[CascadeIndex];
				if (!Cascade.bEnabled) continue;
				++Result.CascadeClassificationTests;
				const EDirectionalShadowBoundsClassification Classification =
					bDisableCulling
						? EDirectionalShadowBoundsClassification::InsideOrIntersecting
						: ClassifyDirectionalShadowCasterBounds(
							Cascade, Info->GetWorldBounds());
				if (Classification == EDirectionalShadowBoundsClassification::Outside)
					continue;
				const uint8 Bit = static_cast<uint8>(1u << CascadeIndex);
				Record.CascadeMask |= Bit;
				if (Classification
					== EDirectionalShadowBoundsClassification::InvalidBoundsFallback)
					Record.InvalidBoundsFallbackMask |= Bit;
			}
			Result.Records.push_back(Record);
		}
		size_t EnabledCascades = 0;
		for (uint32 CascadeIndex = 0;
			 CascadeIndex < Shadow.CascadeCount; ++CascadeIndex)
			EnabledCascades += Shadow.Cascades[CascadeIndex].bEnabled ? 1u : 0u;
		check(Result.CascadeClassificationTests
			== Result.Records.size() * EnabledCascades);
		for (uint32 CascadeIndex = 0;
			 CascadeIndex < Shadow.CascadeCount; ++CascadeIndex)
		{
			auto& Candidates = Result.Cascades[CascadeIndex];
			Candidates.Submitted = Result.UniqueSubmitted;
			Candidates.Hidden = Result.UniqueHidden;
			const uint8 Bit = static_cast<uint8>(1u << CascadeIndex);
			std::array<size_t, 4> FamilyMemberships{};
			for (const FDirectionalShadowCasterRecord& Record : Result.Records)
			{
				if ((Record.CascadeMask & Bit) == 0) continue;
				switch (Record.Kind)
				{
				case EDirectionalShadowCasterKind::StaticMesh:
					++FamilyMemberships[0];
					break;
				case EDirectionalShadowCasterKind::SplineMesh:
					++FamilyMemberships[1];
					break;
				case EDirectionalShadowCasterKind::SkeletalMesh:
					++FamilyMemberships[2];
					break;
				case EDirectionalShadowCasterKind::Terrain:
					++FamilyMemberships[3];
					break;
				}
			}
			Candidates.StaticMeshes.reserve(FamilyMemberships[0]);
			Candidates.SplineMeshes.reserve(FamilyMemberships[1]);
			Candidates.SkeletalMeshes.reserve(FamilyMemberships[2]);
			Candidates.Terrains.reserve(FamilyMemberships[3]);
			for (const FDirectionalShadowCasterRecord& Record : Result.Records)
			{
				if ((Record.CascadeMask & Bit) == 0) continue;
				++Result.MembershipPopcount;
				if ((Record.InvalidBoundsFallbackMask & Bit) != 0)
					++Candidates.InvalidBoundsFallbacks;
				switch (Record.Kind)
				{
				case EDirectionalShadowCasterKind::StaticMesh:
					Candidates.StaticMeshes.push_back(Record.SceneInfo);
					break;
				case EDirectionalShadowCasterKind::SplineMesh:
					Candidates.SplineMeshes.push_back(Record.SceneInfo);
					break;
				case EDirectionalShadowCasterKind::SkeletalMesh:
					Candidates.SkeletalMeshes.push_back(Record.SceneInfo);
					break;
				case EDirectionalShadowCasterKind::Terrain:
					Candidates.Terrains.push_back(Record.SceneInfo);
					break;
				}
			}
			const size_t Memberships = Candidates.StaticMeshes.size()
				+ Candidates.SplineMeshes.size()
				+ Candidates.SkeletalMeshes.size() + Candidates.Terrains.size();
			Candidates.Culled = Result.Records.size() - Memberships;
			check(Candidates.Submitted == Candidates.Hidden + Candidates.Culled
				+ Memberships);
			Result.TemporaryBytes += Candidates.StaticMeshes.capacity()
				* sizeof(const FPrimitiveSceneInfo*);
			Result.TemporaryBytes += Candidates.SplineMeshes.capacity()
				* sizeof(const FPrimitiveSceneInfo*);
			Result.TemporaryBytes += Candidates.SkeletalMeshes.capacity()
				* sizeof(const FPrimitiveSceneInfo*);
			Result.TemporaryBytes += Candidates.Terrains.capacity()
				* sizeof(const FPrimitiveSceneInfo*);
		}
		Result.TemporaryBytes += Result.Records.capacity()
			* sizeof(FDirectionalShadowCasterRecord);
		return Result;
	}

	auto SelectDirectionalShadowCascade(
		const FPreparedDirectionalShadowView& Shadow,
		double ReceiverDepth,
		uint32& OutCascade,
		uint32& OutNearCascade,
		double& OutTransitionWeight) -> bool
	{
		OutCascade = 0;
		OutNearCascade = 0;
		OutTransitionWeight = 0.0;
		if (!Shadow.bEnabled || Shadow.CascadeCount == 0
			|| Shadow.CascadeCount > DirectionalShadowCascadeCount
			|| !std::isfinite(ReceiverDepth)
			|| ReceiverDepth < Shadow.SplitDepths[0]
			|| ReceiverDepth > Shadow.SplitDepths[Shadow.CascadeCount])
			return false;
		uint32 Primary = Shadow.CascadeCount - 1;
		for (uint32 Index = 0; Index < Shadow.CascadeCount; ++Index)
		{
			if (ReceiverDepth <= Shadow.SplitDepths[Index + 1])
			{
				Primary = Index;
				break;
			}
		}
		OutCascade = Primary;
		OutNearCascade = Primary;
		if (Primary + 1 < Shadow.CascadeCount)
		{
			const auto& FarCascade = Shadow.Cascades[Primary + 1];
			if (ReceiverDepth >= FarCascade.TransitionStartDepth)
			{
				const double Width = FarCascade.NearDepth
					- FarCascade.TransitionStartDepth;
				if (!std::isfinite(Width) || Width <= MatrixEpsilon) return false;
				OutCascade = Primary + 1;
				OutTransitionWeight = std::clamp(
					(ReceiverDepth - FarCascade.TransitionStartDepth) / Width,
					0.0, 1.0);
			}
		}
		return true;
	}
}
