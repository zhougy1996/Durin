#include "MaterialResourceTestSupport.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionArithmetic.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionSwitch.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Misc/UnifiedError.h"
#include "NativePackageUnloadTestSupport.h"
#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "StaticMesh/StaticMeshTestEnvironment.h"
#include "StaticMeshMaterialTestFixture.h"
#include "Materials/MaterialCustomVersion.h"
#include "Asset/LoadErrors.h"
#include "Materials/MaterialFunction.h"

TEST(FMaterialPackageTests, MaterialInstanceAssetsRoundTripParentAndOverrides)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "Materials";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::Testing::RegisterMountPointForTests("/MaterialTests/", Root.generic_string() + "/");

	Durin::FPackagePath BasePath;
	Durin::FPackagePath InstancePath;
	Durin::FPackagePath TexturePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/MaterialTests/Base", BasePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/MaterialTests/Instance", InstancePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/MaterialTests/BaseColorTexture", TexturePath));

	const std::filesystem::path TextureSource =
		Durin::Testing::GetTestWorkDirectory() / "MaterialBaseColor.png";
	WriteMaterialTextureFixture(TextureSource);
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> TextureImport = Durin::AssetForge::Builtins::ImportTexture2DForTest(TextureSource.generic_string(), TexturePath.ToString());
	ASSERT_TRUE(TextureImport) << TextureImport.Message;
	ASSERT_NE(TextureImport.Asset, nullptr);

	Durin::DMaterial* Base = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(BasePath, Base));
	auto BaseGraph = Durin::Testing::MakePBRMaterialExpressionsForTest();
	auto& StaticBool = BaseGraph.Add(Durin::EMaterialProgramOpcode::StaticBool,
		Durin::EMaterialProgramValueType::StaticBool, {}, {}, {});
	auto* StaticDeclaration = Durin::Cast<Durin::DMaterialExpressionStaticBool>(&StaticBool);
	ASSERT_NE(StaticDeclaration, nullptr);
	StaticDeclaration->Name = Durin::FName("UsePackageDetail");
	StaticDeclaration->DefaultValue = false;
	const Durin::FGuid StaticBoolId = StaticDeclaration->DeclarationId;
	ASSERT_TRUE(BaseGraph.Apply(*Base));
	Base->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	Base->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::NormalName(), Durin::FVector3(0.0, 1.0, 1.0));
	Base->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::RoughnessName(), 0.7f);
	Durin::FMaterialStaticProperties StaticProperties;
	StaticProperties.BlendMode = Durin::EMaterialBlendMode::Masked;
	StaticProperties.ShadingModel = Durin::EMaterialShadingModel::Unlit;
	StaticProperties.bTwoSided = true;
	StaticProperties.DepthWritePolicy = Durin::EMaterialDepthWritePolicy::Enabled;
	StaticProperties.OpacityMaskThreshold = 0.4f;
	ASSERT_TRUE(Base->SetStaticProperties(StaticProperties));
	ASSERT_TRUE(Durin::SavePackage(Base->GetPackage()));

	Durin::DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(InstancePath, Instance));
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetStaticBoolOverrideByInfo({.Name = "UsePackageDetail"}, true, false));
	(void)Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(Instance->GetAcceptedCompiledProgram());
	ASSERT_EQ(Instance->GetAcceptedCompiledProgram()->StaticBools.size(), 1u);
	EXPECT_FALSE(Instance->GetAcceptedCompiledProgram()->StaticBools.front().Value);
	ASSERT_EQ(Instance->GetStaticBoolOverrides().size(), 1u);
	EXPECT_TRUE(Instance->GetStaticBoolOverrides().front().Value);
	ASSERT_TRUE(Instance->SetStaticBoolOverrideByInfo({.Name = "UsePackageDetail"}, true));
	(void)Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(Instance->GetAcceptedCompiledProgram());
	ASSERT_EQ(Instance->GetAcceptedCompiledProgram()->StaticBools.size(), 1u);
	EXPECT_EQ(Instance->GetAcceptedCompiledProgram()->StaticBools.front(),
		(Durin::FMaterialCompilerEnvironment::FStaticBoolValue{StaticBoolId, true}));
	auto* Dynamic = Durin::DMaterialInstance::CreateDynamic(
		Instance, nullptr, "StaticOverrideDynamic");
	ASSERT_NE(Dynamic, nullptr);
	EXPECT_FALSE(Dynamic->SetStaticBoolOverrideByInfo({.Name = "UsePackageDetail"}, false));
	Durin::MarkAsGarbage(Dynamic);
	ASSERT_TRUE(Instance->SetStaticBoolOverrideByInfo({.Name = "UsePackageDetail"}, true, false));
	Instance->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.35f);
	Instance->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::MetallicName(), 0.8f);
	Instance->SetScalarParameterValue(Durin::FName("BaseColorUVChannel"), 2.0f);
	Instance->SetVector2ParameterValue(Durin::FName("BaseColorUVScale"), Durin::FVector2(2.0, -1.0));
	Instance->SetVector2ParameterValue(Durin::FName("BaseColorUVOffset"), Durin::FVector2(0.25, 0.5));
	Instance->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), TextureImport.Asset);
	// Reproduce a source record written before overrides stored parameter info.
	auto* Property = Instance->GetClass()->FindPropertyByName("StaticBoolOverrides");
	ASSERT_NE(Property, nullptr);
	auto& Legacy = *Property->ContainerPtrToValuePtr<std::vector<Durin::FMaterialStaticBoolOverride>>(Instance);
	EXPECT_FALSE(Instance->ClearOrphanedStaticBoolOverride(StaticBoolId));
	Legacy.front().ParameterInfo = {};
	Legacy.front().bOverride = true;
	EXPECT_FALSE(Durin::SnapshotMaterialCompilerInput(*Instance, {.CompilerIdentity = "LegacySwitch"}));
	Legacy.front().bOverride = false;
	const auto OrphanId = Durin::FGuid::NewGuid();
	Legacy.push_back({OrphanId, false, {}, false});
	ASSERT_TRUE(Durin::SavePackage(Instance->GetPackage()));
	const Durin::FPackageCatalogEntry InstanceData =
		Durin::FindPackageExact(InstancePath);
	ASSERT_NE(InstanceData, nullptr);
	EXPECT_NE(std::ranges::find(InstanceData->Dependencies, BasePath), InstanceData->Dependencies.end());
	EXPECT_NE(std::ranges::find(InstanceData->Dependencies, TexturePath), InstanceData->Dependencies.end());
	ASSERT_FALSE(Instance->GetPackage()->IsDirty());
	const uint64 SavedVersion = Instance->GetRenderChangeCount();
	EXPECT_FALSE(Instance->SetScalarParameterValue(Durin::FName("UnknownParameter"), 0.2f));
	EXPECT_FALSE(Instance->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), Durin::FVector3(0.2)));
	EXPECT_FALSE(Instance->GetPackage()->IsDirty());
	EXPECT_EQ(Instance->GetRenderChangeCount(), SavedVersion);
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(InstancePath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(BasePath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(TexturePath));

	Durin::DMaterialInstance* Loaded = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = Durin::LoadObject<Durin::DMaterialInstance>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(InstancePath), nullptr, &LoadedValueError);
		Loaded = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Loaded->GetParent(), nullptr);
	ASSERT_EQ(Loaded->GetStaticBoolOverrides().size(), 2u);
	ASSERT_TRUE(Loaded->ClearOrphanedStaticBoolOverride(OrphanId));
	EXPECT_FALSE(Loaded->ClearOrphanedStaticBoolOverride(StaticBoolId));
	ASSERT_TRUE(Durin::SavePackage(Loaded->GetPackage()));
	EXPECT_EQ(Loaded->GetStaticBoolOverrides().front(),
		(Durin::FMaterialStaticBoolOverride{StaticBoolId, true, {.Name = "UsePackageDetail"}, false}));
	Durin::DTexture* LoadedTexture = nullptr;
	ASSERT_TRUE(Loaded->GetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), LoadedTexture));
	ASSERT_NE(LoadedTexture, nullptr);
	ASSERT_TRUE(LoadedTexture->FinishCachePlatformData(10.0));
	(void)Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(Loaded->GetStaticProperties(), StaticProperties);
	Durin::FVector3 RetainedBaseColor;
	ASSERT_TRUE(Loaded->GetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), RetainedBaseColor));
	ExpectColorNear(Durin::FVector4f(Durin::FVector3f(RetainedBaseColor), 1.f), Durin::FVector4f(.2f, .4f, .6f, 1.f));
	ExpectColorNear(GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Loaded)).BaseColor, Durin::FVector4f(0.f, 0.f, 0.f, 1.f));
	const auto LoadedBinding = GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Loaded));
	EXPECT_FLOAT_EQ(LoadedBinding.Metallic, 0.0f);
	float Retained = 0;
	ASSERT_TRUE(Loaded->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::MetallicName(), Retained));
	EXPECT_FLOAT_EQ(Retained, 0.8f);
	EXPECT_FLOAT_EQ(LoadedBinding.Roughness, 0.5f);
	ASSERT_TRUE(Loaded->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::RoughnessName(), Retained));
	EXPECT_FLOAT_EQ(Retained, 0.7f);
	EXPECT_FLOAT_EQ(LoadedBinding.UVChannels[0], 0.0f);
	EXPECT_EQ(LoadedBinding.UVScales[0], Durin::FVector2f(1.0f));
	EXPECT_EQ(LoadedBinding.UVOffsets[0], Durin::FVector2f(0.0f));
	ASSERT_TRUE(Loaded->GetScalarParameterValue(Durin::FName("BaseColorUVChannel"), Retained));
	EXPECT_FLOAT_EQ(Retained, 2.f);
	Durin::FVector2 RetainedUV;
	ASSERT_TRUE(Loaded->GetVector2ParameterValue(Durin::FName("BaseColorUVScale"), RetainedUV));
	EXPECT_EQ(RetainedUV, Durin::FVector2(2.0, -1.0));
	ASSERT_TRUE(Loaded->GetVector2ParameterValue(Durin::FName("BaseColorUVOffset"), RetainedUV));
	EXPECT_EQ(RetainedUV, Durin::FVector2(.25, .5));
	EXPECT_EQ(
		GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Loaded)).Textures[0],
		nullptr);
	auto* LoadedBase = Durin::Cast<Durin::DMaterial>(Loaded->GetParent());
	ASSERT_NE(LoadedBase, nullptr);
	LoadedBase->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.6, 0.4, 0.2));
	ASSERT_TRUE(Loaded->GetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), RetainedBaseColor));
	ExpectColorNear(Durin::FVector4f(Durin::FVector3f(RetainedBaseColor), 1.f), Durin::FVector4f(.6f, .4f, .2f, 1.f));
	ExpectColorNear(GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Loaded)).BaseColor, Durin::FVector4f(0.f, 0.f, 0.f, 1.f));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(InstancePath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(
		BasePath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(TexturePath));
}

