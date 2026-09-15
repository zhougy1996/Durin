#include "MaterialRenderRepresentationTestFixture.h"

TEST(FMaterialRenderRepresentationTests, DefaultLayoutHasStableIdentityAndPacking)
{
	const Durin::FMaterialRenderLayout Layout =
		Durin::MakeErrorMaterialRenderLayout();
	Durin::FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Durin::ValidateMaterialRenderLayout(Layout, Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Durin::EMaterialRenderValidationFailure::None);
	EXPECT_EQ(Layout.Identity.Version, Durin::CurrentMaterialRenderLayoutVersion);
	EXPECT_TRUE(Layout.Identity.Id.IsValid());
	EXPECT_EQ(Layout.UniformPayloadSize, 16u);
	EXPECT_EQ(Layout.UniformFieldCount, 0u);
	EXPECT_EQ(Layout.ResourceFieldCount, 0u);
	ASSERT_TRUE(Layout.Fields.empty());

	const Durin::FMaterialRenderRepresentation Error;
	EXPECT_TRUE(Error.IsError());
	EXPECT_EQ(Error.GetLayout(), Durin::MakeErrorMaterialRenderLayout());
	EXPECT_EQ(Error.GetLayout().Identity.Version, Durin::CompiledMaterialRenderLayoutVersion);
	EXPECT_TRUE(Error.GetLayout().Fields.empty());
	ASSERT_EQ(Error.GetUniformPayload().size(), Durin::MaterialUniformControlBytes);
	EXPECT_TRUE(std::ranges::all_of(Error.GetUniformPayload(),
		[](std::byte Byte) { return Byte == std::byte{0}; }));
	EXPECT_TRUE(Error.GetResources().empty());
	Durin::FMaterialRenderBinding ErrorBinding;
	ASSERT_TRUE(Durin::TryGetMaterialRenderBinding(Error, ErrorBinding, Diagnostic));
	EXPECT_TRUE(ErrorBinding.CompiledTextures.empty());


	const Durin::FMaterialRenderData& ErrorData =
		Durin::GetErrorMaterialRenderData();
	EXPECT_TRUE(ErrorData.Representation.IsError());
	EXPECT_EQ(
		ErrorData.PlanningPassIdentity.ShaderMap.ShadingModel,
		Durin::EMaterialShadingModel::Unlit);
	EXPECT_TRUE(ErrorData.PlanningPassIdentity.bTwoSided);
	EXPECT_EQ(
		ErrorData.PlanningPassIdentity.DepthWritePolicy,
		Durin::EMaterialDepthWritePolicy::Enabled);
}

