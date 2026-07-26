#include "SkyBoxTestSupport.h"

TEST(FSkyBoxRenderingTests, ReconstructsTranslationInvariantDirectionAndInverseComponentRotation)
{
	Durin::FSkyBoxSceneData SkyBox;
	SkyBox.Rotation = glm::angleAxis(glm::half_pi<double>(), Durin::FVectorConstants::Up);
	SkyBox.Tint = {0.25f, 0.5f, 0.75f};
	SkyBox.Intensity = 2.0f;

	Durin::FSceneView OriginView;
	OriginView.ViewProjectionMatrix = Durin::FMatrix(1.0);
	Durin::SkyBoxRendering::FSkyBoxUniform OriginUniform;
	ASSERT_TRUE(Durin::SkyBoxRendering::BuildUniform(OriginView, SkyBox, OriginUniform));

	Durin::FSceneView TranslatedView = OriginView;
	TranslatedView.ViewLocation = {7.0, -3.0, 11.0};
	TranslatedView.ViewProjectionMatrix = glm::translate(Durin::FMatrix(1.0), -TranslatedView.ViewLocation);
	Durin::SkyBoxRendering::FSkyBoxUniform TranslatedUniform;
	ASSERT_TRUE(Durin::SkyBoxRendering::BuildUniform(TranslatedView, SkyBox, TranslatedUniform));

	const Durin::FVector3 OriginDirection = ReconstructSampleDirection(OriginUniform, {0.0, 0.0});
	const Durin::FVector3 TranslatedDirection = ReconstructSampleDirection(TranslatedUniform, {0.0, 0.0});
	for (Durin::uint32 Axis = 0; Axis < 3; ++Axis)
	{
		EXPECT_NEAR(OriginDirection[Axis], TranslatedDirection[Axis], 1.e-6);
	}
	const Durin::FVector3 ExpectedDirection = glm::inverse(glm::normalize(SkyBox.Rotation)) * Durin::FVectorConstants::Up;
	for (Durin::uint32 Axis = 0; Axis < 3; ++Axis)
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

	SkyBox.Rotation = glm::identity<Durin::FQuat>();
	SkyBox.Intensity = -3.0f;
	ASSERT_TRUE(Durin::SkyBoxRendering::BuildUniform(View, SkyBox, Uniform));
	EXPECT_FLOAT_EQ(Uniform.TintIntensity.w, 0.0f);
}