TEST(FMaterialPackageTests, StaticOverrideRecoversItsNameOnReparentAndTracksRename)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Parent(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, NAME_None));
	auto Graph = Testing::MakePBRMaterialExpressionsForTest();
	auto& Switch = Graph.Add(EMaterialProgramOpcode::StaticBool, EMaterialProgramValueType::StaticBool, {}, {}, {});
	auto* Declaration = Cast<DMaterialExpressionStaticBool>(&Switch);
	ASSERT_NE(Declaration, nullptr);
	Declaration->Name = "Detail";
	ASSERT_TRUE(Graph.Apply(*Parent));
	auto* Property = Instance->GetClass()->FindPropertyByName("StaticBoolOverrides");
	ASSERT_NE(Property, nullptr);
	auto& Overrides = *Property->ContainerPtrToValuePtr<std::vector<FMaterialStaticBoolOverride>>(Instance.Get());
	Overrides.push_back({Declaration->DeclarationId, true, {}, true});
	ASSERT_TRUE(Instance->SetParent(Parent.Get()));
	ASSERT_EQ(Overrides.front().ParameterInfo.Name, Declaration->Name);
	Declaration->Name = "DetailEnabled";
	ASSERT_TRUE(Graph.Apply(*Parent));
	EXPECT_EQ(Overrides.front().ParameterInfo.Name, Declaration->Name);
	EXPECT_FALSE(Instance->ClearStaticBoolOverrideByInfo({.Name = "Detail"}));
	EXPECT_TRUE(Instance->ClearStaticBoolOverrideByInfo({.Name = "DetailEnabled"}));
}

