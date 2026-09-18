#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "Components/PropertyEditValidation.h"
#include "DObject/PackagePersistence.h"
#include "Materials/MaterialObjectValidation.h"
#include "ExplicitMaterialProgramTestFixture.h"
#include "MaterialTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "Asset/OfflinePreparation.h"
#include "NativeAssetTestSupport.h"

TEST(FMaterialInstanceTests, TypedValueAlternativesAndReferenceRewriting)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto Scalar = FMaterialParameterValue::MakeScalar(.5f);
	auto Vector2 = FMaterialParameterValue::MakeVector2(FVector2(1, 2));
	auto Vector3 = FMaterialParameterValue::MakeVector(FVector3(1, 2, 3));
	auto Vector4 = FMaterialParameterValue::MakeVector4(FVector4(1, 2, 3, 4));
	EXPECT_EQ(Scalar.GetType(), EMaterialParameterType::Scalar);
	EXPECT_EQ(Vector2.GetType(), EMaterialParameterType::Vector2);
	EXPECT_EQ(Vector3.GetType(), EMaterialParameterType::Vector);
	EXPECT_EQ(Vector4.GetType(), EMaterialParameterType::Vector4);
	Vector2.GetVector2().y = 5;
	EXPECT_EQ(Vector2, FMaterialParameterValue::MakeVector2(FVector2(1, 5)));
	EXPECT_NE(Vector3, Vector4);
	auto* Original = NewObject<DTexture2D>(nullptr, "TypedValueOriginal");
	auto* Replacement = NewObject<DTexture2D>(nullptr, "TypedValueReplacement");
	auto Texture = FMaterialParameterValue::MakeTexture(Original, {}, EMaterialTextureFallback::Black);
	class FRewrite final : public FReferenceCollector
	{
	public:
		DObject* Replacement = nullptr;
		uint32 Visits = 0;
		auto AddReferencedObject(DObject*& Object) -> void override { ++Visits; Object = Replacement; }
	} Collector;
	Collector.Replacement = Replacement;
	Scalar.AddReferencedObjects(Collector);
	Vector2.AddReferencedObjects(Collector);
	Vector3.AddReferencedObjects(Collector);
	Vector4.AddReferencedObjects(Collector);
	EXPECT_EQ(Collector.Visits, 0u);
	Texture.AddReferencedObjects(Collector);
	EXPECT_EQ(Collector.Visits, 1u);
	EXPECT_EQ(Texture.GetTexture().Texture.Get(), Replacement);
	EXPECT_EQ(Texture.GetTexture().TextureFallback, EMaterialTextureFallback::Black);
	Collector.Replacement = nullptr;
	Texture.AddReferencedObjects(Collector);
	EXPECT_EQ(Texture.GetTexture().Texture.Get(), nullptr);
	MarkAsGarbage(Original);
	MarkAsGarbage(Replacement);
	CollectGarbage();
}

TEST(FMaterialInstanceTests, VectorStorageHasOnlyFourComponentValues)
{
	using namespace Durin;
	FMaterialVectorParameterValue Record;
	Record.SetValue(FMaterialParameterValue::MakeVector4(FVector4(1, 2, 3, 4)));
	EXPECT_EQ(Record.Value, FVector4f(1, 2, 3, 4));
	EXPECT_EQ(Record.GetValue(), FMaterialParameterValue::MakeVector4(FVector4(1, 2, 3, 4)));
	EXPECT_FALSE(FMaterialVectorParameterValue::SupportsType(EMaterialParameterType::Vector2));
	EXPECT_FALSE(FMaterialVectorParameterValue::SupportsType(EMaterialParameterType::Vector));
}