TEST(FMaterialProgramIdentityTests,
	CompiledLayoutSeparatesDynamicShaderAndPipelineIdentity)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedMaterial("M5FixedPathBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
		nullptr, "M5FixedPathInstance");
	ASSERT_TRUE(Instance->SetParent(Base));

	const Durin::FMaterialRenderData Initial = Base->GetRenderData();
	EXPECT_EQ(
		Initial.PlanningPassIdentity.ShaderMap.RenderLayout,
		Base->GetAcceptedCompiledProgram()->Layout.Identity);
	EXPECT_EQ(
		Initial.PlanningPassIdentity.ShaderMap.BlendMode,
		Durin::EMaterialBlendMode::Opaque);
	EXPECT_EQ(
		Initial.PlanningPassIdentity.ShaderMap.ShadingModel,
		Durin::EMaterialShadingModel::Lit);
	EXPECT_FLOAT_EQ(
		Initial.PlanningPassIdentity.ShaderMap.OpacityMaskThreshold,
		0.333f);
	EXPECT_FALSE(Initial.PlanningPassIdentity.bTwoSided);
	EXPECT_EQ(
		Initial.PlanningPassIdentity.DepthWritePolicy,
		Durin::EMaterialDepthWritePolicy::Automatic);

	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.2, 0.4, 0.8)));
	const Durin::FMaterialRenderData Dynamic = Base->GetRenderData();
	EXPECT_FALSE(std::ranges::equal(
		Dynamic.Representation.GetUniformPayload(),
		Initial.Representation.GetUniformPayload()));
	EXPECT_EQ(
		Dynamic.PlanningPassIdentity,
		Initial.PlanningPassIdentity);
	EXPECT_EQ(
		Instance->GetRenderData().PlanningPassIdentity,
		Dynamic.PlanningPassIdentity);

	Durin::FMaterialStaticProperties PipelineOnly =
		Base->GetStaticProperties();
	PipelineOnly.bTwoSided = true;
	PipelineOnly.DepthWritePolicy =
		Durin::EMaterialDepthWritePolicy::Disabled;
	ASSERT_TRUE(Base->SetStaticProperties(PipelineOnly));
	const Durin::FMaterialRenderData PipelineChanged = Base->GetRenderData();
	EXPECT_EQ(
		PipelineChanged.PlanningPassIdentity.ShaderMap,
		Dynamic.PlanningPassIdentity.ShaderMap);
	EXPECT_NE(
		PipelineChanged.PlanningPassIdentity,
		Dynamic.PlanningPassIdentity);

	Durin::FMaterialStaticProperties ShaderProperties = PipelineOnly;
	ShaderProperties.BlendMode = Durin::EMaterialBlendMode::Masked;
	ShaderProperties.ShadingModel = Durin::EMaterialShadingModel::Unlit;
	ShaderProperties.OpacityMaskThreshold = 0.625f;
	ASSERT_TRUE(Base->SetStaticProperties(ShaderProperties));
	ASSERT_TRUE(FinishMaterialCompileForTest(*Base));
	const Durin::FMaterialRenderData ShaderChanged = Base->GetRenderData();
	EXPECT_NE(
		ShaderChanged.PlanningPassIdentity.ShaderMap,
		PipelineChanged.PlanningPassIdentity.ShaderMap);
	EXPECT_EQ(
		Instance->GetRenderData().PlanningPassIdentity,
		ShaderChanged.PlanningPassIdentity);

	const Durin::FMaterialRenderData& Error =
		Durin::GetErrorMaterialRenderData();
	EXPECT_TRUE(Error.Representation.IsError());
	EXPECT_TRUE(std::ranges::all_of(
		Error.Representation.GetResources(),
		[](const auto& Resource) { return Resource == nullptr; }));

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialProgramIdentityTests,
	DualLayerRustFixtureUsesOneCompiledLayoutAcrossIndependentInstances)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto [Definitions, Program] = MakeDualLayerRustFixture();
	const FGuid RustAmountId = Definitions[5].Id;
	const FGuid RustMaskId = Definitions[4].Id;

	auto* Root = NewObject<DMaterial>(nullptr, "DualLayerRust");
	auto* LightRust = NewObject<DMaterialInstance>(nullptr, "LightRust");
	auto* HeavyRust = NewObject<DMaterialInstance>(nullptr, "HeavyRust");
	Program.SetParameterDefaults(Definitions);
	ASSERT_TRUE(Program.Apply(*Root));
	ASSERT_TRUE(FinishMaterialCompileForTest(*Root));
	ASSERT_TRUE(LightRust->SetParent(Root));
	ASSERT_TRUE(HeavyRust->SetParent(Root));
	ASSERT_TRUE(LightRust->SetScalarParameterValue(FName("RustAmount"), 0.1f));
	ASSERT_TRUE(HeavyRust->SetScalarParameterValue(FName("RustAmount"), 0.9f));
	auto HeavyMask = Definitions[4].Value;
	HeavyMask.GetTexture().TextureFallback = EMaterialTextureFallback::White;
	HeavyMask.GetTexture().SamplerState.AddressU = EMaterialSamplerAddressMode::ClampToEdge;
	ASSERT_TRUE(HeavyRust->SetParameterValue(
		RustMaskId, HeavyMask));
	const auto Accepted = Root->GetAcceptedCompiledProgram();
	ASSERT_NE(Accepted, nullptr);
	EXPECT_EQ(Accepted->Layout.ResourceFieldCount, 5u);
	EXPECT_EQ(Accepted->ActiveParameters.size(), 7u);
	const auto LightData = LightRust->GetRenderData();
	const auto HeavyData = HeavyRust->GetRenderData();
	EXPECT_EQ(LightData.CompiledProgram, Accepted);
	EXPECT_EQ(HeavyData.CompiledProgram, Accepted);
	EXPECT_FLOAT_EQ(ReadParameterFloat(LightData, RustAmountId), 0.1f);
	EXPECT_FLOAT_EQ(ReadParameterFloat(HeavyData, RustAmountId), 0.9f);
	const auto MaskField = std::ranges::find(Accepted->Layout.Fields,
		RustMaskId, &FMaterialRenderField::ParameterId);
	ASSERT_NE(MaskField, Accepted->Layout.Fields.end());
	const auto LightBinding = GetMaterialBinding(LightData);
	const auto HeavyBinding = GetMaterialBinding(HeavyData);
	ASSERT_LT(MaskField->CompactIndex,
		HeavyBinding.CompiledTextureFallbacks.size());
	EXPECT_EQ(LightBinding.CompiledTextureFallbacks[MaskField->CompactIndex],
		EMaterialTextureFallback::Black);
	EXPECT_EQ(HeavyBinding.CompiledTextureFallbacks[MaskField->CompactIndex],
		EMaterialTextureFallback::White);
	MarkAsGarbage(HeavyRust); MarkAsGarbage(LightRust); MarkAsGarbage(Root);
	CollectGarbage();
}

