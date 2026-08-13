#include <gtest/gtest.h>

#include "Renderers/SceneRenderer.h"
#include "Renderers/PreparedSceneView.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/ViewPreparationMath.h"

namespace Durin
{
	namespace
	{
		auto MakePerspectiveProjection(
			double VerticalFieldOfViewDegrees,
			double AspectRatio,
			double NearClip,
			double FarClip) -> FMatrix
		{
			const double YScale = 1.0 / std::tan(
				Math::DegreesToRadians(VerticalFieldOfViewDegrees) * 0.5);
			FMatrix Projection(0.0);
			Projection[1][0] = YScale / AspectRatio;
			Projection[2][1] = -YScale;
			Projection[0][2] = FarClip / (FarClip - NearClip);
			Projection[3][2] =
				-NearClip * FarClip / (FarClip - NearClip);
			Projection[0][3] = 1.0;
			return Projection;
		}

		auto MakeOrthographicProjection(
			double HalfWidth,
			double HalfHeight,
			double NearClip,
			double FarClip) -> FMatrix
		{
			FMatrix Projection(0.0);
			Projection[1][0] = 1.0 / HalfWidth;
			Projection[2][1] = -1.0 / HalfHeight;
			Projection[0][2] = 1.0 / (FarClip - NearClip);
			Projection[3][2] = -NearClip / (FarClip - NearClip);
			Projection[3][3] = 1.0;
			return Projection;
		}

		auto ExpectDepthMapping(
			const FMatrix& Projection,
			double NearClip,
			double FarClip) -> void
		{
			const FVector4 NearPoint =
				Projection * FVector4(NearClip, 0.0, 0.0, 1.0);
			const FVector4 FarPoint =
				Projection * FVector4(FarClip, 0.0, 0.0, 1.0);
			EXPECT_NEAR(NearPoint.z / NearPoint.w, 0.0, 1.0e-12);
			EXPECT_NEAR(FarPoint.z / FarPoint.w, 1.0, 1.0e-12);
		}
	} // namespace

	TEST(FRendererSceneViewTests, UnconstrainedViewsFitIndependentOutputs)
	{
		FSceneView View;
		View.ViewportX = 11;
		View.ViewportY = 13;
		View.ViewportWidth = 17;
		View.ViewportHeight = 19;

		const FSceneView MainView =
			FSceneRenderer::FitViewToOutput(View, 800, 600);
		const FSceneView AuxiliaryView =
			FSceneRenderer::FitViewToOutput(View, 320, 180);

		EXPECT_EQ(MainView.ViewportX, 0u);
		EXPECT_EQ(MainView.ViewportY, 0u);
		EXPECT_EQ(MainView.ViewportWidth, 800u);
		EXPECT_EQ(MainView.ViewportHeight, 600u);
		EXPECT_EQ(AuxiliaryView.ViewportX, 0u);
		EXPECT_EQ(AuxiliaryView.ViewportY, 0u);
		EXPECT_EQ(AuxiliaryView.ViewportWidth, 320u);
		EXPECT_EQ(AuxiliaryView.ViewportHeight, 180u);
		EXPECT_EQ(View.ViewportX, 11u);
		EXPECT_EQ(View.ViewportY, 13u);
		EXPECT_EQ(View.ViewportWidth, 17u);
		EXPECT_EQ(View.ViewportHeight, 19u);
	}