TEST(FMaterialPackageTests, ReusingDeletedStaticSwitchNamePreservesSurvivingOverride)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Parent(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, NAME_None));
	auto Graph = Testing::MakePBRMaterialExpressionsForTest();
	const auto AddSwitch = [&](FName Name) {
		auto& Node = Graph.Add(EMaterialProgramOpcode::StaticBool, EMaterialProgramValueType::StaticBool, {}, {}, {});
		auto* Switch = Cast<DMaterialExpressionStaticBool>(&Node);
		if (Switch) Switch->Name = Name;
		return Switch;
	};
	auto* Survivor = AddSwitch("Detail");
	auto* Removed = AddSwitch("UseDetail");
	ASSERT_NE(Survivor, nullptr);
	ASSERT_NE(Removed, nullptr);
	const FGuid RemovedId = Removed->DeclarationId;
	ASSERT_TRUE(Graph.Apply(*Parent));
	ASSERT_TRUE(Instance->SetParent(Parent.Get()));
	ASSERT_TRUE(Instance->SetStaticBoolOverrideByInfo({.Name = Survivor->Name}, true));
	ASSERT_TRUE(Instance->SetStaticBoolOverrideByInfo({.Name = Removed->Name}, false));

	Graph.Expressions.pop_back();
	ASSERT_TRUE(Graph.Apply(*Parent));
	Survivor->Name = "UseDetail";
	ASSERT_TRUE(Graph.Apply(*Parent));
	const auto Overrides = Instance->GetStaticBoolOverrides();
	ASSERT_EQ(Overrides.size(), 2u);
	EXPECT_EQ(Overrides[0], (FMaterialStaticBoolOverride{Survivor->DeclarationId, true, {.Name = Survivor->Name}, true}));
	EXPECT_EQ(Overrides[1], (FMaterialStaticBoolOverride{RemovedId, false, {}, true}));
	EXPECT_FALSE(Instance->ClearOrphanedStaticBoolOverride(Survivor->DeclarationId));
	ASSERT_TRUE(Instance->ClearOrphanedStaticBoolOverride(RemovedId));
	ASSERT_EQ(Instance->GetStaticBoolOverrides().size(), 1u);
	EXPECT_TRUE(Instance->ClearStaticBoolOverrideByInfo({.Name = Survivor->Name}));
}