TEST(FDefaultMaterialServiceTests, LoadsAndRetainsOneNeutralAuthoredProxy)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	const Durin::FAssetCatalogRefreshResult Refresh =
		Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Refresh) << (Refresh.Errors.empty()
		? "Asset catalog refresh failed without a diagnostic."
		: Refresh.Errors.front().Message);
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
	const auto Binding = GetMaterialBinding(Resolved);
	EXPECT_EQ(Binding.BaseColor, Durin::FVector4f(0.5f, 0.5f, 0.5f, 1.0f));
	EXPECT_EQ(Binding.Normal, Durin::FVector3f(0.0f, 0.0f, 1.0f));
	EXPECT_FLOAT_EQ(Binding.Metallic, 0.0f);
	EXPECT_FLOAT_EQ(Binding.Roughness, 0.5f);
	EXPECT_FLOAT_EQ(Binding.AmbientOcclusion, 1.0f);
	EXPECT_EQ(Binding.Emissive, Durin::FVector3f(0.0f));
	EXPECT_FALSE(Resolved.Representation.IsError());
	EXPECT_EQ(
		Resolved.PlanningPassIdentity.ShaderMap.ShadingModel,
		Durin::EMaterialShadingModel::Lit);
	EXPECT_FALSE(Resolved.PlanningPassIdentity.bTwoSided);

	const auto CookRoots = Durin::GetEngineBuiltInCookRoots();
	ASSERT_EQ(CookRoots.size(), 1u);
	EXPECT_EQ(CookRoots[0], Durin::DefaultMaterialPackagePath);

	Durin::ShutdownDefaultMaterialService();
	EXPECT_FALSE(Durin::IsDefaultMaterialServiceAvailable());
	EXPECT_FALSE(Durin::GetDefaultMaterialRenderProxy());
	First = {};
	Second = {};
	Durin::FPackagePath DefaultPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::DefaultMaterialPackagePath, DefaultPath));
	ASSERT_TRUE(Durin::UnloadPackage(DefaultPath));
	Durin::CollectGarbage();
	WaitForRenderingThread();
	if (bOwnsRenderingThread) Durin::ShutdownRenderingThread();
}