	TEST(FRendererSceneViewTests,
		CombinedTranslucencyOrdersDistanceThenCompleteStableTies)
	{
		FPreparedSceneView Prepared;
		FPreparedStaticMeshDraw StaticNear;
		StaticNear.TranslucentDistanceSquared = 10.0;
		StaticNear.SortKey.PrimitiveId = 20;
		Prepared.StaticMeshes.Translucent.push_back(StaticNear);
		FPreparedSkeletalMeshDraw SkeletalFar;
		SkeletalFar.TranslucentDistanceSquared = 20.0;
		SkeletalFar.SortKey.PrimitiveId = 30;
		Prepared.SkeletalMeshes.Translucent.push_back(SkeletalFar);
		FPreparedSkeletalMeshDraw SkeletalNear;
		SkeletalNear.TranslucentDistanceSquared = 10.0;
		SkeletalNear.SortKey.PrimitiveId = 10;
		Prepared.SkeletalMeshes.Translucent.push_back(SkeletalNear);
		FPreparedSkeletalMeshDraw ExactTie;
		ExactTie.TranslucentDistanceSquared = 10.0;
		ExactTie.SortKey.PrimitiveId = 20;
		Prepared.SkeletalMeshes.Translucent.push_back(ExactTie);

		PrepareCombinedTranslucentGeometry(Prepared);

		ASSERT_EQ(Prepared.TranslucentGeometry.size(), 4u);
		EXPECT_EQ(Prepared.TranslucentGeometry[0].Family,
			EPreparedTranslucentGeometryFamily::SkeletalMesh);
		EXPECT_EQ(Prepared.TranslucentGeometry[0].SortKey.PrimitiveId, 30u);
		EXPECT_EQ(Prepared.TranslucentGeometry[1].SortKey.PrimitiveId, 10u);
		EXPECT_EQ(Prepared.TranslucentGeometry[2].Family,
			EPreparedTranslucentGeometryFamily::StaticMesh);
		EXPECT_EQ(Prepared.TranslucentGeometry[3].Family,
			EPreparedTranslucentGeometryFamily::SkeletalMesh);
	}

	TEST(FRendererSceneViewTests, FixedAspectViewsAreCenteredPerOutput)
	{
		FSceneView WideView;
		WideView.AspectRatioConstraint = 16.0f / 9.0f;
		const FSceneView WideResult =
			FSceneRenderer::FitViewToOutput(WideView, 800, 600);

		EXPECT_EQ(WideResult.ViewportX, 0u);
		EXPECT_EQ(WideResult.ViewportY, 75u);
		EXPECT_EQ(WideResult.ViewportWidth, 800u);
		EXPECT_EQ(WideResult.ViewportHeight, 450u);

		FSceneView TallView;
		TallView.AspectRatioConstraint = 0.5f;
		const FSceneView TallResult =
			FSceneRenderer::FitViewToOutput(TallView, 800, 600);

		EXPECT_EQ(TallResult.ViewportX, 250u);
		EXPECT_EQ(TallResult.ViewportY, 0u);
		EXPECT_EQ(TallResult.ViewportWidth, 300u);
		EXPECT_EQ(TallResult.ViewportHeight, 600u);
	}

	TEST(FRendererSceneViewTests, ProjectionGoldensUseForwardXAndOrdinaryZeroToOneDepth)
	{
		const FMatrix Perspective =
			MakePerspectiveProjection(90.0, 2.0, 1.0, 11.0);
		const FMatrix Orthographic =
			MakeOrthographicProjection(2.0, 1.0, 1.0, 11.0);
		ExpectDepthMapping(Perspective, 1.0, 11.0);
		ExpectDepthMapping(Orthographic, 1.0, 11.0);

		const FVector4 PerspectiveRight =
			Perspective * FVector4(1.0, 2.0, 0.0, 1.0);
		const FVector4 PerspectiveUp =
			Perspective * FVector4(1.0, 0.0, 1.0, 1.0);
		EXPECT_NEAR(PerspectiveRight.x / PerspectiveRight.w, 1.0, 1.0e-12);
		EXPECT_NEAR(PerspectiveUp.y / PerspectiveUp.w, -1.0, 1.0e-12);
		const FVector4 OrthographicRight =
			Orthographic * FVector4(1.0, 2.0, 0.0, 1.0);
		const FVector4 OrthographicUp =
			Orthographic * FVector4(1.0, 0.0, 1.0, 1.0);
		EXPECT_NEAR(OrthographicRight.x, 1.0, 1.0e-12);
		EXPECT_NEAR(OrthographicUp.y, -1.0, 1.0e-12);
	}