TEST(FMaterialInstanceTests, TypedOverrideArraysRoundTripOrphansAndRejectCrossTypeDuplicates)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("TypedOverrides");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/TypedOverrides/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TypedOverrides/Instance", Path));
	DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Instance));
	EXPECT_EQ(Instance->GetClass()->FindPropertyByName("ParameterOverrides"), nullptr);
	uint32 Index = 0;
	for (const auto Type : {EMaterialParameterType::Scalar, EMaterialParameterType::Vector4, EMaterialParameterType::Texture})
	{
		ASSERT_TRUE(VisitMaterialParameterValueType(Type, [&]<typename TRecord>() {
			auto* Property = Instance->GetClass()->FindPropertyByName(TRecord::PropertyName());
			if (!Property) return false;
			auto* Records = Property->template ContainerPtrToValuePtr<std::vector<TRecord>>(Instance);
			TRecord Record;
			Record.ParameterId = {0x47ddc368, 1, 2, ++Index};
			if constexpr (std::is_same_v<TRecord, FMaterialScalarParameterValue>) Record.Value = .75f;
			else if constexpr (std::is_same_v<TRecord, FMaterialTextureParameterValue>)
			{
				Record.Value.SamplerState.AddressU = EMaterialSamplerAddressMode::ClampToEdge;
				Record.Value.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
			}
			else
			{
				Record.Value = FVector4f(.25f);
			}
			Records->push_back(Record);
			return true;
		}));
	}
	ASSERT_TRUE(SavePackage(Instance->GetPackage()));
	auto Capture = [](const DMaterialInstance& Value) {
		std::vector<std::pair<FGuid, FMaterialParameterValue>> Result;
		Value.VisitLocalParameterValues([&](const FGuid& Id, const FMaterialParameterValue& Local) { Result.emplace_back(Id, Local); });
		return Result;
	};
	const auto Before = Capture(*Instance);
	ASSERT_EQ(Before.size(), 3u);
	for (const auto& [Id, Value] : Before) EXPECT_TRUE(Instance->IsParameterValueOrphan(Id));
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage();
	Instance = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Instance));
	EXPECT_EQ(Capture(*Instance), Before);
	auto* Property = Instance->GetClass()->FindPropertyByName("VectorParameterValues");
	auto* Records = Property->ContainerPtrToValuePtr<std::vector<FMaterialVectorParameterValue>>(Instance);
	EXPECT_EQ(FMaterialVectorParameterValue::StaticStruct()->FindPropertyByName("ParameterType"), nullptr);
	const auto Id = Records->front().ParameterId;
	Records->front().ParameterId = Before.front().first;
	FByteBuffer Rejected;
	EXPECT_FALSE(SerializeAssetPackageBytes(Instance->GetPackage(), Rejected));
	FPropertyEditProposal Proposal;
	Proposal.MemberProperty = Property;
	Proposal.DraftRootProperty = Property;
	Proposal.DraftRootContainer = Instance;
	const auto Edit = Instance->PreEditChangeProperty(Proposal);
	EXPECT_EQ(Edit.Error.Code, EObjectValidationError::PropertyRejected);
	EXPECT_EQ(Edit.Error.PropertyName, "VectorParameterValues");
	const auto EditCause = std::dynamic_pointer_cast<const FEnginePropertyEditCause>(Edit.Error.Cause);
	ASSERT_TRUE(EditCause);
	const auto* MaterialError = std::get_if<FMaterialError>(&EditCause->Error);
	ASSERT_NE(MaterialError, nullptr);
	EXPECT_EQ(MaterialError->Code, FMaterialError::FCode(EMaterialInstanceError::DuplicateParameterId));
	EXPECT_EQ(MaterialError->ParameterId, Before.front().first);
	ObjectPackage::FLinkerTables Linker;
	const auto Captured = FSavePackageContext{}.Capture(Instance->GetPackage(), Linker);
	ASSERT_FALSE(Captured);
	Records->front().ParameterId = Id;
	ASSERT_TRUE(SavePackage(Instance->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage();
}

namespace
{
	auto MakeExpandedMaterial(Durin::DObject* Outer, const char* Name)
		-> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(Outer, Name);
		if (!Material || !Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material)) return nullptr;
		if (!FinishMaterialCompileForTest(*Material)) return nullptr;
		return Material;
	}
}

