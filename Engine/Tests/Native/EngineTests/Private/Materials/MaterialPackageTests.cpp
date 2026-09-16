#include "StaticMesh/StaticMeshTestEnvironment.h"
#include "StaticMeshMaterialTestFixture.h"
#include "Materials/MaterialCustomVersion.h"
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
	ASSERT_TRUE(SetExpandedProgram(*Base));
	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	Base->SetVectorParameterValue(Durin::MaterialParameters::NormalName(), Durin::FVector3(0.0, 1.0, 1.0));
	Base->SetScalarParameterValue(Durin::MaterialParameters::RoughnessName(), 0.7f);
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
	Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.35f);
	Instance->SetScalarParameterValue(Durin::MaterialParameters::MetallicName(), 0.8f);
	Instance->SetScalarParameterValue(Durin::FName("BaseColorUVChannel"), 2.0f);
	Instance->SetVector2ParameterValue(Durin::FName("BaseColorUVScale"), Durin::FVector2(2.0, -1.0));
	Instance->SetVector2ParameterValue(Durin::FName("BaseColorUVOffset"), Durin::FVector2(0.25, 0.5));
	Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), TextureImport.Asset);
	ASSERT_TRUE(Durin::SavePackage(Instance->GetPackage()));
	const Durin::FAssetCatalogEntry InstanceData =
		Durin::FindAssetExact(InstancePath);
	ASSERT_NE(InstanceData, nullptr);
	EXPECT_NE(std::ranges::find(InstanceData->Dependencies, BasePath), InstanceData->Dependencies.end());
	EXPECT_NE(std::ranges::find(InstanceData->Dependencies, TexturePath), InstanceData->Dependencies.end());
	ASSERT_FALSE(Instance->GetPackage()->IsDirty());
	const uint64 SavedVersion = Instance->GetRenderStateVersion();
	EXPECT_FALSE(Instance->SetScalarParameterValue(Durin::FName("UnknownParameter"), 0.2f));
	EXPECT_FALSE(Instance->SetVectorParameterValue(Durin::MaterialParameters::OpacityName(), Durin::FVector3(0.2)));
	EXPECT_FALSE(Instance->GetPackage()->IsDirty());
	EXPECT_EQ(Instance->GetRenderStateVersion(), SavedVersion);
	ASSERT_TRUE(Durin::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::UnloadPackage(BasePath));
	ASSERT_TRUE(Durin::UnloadPackage(TexturePath));

	Durin::DMaterialInstance* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(InstancePath), Loaded));
	ASSERT_NE(Loaded->GetParent(), nullptr);
	Durin::DTexture2D* LoadedTexture = nullptr;
	ASSERT_TRUE(Loaded->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), LoadedTexture));
	ASSERT_NE(LoadedTexture, nullptr);
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*LoadedTexture, 10.0));
	(void)Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(Loaded->GetStaticProperties(), StaticProperties);
	ExpectColorNear(GetMaterialBinding(Loaded->GetRenderData()).BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 0.35f));
	const auto LoadedBinding = GetMaterialBinding(Loaded->GetRenderData());
	EXPECT_FLOAT_EQ(LoadedBinding.Metallic, 0.8f);
	EXPECT_FLOAT_EQ(LoadedBinding.Roughness, 0.7f);
	EXPECT_FLOAT_EQ(LoadedBinding.UVChannels[0], 2.0f);
	EXPECT_EQ(LoadedBinding.UVScales[0], Durin::FVector2f(2.0f, -1.0f));
	EXPECT_EQ(LoadedBinding.UVOffsets[0], Durin::FVector2f(0.25f, 0.5f));
	EXPECT_EQ(
		GetMaterialBinding(Loaded->GetRenderData()).Textures[0],
		LoadedTexture->GetTextureReferenceRHI());
	auto* LoadedBase = Durin::Cast<Durin::DMaterial>(Loaded->GetParent());
	ASSERT_NE(LoadedBase, nullptr);
	LoadedBase->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.6, 0.4, 0.2));
	ExpectColorNear(GetMaterialBinding(Loaded->GetRenderData()).BaseColor, Durin::FVector4f(0.6f, 0.4f, 0.2f, 0.35f));
	ASSERT_TRUE(Durin::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::UnloadPackage(
		BasePath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::UnloadPackage(TexturePath));
}

