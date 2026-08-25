#include "SkyBoxTestSupport.h"

namespace
{
	class FSkyBoxAssetCompilingEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
		}

		auto TearDown() -> void override
		{
			Durin::ShutdownAssetCompilingManager();
		}
	};

	[[maybe_unused]] testing::Environment* GSkyBoxAssetCompilingEnvironment =
		testing::AddGlobalTestEnvironment(new FSkyBoxAssetCompilingEnvironment);
}
#include "Math/Operations.h"
#include "SceneViewProjection.h"

TEST(FSkyBoxRenderingTests, ReconstructsTranslationInvariantDirectionAndInverseComponentRotation)
{
	Durin::FSkyBoxSceneData SkyBox;
	SkyBox.Rotation = Durin::Math::MakeQuaternionFromAxisAngleRadians(
		Durin::Math::HalfPi<Durin::FReal>(), Durin::FVectorConstants::Up);
	SkyBox.Tint = {0.25f, 0.5f, 0.75f};
	SkyBox.Intensity = 2.0f;

	Durin::FSceneView OriginView;
	OriginView.ViewProjectionMatrix = Durin::FMatrix(1.0);
	Durin::SkyBoxRendering::FSkyBoxUniform OriginUniform;
	ASSERT_TRUE(Durin::SkyBoxRendering::BuildUniform(OriginView, SkyBox, OriginUniform));

	Durin::FSceneView TranslatedView = OriginView;
	TranslatedView.ViewLocation = {7.0, -3.0, 11.0};
	TranslatedView.ViewProjectionMatrix = Durin::Math::TranslationMatrix(-TranslatedView.ViewLocation);
	Durin::SkyBoxRendering::FSkyBoxUniform TranslatedUniform;
	ASSERT_TRUE(Durin::SkyBoxRendering::BuildUniform(TranslatedView, SkyBox, TranslatedUniform));

	const Durin::FVector3 OriginDirection = ReconstructSampleDirection(OriginUniform, {0.0, 0.0});
	const Durin::FVector3 TranslatedDirection = ReconstructSampleDirection(TranslatedUniform, {0.0, 0.0});
	for (uint32 Axis = 0; Axis < 3; ++Axis)
	{
		EXPECT_NEAR(OriginDirection[Axis], TranslatedDirection[Axis], 1.e-6);
	}
	const Durin::FVector3 ExpectedDirection = Durin::Math::RotateVector(
		Durin::Math::Inverse(Durin::Math::Normalize(SkyBox.Rotation)), Durin::FVectorConstants::Up);
	for (uint32 Axis = 0; Axis < 3; ++Axis)
	{
		EXPECT_NEAR(OriginDirection[Axis], ExpectedDirection[Axis], 1.e-6);
	}
	EXPECT_EQ(OriginUniform.TintIntensity, Durin::FVector4f(0.25f, 0.5f, 0.75f, 2.0f));
}

TEST(FSkyBoxRenderingTests, RejectsInvalidTransformsAndClampsIntensity)
{
	Durin::FSceneView View;
	Durin::FSkyBoxSceneData SkyBox;
	Durin::SkyBoxRendering::FSkyBoxUniform Uniform;

	View.ViewProjectionMatrix = Durin::FMatrix(0.0);
	EXPECT_FALSE(Durin::SkyBoxRendering::BuildUniform(View, SkyBox, Uniform));

	View.ViewProjectionMatrix = Durin::FMatrix(1.0);
	SkyBox.Rotation = Durin::FQuat(0.0, 0.0, 0.0, 0.0);
	EXPECT_FALSE(Durin::SkyBoxRendering::BuildUniform(View, SkyBox, Uniform));

	SkyBox.Rotation = Durin::FQuatConstants::Identity;
	SkyBox.Intensity = -3.0f;
	ASSERT_TRUE(Durin::SkyBoxRendering::BuildUniform(View, SkyBox, Uniform));
	EXPECT_FLOAT_EQ(Uniform.TintIntensity.w, 0.0f);
}

TEST(FSkyBoxRenderingTests, KeepsDirectionsStableForTinyNearClipAtLargeWorldCoordinates)
{
	constexpr double NearClip = 0.001;
	constexpr double FarClip = 500000.0;
	Durin::FSceneView View;
	View.ViewLocation = {4.0e7, -3.0e7, 2.0e7};
	View.ViewMatrix = Durin::Math::TranslationMatrix(-View.ViewLocation);
	ASSERT_TRUE(Durin::SceneViewProjection::BuildPerspectiveProjection(
		60.0, 16.0 / 9.0, NearClip, FarClip,
		Durin::ESceneDepthConvention::ReversedZ, View.ProjectionMatrix));
	View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;

	Durin::SkyBoxRendering::FSkyBoxUniform Uniform;
	ASSERT_TRUE(Durin::SkyBoxRendering::BuildUniform(
		View, Durin::FSkyBoxSceneData{}, Uniform));
	const Durin::FVector3 Center = ReconstructSampleDirection(Uniform, {0.0, 0.0});
	const Durin::FVector3 Neighbor = ReconstructSampleDirection(Uniform, {0.001, 0.0});
	EXPECT_GT(Durin::Math::Dot(Center, Neighbor), 0.999999);
	EXPECT_NEAR(Center.x, 1.0, 1.e-6);
	EXPECT_NEAR(Center.y, 0.0, 1.e-5);
	EXPECT_NEAR(Center.z, 0.0, 1.e-5);
}
