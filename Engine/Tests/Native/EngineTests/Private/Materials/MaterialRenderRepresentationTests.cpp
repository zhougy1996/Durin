#include "MaterialTestSupport.h"
#include "Materials/MaterialTypes.h"

#include <cstring>
#include <limits>

namespace
{
	auto ReadFloat(std::span<const std::byte> Bytes, Durin::uint32 Offset) -> float
	{
		float Value = 0.0f;
		std::memcpy(&Value, Bytes.data() + Offset, sizeof(Value));
		return Value;
	}
}

TEST(FMaterialRenderRepresentationTests, DefaultLayoutHasStableIdentityAndPacking)
{
	const Durin::FMaterialRenderLayout Layout =
		Durin::MakeDefaultMaterialRenderLayout();
	Durin::FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Durin::ValidateMaterialRenderLayout(Layout, Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Durin::EMaterialRenderValidationFailure::None);
	EXPECT_EQ(Layout.Identity.Version, Durin::CurrentMaterialRenderLayoutVersion);
	EXPECT_EQ(Layout.Identity.Id, Durin::MaterialRenderLayoutV1Id);
	EXPECT_EQ(Layout.UniformPayloadSize, 32u);
	EXPECT_EQ(Layout.UniformFieldCount, 4u);
	EXPECT_EQ(Layout.ResourceFieldCount, 1u);
	ASSERT_EQ(Layout.Fields.size(), 5u);

	const Durin::FMaterialRenderRepresentation Fallback;
	EXPECT_TRUE(Fallback.IsFallback());
	EXPECT_EQ(Fallback.GetLayout().Identity, Layout.Identity);
	ASSERT_EQ(Fallback.GetUniformPayload().size(), 32u);
	EXPECT_FLOAT_EQ(ReadFloat(Fallback.GetUniformPayload(), 0), 0.95f);
	EXPECT_FLOAT_EQ(ReadFloat(Fallback.GetUniformPayload(), 4), 0.62f);
	EXPECT_FLOAT_EQ(ReadFloat(Fallback.GetUniformPayload(), 8), 0.22f);
	EXPECT_FLOAT_EQ(ReadFloat(Fallback.GetUniformPayload(), 12), 1.0f);
	EXPECT_FLOAT_EQ(ReadFloat(Fallback.GetUniformPayload(), 16), 0.35f);
	EXPECT_FLOAT_EQ(ReadFloat(Fallback.GetUniformPayload(), 20), 32.0f);
	EXPECT_EQ(Fallback.GetResources().size(), 1u);
}

TEST(FMaterialRenderRepresentationTests, ValidPayloadIsAcceptedAsOneCompleteRepresentation)
{
	const Durin::FMaterialRenderRepresentation Fallback;
	Durin::FMaterialRenderRepresentationInput Input;
	Input.Layout = Fallback.GetLayout();
	Input.UniformPayload.assign(
		Fallback.GetUniformPayload().begin(), Fallback.GetUniformPayload().end());
	Input.Resources.assign(Fallback.GetResources().begin(), Fallback.GetResources().end());

	Durin::FMaterialRenderValidationDiagnostic Diagnostic;
	Durin::FMaterialRenderRepresentation Representation;
	ASSERT_TRUE(Durin::FMaterialRenderRepresentation::TryCreate(
		std::move(Input), Representation, Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Durin::EMaterialRenderValidationFailure::None);
	EXPECT_FALSE(Representation.IsFallback());
	EXPECT_EQ(Representation.GetUniformPayload().size(), 32u);
}

TEST(FMaterialRenderRepresentationTests, RejectsUnsupportedLayoutAndMalformedPayloads)
{
	const Durin::FMaterialRenderRepresentation Fallback;

	{
		Durin::FMaterialRenderRepresentationInput Input;
		Input.Layout = Fallback.GetLayout();
		Input.Layout.Identity.Version++;
		Input.UniformPayload.assign(
			Fallback.GetUniformPayload().begin(), Fallback.GetUniformPayload().end());
		Input.Resources.assign(Fallback.GetResources().begin(), Fallback.GetResources().end());
		Durin::FMaterialRenderValidationDiagnostic Diagnostic;
		Durin::FMaterialRenderRepresentation Representation;
		EXPECT_FALSE(Durin::FMaterialRenderRepresentation::TryCreate(
			std::move(Input), Representation, Diagnostic));
		EXPECT_EQ(
			Diagnostic.Failure,
			Durin::EMaterialRenderValidationFailure::UnsupportedVersion);
	}

	{
		Durin::FMaterialRenderRepresentationInput Input;
		Input.Layout = Fallback.GetLayout();
		Input.Layout.Fields[0].Offset = 4;
		Input.UniformPayload.assign(
			Fallback.GetUniformPayload().begin(), Fallback.GetUniformPayload().end());
		Input.Resources.assign(Fallback.GetResources().begin(), Fallback.GetResources().end());
		Durin::FMaterialRenderValidationDiagnostic Diagnostic;
		Durin::FMaterialRenderRepresentation Representation;
		EXPECT_FALSE(Durin::FMaterialRenderRepresentation::TryCreate(
			std::move(Input), Representation, Diagnostic));
		EXPECT_EQ(
			Diagnostic.Failure,
			Durin::EMaterialRenderValidationFailure::InvalidAlignment);
	}

	{
		Durin::FMaterialRenderRepresentationInput Input;
		Input.Layout = Fallback.GetLayout();
		Input.UniformPayload.assign(
			Fallback.GetUniformPayload().begin(), Fallback.GetUniformPayload().end());
		const float NaN = std::numeric_limits<float>::quiet_NaN();
		std::memcpy(Input.UniformPayload.data(), &NaN, sizeof(NaN));
		Input.Resources.assign(Fallback.GetResources().begin(), Fallback.GetResources().end());
		Durin::FMaterialRenderValidationDiagnostic Diagnostic;
		Durin::FMaterialRenderRepresentation Representation;
		EXPECT_FALSE(Durin::FMaterialRenderRepresentation::TryCreate(
			std::move(Input), Representation, Diagnostic));
		EXPECT_EQ(
			Diagnostic.Failure,
			Durin::EMaterialRenderValidationFailure::NonFiniteValue);
	}

	{
		Durin::FMaterialRenderRepresentationInput Input;
		Input.Layout = Fallback.GetLayout();
		Input.UniformPayload.assign(
			Fallback.GetUniformPayload().begin(), Fallback.GetUniformPayload().end());
		Input.UniformPayload[24] = std::byte{1};
		Input.Resources.assign(Fallback.GetResources().begin(), Fallback.GetResources().end());
		Durin::FMaterialRenderValidationDiagnostic Diagnostic;
		Durin::FMaterialRenderRepresentation Representation;
		EXPECT_FALSE(Durin::FMaterialRenderRepresentation::TryCreate(
			std::move(Input), Representation, Diagnostic));
		EXPECT_EQ(
			Diagnostic.Failure,
			Durin::EMaterialRenderValidationFailure::NonZeroPadding);
	}
}

TEST(FMaterialRenderRepresentationTests, BuilderCompilesValuesIntoCompactSlots)
{
	const Durin::FMaterialRenderRepresentation Fallback;
	Durin::FMaterialRenderRepresentationBuilder Builder(Fallback);
	ASSERT_TRUE(Builder.SetVector(
		Durin::MaterialParameters::BaseColorId,
		Durin::FVector3(0.2, 0.4, 0.6)));
	ASSERT_TRUE(Builder.SetScalar(
		Durin::MaterialParameters::OpacityId, 0.35f));
	ASSERT_TRUE(Builder.SetScalar(
		Durin::MaterialParameters::SpecularStrengthId, 0.8f));
	ASSERT_TRUE(Builder.SetScalar(
		Durin::MaterialParameters::ShininessId, 64.0f));
	ASSERT_TRUE(Builder.SetTexture(
		Durin::MaterialParameters::BaseColorTextureId,
		Durin::FRHITextureReferenceRef{}));

	Durin::FMaterialRenderRepresentation Representation;
	Durin::FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Builder.Build(Representation, Diagnostic));
	EXPECT_FALSE(Representation.IsFallback());
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 0), 0.2f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 4), 0.4f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 8), 0.6f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 12), 0.35f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 16), 0.8f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 20), 64.0f);
}