TEST(FMaterialPackageTests, TypedExpressionsRoundTripDuplicateAndRejectMalformedSaves)
{
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
		ASSERT_EQ(Children.size(), Authored.Expressions.size() + 1);
		EXPECT_EQ(Candidate->GetExpressionOutputs(), Authored.Outputs);
		EXPECT_EQ(Candidate->GetMaterialGraphPresentation(), Presentation);
		for (size_t Index = 0; Index < Authored.Expressions.size(); ++Index)
		{
			const auto* Expected = Authored.Expressions[Index].Get();
			const auto* Actual = Children[Index].Get();
			ASSERT_NE(Actual, nullptr);
			EXPECT_NE(Actual, Expected);
			EXPECT_EQ(Actual->GetOuter(), Candidate);
			ASSERT_EQ(Actual->GetClass(), Expected->GetClass());
			Actual->GetClass()->ForEachProperty([&](const FProperty* Property) {
				EXPECT_TRUE(ArePropertyValuesIdentical(Property, Actual, 0, Expected, 0));
			});
		}
		std::string Error;
		EXPECT_TRUE(Candidate->ValidateLoadedObjectGraph({}, Error)) << Error;
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
	auto* Duplicate = Cast<DMaterial>(DuplicateObject(Material, nullptr, "DuplicatedExpressions"));
	ASSERT_NO_FATAL_FAILURE(CheckGraph(Duplicate));
	EXPECT_NE(Duplicate->GetExpressionCollection().Expressions.front().Get(), Material->GetExpressionCollection().Expressions.front().Get());
	MarkObjectHierarchyAsGarbage(Duplicate);
	ASSERT_TRUE(UnloadPackage(Path));
	DMaterial* Loaded = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded));
	ASSERT_NO_FATAL_FAILURE(CheckGraph(Loaded));
	const auto CatalogEntry = FindAssetExact(Path);
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
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded));
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
	const auto Rejected = LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded);
	EXPECT_FALSE(Rejected);
	EXPECT_FALSE(Rejected.Message.empty());
	EXPECT_EQ(Loaded, nullptr);
	EXPECT_EQ(FindResidentPackage(Path), nullptr);
	CollectGarbage();
}