TEST(FDefaultMaterialServiceTests, MissingEngineContentSelectsErrorTerminal)
{
	InitializeDObjectSystem();
	Durin::ShutdownDefaultMaterialService();
	Durin::FPackagePath DefaultPath;
	if (Durin::FPackagePath::TryCreate(
			Durin::DefaultMaterialPackagePath, DefaultPath))
	{
		Durin::UnloadPackage(DefaultPath);
	}
	Durin::CollectGarbage();
	Durin::ResetMaterialFallbackDiagnosticsForTests();
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("MissingDefaultMaterial");
	const std::array Definitions{
		Durin::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::EMountOwner::Engine,
			.Root = Root,
			.bAutoScan = true,
			.bContentWritable = false}};
	Durin::Testing::FScopedMountRegistryFixture Registry(Definitions);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	EXPECT_FALSE(Durin::InitializeDefaultMaterialService());
	EXPECT_FALSE(Durin::GetDefaultMaterialRenderProxy());
	EXPECT_EQ(
		Durin::GetMaterialFallbackDiagnosticsSnapshot().Get(
			Durin::EMaterialFallbackReason::DefaultAssetUnavailable),
		1u);
	const Durin::FMaterialRenderData& Error =
		Durin::GetErrorMaterialRenderData();
	EXPECT_TRUE(Error.Representation.IsError());
	const auto Binding = GetMaterialBinding(Error);
	EXPECT_EQ(Binding.BaseColor, Durin::FVector4f(1.0f, 0.0f, 1.0f, 1.0f));
	Durin::ShutdownDefaultMaterialService();
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
		MakeRenderFixture();
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
	EXPECT_EQ(Representation.GetUniformPayload().size(), 48u);
}

TEST(FMaterialRenderRepresentationTests, RejectsUnsupportedLayoutAndMalformedPayloads)
{
	const auto Fallback = MakeRenderFixture();

	{
		Durin::FMaterialRenderRepresentationInput Input;
		Input.Layout = Fallback.GetLayout();
		Input.Layout.Identity.Version = 1;
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
			Durin::EMaterialRenderValidationFailure::InvalidField);
	}

	{
		Durin::FMaterialRenderRepresentationInput Input;
		Input.Layout = Fallback.GetLayout();
		Input.UniformPayload.assign(
			Fallback.GetUniformPayload().begin(), Fallback.GetUniformPayload().end());
		const float NaN = std::numeric_limits<float>::quiet_NaN();
		std::memcpy(Input.UniformPayload.data() + 16, &NaN, sizeof(NaN));
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
		Input.UniformPayload[28] = std::byte{1};
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
	using namespace Durin;
	FMaterialRenderRepresentationBuilder Builder(MakeRenderFixture());
	ASSERT_TRUE(Builder.SetVector(FGuid{1,0,0,1}, FVector3(0.2, 0.4, 0.6)));
	ASSERT_TRUE(Builder.SetScalar(FGuid{2,0,0,1}, 0.35f));
	FMaterialRenderRepresentation Representation;
	FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Builder.Build(Representation, Diagnostic));
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 16), 0.2f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 20), 0.4f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 24), 0.6f);
	EXPECT_FLOAT_EQ(ReadFloat(Representation.GetUniformPayload(), 32), 0.35f);
	FMaterialRenderBinding Binding;
	ASSERT_TRUE(TryGetMaterialRenderBinding(Representation, Binding, Diagnostic));
	EXPECT_EQ(Binding.CompiledUniformPayload.size(), 48u);
	EXPECT_TRUE(Binding.CompiledTextures.empty());
}