TEST(FMaterialPackageTests, TypedExpressionsRoundTripDuplicateAndRejectMalformedSaves)
{
	std::optional<Durin::UnifiedError::FError> RejectedError;
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::GetTestWorkDirectory() / "MaterialExpressions";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/MaterialExpressionTests/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/MaterialExpressionTests/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	auto Authored = Testing::MakePBRMaterialExpressionsForTest();
	auto& WorldPosition = Authored.Add(EMaterialProgramOpcode::WorldPosition,
		EMaterialProgramValueType::Float3, {}, {}, {});
	auto& Transform = Authored.Add(EMaterialProgramOpcode::TransformPosition,
		EMaterialProgramValueType::Float3, {Testing::MakeLink(WorldPosition)}, {}, {});
	auto* TransformPosition = Cast<DMaterialExpressionTransformPosition>(&Transform);
	ASSERT_NE(TransformPosition, nullptr);
	TransformPosition->Source = EMaterialCoordinateSpace::World;
	TransformPosition->Destination = EMaterialCoordinateSpace::View;
	Authored.Outputs.Get(EMaterialProperty::MP_WorldPositionOffset).SetConstant({1.f, 2.f, 3.f});
	const auto Time = Testing::MakeLink(Authored.Add(EMaterialProgramOpcode::Time,
		EMaterialProgramValueType::Float, {}, {}, {}));
	Authored.Outputs.Get(EMaterialProperty::MP_EmissiveColor) = Testing::MakeLink(Authored.Add(EMaterialProgramOpcode::VertexInterpolator,
		EMaterialProgramValueType::Float3, {Time}, {}, {}));
	std::ranges::reverse(Authored.Expressions);
	const auto LabelId = Authored.Expressions.front()->Id;
	const auto Label = std::ranges::find(Authored.Presentation.Nodes, LabelId, &FMaterialGraphNodePresentation::NodeId);
	if (Label == Authored.Presentation.Nodes.end())
		Authored.Presentation.Nodes.push_back({.NodeId = LabelId, .DisplayName = "Persisted presentation metadata"});
	else Label->DisplayName = "Persisted presentation metadata";
	ASSERT_TRUE(Authored.Apply(*Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const auto Presentation = Material->GetMaterialGraphPresentation();
	const auto CheckGraph = [&](DMaterial* Candidate) {
		ASSERT_NE(Candidate, nullptr);
		const auto& Children = Candidate->GetExpressionCollection().Expressions;
		ASSERT_EQ(Children.size(), Authored.Expressions.size());
		EXPECT_EQ(CaptureMaterialOutputBindings(*Candidate), CaptureMaterialOutputBindings(Authored.Outputs));
		EXPECT_EQ(Candidate->GetMaterialGraphPresentation(), Presentation);
		for (size_t Index = 0; Index < Authored.Expressions.size(); ++Index)
		{
			const auto* Expected = Authored.Expressions[Index].Get();
			const auto* Actual = Children[Index].Get();
			ASSERT_NE(Actual, nullptr);
			EXPECT_NE(Actual, Expected);
			EXPECT_EQ(Actual->GetOuter(), Candidate);
			ASSERT_EQ(Actual->GetClass(), Expected->GetClass());
			EXPECT_TRUE(HaveSameMaterialExpressionFields(*Children[Index], *Authored.Expressions[Index]));
		}
		EXPECT_TRUE(Candidate->ValidateLoadedObjectGraph({}));
	};
	FByteBuffer FirstSerialization, SecondSerialization;
	ASSERT_TRUE(SerializeAssetPackageBytes(Material->GetPackage(), FirstSerialization));
	ASSERT_TRUE(SerializeAssetPackageBytes(Material->GetPackage(), SecondSerialization));
	EXPECT_EQ(FirstSerialization, SecondSerialization);
	ObjectPackage::FLinkerTables Linker;
	ASSERT_TRUE(ObjectPackage::ReadPackage(FirstSerialization, {}, Path, Linker));
	EXPECT_FALSE(ContainsSerializedField(Linker, "GraphOwnershipVersion"));
	ASSERT_EQ(Linker.CustomVersions.size(), 2u);
	EXPECT_EQ(Linker.CustomVersions.front(), (FCustomVersion{FMaterialGraphVersion::Guid, FMaterialGraphVersion::CurrentVersion}));
	EXPECT_TRUE(ContainsSerializedField(Linker, "ExpressionCollection"));
	EXPECT_TRUE(ContainsSerializedField(Linker, "Expressions"));
	EXPECT_FALSE(ContainsSerializedField(Linker, "ExpressionOutputs"));
	EXPECT_FALSE(ContainsSerializedField(Linker, "Program"));
	EXPECT_FALSE(ContainsSerializedField(Linker, "FunctionCalls"));
	auto* Duplicate = Cast<DMaterial>(DuplicateObject(Material, nullptr, "DuplicatedExpressions").value());
	ASSERT_NO_FATAL_FAILURE(CheckGraph(Duplicate));
	EXPECT_NE(Duplicate->GetExpressionCollection().Expressions.front().Get(), Material->GetExpressionCollection().Expressions.front().Get());
	MarkObjectHierarchyAsGarbage(Duplicate);
	ASSERT_TRUE(UnloadPackage(Path));
	DMaterial* Loaded = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		Loaded = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NO_FATAL_FAILURE(CheckGraph(Loaded));
	const auto CatalogEntry = FindPackageExact(Path);
	ASSERT_NE(CatalogEntry, nullptr);
	EXPECT_TRUE(CatalogEntry->Dependencies.empty());
	// Failed authoring saves leave the previously committed package intact.
	auto* CollectionProperty = Loaded->GetClass()->FindPropertyByName("ExpressionCollection");
	ASSERT_NE(CollectionProperty, nullptr);
	CollectionProperty->ContainerPtrToValuePtr<FMaterialExpressionCollection>(Loaded)->Expressions.clear();
	Loaded->MarkPackageDirty();
	EXPECT_FALSE(SavePackage(Loaded->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path, EAssetPackageUnloadPolicy::DiscardUnsaved));
	Loaded = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		Loaded = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NO_FATAL_FAILURE(CheckGraph(Loaded));
	ASSERT_TRUE(UnloadPackage(Path));
	// A valid package envelope cannot publish abandoned expression children.
	bool bRemovedExpressions = false;
	for (auto& Export : Linker.Exports)
		for (auto& Property : Export.Properties)
			if (Property.FieldName == "ExpressionCollection")
			{
				const auto Field = std::ranges::find(Property.Value.FieldNames, "Expressions");
				ASSERT_NE(Field, Property.Value.FieldNames.end());
				Property.Value.Elements[Field - Property.Value.FieldNames.begin()].Elements.clear();
				bRemovedExpressions = true;
			}
	ASSERT_TRUE(bRemovedExpressions);
	FByteBuffer MalformedBytes, Bulk;
	ASSERT_TRUE(ObjectPackage::WritePackage(Linker, MalformedBytes, Bulk));
	ASSERT_TRUE(Bulk.empty());
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(MalformedBytes, Root / "Base.dasset"));
	Loaded = nullptr;
	const auto Rejected = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &RejectedError);
	Loaded = Rejected;
	EXPECT_FALSE(Rejected);
	ASSERT_TRUE(RejectedError);
	EXPECT_EQ(Loaded, nullptr);
	EXPECT_EQ(FindResidentPackage(Path), nullptr);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialPackageTests, MissingInstanceCustomVersionRejectsInstanceWithoutChangingParent)
{
	std::optional<Durin::UnifiedError::FError> BaseLoadError;
	std::optional<Durin::UnifiedError::FError> LoadError;
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "LegacyMaterials";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::Testing::RegisterMountPointForTests("/LegacyMaterialTests/", Root.generic_string() + "/");

	Durin::FPackagePath BasePath;
	Durin::FPackagePath InstancePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/LegacyMaterialTests/Base", BasePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/LegacyMaterialTests/Instance", InstancePath));

	Durin::DMaterial* Base = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(BasePath, Base));
	ASSERT_TRUE(SetBindingProgram(*Base));
	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3)));
	ASSERT_TRUE(Durin::SavePackage(Base->GetPackage()));

	Durin::DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(InstancePath, Instance));
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.25f));
	ASSERT_TRUE(Durin::SavePackage(Instance->GetPackage()));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(InstancePath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(BasePath));

	Durin::FByteBuffer BaseBytes;
	auto BaseBytesRead = Durin::FFileHelper::LoadFileToArray((Root / "Base.dasset"));
	ASSERT_TRUE(BaseBytesRead) << BaseBytesRead.error().GetMessage();
	BaseBytes = std::move(*BaseBytesRead);

	Durin::FByteBuffer InstanceBytes;
	auto InstanceBytesRead = Durin::FFileHelper::LoadFileToArray((Root / "Instance.dasset"));
	ASSERT_TRUE(InstanceBytesRead) << InstanceBytesRead.error().GetMessage();
	InstanceBytes = std::move(*InstanceBytesRead);
	Durin::ObjectPackage::FLinkerTables Linker;
	ASSERT_TRUE(Durin::ObjectPackage::ReadPackage(InstanceBytes, {}, InstancePath, Linker));
	EXPECT_FALSE(ContainsSerializedField(Linker, "ParameterStorageVersion"));
	ASSERT_EQ(std::erase_if(Linker.CustomVersions, [](const auto& Version) {
		return Version.Guid == Durin::FMaterialInstanceVersion::Guid;
	}), 1u);
	Durin::FByteBuffer Bulk;
	ASSERT_TRUE(Durin::ObjectPackage::WritePackage(Linker, InstanceBytes, Bulk));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(InstanceBytes)), Root / "Instance.dasset"));

	Durin::DMaterialInstance* LoadedInstance = nullptr;
	const auto Load =
		Durin::LoadObject<Durin::DMaterialInstance>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(InstancePath), nullptr, &LoadError);
		LoadedInstance = Load;
	EXPECT_FALSE(Load);
	EXPECT_EQ(LoadedInstance, nullptr);
	EXPECT_NE(Durin::FindResidentPackage(BasePath), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(InstancePath), nullptr);
	Durin::DMaterial* LoadedBase = nullptr;
	const auto BaseLoad = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(BasePath), nullptr, &BaseLoadError);
	LoadedBase = BaseLoad;
	EXPECT_TRUE(BaseLoad) << (BaseLoadError ? BaseLoadError->ToString() : "");
	EXPECT_NE(LoadedBase, nullptr);
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(BasePath));
	Durin::FByteBuffer After;
	auto AfterRead = Durin::FFileHelper::LoadFileToArray(Root / "Base.dasset");
	ASSERT_TRUE(AfterRead) << AfterRead.error().GetMessage();
	After = std::move(*AfterRead);
	EXPECT_EQ(After, BaseBytes);
}

