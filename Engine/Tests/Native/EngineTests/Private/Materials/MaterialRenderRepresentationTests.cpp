#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "MaterialTestSupport.h"
#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Materials/MaterialTypes.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"

#include <cstring>
#include <limits>

namespace
{
	auto MakeExpandedMaterial(const char* Name) -> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, Name);
		if (!Material || !Material->SetMaterialProgram(
			Durin::MakeCanonicalMaterialProgram())) return nullptr;
		return Material;
	}

	auto ReadFloat(Durin::FByteView Bytes, uint32 Offset) -> float
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
		ErrorData.PlanningPassIdentity.ShaderMap.ShadingModel,
		Durin::EMaterialShadingModel::Unlit);
	EXPECT_TRUE(ErrorData.PlanningPassIdentity.bTwoSided);
	EXPECT_EQ(
		ErrorData.PlanningPassIdentity.DepthWritePolicy,
		Durin::EMaterialDepthWritePolicy::Enabled);
}

TEST(FMaterialProgramCharacterizationTests,
	FixedPathSeparatesDynamicShaderAndPipelineIdentity)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedMaterial("M5FixedPathBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
		nullptr, "M5FixedPathInstance");
	ASSERT_TRUE(Instance->SetParent(Base));

	const Durin::FMaterialRenderData Initial = Base->GetRenderData();
	EXPECT_EQ(
		Initial.PlanningPassIdentity.ShaderMap.RenderLayout,
		Durin::FMaterialRenderLayoutIdentity{});
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
	EXPECT_EQ(
		Error.Representation.GetLayout().Identity,
		Initial.Representation.GetLayout().Identity);
	EXPECT_EQ(Error.Representation.GetResources().size(), 8u);
	EXPECT_TRUE(std::ranges::all_of(
		Error.Representation.GetResources(),
		[](const auto& Resource) { return Resource == nullptr; }));

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
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
	const Durin::FMaterialRenderBinding Binding =
		GetMaterialBinding(Resolved);
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
	const Durin::FMaterialRenderBinding Binding = GetMaterialBinding(Error);
	EXPECT_EQ(Binding.BaseColor, Durin::FVector4f(1.0f, 0.0f, 1.0f, 1.0f));
	Durin::ShutdownDefaultMaterialService();
}

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
	auto Validation = Source->SetMaterialProgram(
		Durin::MakeStandardSurfaceMaterialProgram());
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
	EXPECT_TRUE(Cooked->GetMaterialProgram()->Nodes.empty());
	EXPECT_FALSE(Cooked->GetAcceptedCompiledProgram()->ActiveParameters.empty());
	Durin::DMaterialInstance* Instance = nullptr;
	Result = Durin::LoadObject(InstancePath, Instance);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Instance, nullptr);
	EXPECT_EQ(Instance->GetParent(), Cooked);
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
	EXPECT_FALSE(Instance->IsParameterOverrideOrphan(
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
		Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value,
		Durin::FVector3(0.2, 0.4, 0.6)));
	ASSERT_TRUE(Builder.SetScalar(
		Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value, 0.35f));
	ASSERT_TRUE(Builder.SetScalar(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Metallic).Value, 0.8f));
	ASSERT_TRUE(Builder.SetScalar(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Roughness).Value, 0.25f));
	ASSERT_TRUE(Builder.SetVector(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Normal).Value, Durin::FVector3(0.0, 0.0, 1.0)));
	ASSERT_TRUE(Builder.SetScalar(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).UVChannel, 3.0f));
	ASSERT_TRUE(Builder.SetScalar(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).UVRotation, 0.5f));
	Durin::FMaterialSamplerState Sampler;
	Sampler.MinFilter = Durin::EMaterialSamplerMinFilter::NearestMipmapLinear;
	Sampler.MagFilter = Durin::EMaterialSamplerMagFilter::Nearest;
	Sampler.AddressU = Durin::EMaterialSamplerAddressMode::MirroredRepeat;
	Sampler.AddressV = Durin::EMaterialSamplerAddressMode::ClampToEdge;
	ASSERT_TRUE(Builder.SetScalar(
		Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).SamplerState,
		Durin::EncodeMaterialSamplerState(Sampler)));
	ASSERT_TRUE(Builder.SetTexture(
		Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Texture,
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
	Durin::FMaterialRenderBinding Binding;
	ASSERT_TRUE(Durin::TryGetMaterialRenderBinding(
		Representation, Binding, Diagnostic)) << Diagnostic.Message;
	EXPECT_FLOAT_EQ(Binding.UVRotations[0], 0.5f);
	EXPECT_EQ(Binding.Samplers[0], Sampler);
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
	const Durin::FMaterialRenderBinding Binding = GetMaterialBinding(RenderData);
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
	Durin::DMaterial* Material = MakeExpandedMaterial("CanonicalPBRMaterial");
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

	const Durin::FMaterialRenderBinding Binding = GetMaterialBinding(Material->GetRenderData());
	EXPECT_EQ(Binding.BaseColor, Durin::FVector4f(0.5f, 0.5f, 0.5f, 1.0f));
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
	Definition.Value.SamplerState.AddressU = EMaterialSamplerAddressMode::ClampToEdge;
	Definition.Value.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
	FMaterialProgram Program;
	FMaterialProgramNode Texture, UV, Sample, Color;
	Texture.Id = FGuid::NewGuid(); Texture.Opcode = EMaterialProgramOpcode::TextureParameter;
	Texture.ResultType = EMaterialProgramValueType::Texture2D; Texture.ParameterId = Definition.Id;
	UV.Id = FGuid::NewGuid(); UV.ResultType = EMaterialProgramValueType::Float2;
	Sample.Id = FGuid::NewGuid(); Sample.Opcode = EMaterialProgramOpcode::TextureSample2D;
	Sample.ResultType = EMaterialProgramValueType::Float4; Sample.Inputs = {{Texture.Id, 0}, {UV.Id, 0}};
	Color.Id = FGuid::NewGuid(); Color.Opcode = EMaterialProgramOpcode::TruncateToFloat3;
	Color.ResultType = EMaterialProgramValueType::Float3; Color.Inputs = {{Sample.Id, 0}};
	Program.Nodes = {Texture, UV, Sample, Color}; Program.Outputs.BaseColor = {Color.Id, 0};
	ASSERT_TRUE(Root->SetMaterialDefinitionsAndProgram({Definition}, Program));
	ASSERT_TRUE(Child->SetParent(Root));
	const auto Accepted = Root->GetAcceptedCompiledProgram();
	ASSERT_NE(Accepted, nullptr);
	ASSERT_EQ(Accepted->Layout.Identity.Version, CompiledMaterialRenderLayoutVersion);
	auto Binding = GetMaterialBinding(Child->GetRenderData());
	ASSERT_EQ(Binding.CompiledSamplers.size(), 1u);
	EXPECT_EQ(Binding.CompiledSamplers[0], Definition.Value.SamplerState);
	EXPECT_EQ(Binding.CompiledTextureFallbacks[0], EMaterialTextureFallback::FlatRGNormal);
	auto Value = Definition.Value;
	Value.SamplerState.AddressV = EMaterialSamplerAddressMode::MirroredRepeat;
	Value.TextureFallback = EMaterialTextureFallback::Black;
	ASSERT_TRUE(Child->SetParameterOverride(Definition.Id, Definition.Type, Value));
	Binding = GetMaterialBinding(Child->GetRenderData());
	EXPECT_EQ(Binding.CompiledSamplers[0], Value.SamplerState);
	EXPECT_EQ(Binding.CompiledTextureFallbacks[0], EMaterialTextureFallback::Black);
	EXPECT_EQ(Child->GetRenderData().CompiledProgram, Accepted);
	ASSERT_TRUE(Root->SetParameterValue(Definition.Id, Value));
	EXPECT_EQ(Root->GetAcceptedCompiledProgram(), Accepted);
	EXPECT_EQ(Root->GetRenderData().PlanningPassIdentity.ShaderMap.ProgramIdentity, Accepted->Identity);
	ASSERT_TRUE(Child->ClearParameterOverride(Definition.Id));
	Binding = GetMaterialBinding(Child->GetRenderData());
	EXPECT_EQ(Binding.CompiledSamplers[0], Value.SamplerState);
	EXPECT_EQ(Binding.CompiledTextureFallbacks[0], EMaterialTextureFallback::Black);
	MarkAsGarbage(Child); MarkAsGarbage(Root); CollectGarbage();
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
	Texture.Type = EMaterialParameterType::Texture; Texture.Value.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
	Texture.Value.SamplerState.AddressU = EMaterialSamplerAddressMode::ClampToEdge;
	FMaterialProgram Program;
	auto Add = [&](EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type, FGuid Id = {},
		std::vector<FMaterialProgramLink> Inputs = {}) {
		FMaterialProgramNode Node;
		Node.Id = FGuid::NewGuid(); Node.Opcode = Opcode; Node.ResultType = Type;
		Node.ParameterId = Id; Node.Inputs = std::move(Inputs); Program.Nodes.push_back(Node);
		return FMaterialProgramLink{Node.Id, 0};
	};
	const auto TintNode = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float4, Tint.Id);
	Program.Outputs.BaseColor = Add(EMaterialProgramOpcode::TruncateToFloat3, EMaterialProgramValueType::Float3, {}, {TintNode});
	const auto TextureNode = Add(EMaterialProgramOpcode::TextureParameter, EMaterialProgramValueType::Texture2D, Texture.Id);
	const auto UV = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2);
	const auto Sample = Add(EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4, {}, {TextureNode, UV});
	Program.Outputs.Emissive = Add(EMaterialProgramOpcode::TruncateToFloat3, EMaterialProgramValueType::Float3, {}, {Sample});
	ASSERT_TRUE(Source->SetMaterialDefinitionsAndProgram({Tint, Texture}, Program));
	ASSERT_NE(Source->GetAcceptedCompiledProgram(), nullptr);
	const auto ExpectedLayout = Source->GetAcceptedCompiledProgram()->Layout;
	auto* Instance = NewObject<DMaterialInstance>(Source->GetPackage(), "CustomCookedOverrides");
	ASSERT_TRUE(Instance->SetParent(Source));
	ASSERT_TRUE(Instance->SetParameterOverride(Tint.Id, Tint.Type, FMaterialParameterValue::MakeVector4(FVector4(0.8, 0.3, 0.1, 1.0))));
	auto Sampling = Texture.Value;
	Sampling.SamplerState.AddressV = EMaterialSamplerAddressMode::MirroredRepeat;
	Sampling.TextureFallback = EMaterialTextureFallback::Black;
	ASSERT_TRUE(Instance->SetParameterOverride(Texture.Id, Texture.Type, Sampling));
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
		EXPECT_TRUE(Loaded->GetMaterialProgram()->Nodes.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->IR.Nodes.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GeneratedSource.empty());
		DMaterialInstance* LoadedInstance = nullptr;
		ASSERT_TRUE(LoadObject(InstancePath, LoadedInstance));
		ASSERT_NE(LoadedInstance, nullptr);
		EXPECT_FALSE(LoadedInstance->IsParameterOverrideOrphan(Texture.Id));
		const auto RootBinding = GetMaterialBinding(Loaded->GetRenderData());
		const auto InstanceBinding = GetMaterialBinding(LoadedInstance->GetRenderData());
		EXPECT_EQ(RootBinding.LayoutIdentity, ExpectedLayout.Identity);
		EXPECT_EQ(InstanceBinding.LayoutIdentity, ExpectedLayout.Identity);
		ASSERT_EQ(InstanceBinding.CompiledSamplers.size(), 1u);
		EXPECT_EQ(RootBinding.CompiledSamplers[0], Texture.Value.SamplerState);
		EXPECT_EQ(InstanceBinding.CompiledSamplers[0], Sampling.SamplerState);
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