TEST(FMaterialRenderRepresentationTests, MaterialSnapshotsResolveThroughTheSelectedLayout)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial("RepresentationBase");
	Durin::DMaterialInstance* Instance =
		Durin::NewObject<Durin::DMaterialInstance>(nullptr, "RepresentationInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.15, 0.25, 0.35)));
	ASSERT_TRUE(Base->SetScalarParameterValue(
		Durin::MaterialParameters::OpacityName(), 0.45f));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::RoughnessName(), 0.25f));

	const Durin::FMaterialRenderData RenderData = Instance->GetRenderData();
	const auto Binding = GetMaterialBinding(RenderData);
	EXPECT_FLOAT_EQ(Binding.BaseColor.r, 0.15f);
	EXPECT_FLOAT_EQ(Binding.BaseColor.g, 0.25f);
	EXPECT_FLOAT_EQ(Binding.BaseColor.b, 0.35f);
	EXPECT_FLOAT_EQ(Binding.BaseColor.a, 0.45f);
	EXPECT_FLOAT_EQ(Binding.Roughness, 0.25f);
	EXPECT_FALSE(RenderData.Representation.IsError());
	using Role = Durin::MaterialParameters::EMaterialBuiltinParameterRole;
	using Kind = Durin::MaterialParameters::EMaterialBuiltinParameterKind;
	const auto BaseId = Durin::MaterialParameters::GetBuiltinParameterId(
		Role::BaseColor, Kind::Value);
	EXPECT_FLOAT_EQ(ReadParameterFloat(RenderData, BaseId, 0), 0.15f);
	EXPECT_FLOAT_EQ(ReadParameterFloat(RenderData, BaseId, 1), 0.25f);
	EXPECT_FLOAT_EQ(ReadParameterFloat(RenderData, BaseId, 2), 0.35f);
	EXPECT_FLOAT_EQ(ReadParameterFloat(RenderData,
		Durin::MaterialParameters::GetBuiltinParameterId(Role::Opacity, Kind::Value)), 0.45f);
	EXPECT_FLOAT_EQ(ReadParameterFloat(RenderData,
		Durin::MaterialParameters::GetBuiltinParameterId(Role::Roughness, Kind::Value)), 0.25f);
}