TEST(FMaterialPackageTests, MixedPackageRequiresAllVersionDomainsAndPreservesInstanceDuplication)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::GetTestWorkDirectory() / "MixedMaterialVersions";
	Testing::RegisterMountPointForTests("/MixedMaterialVersions/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/MixedMaterialVersions/Base", Path));
	DMaterial* Base = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Base));
	ASSERT_TRUE(SetBindingProgram(*Base));
	NewObject<DMaterialFunction>(Base->GetPackage(), "Function");
	auto* Instance = NewObject<DMaterialInstance>(Base->GetPackage(), "Overrides");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), .25f));
	auto* Copy = Cast<DMaterialInstance>(DuplicateObject(Instance, nullptr, "CopiedOverrides").value());
	ASSERT_NE(Copy, nullptr);
	EXPECT_TRUE(std::ranges::contains(GetLoadedDirectMaterialChildren(Base), FObjectKey(Copy)));
	float Opacity = 0;
	ASSERT_TRUE(Copy->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, .25f);
	MarkObjectHierarchyAsGarbage(Copy);
	ASSERT_TRUE(SavePackage(Base->GetPackage()));
	FByteBuffer Original;
	auto OriginalRead = FFileHelper::LoadFileToArray(Root / "Base.dasset");
	ASSERT_TRUE(OriginalRead) << OriginalRead.error().GetMessage();
	Original = std::move(*OriginalRead);
	ObjectPackage::FLinkerTables Saved;
	ASSERT_TRUE(ObjectPackage::ReadPackage(Original, {}, Path, Saved));
	ASSERT_EQ(Saved.CustomVersions.size(), 4u);
	EXPECT_FALSE(ContainsSerializedField(Saved, "ParameterStorageVersion"));
	EXPECT_FALSE(ContainsSerializedField(Saved, "Signature"));
	EXPECT_FALSE(ContainsSerializedField(Saved, "PortId"));
	ASSERT_TRUE(UnloadPackage(Path));
	const std::array RejectedVersionBoundaries{
		std::pair{FMaterialGraphVersion::Guid, std::array{-1, 5, 7}},
		std::pair{FMaterialInstanceVersion::Guid, std::array{3, 4, FMaterialInstanceVersion::CurrentVersion + 1}},
		std::pair{FMaterialFunctionVersion::Guid, std::array{-1, 2, 4}},
		std::pair{FMaterialOutputVersion::Guid, std::array{-1, 5, 7}},
	};
	for (const auto& [Guid, Versions] : RejectedVersionBoundaries)
		for (const int32 Version : Versions)
		{
			std::optional<Durin::UnifiedError::FError> ResultError;
			auto Candidate = Saved;
			const auto Record = std::ranges::find(Candidate.CustomVersions, Guid, &FCustomVersion::Guid);
			ASSERT_NE(Record, Candidate.CustomVersions.end());
			if (Version == Record->Version) continue;
			if (Version < 0) Candidate.CustomVersions.erase(Record);
			else Record->Version = Version;
			FByteBuffer Bytes, Bulk;
			ASSERT_TRUE(ObjectPackage::WritePackage(Candidate, Bytes, Bulk));
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, Root / "Base.dasset"));
			DMaterial* Loaded = nullptr;
			const auto Result = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &ResultError);
			Loaded = Result;
			ASSERT_TRUE(ResultError);
			EXPECT_TRUE(ResultError->Is(PackageReadErrors::UnsupportedVersion)) << ResultError->ToString();
			EXPECT_FALSE(Result);
			EXPECT_EQ(Loaded, nullptr);
			EXPECT_EQ(FindResidentPackage(Path), nullptr);
		}
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Original, Root / "Base.dasset"));
	DMaterial* Loaded = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		Loaded = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	const auto LoadedChildren = GetLoadedDirectMaterialChildren(Loaded);
	ASSERT_EQ(LoadedChildren.size(), 1u);
	EXPECT_EQ(LoadedChildren.front().ResolveObjectPtr(), Loaded->GetPackage()->FindTopLevelAsset("Overrides"));
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST(FMaterialPackageTests, StandaloneMakeRequiresOutputVersionBeforeBindingItsFields)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture Registry;
	const auto Root = Testing::CreateTestFixtureDirectory("MakeOutputVersion");
	Testing::RegisterMountPointForTests("/MakeOutputVersion/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/MakeOutputVersion/Function", Path));
	DMaterialFunction* Function = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Function));
	TStrongObjectPtr<DMaterialExpressionMakeMaterialAttributes> Make(NewObject<DMaterialExpressionMakeMaterialAttributes>(nullptr, NAME_None));
	Make->Id = FGuid::NewGuid();
	TStrongObjectPtr<DMaterialExpressionScalarConstant> Value(NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None));
	Value->Id = FGuid::NewGuid();
	Value->Value = .375f;
	Make->Roughness = {Value.Get()};
	std::vector<DMaterialExpression*> Body;
	for (const auto& Expression : Function->GetExpressionCollection().Expressions) Body.push_back(Expression.Get());
	Body.push_back(Make.Get());
	Body.push_back(Value.Get());
	ASSERT_TRUE(Function->SetFunctionExpressions(Body));
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));

	const auto File = Root / "Function.dasset";
	const auto Original = FFileHelper::LoadFileToArray(File);
	ASSERT_TRUE(Original);
	ObjectPackage::FLinkerTables Tables;
	ASSERT_TRUE(ObjectPackage::ReadPackage(*Original, {}, Path, Tables));
	ASSERT_EQ(std::erase_if(Tables.CustomVersions, [](const auto& V) { return V.Guid == FMaterialOutputVersion::Guid; }), 1u);
	// Give Roughness a wire type that cannot bind to an expression connection.
	const auto ClassName = Make->GetClass()->GetQualifiedName().ToString();
	auto Schema = std::ranges::find(Tables.Schemas, ClassName, &ObjectPackage::FSerializedSchema::QualifiedName);
	ASSERT_NE(Schema, Tables.Schemas.end());
	auto Field = std::ranges::find(Schema->Fields, "Roughness", &ObjectPackage::FSerializedField::Name);
	ASSERT_NE(Field, Schema->Fields.end());
	Field->Type = {.Kind = ObjectPackage::EValueKind::Bool};
	auto Export = std::ranges::find(Tables.Exports, ClassName, &ObjectPackage::FPackageExport::ClassName);
	ASSERT_NE(Export, Tables.Exports.end());
	auto Tag = std::ranges::find(Export->Properties, "Roughness", &ObjectPackage::FPropertyTag::FieldName);
	ASSERT_NE(Tag, Export->Properties.end());
	Tag->Type = Field->Type;
	Tag->Value = {};
	Tag->Payload.clear();
	FByteBuffer Bytes, Bulk;
	ASSERT_TRUE(ObjectPackage::WritePackage(Tables, Bytes, Bulk));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, File));
	const auto ObjectPath = Testing::MakePackageLeafAssetObjectPathForTests(Path);
	std::optional<UnifiedError::FError> Error;
	EXPECT_FALSE(LoadObject<DMaterialFunction>(ObjectPath, nullptr, &Error));
	ASSERT_TRUE(Error);
	EXPECT_TRUE(Error->Is(PackageReadErrors::UnsupportedVersion)) << Error->ToString();
	EXPECT_EQ(FindResidentPackage(Path), nullptr);

	ASSERT_TRUE(FFileHelper::SaveArrayToFile(*Original, File));
	Function = LoadObject<DMaterialFunction>(ObjectPath);
	ASSERT_NE(Function, nullptr);
	const auto* Reopened = Cast<DMaterialExpressionMakeMaterialAttributes>(
		FindMaterialExpression(Function->GetExpressionCollection().Expressions, Make->Id));
	ASSERT_NE(Reopened, nullptr);
	EXPECT_EQ(Reopened->Roughness.GetExpressionId(), Value->Id);
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST(FMaterialPackageTests, StandaloneSetRequiresOutputVersion)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture Registry;
	const auto Root = Testing::CreateTestFixtureDirectory("SetOutputVersion");
	Testing::RegisterMountPointForTests("/SetOutputVersion/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/SetOutputVersion/Function", Path));
	DMaterialFunction* Function = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Function));

	TStrongObjectPtr<DMaterialExpressionFunctionInput> Input(NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialExpressionSetMaterialAttributes> Set(NewObject<DMaterialExpressionSetMaterialAttributes>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialExpressionFunctionOutput> Output(NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None));
	Input->Id = FGuid::NewGuid();
	Input->Port = Function->GetFunctionSignature().Inputs.front();
	Set->Id = FGuid::NewGuid();
	Set->Surface = {Input.Get()};
	Output->Id = FGuid::NewGuid();
	Output->Port = Function->GetFunctionSignature().Outputs.front();
	Output->Source = {Set.Get()};
	const std::array<DMaterialExpression*, 3> Body{Input.Get(), Set.Get(), Output.Get()};
	const auto Validation = Function->SetFunctionExpressions(Body);
	ASSERT_TRUE(Validation) << FormatMaterialProgramDiagnostics(Validation.Diagnostics);
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));

	const auto File = Root / "Function.dasset";
	const auto Original = FFileHelper::LoadFileToArray(File);
	ASSERT_TRUE(Original);
	ObjectPackage::FLinkerTables Tables;
	ASSERT_TRUE(ObjectPackage::ReadPackage(*Original, {}, Path, Tables));
	const auto Version = std::ranges::find(Tables.CustomVersions, FMaterialOutputVersion::Guid, &FCustomVersion::Guid);
	ASSERT_NE(Version, Tables.CustomVersions.end());
	EXPECT_EQ(Version->Version, FMaterialOutputVersion::CurrentVersion);
	Tables.CustomVersions.erase(Version);
	FByteBuffer Bytes, Bulk;
	ASSERT_TRUE(ObjectPackage::WritePackage(Tables, Bytes, Bulk));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, File));
	const auto ObjectPath = Testing::MakePackageLeafAssetObjectPathForTests(Path);
	std::optional<UnifiedError::FError> Error;
	EXPECT_FALSE(LoadObject<DMaterialFunction>(ObjectPath, nullptr, &Error));
	ASSERT_TRUE(Error);
	EXPECT_TRUE(Error->Is(PackageReadErrors::UnsupportedVersion)) << Error->ToString();
	EXPECT_EQ(FindResidentPackage(Path), nullptr);

	ASSERT_TRUE(FFileHelper::SaveArrayToFile(*Original, File));
	Function = LoadObject<DMaterialFunction>(ObjectPath);
	ASSERT_TRUE(Function);
	const auto* Reopened = Cast<DMaterialExpressionSetMaterialAttributes>(
		FindMaterialExpression(Function->GetExpressionCollection().Expressions, Set->Id));
	ASSERT_TRUE(Reopened);
	EXPECT_EQ(Reopened->Surface.GetExpressionId(), Input->Id);
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST(FMaterialPackageTests, AuthoredGraphVersionsLoadAfterRestartAndFunctionsDuplicate)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::CreateTestFixtureDirectory("MaterialGraphRestart");
	Testing::RegisterMountPointForTests("/MaterialGraphRestart/", Root.generic_string() + "/");
	FPackagePath MaterialPath, FunctionPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/MaterialGraphRestart/Material", MaterialPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/MaterialGraphRestart/Function", FunctionPath));
	DMaterial* Material = nullptr;
	DMaterialFunction* Function = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	auto Graph = Testing::MakePBRMaterialExpressionsForTest();
	Graph.Add(EMaterialProgramOpcode::Pow, EMaterialProgramValueType::Float3, {}, {}, {});
	ASSERT_TRUE(Graph.Apply(*Material));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FunctionPath, Function));
	const auto ExpectedSignature = Function->GetFunctionSignature();
	const auto ExpectedExpressionCount = Function->GetExpressionCollection().Expressions.size();
	ASSERT_GT(ExpectedExpressionCount, 0u);
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(MaterialPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(FunctionPath));
	Material = nullptr;
	Function = nullptr;
	ShutdownAssetRuntime();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(InitializeAssetRuntime());
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	for (const auto& Path : {MaterialPath, FunctionPath})
	{
		std::optional<Durin::UnifiedError::FError> ResultError;
		SCOPED_TRACE(Path.ToString());
		const auto Data = FindPackageExact(Path);
		ASSERT_NE(Data, nullptr);
		FByteBuffer Bytes;
		auto BytesRead = FFileHelper::LoadFileToArray(Data->PhysicalPath);
		ASSERT_TRUE(BytesRead) << BytesRead.error().GetMessage();
		Bytes = std::move(*BytesRead);
		ObjectPackage::FLinkerTables Linker;
		ASSERT_TRUE(ObjectPackage::ReadPackage(Bytes, {}, Path, Linker));
		auto ExpectedVersions = std::vector<FCustomVersion>{{FMaterialGraphVersion::Guid, FMaterialGraphVersion::CurrentVersion}};
		if (Path == MaterialPath) ExpectedVersions.push_back({FMaterialOutputVersion::Guid, FMaterialOutputVersion::CurrentVersion});
		if (Path == FunctionPath) ExpectedVersions.push_back({FMaterialFunctionVersion::Guid, FMaterialFunctionVersion::CurrentVersion});
		std::ranges::sort(ExpectedVersions, {}, &FCustomVersion::Guid);
		ASSERT_EQ(Linker.CustomVersions, ExpectedVersions);
		DObject* Loaded = nullptr;
		const auto Result = LoadObject<DObject>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &ResultError);
		Loaded = Result;
		ASSERT_TRUE(Result) << (ResultError ? ResultError->ToString() : "");
		if (auto* Function = Cast<DMaterialFunction>(Loaded))
		{
			EXPECT_EQ(Function->GetFunctionSignature(), ExpectedSignature);
			EXPECT_EQ(Function->GetExpressionCollection().Expressions.size(), ExpectedExpressionCount);
			auto* Copy = Cast<DMaterialFunction>(DuplicateObject(Function, nullptr, NAME_None).value());
			ASSERT_NE(Copy, nullptr);
			EXPECT_EQ(Copy->GetExpressionCollection().Expressions.size(), Function->GetExpressionCollection().Expressions.size());
			EXPECT_EQ(Copy->GetFunctionSignature(), ExpectedSignature);
			MarkObjectHierarchyAsGarbage(Copy);
		}
		if (auto* LoadedMaterial = Cast<DMaterial>(Loaded))
			EXPECT_TRUE(std::ranges::any_of(
				LoadedMaterial->GetExpressionCollection().Expressions,
				[](const auto& Expression) {
					return Cast<DMaterialExpressionPow>(Expression.Get()) != nullptr;
				}));
		ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(Path));
	}
	ShutdownAssetRuntime();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(InitializeAssetRuntime());
}

