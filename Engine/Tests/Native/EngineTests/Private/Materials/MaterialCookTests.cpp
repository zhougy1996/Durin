#include "MaterialRenderRepresentationTestFixture.h"
#include "Materials/MaterialCustomVersion.h"

TEST(FDefaultMaterialCookTests, UnreferencedBuiltInRootPublishesAndLoadsCooked)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::DefaultMaterialPackagePath, Path));
	Durin::DMaterial* Source = nullptr;
	Durin::FAssetResult Result = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Source);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Source, nullptr);
	const Durin::FMaterialProgramIdentity ExpectedIdentity =
		Source->GetAcceptedCompiledProgram()->Identity;

	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::CreateTestFixtureDirectory("DefaultMaterialCook"));
	Durin::FCookContext Cook(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Source, Durin::DefaultMaterialPackagePath, Cook, Error)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Cook, CookRoot, &Error)) << Error;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dasset"));
	EXPECT_FALSE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dbulk"));
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	auto RuntimeConfiguration = Durin::FAssetRuntimeConfiguration::Authored();
	Result = Durin::FAssetRuntimeConfiguration::Cooked(
		CookRoot, RuntimeConfiguration);
	ASSERT_TRUE(Result) << Result.Message;
	Result = Durin::InitializeAssetManager(std::move(RuntimeConfiguration));
	ASSERT_TRUE(Result) << Result.Message;
	{
	const std::array CookMountDefinitions{
		Durin::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::EMountOwner::Test,
			.Root = CookRoot / "Engine",
			.bAutoScan = true}};
	Durin::Testing::FScopedMountRegistryFixture CookMounts(
		CookMountDefinitions);
	ASSERT_TRUE(CookMounts.IsValid()) << CookMounts.GetError();
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(CookRoot / "Engine/Materials/DefaultMaterial.dasset").generic_string(),
		Inspection));
	EXPECT_NE(Inspection.FindField("ProgramData"), nullptr);
	const auto CookedFile = CookRoot / "Engine/Materials/DefaultMaterial.dasset";
	Durin::FByteBuffer OriginalBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(OriginalBytes, CookedFile));
	Durin::ObjectPackage::FLinkerTables VersionLinker;
	ASSERT_TRUE(Durin::ObjectPackage::ReadPackage(OriginalBytes, {}, Path, VersionLinker));
	ASSERT_EQ(VersionLinker.CustomVersions, (std::vector<Durin::FCustomVersion>{
		{Durin::FMaterialGraphVersion::Guid, Durin::FMaterialGraphVersion::CurrentVersion}}));
	VersionLinker.CustomVersions.clear();
	Durin::FByteBuffer MissingVersionBytes, UnusedBulk;
	ASSERT_TRUE(Durin::ObjectPackage::WritePackage(VersionLinker, MissingVersionBytes, UnusedBulk));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(MissingVersionBytes, CookedFile));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(Durin::EAssetRegistryScanMode::FullValidation));
	Durin::DMaterial* Rejected = nullptr;
	const auto MissingVersion = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Rejected);
	EXPECT_EQ(MissingVersion.Error, Durin::EAssetError::UnsupportedVersion) << MissingVersion.Message;
	EXPECT_EQ(Rejected, nullptr);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(OriginalBytes, CookedFile));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::DMaterial* Cooked = nullptr;
	Result = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Cooked);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Cooked, nullptr);
	ASSERT_TRUE(Cooked->GetAcceptedCompiledProgram());
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram()->Identity, ExpectedIdentity);
	EXPECT_TRUE(Cooked->GetMaterialCompileStatus().IsCurrent());
	EXPECT_EQ(
		GetMaterialBinding(Cooked->GetRenderData()).BaseColor,
		Durin::FVector4f(0.5f, 0.5f, 0.5f, 1.0f));
	}

	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::InitializeAssetManager());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
}

