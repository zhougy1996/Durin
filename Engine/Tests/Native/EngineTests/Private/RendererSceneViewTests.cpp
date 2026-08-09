#include <gtest/gtest.h>

#include "Renderers/SceneRenderer.h"
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
