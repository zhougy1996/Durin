#include <gtest/gtest.h>

#include "GBufferContract.h"
#include "Renderers/DisplayMapping.h"
#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/ViewPreparationMath.h"
#include "SceneViewProjection.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		template <typename T>
		concept CHasResourcesReady = requires(T Value) { Value.bResourcesReady; };
		template <typename T>
		concept CHasExecutionPhase = requires(T Value) { Value.Phase; };
		template <typename T>
		concept CHasShadowTarget = requires(T Value) {
			Value.DirectionalShadowTexture;
		};
		template <typename T>
		concept CHasExecutionCounter = requires(T Value) { Value.AttemptedDraws; };
		template <typename T>
		concept CHasMaterialBinding = requires(T Value) { Value.MaterialBinding; };

		static_assert(!CHasResourcesReady<FPreparedStaticMeshDraw>);
		static_assert(!CHasExecutionPhase<FPreparedStaticMeshView>);
		static_assert(!CHasShadowTarget<FPreparedStaticMeshDraw>);
		static_assert(!CHasExecutionCounter<FPreparedStaticMeshView>);
		static_assert(!CHasMaterialBinding<FPreparedStaticMeshDraw>);
		static_assert(std::is_same_v<
			decltype(FGBufferPassResult{}.IsComplete()), bool>);
	} // namespace

	TEST(FDisplayMappingTests, ACESGoldensAreFiniteMonotonicAndClamped)
	{
		const std::array<float, 5> Inputs{0.0f, 0.18f, 1.0f, 4.0f, 64.0f};
		const std::array<float, 5> Expected{
			0.0f, 0.26689893f, 0.80379748f, 0.97341710f, 1.0f};
		float Previous = -1.0f;
		for (size_t Index = 0; Index < Inputs.size(); ++Index)
		{
			const FVector3f Mapped =
				DisplayMapping::MapSceneLinearToDisplayLinear(
					FVector3f(Inputs[Index]), 0.0f);
			EXPECT_NEAR(Mapped.x, Expected[Index], 1.0e-6f);
			EXPECT_EQ(Mapped.x, Mapped.y);
			EXPECT_EQ(Mapped.y, Mapped.z);
			EXPECT_TRUE(std::isfinite(Mapped.x));
			EXPECT_GE(Mapped.x, Previous);
			EXPECT_GE(Mapped.x, 0.0f);
			EXPECT_LE(Mapped.x, 1.0f);
			Previous = Mapped.x;
		}
	}

	TEST(FDisplayMappingTests, ExposureAndInvalidValuesUseFrozenFallbacks)
	{
		EXPECT_FLOAT_EQ(
			DisplayMapping::CanonicalizeExposureEV(
				std::numeric_limits<float>::quiet_NaN()),
			DisplayMapping::DefaultExposureEV);
		EXPECT_FLOAT_EQ(
			DisplayMapping::CanonicalizeExposureEV(100.0f),
			DisplayMapping::MaximumExposureEV);
		EXPECT_FLOAT_EQ(
			DisplayMapping::CanonicalizeExposureEV(-100.0f),
			DisplayMapping::MinimumExposureEV);
		EXPECT_FLOAT_EQ(DisplayMapping::CalculateExposureScale(1.0f), 2.0f);
		const FVector3f PlusOne =
			DisplayMapping::MapSceneLinearToDisplayLinear(
				FVector3f(0.18f), 1.0f);
		EXPECT_NEAR(PlusOne.x, 0.50364438f, 1.0e-6f);
		const FVector3f Invalid =
			DisplayMapping::MapSceneLinearToDisplayLinear(
				{std::numeric_limits<float>::infinity(),
				 std::numeric_limits<float>::quiet_NaN(), -1.0f},
				std::numeric_limits<float>::quiet_NaN());
		EXPECT_EQ(Invalid, FVector3f(0.0f));
		EXPECT_FLOAT_EQ(DisplayMapping::MapOutputAlpha(0.4f), 0.4f);
		EXPECT_FLOAT_EQ(DisplayMapping::MapOutputAlpha(2.0f), 1.0f);
		EXPECT_FLOAT_EQ(
			DisplayMapping::MapOutputAlpha(
				std::numeric_limits<float>::quiet_NaN()),
			0.0f);
	}

	TEST(FDisplayMappingTests, SceneViewExposureDefaultsAndCopiesPerView)
	{
		FSceneView First;
		FSceneView Second;
		EXPECT_FLOAT_EQ(First.Settings.PostProcess.ExposureEV, 0.0f);
		First.Settings.PostProcess.ExposureEV = 2.0f;
		EXPECT_FLOAT_EQ(First.Settings.PostProcess.ExposureEV, 2.0f);
		EXPECT_FLOAT_EQ(Second.Settings.PostProcess.ExposureEV, 0.0f);
	}

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

	TEST(FRendererSceneViewTests,
		GBufferStage0OctahedralUNorm8NormalsMeetAngularTolerance)
	{
		double MaximumErrorDegrees = 0.0;
		for (int Latitude = -90; Latitude <= 90; ++Latitude)
		{
			const double LatitudeRadians =
				Math::DegreesToRadians(static_cast<double>(Latitude));
			for (int Longitude = 0; Longitude < 360; ++Longitude)
			{
				const double LongitudeRadians =
					Math::DegreesToRadians(static_cast<double>(Longitude));
				const FVector3f Normal{
					static_cast<float>(std::cos(LatitudeRadians)
						* std::cos(LongitudeRadians)),
					static_cast<float>(std::cos(LatitudeRadians)
						* std::sin(LongitudeRadians)),
					static_cast<float>(std::sin(LatitudeRadians))};
				FVector2f Encoded =
					GBufferContract::EncodeOctahedralNormal(Normal);
				Encoded.x = std::round(std::clamp(Encoded.x, 0.0f, 1.0f)
					* 255.0f) / 255.0f;
				Encoded.y = std::round(std::clamp(Encoded.y, 0.0f, 1.0f)
					* 255.0f) / 255.0f;
				const FVector3f Decoded =
					GBufferContract::DecodeOctahedralNormal(Encoded);
				const double ErrorDegrees = Math::RadiansToDegrees(std::acos(
					std::clamp(static_cast<double>(Math::Dot(Normal, Decoded)),
						-1.0, 1.0)));
				MaximumErrorDegrees = std::max(
					MaximumErrorDegrees, ErrorDegrees);
			}
		}
		EXPECT_LE(MaximumErrorDegrees, 1.0);
	}

	TEST(FRendererSceneViewTests,
		GBufferStage0DepthReconstructionMeetsViewRelativeTolerance)
	{
		auto GetDevicePosition = [](const FMatrix& Projection,
			const FVector3f& Point) {
			const FMatrix4f ShaderProjection(Projection);
			const FVector4f Clip =
				ShaderProjection * FVector4f(Point, 1.0f);
			EXPECT_GT(std::abs(Clip.w), 1.0e-8f);
			return FVector3f(Clip) / Clip.w;
		};
		auto ExpectError = [](const FVector3f& Point,
			const FVector3f& Reconstructed) {
				const double Error = Math::Length(
					FVector3(Reconstructed) - FVector3(Point));
				const double Distance = Math::Length(FVector3(Point));
				EXPECT_LE(Error,
					GBufferContract::GetPositionTolerance(Distance));
		};

		constexpr float NearClip = 0.1f;
		constexpr float FarClip = 500000.0f;
		constexpr float AspectRatio = 16.0f / 9.0f;
		constexpr float FieldOfViewDegrees = 60.0f;
		FMatrix Perspective;
		ASSERT_TRUE(SceneViewProjection::BuildPerspectiveProjection(
			FieldOfViewDegrees, AspectRatio, NearClip, FarClip,
			ESceneDepthConvention::ReversedZ, Perspective));
		const std::array PerspectivePoints{
			FVector3f{0.1f, 0.0f, 0.0f},
			FVector3f{1.0f, 0.25f, -0.1f},
			FVector3f{1000.0f, 700.0f, 300.0f},
			FVector3f{200000.0f, -150000.0f, 80000.0f}};
		for (const FVector3f& Point : PerspectivePoints)
		{
			const FVector3f Device = GetDevicePosition(Perspective, Point);
			FVector3 Reconstructed;
			ASSERT_TRUE(GBufferContract::ReconstructViewPositionAnalytic(
				Perspective, FVector2f(Device), Device.z, Reconstructed));
			ExpectError(Point, FVector3f(Reconstructed));
		}

		constexpr float HalfWidth = 200000.0f;
		constexpr float HalfHeight = 100000.0f;
		FMatrix Orthographic = MakeOrthographicProjection(
			HalfWidth, HalfHeight, NearClip, FarClip);
		Orthographic[0][2] = -Orthographic[0][2];
		Orthographic[3][2] =
			500000.0 / (500000.0 - 0.1);
		const std::array OrthographicPoints{
			FVector3f{0.1f, 0.0f, 0.0f},
			FVector3f{1000.0f, 700.0f, 300.0f},
			FVector3f{200000.0f, -150000.0f, 80000.0f}};
		for (const FVector3f& Point : OrthographicPoints)
		{
			const FVector3f Device = GetDevicePosition(Orthographic, Point);
			FVector3 Reconstructed;
			ASSERT_TRUE(GBufferContract::ReconstructViewPositionAnalytic(
				Orthographic, FVector2f(Device), Device.z, Reconstructed));
			ExpectError(Point, FVector3f(Reconstructed));
		}
	}

	TEST(FRendererSceneViewTests,
		GBufferDecodePublishesFrozenChannelsAndPackedEmissive)
	{
		const uint32 PackedOne = (15u << 6u)
			| ((15u << 6u) << 11u)
			| ((15u << 5u) << 22u);
		EXPECT_EQ(GBufferContract::DecodeR11G11B10Float(0u), FVector3f(0.0f));
		const FVector3f DecodedOne =
			GBufferContract::DecodeR11G11B10Float(PackedOne);
		EXPECT_EQ(DecodedOne, FVector3f(1.0f));

		const GBufferContract::FDecodedRecord Record =
			GBufferContract::DecodeRecord(
				{0.25f, 0.5f, 0.75f, 0.125f},
				{0.5f, 0.5f, 0.5f, 0.5f},
				{0.4f, 0.8f, 1.0f, 1.0f / 255.0f},
				{2.0f, 3.0f, 4.0f});
		EXPECT_EQ(Record.BaseColor, FVector3f(0.25f, 0.5f, 0.75f));
		EXPECT_FLOAT_EQ(Record.Metallic, 0.125f);
		EXPECT_FLOAT_EQ(Record.Roughness, 0.4f);
		EXPECT_FLOAT_EQ(Record.AmbientOcclusion, 0.8f);
		EXPECT_FLOAT_EQ(Record.EffectiveOpacity, 1.0f);
		EXPECT_EQ(Record.Emissive, FVector3f(2.0f, 3.0f, 4.0f));
		EXPECT_EQ(Record.Flags, GBufferContract::StandardLitFlag);
		EXPECT_TRUE(Record.IsStandardLit());
		EXPECT_EQ(Record.ShadingNormal, FVector3f(0.0f, 0.0f, 1.0f));
		EXPECT_EQ(Record.GeometricNormal, FVector3f(0.0f, 0.0f, 1.0f));

		FVector3 Reconstructed;
		EXPECT_FALSE(GBufferContract::ReconstructViewPositionAnalytic(
			FMatrix(0.0), FVector2f(0.0f), 0.5, Reconstructed));
	}

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
		FPreparedReceiverGeometry Prepared;
		FPreparedStaticMeshDraw StaticNear;
		StaticNear.TranslucentDistanceSquared = 10.0;
		StaticNear.SortKey.PrimitiveId = 20;
		Prepared.StaticMeshes.Translucent.push_back(StaticNear);
		FPreparedStaticMeshDraw StaticFar;
		StaticFar.TranslucentDistanceSquared = 20.0;
		StaticFar.SortKey.PrimitiveId = 30;
		Prepared.StaticMeshes.Translucent.push_back(StaticFar);
		FPreparedStaticMeshDraw StaticEarlier;
		StaticEarlier.TranslucentDistanceSquared = 10.0;
		StaticEarlier.SortKey.PrimitiveId = 10;
		Prepared.StaticMeshes.Translucent.push_back(StaticEarlier);
		FPreparedStaticMeshDraw ExactTie;
		ExactTie.TranslucentDistanceSquared = 10.0;
		ExactTie.SortKey.PrimitiveId = 20;
		Prepared.StaticMeshes.Translucent.push_back(ExactTie);

		PrepareCombinedTranslucentGeometry(Prepared);

		ASSERT_EQ(Prepared.TranslucentGeometry.size(), 4u);
		EXPECT_EQ(Prepared.TranslucentGeometry[0].Family,
			EPreparedTranslucentGeometryFamily::StaticMesh);
		EXPECT_EQ(Prepared.TranslucentGeometry[0].SortKey.PrimitiveId, 30u);
		EXPECT_EQ(Prepared.TranslucentGeometry[1].SortKey.PrimitiveId, 10u);
		EXPECT_EQ(Prepared.TranslucentGeometry[2].Family,
			EPreparedTranslucentGeometryFamily::StaticMesh);
		EXPECT_EQ(Prepared.TranslucentGeometry[3].Family,
			EPreparedTranslucentGeometryFamily::StaticMesh);
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
		FiniteReversedZProjectionMatchesFrozenDepthOracle)
	{
		EXPECT_EQ(SceneViewProjection::GetNearDeviceDepth(
			ESceneDepthConvention::ForwardZ), 0.0);
		EXPECT_EQ(SceneViewProjection::GetFarDeviceDepth(
			ESceneDepthConvention::ForwardZ), 1.0);
		EXPECT_EQ(SceneViewProjection::GetNearDeviceDepth(
			ESceneDepthConvention::ReversedZ), 1.0);
		EXPECT_EQ(SceneViewProjection::GetFarDeviceDepth(
			ESceneDepthConvention::ReversedZ), 0.0);
		FMatrix Projection;
		ASSERT_TRUE(SceneViewProjection::BuildPerspectiveProjection(
			90.0, 2.0, 1.0, 1001.0,
			ESceneDepthConvention::ReversedZ, Projection));
		auto DepthAt = [&Projection](double Distance) {
			const FVector4 Clip = Projection
				* FVector4(Distance, 0.0, 0.0, 1.0);
			return Clip.z / Clip.w;
		};
		EXPECT_NEAR(DepthAt(1.0), 1.0, 1.0e-12);
		EXPECT_NEAR(DepthAt(1001.0), 0.0, 1.0e-12);
		EXPECT_GT(DepthAt(500.0), DepthAt(900.0));
		EXPECT_FALSE(SceneViewProjection::BuildPerspectiveProjection(
			90.0, 2.0, 0.0, 1001.0,
			ESceneDepthConvention::ReversedZ, Projection));
		EXPECT_FALSE(SceneViewProjection::BuildPerspectiveProjection(
			90.0, 2.0, 1001.0, 1001.0,
			ESceneDepthConvention::ReversedZ, Projection));
	}

	TEST(FRendererSceneViewTests,
		DirectionalShadowReconstructsReversedZSingleAndCascadedReceivers)
	{
		FSceneView View;
		ASSERT_TRUE(SceneViewProjection::BuildPerspectiveProjection(
			75.0, 16.0 / 9.0, 0.1, 500000.0,
			ESceneDepthConvention::ReversedZ, View.ProjectionMatrix));
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.DepthConvention = ESceneDepthConvention::ReversedZ;
		View.ViewportWidth = 1920;
		View.ViewportHeight = 1080;
		FDirectionalLightSceneData Light;
		Light.Direction = {-0.5, -0.5, -1.0};
		Light.Intensity = 1.0f;

		for (const EDirectionalShadowCandidate Candidate : {
				EDirectionalShadowCandidate::SingleMap,
				EDirectionalShadowCandidate::ThreeCascades})
		{
			View.Settings.DirectionalShadow.Candidate = Candidate;
			FPreparedDirectionalShadowView Shadow;
			ASSERT_TRUE(TryPrepareDirectionalShadowView(
				View, FLightSceneId(9), Light, Shadow));
			EXPECT_TRUE(Shadow.bEnabled);
			EXPECT_EQ(Shadow.CascadeCount,
				Candidate == EDirectionalShadowCandidate::ThreeCascades
					? DirectionalShadowCascadeCount : 1u);
			for (uint32 CascadeIndex = 0;
				CascadeIndex < Shadow.CascadeCount; ++CascadeIndex)
			{
				const auto& Cascade = Shadow.Cascades[CascadeIndex];
				EXPECT_TRUE(Cascade.bEnabled);
				EXPECT_EQ(Cascade.CasterView.DepthConvention,
					ESceneDepthConvention::ForwardZ);
				EXPECT_GT(Cascade.FarDepth, Cascade.NearDepth);
			}
		}
	}

	TEST(FRendererSceneViewTests,
		DirectionalShadowFitsZeroToOneReceiverAndConservativeCasterVolume)
	{
		FSceneView View;
		View.ProjectionMatrix = MakePerspectiveProjection(90.0, 2.0, 1.0, 11.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.Settings.DirectionalShadow.Candidate =
			EDirectionalShadowCandidate::SingleMap;
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
		ASSERT_EQ(Shadow.CascadeCount, 1u);
		const auto& Cascade = Shadow.Cascades[0];
		EXPECT_EQ(Cascade.CasterView.ViewportWidth, DirectionalShadowResolution);
		EXPECT_EQ(Cascade.CasterView.Settings.Mode.RasterMode, ERasterMode::Solid);
		EXPECT_TRUE(Math::IsFinite(Cascade.WorldToShadowMatrix));
		EXPECT_GT(Cascade.TexelWorldSize.x, 0.0);
		EXPECT_GT(Cascade.TexelWorldSize.y, 0.0);
		for (const FVector3& Corner : Cascade.ReceiverCorners)
		{
			const FVector4 Projected =
				Cascade.WorldToShadowMatrix * FVector4(Corner, 1.0);
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
			Cascade, FBox({2.0, -0.25, -0.25}, {3.0, 0.25, 0.25})),
			EDirectionalShadowBoundsClassification::InsideOrIntersecting);
		EXPECT_EQ(ClassifyDirectionalShadowCasterBounds(
			Cascade, FBox({2.0, 1000.0, 1000.0}, {3.0, 1001.0, 1001.0})),
			EDirectionalShadowBoundsClassification::Outside);
		EXPECT_EQ(ClassifyDirectionalShadowCasterBounds(Cascade, FBox{}),
			EDirectionalShadowBoundsClassification::InvalidBoundsFallback);
	}

	TEST(FRendererSceneViewTests,
		DirectionalShadowSupportsOrthographicClampAndRejectsInvalidInputs)
	{
		FSceneView View;
		View.ProjectionMatrix = MakeOrthographicProjection(2.0, 1.0, 1.0, 600.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.Settings.DirectionalShadow.Candidate =
			EDirectionalShadowCandidate::SingleMap;
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
			EXPECT_NEAR(Math::Length(Shadow.Cascades[0].ReceiverCorners[Corner + 4]
				- Shadow.Cascades[0].ReceiverCorners[Corner]),
				DirectionalShadowDistance, 1.0e-8);
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
		DirectionalShadowCascadesUseFrozenSplitsOverlapAndSelection)
	{
		FSceneView View;
		View.ProjectionMatrix = MakePerspectiveProjection(90.0, 2.0, 1.0, 600.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.Settings.DirectionalShadow.Candidate =
			EDirectionalShadowCandidate::ThreeCascades;
		FDirectionalLightSceneData Light;
		Light.Direction = {0.0, 0.0, -1.0};
		FPreparedDirectionalShadowView Shadow;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(9), Light, Shadow));
		ASSERT_EQ(Shadow.CascadeCount, DirectionalShadowCascadeCount);
		EXPECT_EQ(Shadow.Candidate,
			EDirectionalShadowCandidate::ThreeCascades);
		EXPECT_DOUBLE_EQ(Shadow.SplitDepths[0], 1.0);
		EXPECT_DOUBLE_EQ(Shadow.SplitDepths[3], DirectionalShadowDistance);
		for (uint32 Boundary = 1; Boundary < 3; ++Boundary)
		{
			const double P = static_cast<double>(Boundary) / 3.0;
			const double Expected = DirectionalShadowSplitLambda
				* std::pow(DirectionalShadowDistance, P)
				+ (1.0 - DirectionalShadowSplitLambda)
					* (1.0 + (DirectionalShadowDistance - 1.0) * P);
			EXPECT_NEAR(Shadow.SplitDepths[Boundary], Expected, 1.0e-10);
			EXPECT_LT(Shadow.SplitDepths[Boundary - 1],
				Shadow.SplitDepths[Boundary]);
		}
		for (uint32 CascadeIndex = 0;
			CascadeIndex < DirectionalShadowCascadeCount; ++CascadeIndex)
		{
			const auto& Cascade = Shadow.Cascades[CascadeIndex];
			EXPECT_TRUE(Cascade.bEnabled);
			EXPECT_EQ(Cascade.Layer, CascadeIndex);
			EXPECT_GT(Cascade.TexelWorldSize.x, 0.0);
			EXPECT_GT(Cascade.TexelWorldSize.y, 0.0);
			EXPECT_EQ(Cascade.Filter.ComparisonOperations, 9u);
			EXPECT_EQ(Cascade.Filter.GuardTexels, 2u);
		}

		const auto& Mid = Shadow.Cascades[1];
		const double TransitionMiddle =
			(Mid.TransitionStartDepth + Mid.NearDepth) * 0.5;
		uint32 Selected = 0;
		uint32 Near = 0;
		double Weight = 0.0;
		ASSERT_TRUE(SelectDirectionalShadowCascade(
			Shadow, TransitionMiddle, Selected, Near, Weight));
		EXPECT_EQ(Near, 0u);
		EXPECT_EQ(Selected, 1u);
		EXPECT_NEAR(Weight, 0.5, 1.0e-12);
		ASSERT_TRUE(SelectDirectionalShadowCascade(
			Shadow, Shadow.SplitDepths[2] + 1.0, Selected, Near, Weight));
		EXPECT_EQ(Selected, 2u);
		EXPECT_EQ(Near, 2u);
		EXPECT_DOUBLE_EQ(Weight, 0.0);
		EXPECT_FALSE(SelectDirectionalShadowCascade(
			Shadow, DirectionalShadowDistance + 1.0, Selected, Near, Weight));
	}

	TEST(FRendererSceneViewTests,
		DirectionalShadowOrthographicSplitsAreUniformAndInvalidIdentityFallsBack)
	{
		FSceneView View;
		View.ProjectionMatrix = MakeOrthographicProjection(2.0, 1.0, 1.0, 600.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.Settings.DirectionalShadow.Candidate =
			static_cast<EDirectionalShadowCandidate>(255);
		FDirectionalLightSceneData Light;
		Light.Direction = {0.0, -1.0, -1.0};
		FPreparedDirectionalShadowView Single;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(11), Light, Single));
		EXPECT_EQ(Single.Candidate, EDirectionalShadowCandidate::SingleMap);
		EXPECT_EQ(Single.CascadeCount, 1u);

		View.Settings.DirectionalShadow.Candidate =
			EDirectionalShadowCandidate::ThreeCascades;
		FPreparedDirectionalShadowView Cascaded;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(11), Light, Cascaded));
		EXPECT_DOUBLE_EQ(Cascaded.SplitDepths[0], 1.0);
		EXPECT_DOUBLE_EQ(Cascaded.SplitDepths[1], 86.0);
		EXPECT_DOUBLE_EQ(Cascaded.SplitDepths[2], 171.0);
		EXPECT_DOUBLE_EQ(Cascaded.SplitDepths[3], 256.0);
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
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Cascades[0].TexelBias.z,
			Shadow.Cascades[0].Bias.ReceiverWorld);
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Cascades[0].Filter.x,
			1.0f / static_cast<float>(DirectionalShadowResolution));
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Cascades[0].Filter.y,
			1.0f / static_cast<float>(DirectionalShadowResolution));
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Cascades[0].Filter.z,
			static_cast<float>(EDirectionalShadowFilterQuality::Medium));
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Cascades[0].Filter.w, 1.5f);
		EXPECT_FLOAT_EQ(Enabled.DirectionalShadow.Cascades[0].ValidRegion.x,
			2.0f / static_cast<float>(DirectionalShadowResolution));

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
		EXPECT_FLOAT_EQ(Facing.ReceiverWorld, 0.0f);
		EXPECT_FLOAT_EQ(Facing.NormalWorld, 0.0f);
		EXPECT_FALSE(Facing.bUsedFallback);
		EXPECT_FALSE(Facing.bTotalClamped);

		const FDirectionalShadowBias Grazing =
			CalculateDirectionalShadowBias({0.125, 0.0625}, 0.0);
		EXPECT_FLOAT_EQ(Grazing.ReceiverWorld, 0.0f);
		EXPECT_FLOAT_EQ(Grazing.NormalWorld, 0.0f);
		EXPECT_FALSE(Grazing.bTotalClamped);
		EXPECT_LE(Grazing.ReceiverWorld + Grazing.NormalWorld, 0.08f);

		const FDirectionalShadowBias Maximum =
			CalculateDirectionalShadowBias({0.25, 0.25}, 0.0);
		EXPECT_FLOAT_EQ(Maximum.RasterConstant, 1.5f);
		EXPECT_FLOAT_EQ(Maximum.RasterSlope, 1.5f);
		EXPECT_FLOAT_EQ(Maximum.RasterClamp, 4.0f);
		EXPECT_FLOAT_EQ(Maximum.ReceiverWorld, 0.0f);
		EXPECT_FLOAT_EQ(Maximum.NormalWorld, 0.0f);
		EXPECT_FALSE(Maximum.bTotalClamped);

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
				EXPECT_GE(Bias.ReceiverWorld, 0.0f);
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
		View.Settings.DirectionalShadow.DiagnosticMode =
			EDirectionalShadowDiagnosticMode::ReceiverBiased;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, First));
		FPreparedDirectionalShadowView Second;
		View.Settings.DirectionalShadow.DiagnosticMode =
			EDirectionalShadowDiagnosticMode::TexelGrid;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, Second));
		EXPECT_EQ(First.DiagnosticMode,
			EDirectionalShadowDiagnosticMode::ReceiverBiased);
		EXPECT_EQ(Second.DiagnosticMode,
			EDirectionalShadowDiagnosticMode::TexelGrid);
		FPreparedDirectionalShadowView Invalid;
		View.Settings.DirectionalShadow.DiagnosticMode =
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

		View.Settings.DirectionalShadow.FilterQuality =
			EDirectionalShadowFilterQuality::High;
		FPreparedDirectionalShadowView High;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, High));
		View.Settings.DirectionalShadow.FilterQuality =
			EDirectionalShadowFilterQuality::Low;
		FPreparedDirectionalShadowView Low;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, Low));
		View.Settings.DirectionalShadow.FilterQuality =
			static_cast<EDirectionalShadowFilterQuality>(255);
		FPreparedDirectionalShadowView Invalid;
		ASSERT_TRUE(TryPrepareDirectionalShadowView(
			View, FLightSceneId(1), Light, Invalid));

		EXPECT_EQ(High.Cascades[0].Filter.Quality,
			EDirectionalShadowFilterQuality::High);
		EXPECT_EQ(High.Cascades[0].Filter.GuardTexels, 3u);
		EXPECT_EQ(Low.Cascades[0].Filter.Quality,
			EDirectionalShadowFilterQuality::Low);
		EXPECT_EQ(Low.Cascades[0].Filter.GuardTexels, 2u);
		EXPECT_EQ(Invalid.Cascades[0].Filter.Quality,
			EDirectionalShadowFilterQuality::Low);
		EXPECT_TRUE(Invalid.Cascades[0].Filter.bUsedInvalidQualityFallback);
		EXPECT_NE(High.Cascades[0].TexelWorldSize,
			Low.Cascades[0].TexelWorldSize);
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
		std::array<FStaticMeshLODResources, 3> LODResources;
		for (size_t LODIndex = 0; LODIndex < LODResources.size(); ++LODIndex)
		{
			LODResources[LODIndex].ScreenSize = ScreenSizes[LODIndex];
		}
		EXPECT_EQ(SelectStaticMeshLOD(0.5f, LODResources), 0u);
		EXPECT_EQ(SelectStaticMeshLOD(0.49f, LODResources), 1u);
		EXPECT_EQ(SelectStaticMeshLOD(0.25f, LODResources), 1u);
		EXPECT_EQ(SelectStaticMeshLOD(0.24f, LODResources), 2u);
		EXPECT_EQ(
			SelectStaticMeshLOD(std::numeric_limits<float>::quiet_NaN(), LODResources),
			0u);
		LODResources[2].bReadyForRendering = true;
		EXPECT_EQ(
			ResolveAvailableStaticMeshLOD(
				1, LODResources),
			2u);
		LODResources[0].bReadyForRendering = true;
		LODResources[2].bReadyForRendering = false;
		EXPECT_EQ(
			ResolveAvailableStaticMeshLOD(
				2, LODResources),
			0u);
		LODResources[0].bReadyForRendering = false;
		EXPECT_EQ(
			ResolveAvailableStaticMeshLOD(
				0, LODResources),
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
			MeshLODs[SelectStaticMeshLOD(0.6f, LODResources)].GetTriangleCount(),
			4u);
		EXPECT_EQ(
			MeshLODs[SelectStaticMeshLOD(0.3f, LODResources)].GetTriangleCount(),
			2u);
		EXPECT_EQ(
			MeshLODs[SelectStaticMeshLOD(0.1f, LODResources)].GetTriangleCount(),
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

	TEST(FRendererSceneViewTests, ContactShadowsDefaultToOptInDetail)
	{
		const FSceneViewSettings Settings;
		EXPECT_FALSE(Settings.DirectionalShadow.bEnableContactShadows);
		EXPECT_FALSE(Settings.DirectionalShadow.bShowContactDebug);
	}
} // namespace Durin