TEST(FMaterialRenderRepresentationTests, MaterialSnapshotsResolveThroughTheSelectedLayout)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base =
		Durin::NewObject<Durin::DMaterial>(nullptr, "RepresentationBase");
	Durin::DMaterialInstance* Instance =
		Durin::NewObject<Durin::DMaterialInstance>(nullptr, "RepresentationInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.15, 0.25, 0.35)));
	ASSERT_TRUE(Base->SetScalarParameterValue(
		Durin::MaterialParameters::OpacityName(), 0.45f));
	ASSERT_TRUE(Instance->SetScalarParameterValue(
		Durin::MaterialParameters::ShininessName(), 96.0f));

	const Durin::FMaterialRenderData RenderData = Instance->GetRenderData();
	EXPECT_FLOAT_EQ(RenderData.BaseColor.r, 0.15f);
	EXPECT_FLOAT_EQ(RenderData.BaseColor.g, 0.25f);
	EXPECT_FLOAT_EQ(RenderData.BaseColor.b, 0.35f);
	EXPECT_FLOAT_EQ(RenderData.BaseColor.a, 0.45f);
	EXPECT_FLOAT_EQ(RenderData.Shininess, 96.0f);
	EXPECT_FALSE(RenderData.Representation.IsFallback());
	EXPECT_FLOAT_EQ(ReadFloat(RenderData.Representation.GetUniformPayload(), 0), 0.15f);
	EXPECT_FLOAT_EQ(ReadFloat(RenderData.Representation.GetUniformPayload(), 4), 0.25f);
	EXPECT_FLOAT_EQ(ReadFloat(RenderData.Representation.GetUniformPayload(), 8), 0.35f);
	EXPECT_FLOAT_EQ(ReadFloat(RenderData.Representation.GetUniformPayload(), 12), 0.45f);
	EXPECT_FLOAT_EQ(ReadFloat(RenderData.Representation.GetUniformPayload(), 20), 96.0f);
}

TEST(FMaterialRenderRepresentationTests, AssetSchemaVersionIsSeparateAndBounded)
{
	Durin::FMaterialParameterSchemaVersion Version = 0;
	std::string Warning;
	std::string Error;
	EXPECT_TRUE(Durin::UpgradeMaterialParameterSchemaVersion(Version, Warning, Error));
	EXPECT_EQ(Version, Durin::CurrentMaterialParameterSchemaVersion);
	EXPECT_FALSE(Warning.empty());
	EXPECT_TRUE(Error.empty());

	Version = Durin::CurrentMaterialParameterSchemaVersion;
	Warning.clear();
	Error.clear();
	EXPECT_TRUE(Durin::UpgradeMaterialParameterSchemaVersion(Version, Warning, Error));
	EXPECT_TRUE(Warning.empty());
	EXPECT_TRUE(Error.empty());

	Version = Durin::CurrentMaterialParameterSchemaVersion + 1;
	EXPECT_FALSE(Durin::UpgradeMaterialParameterSchemaVersion(Version, Warning, Error));
	EXPECT_FALSE(Error.empty());
}