TEST(FMaterialPackageTests, OutputModePersistsBothConnections)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::GetTestWorkDirectory() / "MaterialOutputModes";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/MaterialOutputModes/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/MaterialOutputModes/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Graph = Testing::MakePBRMaterialExpressionsForTest();
	TStrongObjectPtr<DMaterialExpressionMakeMaterialAttributes> Packed(NewObject<DMaterialExpressionMakeMaterialAttributes>(nullptr, NAME_None));
	Packed->Id = FGuid::NewGuid();
	for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
		*Packed->GetExpressionInputForProperty(Field.Property) = Graph.Outputs.Get(Field.Property).Connection;
	Graph.Expressions.emplace_back(Packed.Get());
	Graph.Outputs.Attributes = {Packed.Get()};
	Graph.Outputs.bUseMaterialAttributes = true;
	ASSERT_TRUE(Graph.Apply(*Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const auto Expected = CaptureMaterialOutputBindings(*Material);
	ASSERT_TRUE(UnloadPackage(Path));
	Material = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		Material = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	EXPECT_EQ(CaptureMaterialOutputBindings(*Material), Expected);
	// Write a current package with the packed connection retained but inactive.
	Graph.Outputs.bUseMaterialAttributes = false;
	ASSERT_TRUE(Graph.Apply(*Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		Material = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	EXPECT_EQ(CaptureMaterialOutputBindings(*Material), CaptureMaterialOutputBindings(Graph.Outputs));
	ASSERT_TRUE(UnloadPackage(Path));
	Testing::RemoveTestWorkDirectory(Root);
}
