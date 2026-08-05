#include "MaterialTestSupport.h"
#include "CookedAsset.h"
#include "Materials/MaterialTypes.h"
#include "NativeTestSupport.h"

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

	auto MakeV1Input() -> Durin::FMaterialRenderRepresentationInput
	{
		Durin::FMaterialRenderRepresentationInput Input;
		Input.Layout = Durin::MakeMaterialRenderLayoutV1();
		Input.UniformPayload.resize(32, std::byte{0});
		auto Write = [&Input](Durin::uint32 Offset, float Value) { std::memcpy(Input.UniformPayload.data() + Offset, &Value, sizeof(Value)); };
		Write(0, 0.95f); Write(4, 0.62f); Write(8, 0.22f); Write(12, 1.0f); Write(16, 0.35f); Write(20, 32.0f);
		Input.Resources.resize(1);
		return Input;
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
	EXPECT_EQ(Layout.Identity.Id, Durin::MaterialRenderLayoutV3Id);
	EXPECT_EQ(Layout.UniformPayloadSize, 416u);
	EXPECT_EQ(Layout.UniformFieldCount, 48u);
	EXPECT_EQ(Layout.ResourceFieldCount, 8u);
	ASSERT_EQ(Layout.Fields.size(), 56u);

	const Durin::FMaterialRenderRepresentation Error;
	EXPECT_TRUE(Error.IsError());
	EXPECT_EQ(Error.GetLayout().Identity, Layout.Identity);
	ASSERT_EQ(Error.GetUniformPayload().size(), 416u);
	EXPECT_FLOAT_EQ(ReadFloat(Error.GetUniformPayload(), 0), 1.0f);
	EXPECT_FLOAT_EQ(ReadFloat(Error.GetUniformPayload(), 4), 0.0f);
	EXPECT_FLOAT_EQ(ReadFloat(Error.GetUniformPayload(), 8), 1.0f);
	EXPECT_FLOAT_EQ(ReadFloat(Error.GetUniformPayload(), 12), 1.0f);
	EXPECT_FLOAT_EQ(ReadFloat(Error.GetUniformPayload(), 28), 0.0f);
	EXPECT_FLOAT_EQ(ReadFloat(Error.GetUniformPayload(), 40), 1.0f);
	EXPECT_FLOAT_EQ(ReadFloat(Error.GetUniformPayload(), 44), 0.5f);
	EXPECT_FLOAT_EQ(ReadFloat(Error.GetUniformPayload(), 48), 1.0f);
	EXPECT_FLOAT_EQ(ReadFloat(Error.GetUniformPayload(), 52), 1.0f);
	EXPECT_FLOAT_EQ(ReadFloat(Error.GetUniformPayload(), 384), 13.0f);
	EXPECT_EQ(Error.GetResources().size(), 8u);

	const Durin::FMaterialRenderData& ErrorData =
		Durin::GetErrorMaterialRenderData();
	EXPECT_TRUE(ErrorData.Representation.IsError());
	EXPECT_EQ(
		ErrorData.PipelineIdentity.ShaderMap.ShadingModel,
		Durin::EMaterialShadingModel::Unlit);
	EXPECT_TRUE(ErrorData.PipelineIdentity.bTwoSided);
	EXPECT_EQ(
		ErrorData.PipelineIdentity.DepthWritePolicy,
		Durin::EMaterialDepthWritePolicy::Enabled);
}