TEST(FDefaultMaterialCookTests, ActiveParametersSurviveGraphStripping)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::DefaultMaterialPackagePath, Path));
	Durin::DMaterial* Source = nullptr;
	Durin::FAssetResult Result = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Source);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Source, nullptr);
	auto Validation = Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Source);
	ASSERT_TRUE(Validation);
	ASSERT_TRUE(Source->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.7)));
	auto* AuthoredInstance = Durin::NewObject<Durin::DMaterialInstance>(
		Source->GetPackage(), "CookedOverrides");
	ASSERT_TRUE(AuthoredInstance->SetParent(Source));
	ASSERT_TRUE(AuthoredInstance->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.3, 0.1)));
	Durin::FObjectPath InstancePath;
	ASSERT_TRUE(Durin::FObjectPath::TryCreate(AuthoredInstance->GetObjectPath(), InstancePath));
	const Durin::FMaterialProgramIdentity ExpectedIdentity =
		Source->GetAcceptedCompiledProgram()->Identity;
	std::array<Durin::FObjectPath, 3> VariantPaths;
	std::array<Durin::FMaterialProgramIdentity, 3> VariantIdentities;
	for (size_t Index = 0; Index < VariantPaths.size(); ++Index)
	{
		auto* Variant = Durin::NewObject<Durin::DMaterialInstance>(Source->GetPackage(),
			Durin::FName(std::format("CookedVariant{}", Index)));
		Durin::FMaterialPropertyOverrides Overrides;
		Overrides.bOverrideBlendMode = true;
		Overrides.bOverrideOpacityMaskThreshold = true;
		Overrides.Values.BlendMode = Index < 2 ? Durin::EMaterialBlendMode::Masked : Durin::EMaterialBlendMode::Translucent;
		Overrides.Values.OpacityMaskThreshold = Index == 0 ? 0.25f : 0.75f;
		ASSERT_TRUE(Variant->SetParentAndPropertyOverrides(AuthoredInstance, Overrides));
		ASSERT_TRUE(Variant->GetAcceptedCompiledProgram());
		VariantIdentities[Index] = Variant->GetAcceptedCompiledProgram()->Identity;
		ASSERT_TRUE(Durin::FObjectPath::TryCreate(Variant->GetObjectPath(), VariantPaths[Index]));
	}


	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::CreateTestFixtureDirectory("ActiveMaterialCook"));
	Durin::FCookContext Cook(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Source, Durin::DefaultMaterialPackagePath, Cook, Error)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Cook, CookRoot, &Error)) << Error;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dasset"));
	EXPECT_FALSE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dbulk"));
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	auto RuntimeConfiguration = Durin::FAssetRuntimeConfiguration::Authored();
	Result = Durin::FAssetRuntimeConfiguration::Cooked(
		CookRoot, RuntimeConfiguration);
	ASSERT_TRUE(Result) << Result.Message;
	Result = Durin::InitializeAssetManager(std::move(RuntimeConfiguration));
	ASSERT_TRUE(Result) << Result.Message;
	{
	const std::array CookMountDefinitions{
		Durin::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::EMountOwner::Test,
			.Root = CookRoot / "Engine",
			.bAutoScan = true}};
	Durin::Testing::FScopedMountRegistryFixture CookMounts(
		CookMountDefinitions);
	ASSERT_TRUE(CookMounts.IsValid()) << CookMounts.GetError();
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(CookRoot / "Engine/Materials/DefaultMaterial.dasset").generic_string(),
		Inspection));
	EXPECT_TRUE(std::ranges::any_of(Inspection.Objects, [](const auto& Object) {
		return Object.FindField("ProgramData") != nullptr;
	}));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::DMaterial* Cooked = nullptr;
	Result = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Cooked);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Cooked, nullptr);
	ASSERT_TRUE(Cooked->GetAcceptedCompiledProgram());
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram()->Identity, ExpectedIdentity);
	EXPECT_TRUE(Cooked->GetMaterialCompileStatus().IsCurrent());
	EXPECT_EQ(
		GetMaterialBinding(Cooked->GetRenderData()).BaseColor,
		Durin::FVector4f(0.2f, 0.4f, 0.7f, 1.0f));
	EXPECT_TRUE(std::ranges::none_of(Inspection.Objects, [](const auto& Object) {
		return Object.FindField("Program") != nullptr;
	}));
	EXPECT_TRUE(Cooked->GetExpressionCollection().Expressions.empty());
	EXPECT_FALSE(Cooked->GetAcceptedCompiledProgram()->ActiveParameters.empty());
	Durin::DMaterialInstance* Instance = nullptr;
	Result = Durin::LoadObject(InstancePath, Instance);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Instance, nullptr);
	EXPECT_EQ(Instance->GetParent(), Cooked);
	for (size_t Index = 0; Index < VariantPaths.size(); ++Index)
	{
		Durin::DMaterialInstance* Variant = nullptr;
		ASSERT_TRUE(Durin::LoadObject(VariantPaths[Index], Variant));
		ASSERT_TRUE(Variant);
		ASSERT_GT(Variant->GetCookedProgramData().GetMetadata().LogicalSize, 0u);
		ASSERT_TRUE(Variant->GetAcceptedCompiledProgram());
		EXPECT_EQ(Variant->GetAcceptedCompiledProgram()->Identity, VariantIdentities[Index]);
		EXPECT_NE(Variant->GetAcceptedCompiledProgram()->Identity, ExpectedIdentity);
		EXPECT_TRUE(Variant->GetAcceptedCompiledProgram()->IR.Nodes.empty());
		EXPECT_TRUE(Variant->GetAcceptedCompiledProgram()->GeneratedSource.empty());
		EXPECT_FALSE(Variant->GetRenderData().Representation.IsError());
		ExpectColorNear(GetMaterialBinding(Variant->GetRenderData()).BaseColor,
			Durin::FVector4f(0.8f, 0.3f, 0.1f, 1.0f));
	}

	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CookedChild");
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "CookedDynamicTexture");
	ASSERT_TRUE(Texture->GetTextureReferenceRHI());
	ASSERT_TRUE(Child->SetParent(Instance));
	ASSERT_TRUE(Cooked->SetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), Texture));
	ASSERT_TRUE(Child->SetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), Texture));
	ASSERT_TRUE(Child->SetScalarParameterValue(
		Durin::MaterialParameters::RoughnessName(), 0.23f));
	EXPECT_FALSE(Instance->IsParameterValueOrphan(
		Durin::MaterialParameters::GetBuiltinParameterIds(
			Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value));
	const bool bOwnsRenderingThread = Durin::GetRenderCommandAdmissionState()
		== Durin::ERenderCommandAdmissionState::Stopped;
	if (bOwnsRenderingThread) Durin::InitRenderingThread();
	Durin::FMaterialRenderData BaseData, ChildData;
	auto BaseProxy = Cooked->GetMaterialRenderProxy();
	auto ChildProxy = Child->GetMaterialRenderProxy();
	struct FCaptureCookedBindingsCommand
	{
		static constexpr auto GetName() -> const char* { return "CaptureCookedBindings"; }
	};
	Durin::EnqueueRenderCommand<FCaptureCookedBindingsCommand>(
		[&](Durin::FRHICommandListImmediate&) {
			BaseData = BaseProxy->Resolve_RenderThread();
			ChildData = ChildProxy->Resolve_RenderThread();
		});
	WaitForRenderingThread();
	ExpectColorNear(GetMaterialBinding(BaseData).BaseColor,
		Durin::FVector4f(0.2f, 0.4f, 0.7f, 1.0f));
	ExpectColorNear(GetMaterialBinding(ChildData).BaseColor,
		Durin::FVector4f(0.8f, 0.3f, 0.1f, 1.0f));
	EXPECT_EQ(GetMaterialBinding(ChildData).Textures[0], Texture->GetTextureReferenceRHI());
	EXPECT_EQ(GetMaterialBinding(BaseData).Textures[0], Texture->GetTextureReferenceRHI());
	EXPECT_FLOAT_EQ(GetMaterialBinding(ChildData).Roughness, 0.23f);
	EXPECT_EQ(ChildData.Representation.GetUniformPayload().size(),
		Child->GetRenderData().Representation.GetUniformPayload().size());
	EXPECT_TRUE(std::ranges::equal(ChildData.Representation.GetUniformPayload(),
		Child->GetRenderData().Representation.GetUniformPayload()));
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(ChildProxy));
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(BaseProxy));
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Texture);
	Durin::CollectGarbage();
	WaitForRenderingThread();
	if (bOwnsRenderingThread) Durin::ShutdownRenderingThread();
	}

	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::InitializeAssetManager());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
}