	TEST(FRendererSceneViewTests,
		DirectionalShadowFitsZeroToOneReceiverAndConservativeCasterVolume)
	{
		FSceneView View;
		View.ProjectionMatrix = MakePerspectiveProjection(90.0, 2.0, 1.0, 11.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.ViewportWidth = 1920;
		View.ViewportHeight = 1080;
		FDirectionalLightSceneData Light;
		Light.Direction = {0.0, 0.0, -1.0};
		Light.Intensity = 1.0f;
		FPreparedDirectionalShadowView Shadow;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(7), Light, Shadow));
		EXPECT_TRUE(Shadow.bEnabled);
		EXPECT_EQ(Shadow.LightId.Value, 7u);
		EXPECT_EQ(Shadow.CasterView.ViewportWidth, DirectionalShadowResolution);
		EXPECT_EQ(Shadow.CasterView.Settings.RasterMode, ERasterMode::Solid);
		EXPECT_TRUE(Math::IsFinite(Shadow.WorldToShadowMatrix));
		EXPECT_GT(Shadow.TexelWorldSize.x, 0.0);
		EXPECT_GT(Shadow.TexelWorldSize.y, 0.0);
		for (const FVector3& Corner : Shadow.ReceiverCorners)
		{
			const FVector4 Projected =
				Shadow.WorldToShadowMatrix * FVector4(Corner, 1.0);
			ASSERT_GT(std::abs(Projected.w), 1.0e-8);
			const FVector3 Coordinate = FVector3(Projected) / Projected.w;
			EXPECT_GE(Coordinate.x, -1.0e-9);
			EXPECT_LE(Coordinate.x, 1.0 + 1.0e-9);
			EXPECT_GE(Coordinate.y, -1.0e-9);
			EXPECT_LE(Coordinate.y, 1.0 + 1.0e-9);
			EXPECT_GE(Coordinate.z, 0.0);
			EXPECT_LE(Coordinate.z, 1.0 + 1.0e-9);
		}
		EXPECT_EQ(ClassifyDirectionalShadowCasterBounds(
			Shadow, FBox({2.0, -0.25, -0.25}, {3.0, 0.25, 0.25})),
			EDirectionalShadowBoundsClassification::InsideOrIntersecting);
		EXPECT_EQ(ClassifyDirectionalShadowCasterBounds(
			Shadow, FBox({2.0, 1000.0, 1000.0}, {3.0, 1001.0, 1001.0})),
			EDirectionalShadowBoundsClassification::Outside);
		EXPECT_EQ(ClassifyDirectionalShadowCasterBounds(Shadow, FBox{}),
			EDirectionalShadowBoundsClassification::InvalidBoundsFallback);
	}

	TEST(FRendererSceneViewTests,
		DirectionalShadowSupportsOrthographicClampAndRejectsInvalidInputs)
	{
		FSceneView View;
		View.ProjectionMatrix = MakeOrthographicProjection(2.0, 1.0, 1.0, 600.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.ViewportWidth = 800;
		View.ViewportHeight = 400;
		FDirectionalLightSceneData Light;
		Light.Direction = {-0.5, -0.5, -1.0};
		Light.Intensity = 1.0f;
		FPreparedDirectionalShadowView Shadow;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, Shadow));
		for (uint32 Corner = 0; Corner < 4; ++Corner)
		{
			EXPECT_NEAR(Math::Length(Shadow.ReceiverCorners[Corner + 4]
				- Shadow.ReceiverCorners[Corner]), DirectionalShadowDistance, 1.0e-8);
		}
		Light.bCastShadows = false;
		EXPECT_FALSE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, Shadow));
		Light.bCastShadows = true;
		View.ViewProjectionMatrix[0][0] =
			std::numeric_limits<double>::quiet_NaN();
		EXPECT_FALSE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, Shadow));
	}

	TEST(FRendererSceneViewTests, DirectionalShadowSamplerUsesFrozenComparisonTier)
	{
		const FRHISamplerDesc Sampler = MakeDirectionalShadowSamplerDesc();
		EXPECT_EQ(Sampler.MinFilter, ESamplerFilter::Linear);
		EXPECT_EQ(Sampler.MagFilter, ESamplerFilter::Linear);
		EXPECT_EQ(Sampler.AddressU, ESamplerAddressMode::ClampToBorder);
		EXPECT_EQ(Sampler.AddressV, ESamplerAddressMode::ClampToBorder);
		EXPECT_TRUE(Sampler.bEnableCompare);
		EXPECT_EQ(Sampler.CompareOp, ESamplerCompareOp::LessOrEqual);
		EXPECT_EQ(Sampler.BorderColor, ESamplerBorderColor::FloatOpaqueWhite);
		EXPECT_FLOAT_EQ(Sampler.MinLod, 0.0f);
		EXPECT_FLOAT_EQ(Sampler.MaxLod, 0.0f);
	}

	TEST(FRendererSceneViewTests,
		DirectionalShadowFilterTiersHaveExactMetadataAndInvalidFallback)
	{
		const FDirectionalShadowFilter Low = PrepareDirectionalShadowFilter(
			EDirectionalShadowFilterQuality::Low);
		EXPECT_EQ(Low.Quality, EDirectionalShadowFilterQuality::Low);
		EXPECT_EQ(Low.ComparisonOperations, 1u);
		EXPECT_EQ(Low.GuardTexels, 2u);
		EXPECT_FLOAT_EQ(Low.FootprintRadiusTexels, 0.5f);
		EXPECT_FALSE(Low.bUsedInvalidQualityFallback);

		const FDirectionalShadowFilter Medium = PrepareDirectionalShadowFilter(
			EDirectionalShadowFilterQuality::Medium);
		EXPECT_EQ(Medium.Quality, EDirectionalShadowFilterQuality::Medium);
		EXPECT_EQ(Medium.ComparisonOperations, 9u);
		EXPECT_EQ(Medium.GuardTexels, 2u);
		EXPECT_FLOAT_EQ(Medium.FootprintRadiusTexels, 1.5f);

		const FDirectionalShadowFilter High = PrepareDirectionalShadowFilter(
			EDirectionalShadowFilterQuality::High);
		EXPECT_EQ(High.Quality, EDirectionalShadowFilterQuality::High);
		EXPECT_EQ(High.ComparisonOperations, 25u);
		EXPECT_EQ(High.GuardTexels, 3u);
		EXPECT_FLOAT_EQ(High.FootprintRadiusTexels, 2.5f);

		const FDirectionalShadowFilter Invalid = PrepareDirectionalShadowFilter(
			static_cast<EDirectionalShadowFilterQuality>(255));
		EXPECT_EQ(Invalid.Quality, EDirectionalShadowFilterQuality::Low);
		EXPECT_EQ(Invalid.ComparisonOperations, 1u);
		EXPECT_EQ(Invalid.GuardTexels, 2u);
		EXPECT_FLOAT_EQ(Invalid.FootprintRadiusTexels, 0.5f);
		EXPECT_TRUE(Invalid.bUsedInvalidQualityFallback);
	}

	TEST(FRendererSceneViewTests,
		ForwardLightingPublishesShadowOnlyForMatchingSelectedLight)
	{
		FSceneView View;
		View.ProjectionMatrix = MakeOrthographicProjection(2.0, 1.0, 1.0, 11.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.ViewportWidth = 64;
		View.ViewportHeight = 64;
		FPreparedLightView Lights;
		FPreparedDirectionalLight Light;
		Light.Id = FLightSceneId(4);
		Light.Data.Intensity = 1.0f;
		Light.Data.Direction = {0.0, 0.0, -1.0};
		Lights.Directional.push_back(Light);
		FPreparedDirectionalShadowView Shadow;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, Light.Id, Light.Data, Shadow));
		const FForwardLightingUniform Enabled = BuildForwardLightingUniform(
			Lights, View, &Shadow);
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Control.x, 1.0f);
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.TexelBias.z,
			Shadow.Bias.ReceiverWorld);
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Filter.x,
			1.0f / static_cast<float>(DirectionalShadowResolution));
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Filter.y,
			1.0f / static_cast<float>(DirectionalShadowResolution));
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Filter.z,
			static_cast<float>(EDirectionalShadowFilterQuality::Medium));
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Filter.w, 1.5f);
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.LightBounds.w, 2.0f);

		Shadow.LightId = FLightSceneId(5);
		const FForwardLightingUniform Mismatch = BuildForwardLightingUniform(
			Lights, View, &Shadow);
		EXPECT_FLOAT_EQ(Mismatch.DirectionalShadow.Control.x, 0.0f);
		const FForwardLightingUniform Disabled = BuildForwardLightingUniform(
			Lights, View, nullptr);
		EXPECT_FLOAT_EQ(Disabled.DirectionalShadow.Control.x, 0.0f);
	}

	TEST(FRendererSceneViewTests,
		DirectionalShadowBiasUsesBoundedTexelAndOrientationInputs)
	{
		const FDirectionalShadowBias Facing =
			CalculateDirectionalShadowBias({0.125, 0.0625}, 1.0);
		EXPECT_FLOAT_EQ(Facing.RasterConstant, 1.25f);
		EXPECT_FLOAT_EQ(Facing.RasterSlope, 1.375f);
		EXPECT_FLOAT_EQ(Facing.RasterClamp, 3.0f);
		EXPECT_FLOAT_EQ(Facing.ReceiverWorld, 0.00625f);
		EXPECT_FLOAT_EQ(Facing.NormalWorld, 0.025f);
		EXPECT_FALSE(Facing.bUsedFallback);
		EXPECT_FALSE(Facing.bTotalClamped);

		const FDirectionalShadowBias Grazing =
			CalculateDirectionalShadowBias({0.125, 0.0625}, 0.0);
		EXPECT_FLOAT_EQ(Grazing.ReceiverWorld, 0.01875f);
		EXPECT_FLOAT_EQ(Grazing.NormalWorld, 0.075f);
		EXPECT_TRUE(Grazing.bTotalClamped);
		EXPECT_LE(Grazing.ReceiverWorld + Grazing.NormalWorld, 0.09375f);

		const FDirectionalShadowBias Maximum =
			CalculateDirectionalShadowBias({0.25, 0.25}, 0.0);
		EXPECT_FLOAT_EQ(Maximum.RasterConstant, 1.5f);
		EXPECT_FLOAT_EQ(Maximum.RasterSlope, 1.5f);
		EXPECT_FLOAT_EQ(Maximum.RasterClamp, 4.0f);
		EXPECT_FLOAT_EQ(Maximum.ReceiverWorld, 0.02f);
		EXPECT_FLOAT_EQ(Maximum.NormalWorld, 0.08f);
		EXPECT_TRUE(Maximum.bTotalClamped);

		const FDirectionalShadowBias Invalid = CalculateDirectionalShadowBias(
			{std::numeric_limits<double>::quiet_NaN(), 0.125}, 1.0);
		EXPECT_TRUE(Invalid.bUsedFallback);
		EXPECT_FLOAT_EQ(Invalid.RasterConstant,
			DirectionalShadowDepthBiasConstant);
		EXPECT_FLOAT_EQ(Invalid.RasterSlope, DirectionalShadowDepthBiasSlope);
		EXPECT_FLOAT_EQ(Invalid.RasterClamp, DirectionalShadowDepthBiasClamp);
		EXPECT_FLOAT_EQ(Invalid.ReceiverWorld, 0.0f);
		EXPECT_FLOAT_EQ(Invalid.NormalWorld, 0.0f);

		constexpr std::array<double, 4> TexelSizes{
			0.03125, 0.0625, 0.125, 0.25};
		constexpr std::array<double, 4> SurfaceLightCosines{
			1.0, 0.5, 0.1, 0.0};
		for (const double TexelSize : TexelSizes)
			for (const double SurfaceLightCosine : SurfaceLightCosines)
			{
				const FDirectionalShadowBias Bias =
					CalculateDirectionalShadowBias(
						{TexelSize, TexelSize * 0.5}, SurfaceLightCosine);
				EXPECT_FALSE(Bias.bUsedFallback);
				EXPECT_GE(Bias.RasterConstant, 1.0f);
				EXPECT_LE(Bias.RasterConstant, 1.5f);
				EXPECT_GE(Bias.RasterSlope, 1.25f);
				EXPECT_LE(Bias.RasterSlope, 2.0f);
				EXPECT_GE(Bias.RasterClamp, 2.0f);
				EXPECT_LE(Bias.RasterClamp, 4.0f);
				EXPECT_GE(Bias.ReceiverWorld, 0.0005f);
				EXPECT_LE(Bias.ReceiverWorld,
					DirectionalShadowMaximumReceiverWorldBias);
				EXPECT_GE(Bias.NormalWorld, 0.0f);
				EXPECT_LE(Bias.NormalWorld,
					DirectionalShadowMaximumNormalOffset);
				EXPECT_LE(Bias.ReceiverWorld + Bias.NormalWorld,
					std::min(static_cast<float>(0.75 * TexelSize),
						DirectionalShadowMaximumTotalWorldBias));
			}
	}

	TEST(FRendererSceneViewTests,
		DirectionalShadowDiagnosticIdentityIsPreparedPerView)
	{
		FSceneView View;
		View.ProjectionMatrix = MakeOrthographicProjection(2.0, 1.0, 1.0, 11.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.ViewportWidth = 64;
		View.ViewportHeight = 64;
		FDirectionalLightSceneData Light;
		Light.Direction = {0.0, 0.0, -1.0};
		FPreparedDirectionalShadowView First;
		View.Settings.DirectionalShadowDiagnosticMode =
			EDirectionalShadowDiagnosticMode::ReceiverBiased;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, First));
		FPreparedDirectionalShadowView Second;
		View.Settings.DirectionalShadowDiagnosticMode =
			EDirectionalShadowDiagnosticMode::TexelGrid;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, Second));
		EXPECT_EQ(First.DiagnosticMode,
			EDirectionalShadowDiagnosticMode::ReceiverBiased);
		EXPECT_EQ(Second.DiagnosticMode,
			EDirectionalShadowDiagnosticMode::TexelGrid);
		FPreparedDirectionalShadowView Invalid;
		View.Settings.DirectionalShadowDiagnosticMode =
			static_cast<EDirectionalShadowDiagnosticMode>(255);
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, Invalid));
		EXPECT_EQ(Invalid.DiagnosticMode,
			EDirectionalShadowDiagnosticMode::Lit);
	}

	TEST(FRendererSceneViewTests,
		DirectionalShadowFilterIdentityIsPreparedAndIsolatedPerView)
	{
		FSceneView View;
		View.ProjectionMatrix = MakeOrthographicProjection(2.0, 1.0, 1.0, 11.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.ViewportWidth = 64;
		View.ViewportHeight = 64;
		FDirectionalLightSceneData Light;
		Light.Direction = {0.0, 0.0, -1.0};

		View.Settings.DirectionalShadowFilterQuality =
			EDirectionalShadowFilterQuality::High;
		FPreparedDirectionalShadowView High;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, High));
		View.Settings.DirectionalShadowFilterQuality =
			EDirectionalShadowFilterQuality::Low;
		FPreparedDirectionalShadowView Low;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, Low));
		View.Settings.DirectionalShadowFilterQuality =
			static_cast<EDirectionalShadowFilterQuality>(255);
		FPreparedDirectionalShadowView Invalid;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, Invalid));

		EXPECT_EQ(High.Filter.Quality, EDirectionalShadowFilterQuality::High);
		EXPECT_EQ(High.Filter.GuardTexels, 3u);
		EXPECT_EQ(Low.Filter.Quality, EDirectionalShadowFilterQuality::Low);
		EXPECT_EQ(Low.Filter.GuardTexels, 2u);
		EXPECT_EQ(Invalid.Filter.Quality, EDirectionalShadowFilterQuality::Low);
		EXPECT_TRUE(Invalid.Filter.bUsedInvalidQualityFallback);
		EXPECT_NE(High.TexelWorldSize, Low.TexelWorldSize);
	}

	TEST(FRendererSceneViewTests, FrustumGoldensKeepContactAndRejectOnlyFullyOutsideBounds)
	{
		auto ExpectClassification = [](
			const FMatrix& Projection,
			const FBox& Bounds,
			EViewBoundsClassification Expected) {
			FViewFrustum Frustum;
			ASSERT_TRUE(TryBuildViewFrustum(Projection, Frustum));
			EXPECT_EQ(ClassifyWorldBounds(Frustum, Bounds), Expected);
		};
		const FMatrix Perspective =
			MakePerspectiveProjection(90.0, 2.0, 1.0, 11.0);
		ExpectClassification(
			Perspective,
			FBox({2.0, -0.5, -0.5}, {3.0, 0.5, 0.5}),
			EViewBoundsClassification::Inside);
		ExpectClassification(
			Perspective,
			FBox({1.0, -0.25, -0.25}, {2.0, 0.25, 0.25}),
			EViewBoundsClassification::Intersecting);
		for (const FBox& Outside : std::to_array<FBox>({
				FBox({0.1, -0.1, -0.1}, {0.5, 0.1, 0.1}),
				FBox({12.0, -0.1, -0.1}, {13.0, 0.1, 0.1}),
				FBox({2.0, -8.0, -0.5}, {3.0, -7.0, 0.5}),
				FBox({2.0, 7.0, -0.5}, {3.0, 8.0, 0.5}),
				FBox({2.0, -0.5, -6.0}, {3.0, 0.5, -5.0}),
				FBox({2.0, -0.5, 5.0}, {3.0, 0.5, 6.0})}))
		{
			ExpectClassification(
				Perspective,
				Outside,
				EViewBoundsClassification::Outside);
		}

		const FMatrix Orthographic =
			MakeOrthographicProjection(2.0, 1.0, 1.0, 11.0);
		ExpectClassification(
			Orthographic,
			FBox({2.0, -0.5, -0.5}, {3.0, 0.5, 0.5}),
			EViewBoundsClassification::Inside);
		for (const FBox& Outside : std::to_array<FBox>({
				FBox({0.1, -0.1, -0.1}, {0.5, 0.1, 0.1}),
				FBox({12.0, -0.1, -0.1}, {13.0, 0.1, 0.1}),
				FBox({2.0, -4.0, -0.5}, {3.0, -3.0, 0.5}),
				FBox({2.0, 3.0, -0.5}, {3.0, 4.0, 0.5}),
				FBox({2.0, -0.5, -3.0}, {3.0, 0.5, -2.0}),
				FBox({2.0, -0.5, 2.0}, {3.0, 0.5, 3.0})}))
		{
			ExpectClassification(
				Orthographic,
				Outside,
				EViewBoundsClassification::Outside);
		}

		FViewFrustum Frustum;
		ASSERT_TRUE(TryBuildViewFrustum(Perspective, Frustum));
		EXPECT_EQ(
			ClassifyWorldBounds(Frustum, FBox{}),
			EViewBoundsClassification::InvalidBounds);
		FMatrix InvalidProjection = Perspective;
		InvalidProjection[0][0] = std::numeric_limits<double>::quiet_NaN();
		EXPECT_FALSE(TryBuildViewFrustum(InvalidProjection, Frustum));
	}

	TEST(FRendererSceneViewTests, ProjectedSizeUsesFittedContentAndSupportsOrthographicViews)
	{
		FSceneView PerspectiveView;
		PerspectiveView.ProjectionMatrix =
			MakePerspectiveProjection(90.0, 2.0, 1.0, 11.0);
		PerspectiveView.ViewProjectionMatrix =
			PerspectiveView.ProjectionMatrix;
		PerspectiveView.ViewportWidth = 1600;
		PerspectiveView.ViewportHeight = 800;
		const FBox PerspectiveBounds({5.0, -1.0, -1.0}, {5.0, 1.0, 1.0});
		const FProjectedScreenSizeResult LargeOutput =
			ComputeProjectedScreenSize(PerspectiveView, PerspectiveBounds);
		PerspectiveView.ViewportWidth = 800;
		PerspectiveView.ViewportHeight = 400;
		const FProjectedScreenSizeResult SmallOutput =
			ComputeProjectedScreenSize(PerspectiveView, PerspectiveBounds);
		ASSERT_EQ(LargeOutput.Status, EProjectedScreenSizeStatus::Valid);
		ASSERT_EQ(SmallOutput.Status, EProjectedScreenSizeStatus::Valid);
		EXPECT_NEAR(LargeOutput.NormalizedScreenSize, 0.2f, 1.0e-6f);
		EXPECT_FLOAT_EQ(
			LargeOutput.NormalizedScreenSize,
			SmallOutput.NormalizedScreenSize);

		FSceneView OrthographicView;
		OrthographicView.ProjectionMatrix =
			MakeOrthographicProjection(2.0, 1.0, 1.0, 11.0);
		OrthographicView.ViewProjectionMatrix =
			OrthographicView.ProjectionMatrix;
		OrthographicView.ViewportWidth = 1600;
		OrthographicView.ViewportHeight = 800;
		const FProjectedScreenSizeResult OrthographicSize =
			ComputeProjectedScreenSize(
				OrthographicView,
				FBox({5.0, -0.5, -0.5}, {5.0, 0.5, 0.5}));
		ASSERT_EQ(OrthographicSize.Status, EProjectedScreenSizeStatus::Valid);
		EXPECT_NEAR(OrthographicSize.NormalizedScreenSize, 0.5f, 1.0e-6f);

		const FProjectedScreenSizeResult NearCrossing =
			ComputeProjectedScreenSize(
				PerspectiveView,
				FBox({0.5, -0.25, -0.25}, {1.5, 0.25, 0.25}));
		EXPECT_FLOAT_EQ(NearCrossing.NormalizedScreenSize, 1.0f);
		EXPECT_EQ(
			NearCrossing.Status,
			EProjectedScreenSizeStatus::NearPlaneOrCameraCrossing);
	}

	TEST(FRendererSceneViewTests, LODGoldensFreezeDefaultsEqualityAndAvailabilityFallback)
	{
		const std::vector<float> ScreenSizes =
			MakeDefaultStaticMeshLODScreenSizes(3);
		ASSERT_EQ(ScreenSizes, (std::vector<float>{0.5f, 0.25f, 0.0f}));
		ASSERT_TRUE(ValidateStaticMeshLODScreenSizes(ScreenSizes));
		EXPECT_EQ(SelectStaticMeshLOD(0.5f, ScreenSizes), 0u);
		EXPECT_EQ(SelectStaticMeshLOD(0.49f, ScreenSizes), 1u);
		EXPECT_EQ(SelectStaticMeshLOD(0.25f, ScreenSizes), 1u);
		EXPECT_EQ(SelectStaticMeshLOD(0.24f, ScreenSizes), 2u);
		EXPECT_EQ(
			SelectStaticMeshLOD(std::numeric_limits<float>::quiet_NaN(), ScreenSizes),
			0u);
		EXPECT_EQ(
			ResolveAvailableStaticMeshLOD(
				1,
				std::to_array<uint8>({0, 0, 1})),
			2u);
		EXPECT_EQ(
			ResolveAvailableStaticMeshLOD(
				2,
				std::to_array<uint8>({1, 0, 0})),
			0u);
		EXPECT_EQ(
			ResolveAvailableStaticMeshLOD(
				0,
				std::to_array<uint8>({0, 0, 0})),
			InvalidStaticMeshLODIndex);

		struct FDistinctGeometryLOD
		{
			std::vector<FVector3> Positions;
			std::vector<uint32> Indices;

			auto GetLocalBounds() const -> FBox
			{
				FBox Bounds;
				for (const FVector3& Position : Positions)
				{
					Bounds.AddPoint(Position);
				}
				return Bounds;
			}

			auto GetTriangleCount() const -> uint32
			{
				return static_cast<uint32>(Indices.size() / 3);
			}
		};
		const std::array<FDistinctGeometryLOD, 3> MeshLODs{{
			{{{-1.0, -1.0, 0.0},
			  {1.0, -1.0, 0.0},
			  {1.0, 1.0, 0.0},
			  {-1.0, 1.0, 0.0},
			  {0.0, 0.0, 0.0}},
			 {0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4}},
			{{{-1.0, -1.0, 0.0},
			  {1.0, -1.0, 0.0},
			  {1.0, 1.0, 0.0},
			  {-1.0, 1.0, 0.0}},
			 {0, 1, 2, 0, 2, 3}},
			{{{-1.0, -1.0, 0.0},
			  {1.0, -1.0, 0.0},
			  {0.0, 1.0, 0.0}},
			 {0, 1, 2}}}};
		EXPECT_EQ(
			MeshLODs[SelectStaticMeshLOD(0.6f, ScreenSizes)].GetTriangleCount(),
			4u);
		EXPECT_EQ(
			MeshLODs[SelectStaticMeshLOD(0.3f, ScreenSizes)].GetTriangleCount(),
			2u);
		EXPECT_EQ(
			MeshLODs[SelectStaticMeshLOD(0.1f, ScreenSizes)].GetTriangleCount(),
			1u);
		const FBox LOD0Bounds = MeshLODs[0].GetLocalBounds();
		const FBox LOD2Bounds = MeshLODs[2].GetLocalBounds();
		EXPECT_DOUBLE_EQ(
			LOD0Bounds.Min.x,
			LOD2Bounds.Min.x);
		EXPECT_DOUBLE_EQ(
			LOD0Bounds.Max.y,
			LOD2Bounds.Max.y);
	}
} // namespace Durin