TEST(FDefaultMaterialServiceTests, LoadsAndRetainsOneNeutralAuthoredProxy)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::PathUtilities::InitDefaultMountPoints());
	const bool bOwnsRenderingThread =
		Durin::GetRenderCommandAdmissionState()
			== Durin::ERenderCommandAdmissionState::Stopped;
	if (bOwnsRenderingThread) Durin::InitRenderingThread();
	ASSERT_TRUE(Durin::InitializeDefaultMaterialService());
	EXPECT_TRUE(Durin::IsDefaultMaterialServiceAvailable());
	Durin::FMaterialRenderProxyRef First =
		Durin::GetDefaultMaterialRenderProxy();
	Durin::FMaterialRenderProxyRef Second =
		Durin::GetDefaultMaterialRenderProxy();
	ASSERT_TRUE(First);
	EXPECT_EQ(First.GetReference(), Second.GetReference());

	Durin::FMaterialRenderData Resolved;
	struct FCaptureDefaultMaterialCommand
	{
		static constexpr const char* GetName()
		{
			return "CaptureDefaultMaterial";
		}
	};
	Durin::EnqueueRenderCommand<FCaptureDefaultMaterialCommand>(
		[First, &Resolved](Durin::FRHICommandListImmediate&) {
			Resolved = First->Resolve_RenderThread();
		});
	WaitForRenderingThread();
	const Durin::FMaterialRenderV2Binding Binding =
		GetMaterialBinding(Resolved);
	EXPECT_EQ(Binding.BaseColor, Durin::FVector4f(0.5f, 0.5f, 0.5f, 1.0f));
	EXPECT_EQ(Binding.Normal, Durin::FVector3f(0.0f, 0.0f, 1.0f));
	EXPECT_FLOAT_EQ(Binding.Metallic, 0.0f);
	EXPECT_FLOAT_EQ(Binding.Roughness, 0.5f);
	EXPECT_FLOAT_EQ(Binding.AmbientOcclusion, 1.0f);
	EXPECT_EQ(Binding.Emissive, Durin::FVector3f(0.0f));
	EXPECT_FALSE(Resolved.Representation.IsError());
	EXPECT_EQ(
		Resolved.PipelineIdentity.ShaderMap.ShadingModel,
		Durin::EMaterialShadingModel::Lit);
	EXPECT_FALSE(Resolved.PipelineIdentity.bTwoSided);

	const auto CookRoots = Durin::GetEngineBuiltInCookRoots();
	ASSERT_EQ(CookRoots.size(), 1u);
	EXPECT_EQ(CookRoots[0], Durin::DefaultMaterialAssetPath);

	Durin::ShutdownDefaultMaterialService();
	EXPECT_FALSE(Durin::IsDefaultMaterialServiceAvailable());
	EXPECT_FALSE(Durin::GetDefaultMaterialRenderProxy());
	First = {};
	Second = {};
	Durin::FAssetPath DefaultPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::DefaultMaterialAssetPath, DefaultPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(DefaultPath));
	Durin::CollectGarbage();
	WaitForRenderingThread();
	if (bOwnsRenderingThread) Durin::ShutdownRenderingThread();
}

TEST(FDefaultMaterialServiceTests, MissingEngineContentSelectsErrorTerminal)
{
	InitializeDObjectSystem();
	Durin::ResetMaterialFallbackDiagnosticsForTests();
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("MissingDefaultMaterial");
	const std::array Definitions{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Engine,
			.Root = Root,
			.bAutoScan = true,
			.bAuthoringWritable = false}};
	Durin::PathUtilities::FScopedMountRegistryFixture Registry(Definitions);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	EXPECT_FALSE(Durin::InitializeDefaultMaterialService());
	EXPECT_FALSE(Durin::GetDefaultMaterialRenderProxy());
	EXPECT_EQ(
		Durin::GetMaterialFallbackDiagnosticsSnapshot().Get(
			Durin::EMaterialFallbackReason::DefaultAssetUnavailable),
		1u);
	const Durin::FMaterialRenderData& Error =
		Durin::GetErrorMaterialRenderData();
	EXPECT_TRUE(Error.Representation.IsError());
	const Durin::FMaterialRenderV2Binding Binding = GetMaterialBinding(Error);
	EXPECT_EQ(Binding.BaseColor, Durin::FVector4f(1.0f, 0.0f, 1.0f, 1.0f));
	Durin::ShutdownDefaultMaterialService();
}

TEST(FDefaultMaterialCookTests, UnreferencedBuiltInRootPublishesAndLoadsCooked)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::PathUtilities::InitDefaultMountPoints());
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::DefaultMaterialAssetPath, Path));
	Durin::DMaterial* Source = nullptr;
	Durin::Asset::FAssetResult Result = Durin::Asset::LoadAsset(Path, Source);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Source, nullptr);
	std::vector<Durin::uint8> PackageBytes;
	Result = Durin::Asset::SerializeAssetPackageBytes(
		Source->GetPackage(), PackageBytes);
	ASSERT_TRUE(Result) << Result.Message;

	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::CreateTestFixtureDirectory("DefaultMaterialCook"));
	Durin::Asset::FCookContext Cook(
		CookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Cook.AddPackage(
		std::string(Durin::DefaultMaterialAssetPath),
		std::move(PackageBytes),
		{},
		&Error)) << Error;
	ASSERT_TRUE(Cook.Publish(&Error)) << Error;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dasset"));
	EXPECT_FALSE(std::filesystem::exists(
		CookRoot / "Engine/Materials/DefaultMaterial.dbulk"));

	Durin::Asset::ShutdownAssetManager();
	Durin::CollectGarbage();
	Durin::Asset::FAssetManager::Get().Initialize();
	Result = Durin::Asset::ConfigurePackageLoadContext({
		Durin::Asset::EPackageLoadMode::CookedRuntime,
		CookRoot});
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DMaterial* Cooked = nullptr;
	Result = Durin::Asset::LoadAsset(Path, Cooked);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Cooked, nullptr);
	EXPECT_EQ(
		GetMaterialBinding(Cooked->GetRenderData()).BaseColor,
		Durin::FVector4f(0.5f, 0.5f, 0.5f, 1.0f));

	Durin::Asset::ShutdownAssetManager();
	Durin::CollectGarbage();
	Durin::Asset::FAssetManager::Get().Initialize();
}