TEST(FMaterialInstanceTests, BoundMaterialAndParentChangesUpdateProxyInPlace)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "LiveBaseMaterial");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LiveMaterialInstance");
	EXPECT_TRUE(Instance->SetParent(Base));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("LiveMaterialComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Instance);
	Component->RegisterComponent();
	const FSceneSnapshot Initial = CaptureScene(Harness.Scene);

	const uint64 VersionBefore = Base->GetRenderStateVersion();
	Base->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	const FSceneSnapshot ParentChanged = CaptureScene(Harness.Scene);
	EXPECT_EQ(ParentChanged.Proxy, Initial.Proxy);
	EXPECT_GT(Base->GetRenderStateVersion(), VersionBefore);
	EXPECT_EQ(ParentChanged.ComponentRevision, Initial.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(ParentChanged.Material).BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 1.0f));

	const uint64 NoOpVersion = Base->GetRenderStateVersion();
	Base->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	EXPECT_EQ(Base->GetRenderStateVersion(), NoOpVersion);
	Instance->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.7, 0.6));
	const uint64 InstanceNoOpVersion = Instance->GetRenderStateVersion();
	EXPECT_TRUE(Instance->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.7, 0.6)));
	EXPECT_EQ(Instance->GetRenderStateVersion(), InstanceNoOpVersion);
	Instance->ClearVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName());
	Base->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.9, 0.1, 0.3));
	const FSceneSnapshot Final = CaptureScene(Harness.Scene);
	EXPECT_EQ(Final.Proxy, Initial.Proxy);
	ExpectColorNear(GetMaterialBinding(Final.Material).BaseColor, Durin::FVector4f(0.9f, 0.1f, 0.3f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Base->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.5f);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialInstanceTests, InstanceStaticOverridesNeverReuseIncompatibleParentCode)
{
	FRenderSceneHarness Harness;
	auto* Base = MakeExpandedMaterial(nullptr, "StaticPermutationBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
		nullptr, "StaticPermutationInstance");
	ASSERT_NE(Base, nullptr);
	ASSERT_TRUE(Instance->SetParent(Base));
	const auto ParentProgram = Base->GetAcceptedCompiledProgram();
	ASSERT_NE(ParentProgram, nullptr);
	auto* Component = Harness.CreateStaticMeshComponent("StaticPermutationComponent");
	Component->SetStaticMesh(Durin::DStaticMesh::CreateDebugTriangle());
	Component->SetMaterial(Instance);
	Component->RegisterComponent();

	auto PipelineOnly = Base->GetRenderableStaticProperties();
	PipelineOnly.bTwoSided = true;
	ASSERT_TRUE(Instance->SetPropertyOverrides({true, true, true, true, true, PipelineOnly}));
	EXPECT_EQ(Instance->GetAcceptedCompiledProgram(), ParentProgram);
	EXPECT_FALSE(Instance->GetRenderData().Representation.IsError());
	EXPECT_FALSE(CaptureScene(Harness.Scene).Material.Representation.IsError());

	auto Incompatible = PipelineOnly;
	Incompatible.BlendMode = Durin::EMaterialBlendMode::Masked;
	ASSERT_TRUE(Instance->SetPropertyOverrides({true, true, true, true, true, Incompatible}));
	ASSERT_TRUE(Instance->GetAcceptedCompiledProgram());
	EXPECT_NE(Instance->GetAcceptedCompiledProgram()->Identity, ParentProgram->Identity);
	EXPECT_FALSE(Instance->GetRenderData().Representation.IsError());
	const auto Scene = CaptureScene(Harness.Scene);
	EXPECT_FALSE(Scene.Material.Representation.IsError());
	EXPECT_EQ(Scene.Material.PlanningPassIdentity.ShaderMap.BlendMode, Durin::EMaterialBlendMode::Masked);

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialInstanceTests, PerFieldPropertiesPreserveIntentAndResolveSourcesAcrossParents)
{
	InitializeDObjectSystem();
	auto* Root = MakeExpandedMaterial(nullptr, "PropertyRoot");
	ASSERT_NE(Root, nullptr);
	auto* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PropertyParent");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PropertyChild");
	ASSERT_TRUE(Parent->SetParent(Root));
	ASSERT_TRUE(Child->SetParent(Parent));
	Durin::FMaterialPropertyOverrides ParentOverrides;
	ParentOverrides.bOverrideTwoSided = true;
	ParentOverrides.Values.bTwoSided = true;
	ASSERT_TRUE(Parent->SetPropertyOverrides(ParentOverrides));
	Durin::FMaterialPropertyOverrides ChildOverrides;
	ChildOverrides.bOverrideOpacityMaskThreshold = true;
	ChildOverrides.Values.OpacityMaskThreshold = 0.75f;
	ChildOverrides.bOverrideBlendMode = true;
	ASSERT_TRUE(Child->SetPropertyOverrides(ChildOverrides));
	Durin::FResolvedMaterialProperties Resolved;
	Durin::FMaterialOperationResult Error;
	ASSERT_TRUE((Error = Durin::ResolveMaterialProperties(*Child, Resolved))) << Durin::FormatMaterialError(Error.Error);
	EXPECT_TRUE(Resolved.Properties.bTwoSided);
	EXPECT_FLOAT_EQ(Resolved.Properties.OpacityMaskThreshold, 0.75f);
	EXPECT_FLOAT_EQ(Resolved.ShaderProperties.OpacityMaskThreshold, 0.333f);
	EXPECT_EQ(Durin::ResolveObjectKey(Resolved.Sources[0]), Child);
	EXPECT_EQ(Durin::ResolveObjectKey(Resolved.Sources[3]), Parent);
	EXPECT_EQ(Durin::ResolveObjectKey(Resolved.Sources[1]), Root);
	const auto Generation = Root->GetMaterialCompileStatus().RequestGeneration;
	auto RootProperties = Root->GetStaticProperties();
	RootProperties.OpacityMaskThreshold = 0.25f;
	ASSERT_TRUE(Root->SetStaticProperties(RootProperties));
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, Generation);
	EXPECT_EQ(Child->GetAcceptedCompiledProgram(), Root->GetAcceptedCompiledProgram());
	ChildOverrides.Values.BlendMode = Durin::EMaterialBlendMode::Masked;
	ASSERT_TRUE(Child->SetPropertyOverrides(ChildOverrides));
	EXPECT_FLOAT_EQ(Child->GetStaticProperties().OpacityMaskThreshold, 0.75f);
	ASSERT_TRUE(Child->GetAcceptedCompiledProgram());
	EXPECT_NE(Child->GetAcceptedCompiledProgram()->Identity, Root->GetAcceptedCompiledProgram()->Identity);
	ChildOverrides.bOverrideBlendMode = false;
	ChildOverrides.bOverrideOpacityMaskThreshold = false;
	ASSERT_TRUE(Child->SetPropertyOverrides(ChildOverrides));
	EXPECT_FLOAT_EQ(Child->GetStaticProperties().OpacityMaskThreshold, 0.25f);
	EXPECT_FLOAT_EQ(Child->GetPropertyOverrides().Values.OpacityMaskThreshold, 0.75f);
	EXPECT_EQ(Child->GetStaticProperties().BlendMode, Durin::EMaterialBlendMode::Opaque);
	ASSERT_TRUE(Child->SetParent(Root));
	EXPECT_FALSE(Child->GetStaticProperties().bTwoSided);
	auto* Duplicate = Durin::Cast<Durin::DMaterialInstance>(Durin::DuplicateObject(Child, nullptr, "PropertyDuplicate").Object);
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetPropertyOverrides(), ChildOverrides);
	Durin::MarkAsGarbage(Duplicate);
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Root);
	Durin::CollectGarbage();
}