TEST(FDefaultMaterialCookTests, CustomLayoutAndSamplingSurvivePackageCookAndGraphStripping)
{
	using namespace Durin;
	InitializeDObjectSystem();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate(DefaultMaterialPackagePath, Path));
	DMaterial* Source = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Source));
	ASSERT_NE(Source, nullptr);
	FMaterialParameterDefinition Tint, Texture;
	Tint.Id = FGuid::NewGuid(); Tint.Name = FName("CookedTint"); Tint.DisplayName = "Cooked Tint";
	Tint.Type = EMaterialParameterType::Vector4; Tint.Value = FMaterialParameterValue::MakeVector4(FVector4(0.2, 0.4, 0.7, 1.0));
	Texture.Id = FGuid::NewGuid(); Texture.Name = FName("CookedLayer"); Texture.DisplayName = "Cooked Layer";
	Texture.Type = EMaterialParameterType::Texture;
	Texture.Value = FMaterialParameterValue::MakeTexture(nullptr, {}, EMaterialTextureFallback::FlatRGNormal);
	Texture.Value.GetTexture().SamplerState.AddressU = EMaterialSamplerAddressMode::ClampToEdge;
	Testing::FTestMaterialExpressionGraph Graph;
	const std::array Definitions{Tint, Texture};
	auto Add = [&](EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type, FGuid Id = {},
		std::vector<FMaterialExpressionInput> Inputs = {}) {
		return Testing::MakeLink(Graph.Add(Opcode, Type, std::move(Inputs), Id, {}, Definitions));
	};
	const auto TintNode = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float4, Tint.Id);
	Graph.Outputs.BaseColor = Add(EMaterialProgramOpcode::TruncateToFloat3, EMaterialProgramValueType::Float3, {}, {TintNode});
	const auto TextureNode = Add(EMaterialProgramOpcode::TextureParameter, EMaterialProgramValueType::Texture2D, Texture.Id);
	const auto UV = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2);
	const auto Sample = Add(EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4, {}, {TextureNode, UV});
	Graph.Outputs.Emissive = Add(EMaterialProgramOpcode::TruncateToFloat3, EMaterialProgramValueType::Float3, {}, {Sample});
	ASSERT_TRUE(Graph.Apply(*Source));
	ASSERT_NE(Source->GetAcceptedCompiledProgram(), nullptr);
	const auto ExpectedLayout = Source->GetAcceptedCompiledProgram()->Layout;
	auto* Instance = NewObject<DMaterialInstance>(Source->GetPackage(), "CustomCookedOverrides");
	ASSERT_TRUE(Instance->SetParent(Source));
	ASSERT_TRUE(Instance->SetParameterValue(Tint.Id, FMaterialParameterValue::MakeVector4(FVector4(0.8, 0.3, 0.1, 1.0))));
	auto Sampling = Texture.Value;
	Sampling.GetTexture().SamplerState.AddressV = EMaterialSamplerAddressMode::MirroredRepeat;
	Sampling.GetTexture().TextureFallback = EMaterialTextureFallback::Black;
	ASSERT_TRUE(Instance->SetParameterValue(Texture.Id, Sampling));
	FObjectPath InstancePath;
	ASSERT_TRUE(FObjectPath::TryCreate(Instance->GetObjectPath(), InstancePath));
	const auto CookRoot = std::filesystem::absolute(Testing::CreateTestFixtureDirectory("CustomLayoutMaterialCook"));
	FCookContext Cook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(ContributeEngineCookAsset(*Source, DefaultMaterialPackagePath, Cook, Error)) << Error;
	ASSERT_TRUE(PublishCookContext(Cook, CookRoot, &Error)) << Error;
	ShutdownAssetManager(); CollectGarbage();
	auto Configuration = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(CookRoot, Configuration));
	ASSERT_TRUE(InitializeAssetManager(std::move(Configuration)));
	{
		const std::array Mounts{FMountPoint{.VirtualRoot = "/Engine/", .Owner = EMountOwner::Test,
			.Root = CookRoot / "Engine", .bAutoScan = true}};
		Testing::FScopedMountRegistryFixture CookMounts(Mounts);
		ASSERT_TRUE(CookMounts.IsValid());
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		DMaterial* Loaded = nullptr;
		ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded));
		ASSERT_NE(Loaded, nullptr);
		ASSERT_NE(Loaded->GetAcceptedCompiledProgram(), nullptr);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->Layout, ExpectedLayout);
		EXPECT_TRUE(Loaded->GetExpressionCollection().Expressions.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->IR.Nodes.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GeneratedSource.empty());
		DMaterialInstance* LoadedInstance = nullptr;
		ASSERT_TRUE(LoadObject(InstancePath, LoadedInstance));
		ASSERT_NE(LoadedInstance, nullptr);
		EXPECT_FALSE(LoadedInstance->IsParameterValueOrphan(Texture.Id));
		const auto RootBinding = GetMaterialBinding(Loaded->GetRenderData());
		const auto InstanceBinding = GetMaterialBinding(LoadedInstance->GetRenderData());
		EXPECT_EQ(RootBinding.LayoutIdentity, ExpectedLayout.Identity);
		EXPECT_EQ(InstanceBinding.LayoutIdentity, ExpectedLayout.Identity);
		ASSERT_EQ(InstanceBinding.CompiledSamplers.size(), 1u);
		EXPECT_EQ(RootBinding.CompiledSamplers[0], Texture.Value.GetTexture().SamplerState);
		EXPECT_EQ(InstanceBinding.CompiledSamplers[0], Sampling.GetTexture().SamplerState);
		EXPECT_EQ(InstanceBinding.CompiledTextureFallbacks[0], EMaterialTextureFallback::Black);
		for (const auto& Field : ExpectedLayout.Fields) if (Field.ParameterId == Tint.Id)
		{
			EXPECT_FLOAT_EQ(ReadFloat(RootBinding.CompiledUniformPayload, Field.Offset), 0.2f);
			EXPECT_FLOAT_EQ(ReadFloat(InstanceBinding.CompiledUniformPayload, Field.Offset), 0.8f);
		}
	}
	ShutdownAssetManager(); CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
}