TEST(FErrorMaterialTests, MissingStructuralProxyUsesErrorWithoutAssetLookup)
{
	InitializeDObjectSystem();
	Durin::ResetMaterialFallbackDiagnosticsForTests();
	const bool bOwnsRenderingThread =
		Durin::GetRenderCommandAdmissionState()
			== Durin::ERenderCommandAdmissionState::Stopped;
	if (bOwnsRenderingThread) Durin::InitRenderingThread();
	Durin::FStaticMeshSceneProxy Proxy(
		nullptr,
		std::vector<Durin::FMaterialRenderProxyRef>{
			Durin::FMaterialRenderProxyRef{}},
		1);
	Durin::FMaterialRenderData Resolved;
	struct FCaptureMissingProxyErrorCommand
	{
		static constexpr const char* GetName()
		{
			return "CaptureMissingProxyError";
		}
	};
	Durin::EnqueueRenderCommand<FCaptureMissingProxyErrorCommand>(
		[&Proxy, &Resolved](Durin::FRHICommandListImmediate&) {
			Resolved = Proxy.ResolveMaterialRenderData_RenderThread(0);
		});
	WaitForRenderingThread();
	EXPECT_TRUE(Resolved.Representation.IsError());
	EXPECT_EQ(
		GetMaterialBinding(Resolved).BaseColor,
		Durin::FVector4f(1.0f, 0.0f, 1.0f, 1.0f));
	EXPECT_EQ(
		Durin::GetMaterialFallbackDiagnosticsSnapshot().Get(
			Durin::EMaterialFallbackReason::MissingProxy),
		1u);
	if (bOwnsRenderingThread) Durin::ShutdownRenderingThread();
}

TEST(FMaterialRenderRepresentationTests, ValidPayloadIsAcceptedAsOneCompleteRepresentation)
{
	const Durin::FMaterialRenderRepresentation Fallback =
		Durin::MakeCanonicalMaterialRenderRepresentation();
	EXPECT_FALSE(Fallback.IsError());
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
	EXPECT_FALSE(Representation.IsError());
	EXPECT_EQ(Representation.GetUniformPayload().size(), 416u);
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
		Input.UniformPayload[56] = std::byte{1};
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
	ASSERT_TRUE(Builder.SetScalar(Durin::MaterialParameters::MetallicId, 0.8f));
	ASSERT_TRUE(Builder.SetScalar(Durin::MaterialParameters::RoughnessId, 0.25f));
	ASSERT_TRUE(Builder.SetVector(Durin::MaterialParameters::NormalId, Durin::FVector3(0.0, 0.0, 1.0)));
	ASSERT_TRUE(Builder.SetScalar(Durin::MaterialParameters::UVChannelIds[0], 3.0f));
	ASSERT_TRUE(Builder.SetScalar(Durin::MaterialParameters::UVRotationIds[0], 0.5f));
	Durin::FMaterialSamplerState Sampler;
	Sampler.MinFilter = Durin::EMaterialSamplerMinFilter::NearestMipmapLinear;
	Sampler.MagFilter = Durin::EMaterialSamplerMagFilter::Nearest;
	Sampler.AddressU = Durin::EMaterialSamplerAddressMode::MirroredRepeat;
	Sampler.AddressV = Durin::EMaterialSamplerAddressMode::ClampToEdge;
	ASSERT_TRUE(Builder.SetScalar(
		Durin::MaterialParameters::SamplerStateIds[0],
		Durin::EncodeMaterialSamplerState(Sampler)));
	ASSERT_TRUE(Builder.SetTexture(
		Durin::MaterialParameters::BaseColorTextureId,
		Durin::FRHITextureReferenceRef{}));

	Durin::FMaterialRenderRepresentation Representation;
	Durin::FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Builder.Build(Representation, Diagnostic));
	EXPECT_FALSE(Representation.IsError());
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 0), 0.2f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 4), 0.4f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 8), 0.6f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 12), 0.35f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 28), 0.8f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 44), 0.25f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 64), 3.0f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 352), 0.5f);
	Durin::FMaterialRenderV3Binding Binding;
	ASSERT_TRUE(Durin::TryGetMaterialRenderV3Binding(
		Representation, Binding, Diagnostic)) << Diagnostic.Message;
	EXPECT_FLOAT_EQ(Binding.UVRotations[0], 0.5f);
	EXPECT_EQ(Binding.Samplers[0], Sampler);
}