TEST(FMaterialPackageTests, MissingInstanceCustomVersionRejectsInstanceWithoutChangingParent)
{
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
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3)));
	ASSERT_TRUE(Durin::SavePackage(Base->GetPackage()));

	Durin::DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(InstancePath, Instance));
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.25f));
	ASSERT_TRUE(Durin::SavePackage(Instance->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::UnloadPackage(BasePath));

	Durin::FByteBuffer BaseBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BaseBytes, (Root / "Base.dasset")));

	Durin::FByteBuffer InstanceBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(InstanceBytes, (Root / "Instance.dasset")));
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
	const Durin::FAssetResult Load =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(InstancePath), LoadedInstance);
	EXPECT_FALSE(Load);
	EXPECT_EQ(LoadedInstance, nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(BasePath), nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(InstancePath), nullptr);
	Durin::DMaterial* LoadedBase = nullptr;
	const auto BaseLoad = Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(BasePath), LoadedBase);
	EXPECT_TRUE(BaseLoad) << BaseLoad.Message;
	EXPECT_NE(LoadedBase, nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(BasePath));
	Durin::FByteBuffer After;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(After, Root / "Base.dasset"));
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
	ASSERT_TRUE(Instance->SetScalarParameterValue(MaterialParameters::OpacityName(), .25f));
	auto* Copy = Cast<DMaterialInstance>(DuplicateObject(Instance, nullptr, "CopiedOverrides"));
	ASSERT_NE(Copy, nullptr);
	float Opacity = 0;
	ASSERT_TRUE(Copy->GetScalarParameterValue(MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, .25f);
	MarkObjectHierarchyAsGarbage(Copy);
	ASSERT_TRUE(SavePackage(Base->GetPackage()));
	FByteBuffer Original;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Original, Root / "Base.dasset"));
	ObjectPackage::FLinkerTables Saved;
	ASSERT_TRUE(ObjectPackage::ReadPackage(Original, {}, Path, Saved));
	ASSERT_EQ(Saved.CustomVersions.size(), 4u);
	EXPECT_FALSE(ContainsSerializedField(Saved, "ParameterStorageVersion"));
	EXPECT_FALSE(ContainsSerializedField(Saved, "Signature"));
	EXPECT_FALSE(ContainsSerializedField(Saved, "PortId"));
	ASSERT_TRUE(UnloadPackage(Path));
	for (const FGuid Guid : {FMaterialGraphVersion::Guid, FMaterialInstanceVersion::Guid, FMaterialFunctionVersion::Guid, FMaterialOutputVersion::Guid})
		for (const int32 Version : {-1, 0, 1, 2, 3})
		{
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
			const auto Result = LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded);
			EXPECT_EQ(Result.Error, EAssetError::UnsupportedVersion) << Result.Message;
			EXPECT_EQ(Loaded, nullptr);
			EXPECT_EQ(FindResidentPackage(Path), nullptr);
		}
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Original, Root / "Base.dasset"));
	DMaterial* Loaded = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded));
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
	ASSERT_TRUE(SetBindingProgram(*Material));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FunctionPath, Function));
	const auto ExpectedSignature = Function->GetFunctionSignature();
	const auto ExpectedExpressionCount = Function->GetExpressionCollection().Expressions.size();
	ASSERT_GT(ExpectedExpressionCount, 0u);
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(FunctionPath));
	Material = nullptr;
	Function = nullptr;
	ShutdownAssetManager();
	CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	for (const auto& Path : {MaterialPath, FunctionPath})
	{
		SCOPED_TRACE(Path.ToString());
		const auto Data = FindAssetExact(Path);
		ASSERT_NE(Data, nullptr);
		FByteBuffer Bytes;
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Data->PhysicalPath));
		ObjectPackage::FLinkerTables Linker;
		ASSERT_TRUE(ObjectPackage::ReadPackage(Bytes, {}, Path, Linker));
		auto ExpectedVersions = std::vector<FCustomVersion>{{FMaterialGraphVersion::Guid, FMaterialGraphVersion::CurrentVersion}};
		if (Path == MaterialPath) ExpectedVersions.push_back({FMaterialOutputVersion::Guid, FMaterialOutputVersion::CurrentVersion});
		if (Path == FunctionPath) ExpectedVersions.push_back({FMaterialFunctionVersion::Guid, FMaterialFunctionVersion::CurrentVersion});
		std::ranges::sort(ExpectedVersions, {}, &FCustomVersion::Guid);
		ASSERT_EQ(Linker.CustomVersions, ExpectedVersions);
		DObject* Loaded = nullptr;
		const auto Result = LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded);
		ASSERT_TRUE(Result) << Result.Message;
		if (auto* Function = Cast<DMaterialFunction>(Loaded))
		{
			EXPECT_EQ(Function->GetFunctionSignature(), ExpectedSignature);
			EXPECT_EQ(Function->GetExpressionCollection().Expressions.size(), ExpectedExpressionCount);
			auto* Copy = Cast<DMaterialFunction>(DuplicateObject(Function, nullptr, NAME_None));
			ASSERT_NE(Copy, nullptr);
			EXPECT_EQ(Copy->GetExpressionCollection().Expressions.size(), Function->GetExpressionCollection().Expressions.size());
			EXPECT_EQ(Copy->GetFunctionSignature(), ExpectedSignature);
			MarkObjectHierarchyAsGarbage(Copy);
		}
		ASSERT_TRUE(UnloadPackage(Path));
	}
	ShutdownAssetManager();
	CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
}