TEST(FMaterialInstanceTests, PropertyResolutionRejectsDepthOverflowAndCorruptCycles)
{
	using namespace Durin;
	InitializeDObjectSystem();
	// Parent-depth validation does not require parameters or compiled shaders.
	FScopedOfflinePreparation Offline;
	auto* Root = NewObject<DMaterial>(nullptr, "BoundedPropertyRoot");
	ASSERT_NE(Root, nullptr);
	std::vector<DMaterialInstance*> Chain;
	DMaterialInterface* Previous = Root;
	for (uint32 Index = 1; Index < MaterialMaximumParentDepth; ++Index)
	{
		auto* Child = NewObject<DMaterialInstance>(nullptr, FName(std::format("BoundedProperty{}", Index)));
		ASSERT_TRUE(Child->SetParent(Previous));
		Chain.push_back(Child);
		Previous = Child;
	}
	FResolvedMaterialProperties Resolved;
	Durin::FMaterialOperationResult Error;
	ASSERT_TRUE((Error = ResolveMaterialProperties(*Previous, Resolved))) << Durin::FormatMaterialError(Error.Error);
	auto* Overflow = NewObject<DMaterialInstance>(nullptr, "PropertyOverflow");
	EXPECT_FALSE(Overflow->SetParent(Previous));
	auto* ParentProperty = Chain.front()->GetClass()->FindPropertyByName("Parent");
	ASSERT_NE(ParentProperty, nullptr);
	*ParentProperty->ContainerPtrToValuePtr<TObjectPtr<DMaterialInterface>>(Chain.front()) = Chain.back();
	EXPECT_FALSE((Error = ResolveMaterialProperties(*Previous, Resolved)));
	EXPECT_EQ(Error.Error.Code, FMaterialError::FCode(EMaterialPropertyError::ParentCycleOrDepthExceeded));
	*ParentProperty->ContainerPtrToValuePtr<TObjectPtr<DMaterialInterface>>(Chain.front()) = Root;
	for (auto* Child : Chain) MarkAsGarbage(Child);
	MarkAsGarbage(Overflow);
	MarkAsGarbage(Root);
	CollectGarbage();
}

TEST(FMaterialInstanceTests, PerFieldOverridesRoundTrip)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Directory = Testing::GetTestWorkDirectory() / "PropertyOverridePackages";
	Testing::RemoveTestWorkDirectory(Directory);
	Testing::RegisterMountPointForTests("/PropertyOverridePackages/", Directory.generic_string() + "/");
	FPackagePath RootPath, InstancePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/PropertyOverridePackages/Root", RootPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/PropertyOverridePackages/Instance", InstancePath));
	DMaterial* Root = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(RootPath, Root));
	ASSERT_TRUE(SavePackage(Root->GetPackage()));
	DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(InstancePath, Instance));
	ASSERT_TRUE(Instance->SetParent(Root));
	FMaterialPropertyOverrides Overrides;
	Overrides.bOverrideBlendMode = true;
	Overrides.bOverrideOpacityMaskThreshold = true;
	Overrides.Values.OpacityMaskThreshold = 0.75f;
	ASSERT_TRUE(Instance->SetPropertyOverrides(Overrides));
	ASSERT_TRUE(SavePackage(Instance->GetPackage()));
	FByteBuffer CurrentBytes;
	ASSERT_TRUE(SerializeAssetPackageBytes(Instance->GetPackage(), CurrentBytes));
	EXPECT_FALSE(ContainsSerializedField(CurrentBytes, InstancePath, "bOverrideStaticProperties"));
	EXPECT_FALSE(ContainsSerializedField(CurrentBytes, InstancePath, "StaticPropertiesOverride"));
	ASSERT_TRUE(UnloadPackage(InstancePath));
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(InstancePath), Instance));
	EXPECT_EQ(Instance->GetPropertyOverrides(), Overrides);
	ASSERT_TRUE(UnloadPackage(InstancePath));
	ASSERT_TRUE(UnloadPackage(RootPath));
}