TEST(FMaterialRenderRepresentationTests, V2BindingRemainsExactlyDecodable)
{
	Durin::FMaterialRenderRepresentationInput Input;
	Input.Layout = Durin::MakeMaterialRenderLayoutV2();
	Input.UniformPayload.resize(352, std::byte{0});
	auto Write = [&Input](Durin::uint32 Offset, float Value) {
		std::memcpy(Input.UniformPayload.data() + Offset, &Value, sizeof(Value));
	};
	Write(0, 0.2f);
	Write(4, 0.4f);
	Write(8, 0.6f);
	Write(12, 0.8f);
	for (Durin::uint32 Role = 0; Role < 8; ++Role)
	{
		Write(96 + Role * 16, 1.0f);
		Write(100 + Role * 16, 1.0f);
	}
	Input.Resources.resize(8);
	Durin::FMaterialRenderRepresentation Representation;
	Durin::FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Durin::FMaterialRenderRepresentation::TryCreate(
		std::move(Input), Representation, Diagnostic)) << Diagnostic.Message;
	Durin::FMaterialRenderV2Binding Binding;
	ASSERT_TRUE(Durin::TryGetMaterialRenderV2Binding(
		Representation, Binding, Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Binding.BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 0.8f));
}

TEST(FMaterialRenderRepresentationTests, V1BindingReadsCompactValuesWithoutParameterLookup)
{
	Durin::FMaterialRenderRepresentationInput Input = MakeV1Input();
	const std::array Values{0.3f, 0.5f, 0.7f, 0.4f, 0.6f, 48.0f};
	const std::array<Durin::uint32, 6> Offsets{0, 4, 8, 12, 16, 20};
	for (size_t Index = 0; Index < Values.size(); ++Index) std::memcpy(Input.UniformPayload.data() + Offsets[Index], &Values[Index], sizeof(float));
	Durin::FMaterialRenderRepresentation Representation;
	Durin::FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Durin::FMaterialRenderRepresentation::TryCreate(std::move(Input), Representation, Diagnostic));

	Durin::FMaterialRenderV1Binding Binding;
	ASSERT_TRUE(Durin::TryGetMaterialRenderV1Binding(
		Representation, Binding, Diagnostic));
	EXPECT_FLOAT_EQ(Binding.BaseColor.r, 0.3f);
	EXPECT_FLOAT_EQ(Binding.BaseColor.g, 0.5f);
	EXPECT_FLOAT_EQ(Binding.BaseColor.b, 0.7f);
	EXPECT_FLOAT_EQ(Binding.BaseColor.a, 0.4f);
	EXPECT_FLOAT_EQ(Binding.SpecularStrength, 0.6f);
	EXPECT_FLOAT_EQ(Binding.Shininess, 48.0f);
	EXPECT_EQ(Binding.BaseColorTexture, nullptr);
}