TEST(FMaterialRenderRepresentationTests, CompilationPreservesAuthoredInputsAndRejectsNonFinitePayloads)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = MakeExpandedMaterial("CanonicalPBRMaterial");
	ASSERT_TRUE(Material->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(2.0, -1.0, 0.0)));
	ASSERT_TRUE(Material->SetVectorParameterValue(Durin::MaterialParameters::NormalName(), Durin::FVector3(0.0)));
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::MaterialParameters::MetallicName(), 2.0f));
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::MaterialParameters::RoughnessName(), 2.5f));
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::MaterialParameters::AmbientOcclusionName(), -1.0f));
	ASSERT_TRUE(Material->SetVectorParameterValue(Durin::MaterialParameters::EmissiveName(), Durin::FVector3(100.0, -2.0, 4.0)));
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::FName("BaseColorUVChannel"), 2.6f));
	ASSERT_TRUE(Material->SetVector2ParameterValue(Durin::FName("BaseColorUVScale"), Durin::FVector2(2.0, -3.0)));
	ASSERT_TRUE(Material->SetVector2ParameterValue(Durin::FName("BaseColorUVOffset"), Durin::FVector2(2048.0, -2048.0)));
	Durin::DTexture2D* WrongUsageTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "WrongNormalUsage");
	ASSERT_TRUE(Material->SetTextureParameterValue(Durin::MaterialParameters::NormalTextureName(), WrongUsageTexture));

	const auto Binding = GetMaterialBinding(Material->GetRenderData());
	EXPECT_EQ(Binding.BaseColor, Durin::FVector4f(2.0f, -1.0f, 0.0f, 1.0f));
	EXPECT_EQ(Binding.Normal, Durin::FVector3f(0.0f));
	EXPECT_FLOAT_EQ(Binding.Metallic, 2.0f);
	EXPECT_FLOAT_EQ(Binding.Roughness, 2.5f);
	EXPECT_FLOAT_EQ(Binding.AmbientOcclusion, -1.0f);
	EXPECT_EQ(Binding.Emissive, Durin::FVector3f(100.0f, -2.0f, 4.0f));
	EXPECT_FLOAT_EQ(Binding.UVChannels[0], 2.6f);
	EXPECT_EQ(Binding.UVScales[0], Durin::FVector2f(2.0f, -3.0f));
	EXPECT_EQ(Binding.UVOffsets[0], Durin::FVector2f(2048.0f, -2048.0f));
	EXPECT_EQ(Binding.Textures[1], WrongUsageTexture->GetTextureReferenceRHI());

	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::MaterialParameters::RoughnessName(),
		std::numeric_limits<float>::quiet_NaN()));
	EXPECT_FLOAT_EQ(GetMaterialBinding(Material->GetRenderData()).Roughness, 2.5f);
	EXPECT_FALSE(Material->GetRenderData().Representation.IsError());

	Durin::MarkAsGarbage(WrongUsageTexture);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialRenderRepresentationTests, CompiledLayoutPreservesTypedValuesAndSampling)
{
	using namespace Durin;
	const FGuid UV = FGuid::NewGuid(), Tint = FGuid::NewGuid(), Texture = FGuid::NewGuid();
	const std::array<FMaterialCompilerParameterDeclaration, 3> Parameters{{
		{UV, EMaterialParameterType::Vector2}, {Tint, EMaterialParameterType::Vector4},
		{Texture, EMaterialParameterType::Texture}}};
	const auto Compiled = CompileMaterialLayout(Parameters);
	ASSERT_TRUE(Compiled);
	FMaterialRenderRepresentationBuilder Builder(Compiled.Layout);
	ASSERT_TRUE(Builder.SetVector2(UV, FVector2(2.0, 3.0)));
	ASSERT_TRUE(Builder.SetVector4(Tint, FVector4(0.1, 0.2, 0.3, 0.4)));
	FMaterialSamplerState Sampling;
	Sampling.AddressU = EMaterialSamplerAddressMode::ClampToEdge;
	ASSERT_TRUE(Builder.SetTexture(Texture, {}, Sampling, EMaterialTextureFallback::FlatRGNormal));
	FMaterialRenderRepresentation Representation;
	FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Builder.Build(Representation, Diagnostic));
	FMaterialRenderBinding Binding;
	ASSERT_TRUE(TryGetMaterialRenderBinding(Representation, Binding, Diagnostic));
	EXPECT_EQ(Binding.LayoutIdentity, Compiled.Layout.Identity);
	ASSERT_EQ(Binding.CompiledSamplers.size(), 1u);
	EXPECT_EQ(Binding.CompiledSamplers[0], Sampling);
	EXPECT_EQ(Binding.CompiledTextureFallbacks[0], EMaterialTextureFallback::FlatRGNormal);
	for (const auto& Field : Compiled.Layout.Fields)
	{
		if (Field.ParameterId == UV)
		{
			EXPECT_FLOAT_EQ(ReadFloat(Binding.CompiledUniformPayload, Field.Offset), 2.0f);
			EXPECT_FLOAT_EQ(ReadFloat(Binding.CompiledUniformPayload, Field.Offset + 4), 3.0f);
			EXPECT_FLOAT_EQ(ReadFloat(Binding.CompiledUniformPayload, Field.Offset + 8), 0.0f);
		}
		if (Field.ParameterId == Tint)
			EXPECT_FLOAT_EQ(ReadFloat(Binding.CompiledUniformPayload, Field.Offset + 12), 0.4f);
	}
	FMaterialRenderRepresentationInput Input;
	Input.Layout = Compiled.Layout;
	Input.UniformPayload = Binding.CompiledUniformPayload;
	Input.Resources = Binding.CompiledTextures;
	Input.Samplers = Binding.CompiledSamplers;
	Input.TextureFallbacks = Binding.CompiledTextureFallbacks;
	Input.UniformPayload[0] = std::byte{1};
	EXPECT_FALSE(FMaterialRenderRepresentation::TryCreate(Input, Representation, Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, EMaterialRenderValidationFailure::NonZeroPadding);
	Input.UniformPayload[0] = std::byte{0};
	Input.Samplers[0].AddressU = static_cast<EMaterialSamplerAddressMode>(255);
	EXPECT_FALSE(FMaterialRenderRepresentation::TryCreate(Input, Representation, Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, EMaterialRenderValidationFailure::InvalidResource);
	const auto Empty = CompileMaterialLayout({});
	ASSERT_TRUE(Empty);
	FMaterialRenderRepresentationBuilder EmptyBuilder(Empty.Layout);
	ASSERT_TRUE(EmptyBuilder.Build(Representation, Diagnostic));
	EXPECT_TRUE(Representation.GetResources().empty());
	EXPECT_EQ(Representation.GetUniformPayload().size(), MaterialUniformControlBytes);
}

TEST(FMaterialRenderRepresentationTests, TextureSamplingOverridesChangePayloadWithoutRecompiling)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Root = NewObject<DMaterial>(nullptr, "CompiledSamplingRoot");
	auto* Child = NewObject<DMaterialInstance>(nullptr, "CompiledSamplingChild");
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid(); Definition.Name = FName("IndependentTexture");
	Definition.DisplayName = "Independent Texture"; Definition.Type = EMaterialParameterType::Texture;
	Definition.Value = FMaterialParameterValue::MakeTexture(nullptr);
	Definition.Value.GetTexture().SamplerState.AddressU = EMaterialSamplerAddressMode::ClampToEdge;
	Definition.Value.GetTexture().TextureFallback = EMaterialTextureFallback::FlatRGNormal;
	Testing::FTestMaterialExpressionGraph Graph;
	const std::array Definitions{Definition};
	const auto Add = [&](EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		std::vector<FMaterialExpressionInput> Inputs = {}, FGuid Id = {}) {
		return Testing::MakeLink(Graph.Add(Opcode, Type, std::move(Inputs), Id, {}, Definitions));
	};
	const auto Texture = Add(EMaterialProgramOpcode::TextureParameter, EMaterialProgramValueType::Texture2D, {}, Definition.Id);
	const auto UV = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2);
	const auto Sample = Add(EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4, {Texture, UV});
	Graph.Outputs.BaseColor = Add(EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float3, {Sample});
	ASSERT_TRUE(Graph.Apply(*Root));
	ASSERT_TRUE(FinishMaterialCompileForTest(*Root));
	ASSERT_TRUE(Child->SetParent(Root));
	const auto Accepted = Root->GetAcceptedCompiledProgram();
	ASSERT_NE(Accepted, nullptr);
	ASSERT_EQ(Accepted->Layout.Identity.Version, CompiledMaterialRenderLayoutVersion);
	auto Binding = GetMaterialBinding(Child->GetRenderData());
	ASSERT_EQ(Binding.CompiledSamplers.size(), 1u);
	EXPECT_EQ(Binding.CompiledSamplers[0], Definition.Value.GetTexture().SamplerState);
	EXPECT_EQ(Binding.CompiledTextureFallbacks[0], EMaterialTextureFallback::FlatRGNormal);
	auto Value = Definition.Value;
	Value.GetTexture().SamplerState.AddressV = EMaterialSamplerAddressMode::MirroredRepeat;
	Value.GetTexture().TextureFallback = EMaterialTextureFallback::Black;
	ASSERT_TRUE(Child->SetParameterValue(Definition.Id, Value));
	Binding = GetMaterialBinding(Child->GetRenderData());
	EXPECT_EQ(Binding.CompiledSamplers[0], Value.GetTexture().SamplerState);
	EXPECT_EQ(Binding.CompiledTextureFallbacks[0], EMaterialTextureFallback::Black);
	EXPECT_EQ(Child->GetRenderData().CompiledProgram, Accepted);
	ASSERT_TRUE(Root->SetParameterValue(Definition.Id, Value));
	EXPECT_EQ(Root->GetAcceptedCompiledProgram(), Accepted);
	EXPECT_EQ(Root->GetRenderData().PlanningPassIdentity.ShaderMap.ProgramIdentity, Accepted->Identity);
	ASSERT_TRUE(Child->ClearParameterValue(Definition.Id));
	Binding = GetMaterialBinding(Child->GetRenderData());
	EXPECT_EQ(Binding.CompiledSamplers[0], Value.GetTexture().SamplerState);
	EXPECT_EQ(Binding.CompiledTextureFallbacks[0], EMaterialTextureFallback::Black);
	MarkAsGarbage(Child); MarkAsGarbage(Root); CollectGarbage();
}