TEST(FMaterialInstanceTests, PositionalOverrideTransfersAcrossMeshSwitch)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Default = MakeExpandedMaterial(nullptr, "BoundMeshDefault");
	Durin::DMaterial* Orphan = MakeExpandedMaterial(nullptr, "DetachedOrphanOverride");
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Slots = static_cast<Durin::FArrayProperty*>(Mesh->GetClass()->FindPropertyByName("MaterialSlots"));
	static_cast<Durin::FMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 0))->DefaultMaterial = Default;
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("DefaultDependencyComponent");
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();
	const FSceneSnapshot Initial = CaptureScene(Harness.Scene);
	Default->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	const FSceneSnapshot DefaultChanged = CaptureScene(Harness.Scene);
	EXPECT_EQ(DefaultChanged.Proxy, Initial.Proxy);
	EXPECT_EQ(DefaultChanged.ComponentRevision, Initial.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(DefaultChanged.Material).BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 1.0f));

	ASSERT_TRUE(Component->SetMaterial(0, Orphan));
	Durin::DStaticMesh* OtherMesh = Durin::DStaticMesh::CreateDebugTriangle();
	Component->SetStaticMesh(OtherMesh);
	const FSceneSnapshot BeforeOverrideChange = CaptureScene(Harness.Scene);
	EXPECT_EQ(Component->GetMaterial(0), Orphan);
	Orphan->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.1, 0.3));
	const FSceneSnapshot AfterOverrideChange = CaptureScene(Harness.Scene);
	EXPECT_EQ(AfterOverrideChange.Proxy, BeforeOverrideChange.Proxy);
	EXPECT_EQ(AfterOverrideChange.ComponentRevision, BeforeOverrideChange.ComponentRevision);
	ExpectColorNear(
		GetMaterialBinding(AfterOverrideChange.Material).BaseColor,
		Durin::FVector4f(0.8f, 0.1f, 0.3f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(OtherMesh);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Orphan);
	Durin::MarkAsGarbage(Default);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialInstanceTests, BoundTextureChangesUpdateProxyResourceSnapshotInPlace)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "LiveTextureBaseMaterial");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LiveTextureMaterialInstance");
	Durin::DTexture2D* BaseTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "LiveBaseColorTexture");
	Durin::DTexture2D* OverrideTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "LiveOverrideColorTexture");
	Base->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), BaseTexture);
	ASSERT_TRUE(Instance->SetParent(Base));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("LiveTextureMaterialComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Instance);
	Component->RegisterComponent();
	FSceneSnapshot Initial = CaptureScene(Harness.Scene);
	EXPECT_EQ(GetMaterialBinding(Initial.Material).Textures[0], BaseTexture->GetTextureReferenceRHI());

	Instance->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), OverrideTexture);
	FSceneSnapshot Overridden = CaptureScene(Harness.Scene);
	EXPECT_EQ(Overridden.Proxy, Initial.Proxy);
	EXPECT_EQ(Overridden.ComponentRevision, Initial.ComponentRevision);
	EXPECT_EQ(GetMaterialBinding(Overridden.Material).Textures[0], OverrideTexture->GetTextureReferenceRHI());

	EXPECT_TRUE(Instance->ClearTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName()));
	FSceneSnapshot Inherited = CaptureScene(Harness.Scene);
	EXPECT_EQ(Inherited.Proxy, Initial.Proxy);
	EXPECT_EQ(GetMaterialBinding(Inherited.Material).Textures[0], BaseTexture->GetTextureReferenceRHI());
	// Test snapshots cross back to the game thread, so release their proxy owners while each asset still owns its resource.
	Initial.Material = {};
	Overridden.Material = {};
	Inherited.Material = {};

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(OverrideTexture);
	Durin::MarkAsGarbage(BaseTexture);
	Harness.Shutdown();
	Durin::CollectGarbage();
	// DTexture2D destruction enqueues the final release; briefly restart the worker to drain it in render-thread context.
	Durin::InitRenderingThread();
	WaitForRenderingThread();
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialInstanceTests, SceneCommandsPreserveLatestTransformAndReleaseAllProxies)
{
	FRenderSceneHarness Harness;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("SceneCommandComponent");
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();
	Component->SetWorldLocation(Durin::FVector3(4.0, 5.0, 6.0));
	const FSceneSnapshot Updated = CaptureScene(Harness.Scene);
	EXPECT_EQ(Updated.ProxyCount, 1);
	EXPECT_NEAR(Updated.Transform[3][0], 4.0, 1.e-6);
	EXPECT_NEAR(Updated.Transform[3][1], 5.0, 1.e-6);
	EXPECT_NEAR(Updated.Transform[3][2], 6.0, 1.e-6);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	EXPECT_EQ(CaptureScene(Harness.Scene).ProxyCount, 0);
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);

	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialInstanceTests, InstancesInheritOverrideAndRejectParentCycles)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "BaseMaterial");
	Durin::FMaterialStaticProperties OpacityProperties = Base->GetStaticProperties();
	OpacityProperties.BlendMode = Durin::EMaterialBlendMode::Translucent;
	ASSERT_TRUE(Base->SetStaticProperties(OpacityProperties));
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FirstInstance");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "SecondInstance");

	Base->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3));
	ASSERT_TRUE(First->SetParent(Base));
	ASSERT_TRUE(Second->SetParent(First));
	ExpectColorNear(GetMaterialBinding(Second->GetRenderData()).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));

	First->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.4f);
	Second->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.7, 0.6));
	ExpectColorNear(GetMaterialBinding(Second->GetRenderData()).BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 0.4f));
	EXPECT_FALSE(First->SetParent(Second));
	EXPECT_EQ(First->GetParent(), Base);

	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialInstanceTests, RenderLayerResolvesMixedOverridesAndRefreshesEachBuild)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Root = MakeExpandedMaterial(nullptr, "BatchResolutionRoot");
	Durin::FMaterialStaticProperties OpacityProperties = Root->GetStaticProperties();
	OpacityProperties.BlendMode = Durin::EMaterialBlendMode::Translucent;
	ASSERT_TRUE(Root->SetStaticProperties(OpacityProperties));
	auto* Parent = NewObject<DMaterialInstance>(nullptr, "BatchResolutionParent");
	auto* Child = NewObject<DMaterialInstance>(nullptr, "BatchResolutionChild");
	ASSERT_TRUE(Parent->SetParent(Root));
	ASSERT_TRUE(Child->SetParent(Parent));
	ASSERT_TRUE(Parent->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), FVector3(.2, .4, .6)));
	ASSERT_TRUE(Parent->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), .7f));
	ASSERT_TRUE(Child->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), .3f));
	const auto ColorId = Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(
		Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value;
	// Simulate a stored override whose declaration changed type, plus an orphan.
	auto* Property = Child->GetClass()->FindPropertyByName("ScalarParameterValues");
	ASSERT_NE(Property, nullptr);
	auto& Records = *Property->ContainerPtrToValuePtr<std::vector<FMaterialScalarParameterValue>>(Child);
	FMaterialScalarParameterValue Stale;
	Stale.ParameterId = ColorId;
	Stale.Value = .9f;
	Records.push_back(Stale);
	Stale.ParameterId = FGuid{0x12345678, 0x11223344, 0x55667788, 0x99aabbcc};
	Records.push_back(Stale);
	const auto CheckLayer = [&](float Opacity, const FVector3& Color) {
		const auto Data = Child->GetRenderData();
		ASSERT_NE(Data.CompiledProgram, nullptr);
		ASSERT_FALSE(Data.Representation.IsError());
		ExpectColorNear(GetMaterialBinding(Data).BaseColor,
			FVector4f(static_cast<float>(Color.x), static_cast<float>(Color.y), static_cast<float>(Color.z), Opacity));
		EXPECT_EQ(std::ranges::find(Data.CompiledProgram->ActiveParameters, Stale.ParameterId,
			&FMaterialCompilerParameterDeclaration::Id), Data.CompiledProgram->ActiveParameters.end());
	};
	CheckLayer(.3f, FVector3(FVector3f(.2f, .4f, .6f)));
	ASSERT_TRUE(Child->ClearScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName()));
	CheckLayer(.7f, FVector3(FVector3f(.2f, .4f, .6f)));
	ASSERT_TRUE(Parent->ClearVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName()));
	CheckLayer(.7f, FVector3(Root->FindParameterDefinition(ColorId)->Value.GetVector4()));
	MarkAsGarbage(Child);
	MarkAsGarbage(Parent);
	MarkAsGarbage(Root);
	CollectGarbage();
}