TEST(FMaterialRenderRepresentationTests, V1BindingRejectsAChangedFieldTable)
{
	Durin::FMaterialRenderRepresentationInput Input = MakeV1Input();
	Input.Layout.Fields[0].ParameterId = Durin::FGuid{1, 2, 3, 4};

	Durin::FMaterialRenderRepresentation Representation;
	Durin::FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Durin::FMaterialRenderRepresentation::TryCreate(
		std::move(Input), Representation, Diagnostic));

	Durin::FMaterialRenderV1Binding Binding;
	EXPECT_FALSE(Durin::TryGetMaterialRenderV1Binding(
		Representation, Binding, Diagnostic));
	EXPECT_EQ(
		Diagnostic.Failure,
		Durin::EMaterialRenderValidationFailure::InvalidField);
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
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::RoughnessName(), 0.25f));

	const Durin::FMaterialRenderData RenderData = Instance->GetRenderData();
	const Durin::FMaterialRenderV2Binding Binding = GetMaterialBinding(RenderData);
	EXPECT_FLOAT_EQ(Binding.BaseColor.r, 0.15f);
	EXPECT_FLOAT_EQ(Binding.BaseColor.g, 0.25f);
	EXPECT_FLOAT_EQ(Binding.BaseColor.b, 0.35f);
	EXPECT_FLOAT_EQ(Binding.BaseColor.a, 0.45f);
	EXPECT_FLOAT_EQ(Binding.Roughness, 0.25f);
	EXPECT_FALSE(RenderData.Representation.IsError());
	EXPECT_FLOAT_EQ(ReadFloat(RenderData.Representation.GetUniformPayload(), 0), 0.15f);
	EXPECT_FLOAT_EQ(ReadFloat(RenderData.Representation.GetUniformPayload(), 4), 0.25f);
	EXPECT_FLOAT_EQ(ReadFloat(RenderData.Representation.GetUniformPayload(), 8), 0.35f);
	EXPECT_FLOAT_EQ(ReadFloat(RenderData.Representation.GetUniformPayload(), 12), 0.45f);
	EXPECT_FLOAT_EQ(ReadFloat(RenderData.Representation.GetUniformPayload(), 44), 0.25f);
}

TEST(FMaterialRenderRepresentationTests, V3CompilationCanonicalizesEveryInputClass)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "CanonicalPBRMaterial");
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	ASSERT_TRUE(Material->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(NaN, 0.0, 0.0)));
	ASSERT_TRUE(Material->SetVectorParameterValue(Durin::MaterialParameters::NormalName(), Durin::FVector3(0.0)));
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::MaterialParameters::MetallicName(), 2.0f));
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::MaterialParameters::RoughnessName(), std::numeric_limits<float>::quiet_NaN()));
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::MaterialParameters::AmbientOcclusionName(), -1.0f));
	ASSERT_TRUE(Material->SetVectorParameterValue(Durin::MaterialParameters::EmissiveName(), Durin::FVector3(100.0, -2.0, 4.0)));
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::FName("BaseColorUVChannel"), 2.6f));
	ASSERT_TRUE(Material->SetVector2ParameterValue(Durin::FName("BaseColorUVScale"), Durin::FVector2(2.0, -3.0)));
	ASSERT_TRUE(Material->SetVector2ParameterValue(Durin::FName("BaseColorUVOffset"), Durin::FVector2(2048.0, -2048.0)));
	Durin::DTexture2D* WrongUsageTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "WrongNormalUsage");
	ASSERT_TRUE(Material->SetTextureParameterValue(Durin::MaterialParameters::NormalTextureName(), WrongUsageTexture));

	const Durin::FMaterialRenderV2Binding Binding = GetMaterialBinding(Material->GetRenderData());
	EXPECT_EQ(Binding.BaseColor, Durin::FVector4f(0.95f, 0.62f, 0.22f, 1.0f));
	EXPECT_EQ(Binding.Normal, Durin::FVector3f(0.0f, 0.0f, 1.0f));
	EXPECT_FLOAT_EQ(Binding.Metallic, 1.0f);
	EXPECT_FLOAT_EQ(Binding.Roughness, 0.5f);
	EXPECT_FLOAT_EQ(Binding.AmbientOcclusion, 0.0f);
	EXPECT_EQ(Binding.Emissive, Durin::FVector3f(64.0f, 0.0f, 4.0f));
	EXPECT_FLOAT_EQ(Binding.UVChannels[0], 3.0f);
	EXPECT_EQ(Binding.UVScales[0], Durin::FVector2f(2.0f, -3.0f));
	EXPECT_EQ(Binding.UVOffsets[0], Durin::FVector2f(1024.0f, -1024.0f));
	EXPECT_EQ(Binding.Textures[1], nullptr);

	Durin::MarkAsGarbage(WrongUsageTexture);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
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

	Version = Durin::CurrentMaterialParameterSchemaVersion - 1;
	Warning.clear();
	Error.clear();
	EXPECT_TRUE(Durin::UpgradeMaterialParameterSchemaVersion(Version, Warning, Error));
	EXPECT_EQ(Version, Durin::CurrentMaterialParameterSchemaVersion);
	EXPECT_FALSE(Warning.empty());
	EXPECT_TRUE(Error.empty());

	Version = Durin::CurrentMaterialParameterSchemaVersion + 1;
	EXPECT_FALSE(Durin::UpgradeMaterialParameterSchemaVersion(Version, Warning, Error));
	EXPECT_FALSE(Error.empty());
}