TEST(FMaterialInstanceTests, MultiLevelResolutionReportsSupplyingSourceAndCurrentOverrideState)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "ResolutionBase");
	Durin::DMaterialInstance* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "ResolutionParent");
	Durin::DMaterialInstance* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "ResolutionChild");
	ASSERT_TRUE(Parent->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Parent));

	Durin::FResolvedMaterialParameter Resolved;
	ASSERT_TRUE(Child->ResolveParameterValue(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value, Resolved));
	EXPECT_EQ(Resolved.Source, Base);
	EXPECT_FALSE(Resolved.bHasLocalOverride);
	EXPECT_FLOAT_EQ(Resolved.Value.GetScalar(), 1.0f);

	ASSERT_TRUE(Parent->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.6f));
	ASSERT_TRUE(Child->ResolveParameterValue(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value, Resolved));
	EXPECT_EQ(Resolved.Source, Parent);
	EXPECT_FALSE(Resolved.bHasLocalOverride);
	EXPECT_FLOAT_EQ(Resolved.Value.GetScalar(), 0.6f);

	ASSERT_TRUE(Child->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.25f));
	ASSERT_TRUE(Child->ResolveParameterValue(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value, Resolved));
	EXPECT_EQ(Resolved.Source, Child);
	EXPECT_TRUE(Resolved.bHasLocalOverride);
	EXPECT_FLOAT_EQ(Resolved.Value.GetScalar(), 0.25f);

	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialInstanceTests, ParentRemovalPreservesOrphansAndExcludesThemFromRendering)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "OrphanBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "OrphanInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3)));
	ASSERT_TRUE(Instance->HasLocalParameterValue(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value));
	EXPECT_FALSE(Instance->IsParameterValueOrphan(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value));

	ASSERT_TRUE(Instance->SetParent(nullptr));
	ASSERT_EQ(Instance->GetLocalParameterValueCount(), 1u);
	EXPECT_TRUE(Instance->HasLocalParameterValue(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value));
	EXPECT_TRUE(Instance->IsParameterValueOrphan(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value));
	Durin::FResolvedMaterialParameter Resolved;
	EXPECT_FALSE(Instance->ResolveParameterValue(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value, Resolved));
	EXPECT_TRUE(Instance->GetRenderData().Representation.IsError());

	ASSERT_TRUE(Instance->SetParent(Base));
	EXPECT_FALSE(Instance->IsParameterValueOrphan(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value));
	ExpectColorNear(GetMaterialBinding(Instance->GetRenderData()).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ASSERT_TRUE(Instance->ClearParameterValue(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value));
	EXPECT_TRUE(Instance->GetLocalParameterValueCount() == 0);

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialInstanceTests, GuidOverrideRejectsUnknownAndPreservesVersionOnNoOp)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "GuidOverrideBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "GuidOverrideInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	const Durin::FGuid Unknown{1, 2, 3, 4};
	const uint64 InitialVersion = Instance->GetRenderStateVersion();
	const auto MissingName = Instance->SetScalarParameterValue("UnknownParameter", .5f);
	EXPECT_FALSE(MissingName);
	EXPECT_EQ(std::get<Durin::EMaterialParameterError>(MissingName.Error.Code), Durin::EMaterialParameterError::NotFound);
	EXPECT_EQ(MissingName.Error.ParameterName, "UnknownParameter");
	const auto WrongNameType = Instance->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), .5f);
	EXPECT_FALSE(WrongNameType);
	EXPECT_EQ(std::get<Durin::EMaterialParameterError>(WrongNameType.Error.Code), Durin::EMaterialParameterError::InvalidType);
	EXPECT_EQ(WrongNameType.Error.ExpectedParameterType, Durin::EMaterialParameterType::Vector4);
	EXPECT_EQ(WrongNameType.Error.ActualParameterType, Durin::EMaterialParameterType::Scalar);

	const auto Missing = Instance->SetParameterValue(Unknown, Durin::FMaterialParameterValue::MakeScalar(0.5f));
	EXPECT_FALSE(Missing);
	EXPECT_EQ(std::get<Durin::EMaterialParameterError>(Missing.Error.Code), Durin::EMaterialParameterError::NotFound);
	EXPECT_EQ(Missing.Error.ParameterId, Unknown);
	const auto WrongType = Instance->SetParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value,
		Durin::FMaterialParameterValue::MakeScalar(0.5f));
	EXPECT_FALSE(WrongType);
	EXPECT_EQ(std::get<Durin::EMaterialParameterError>(WrongType.Error.Code), Durin::EMaterialParameterError::InvalidType);
	EXPECT_EQ(WrongType.Error.ExpectedParameterType, Durin::EMaterialParameterType::Vector4);
	EXPECT_EQ(WrongType.Error.ActualParameterType, Durin::EMaterialParameterType::Scalar);
	EXPECT_EQ(Instance->GetRenderStateVersion(), InitialVersion);
	EXPECT_TRUE(Instance->GetLocalParameterValueCount() == 0);

	ASSERT_TRUE(Instance->SetParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value,
		Durin::FMaterialParameterValue::MakeScalar(0.5f)));
	const uint64 OverriddenVersion = Instance->GetRenderStateVersion();
	Durin::FMaterialParameterValue SameActiveValue = Durin::FMaterialParameterValue::MakeScalar(0.5f);
	EXPECT_EQ(SameActiveValue.GetType(), Durin::EMaterialParameterType::Scalar);
	ASSERT_TRUE(Instance->SetParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value,
		SameActiveValue));
	EXPECT_EQ(Instance->GetRenderStateVersion(), OverriddenVersion);
	EXPECT_EQ(Instance->GetLocalParameterValueCount(), 1u);
	EXPECT_EQ(WrongType.Error.ExpectedParameterType, Durin::EMaterialParameterType::Vector4);
	EXPECT_EQ(Missing.Error.ParameterId, Unknown);

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialInstanceTests, InstanceOverrideStateTracksSetAndClear)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "OverrideStateBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "OverrideStateInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	EXPECT_FALSE(Instance->HasLocalScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName()));
	EXPECT_FALSE(Instance->HasLocalVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName()));
	EXPECT_FALSE(Instance->HasLocalTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName()));

	Instance->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.5f);
	Instance->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	Instance->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), nullptr);
	EXPECT_TRUE(Instance->HasLocalScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName()));
	EXPECT_TRUE(Instance->HasLocalVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName()));
	EXPECT_TRUE(Instance->HasLocalTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName()));

	EXPECT_TRUE(Instance->ClearScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName()));
	EXPECT_TRUE(Instance->ClearVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName()));
	EXPECT_TRUE(Instance->ClearTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName()));
	EXPECT_FALSE(Instance->HasLocalScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName()));
	EXPECT_FALSE(Instance->HasLocalVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName()));
	EXPECT_FALSE(Instance->HasLocalTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName()));

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialInstanceTests, TextureParametersInheritOverrideAndPreserveExplicitNull)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "TextureBaseMaterial");
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FirstTextureInstance");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "SecondTextureInstance");
	Durin::DTexture2D* BaseTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "InheritedBaseColorTexture");
	Durin::DTexture2D* OverrideTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "OverriddenBaseColorTexture");

	Base->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), BaseTexture);
	ASSERT_TRUE(First->SetParent(Base));
	ASSERT_TRUE(Second->SetParent(First));
	EXPECT_EQ(GetMaterialBinding(Second->GetRenderData()).Textures[0], BaseTexture->GetTextureReferenceRHI());

	First->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), OverrideTexture);
	EXPECT_EQ(GetMaterialBinding(Second->GetRenderData()).Textures[0], OverrideTexture->GetTextureReferenceRHI());
	Second->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), nullptr);
	EXPECT_EQ(GetMaterialBinding(Second->GetRenderData()).Textures[0], nullptr);
	EXPECT_TRUE(Second->ClearTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName()));
	EXPECT_EQ(GetMaterialBinding(Second->GetRenderData()).Textures[0], OverrideTexture->GetTextureReferenceRHI());
	EXPECT_TRUE(First->ClearTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName()));
	EXPECT_EQ(GetMaterialBinding(Second->GetRenderData()).Textures[0], BaseTexture->GetTextureReferenceRHI());

	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(OverrideTexture);
	Durin::MarkAsGarbage(BaseTexture);
	Durin::CollectGarbage();
	WaitForRenderingThread();
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialInstanceTests, ReflectedTextureParameterKeepsTextureReachable)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::DMaterial* Material = MakeExpandedMaterial(nullptr, "RootedTextureMaterial");
	Durin::DTexture2D* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "ReferencedMaterialTexture");
	Material->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), Texture);
	Durin::AddToRoot(Material);

	Durin::CollectGarbage();
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Material));
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Texture));

	Durin::RemoveFromRoot(Material);
	Durin::CollectGarbage();
	WaitForRenderingThread();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Material));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Texture));
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialInstanceTests, ReflectedInstanceOverrideKeepsNestedTextureReachable)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "RootedOverrideBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "RootedOverrideInstance");
	Durin::DTexture2D* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "ReferencedOverrideTexture");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), Texture));
	Durin::AddToRoot(Instance);

	Durin::CollectGarbage();
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Instance));
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Base));
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Texture));

	Durin::RemoveFromRoot(Instance);
	Durin::CollectGarbage();
	WaitForRenderingThread();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Instance));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Base));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Texture));
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialInstanceTests, DuplicateInstancePreservesParentAndNestedTextureOverride)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial(nullptr, "DuplicateOverrideBase");
	Durin::DMaterialInstance* Source = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "DuplicateOverrideSource");
	Durin::DTexture2D* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "DuplicateOverrideTexture");
	ASSERT_TRUE(Source->SetParent(Base));
	ASSERT_TRUE(Source->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), Texture));

	auto* Duplicate = Durin::Cast<Durin::DMaterialInstance>(
		Durin::DuplicateObject(Source, nullptr, "DuplicateOverrideResult").Object);
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetParent(), Base);
	EXPECT_TRUE(Duplicate->HasLocalParameterValue(Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Texture));
	Durin::DTexture2D* DuplicateTexture = nullptr;
	ASSERT_TRUE(Duplicate->GetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), DuplicateTexture));
	EXPECT_EQ(DuplicateTexture, Texture);

	Durin::MarkAsGarbage(Duplicate);
	Durin::MarkAsGarbage(Source);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(Texture);
	Durin::CollectGarbage();
}
