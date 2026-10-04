#include "NativeRHIBackendTestSupport.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "MaterialCookedProgramTestSupport.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialExpressions.h"
#include "Engine/World.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "MaterialTestSupport.h"
#include "StaticMeshMaterialTestFixture.h"
#include "FunctionPortTestFixture.h"
#include "MaterialGraphDocument.h"
#include "Asset/AssetCompilingManager.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Editor::Material;

	auto MakeDeclaration(std::string_view Name,
		EMaterialParameterType Type = EMaterialParameterType::Scalar,
		FVector4 Default = FVector4(0.0))
		-> FMaterialParameterCollectionDeclaration
	{
		return {.Id = FGuid::NewGuid(), .Name = FName(Name), .Type = Type,
			.DefaultValue = Default, .DisplayName = std::string(Name)};
	}

	class FMaterialParameterCollectionTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			SubsystemRegistration = std::make_unique<FWorldSubsystemRegistration>(
				FWorldSubsystemDescriptor{
					.Type = DMaterialParameterCollectionSubsystem::StaticClass()});
		}
		auto TearDown() -> void override
		{
			for (auto& World : Worlds)
			{
				World->Shutdown();
				MarkObjectHierarchyAsGarbage(World.Get());
			}
			Worlds.clear();
			for (auto& Object : Objects)
				MarkObjectHierarchyAsGarbage(Object.Get());
			Objects.clear();
			CollectGarbage();
			SubsystemRegistration.reset();
		}

		template<typename T>
		auto Make(std::string_view Name) -> T*
		{
			auto* Object = NewObject<T>(nullptr, FName(Name));
			Objects.emplace_back(Object);
			return Object;
		}

		auto MakeWorld(std::string_view Name) -> DWorld*
		{
			auto* World = NewObject<DWorld>(nullptr, FName(Name));
			Worlds.emplace_back(World);
			EXPECT_TRUE(World->InitializeSubsystems());
			return World;
		}

		std::vector<TStrongObjectPtr<DObject>> Objects;
		std::vector<TStrongObjectPtr<DWorld>> Worlds;
		std::unique_ptr<FWorldSubsystemRegistration> SubsystemRegistration;
	};
}

TEST_F(FMaterialParameterCollectionTests,
	SchemaValidationIsBoundedStableAndCanonical)
{
	auto* Collection = Make<DMaterialParameterCollection>("CollectionSchema");
	const FGuid PersistentId = Collection->GetCollectionId();
	auto A = MakeDeclaration("Exposure", EMaterialParameterType::Scalar,
		FVector4(1.0, 0.0, 0.0, 0.0));
	auto B = MakeDeclaration("Tint", EMaterialParameterType::Vector4,
		FVector4(1.0, 0.5, 0.25, 1.0));
	ASSERT_TRUE(Collection->SetDeclarations(std::array{A, B}));
	const auto First = Collection->BuildLayout();
	ASSERT_TRUE(First);
	EXPECT_EQ(First->UniformLayout.Fields.front().ParameterId,
		std::min(A.Id, B.Id));

	A.Name = "RenamedExposure";
	ASSERT_TRUE(Collection->SetDeclarations(std::array{B, A}));
	const auto Reordered = Collection->BuildLayout();
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Collection->GetCollectionId(), PersistentId);
	EXPECT_EQ(Reordered->UniformLayout.Identity, First->UniformLayout.Identity);

	auto DuplicateName = B;
	DuplicateName.Id = FGuid::NewGuid();
	DuplicateName.Name = "renamedexposure";
	const auto Rejected = Collection->SetDeclarations(
		std::array{A, DuplicateName});
	EXPECT_EQ(Rejected.Error,
		EMaterialParameterCollectionError::DuplicateName);
	EXPECT_EQ(Collection->GetDeclarations().size(), 2u);

	std::vector<FMaterialParameterCollectionDeclaration> TooMany;
	for (uint32 Index = 0;
		Index <= MaterialParameterCollectionMaxDeclarationCount; ++Index)
		TooMany.push_back(MakeDeclaration(std::format("Value{}", Index)));
	EXPECT_EQ(Collection->SetDeclarations(TooMany).Error,
		EMaterialParameterCollectionError::DeclarationLimit);
}

TEST_F(FMaterialParameterCollectionTests,
	WorldOverridesAreAtomicIsolatedAndFallBackToDefaults)
{
	auto* Collection = Make<DMaterialParameterCollection>("CollectionWorlds");
	auto Value = MakeDeclaration("Value", EMaterialParameterType::Vector4,
		FVector4(1.0, 2.0, 3.0, 4.0));
	ASSERT_TRUE(Collection->SetDeclarations(std::array{Value}));
	auto* FirstWorld = MakeWorld("CollectionFirstWorld");
	auto* SecondWorld = MakeWorld("CollectionSecondWorld");
	auto* First = FirstWorld->GetSubsystem<DMaterialParameterCollectionSubsystem>();
	auto* Second = SecondWorld->GetSubsystem<DMaterialParameterCollectionSubsystem>();
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);

	FVector4 Read;
	ASSERT_TRUE(First->GetValue(*Collection, Value.Id, Read));
	EXPECT_EQ(Read, Value.DefaultValue);
	ASSERT_TRUE(First->SetValue(*Collection, Value.Id,
		FVector4(8.0, 7.0, 6.0, 5.0)));
	ASSERT_TRUE(First->GetValue(*Collection, Value.Id, Read));
	EXPECT_EQ(Read, FVector4(8.0, 7.0, 6.0, 5.0));
	ASSERT_TRUE(Second->GetValue(*Collection, Value.Id, Read));
	EXPECT_EQ(Read, Value.DefaultValue);

	const std::array Invalid{
		FMaterialParameterCollectionUpdate::Set(Value.Id, FVector4(9.0)),
		FMaterialParameterCollectionUpdate::Clear(Value.Id)};
	EXPECT_EQ(First->ApplyUpdates(*Collection, Invalid).Error,
		EMaterialParameterCollectionError::DuplicateUpdate);
	ASSERT_TRUE(First->GetValue(*Collection, Value.Id, Read));
	EXPECT_EQ(Read, FVector4(8.0, 7.0, 6.0, 5.0));
	ASSERT_TRUE(First->ClearValue(*Collection, Value.Id));
	ASSERT_TRUE(First->GetValue(*Collection, Value.Id, Read));
	EXPECT_EQ(Read, Value.DefaultValue);
	const auto BeforeDefaultEdit = First->GetSnapshot(*Collection);
	ASSERT_TRUE(BeforeDefaultEdit);
	ASSERT_TRUE(Collection->SetDefaultValue(Value.Id,
		FVector4(4.0, 3.0, 2.0, 1.0)));
	const auto AfterDefaultEdit = First->GetSnapshot(*Collection);
	ASSERT_TRUE(AfterDefaultEdit);
	EXPECT_GT(AfterDefaultEdit->Version, BeforeDefaultEdit->Version);
	EXPECT_NE(AfterDefaultEdit->Payload, BeforeDefaultEdit->Payload);
	ASSERT_TRUE(Second->GetValue(*Collection, Value.Id, Read));
	EXPECT_EQ(Read, FVector4(4.0, 3.0, 2.0, 1.0));
	const auto Counters = First->GetCounters();
	EXPECT_EQ(Counters.ChangedCommits, 2u);
	EXPECT_EQ(Counters.Publications, 3u);
	EXPECT_EQ(Counters.AssetRefreshPublications, 1u);
	EXPECT_EQ(Counters.RejectedCommits, 1u);
}

TEST_F(FMaterialParameterCollectionTests,
	CompilerCaptureExcludesDefaultsAndEmitsIndexedUniformBindings)
{
	auto* Collection = Make<DMaterialParameterCollection>("CollectionCompiler");
	auto Value = MakeDeclaration("Color", EMaterialParameterType::Vector,
		FVector4(1.0, 0.0, 0.0, 1.0));
	ASSERT_TRUE(Collection->SetDeclarations(std::array{Value}));
	auto* Expression = Make<DMaterialExpressionCollectionParameter>(
		"CollectionExpression");
	Expression->Id = FGuid::NewGuid();
	Expression->Collection = Collection;
	Expression->ParameterId = Value.Id;
	const FMaterialExpressionInput Root{Expression->Id};
	const std::array<DMaterialExpression*, 1> Expressions{Expression};
	auto Built = MIR::BuildGraph(Expressions, std::span(&Root, 1));
	ASSERT_TRUE(Built);
	ASSERT_EQ(Built.Collections.size(), 1u);

	MIR::FCompilerInput Input{
		.IR = Built.IR, .Collections = Built.Collections,
		.Environment = {.CompilerIdentity = "collection-test",
			.Target = "test-target",
			.Dependencies = {{"/Tests/Collection", FXxHash128{1, 2}}}},
		.Sources = Built.Sources};
	Input.IR.SurfaceRoot.Inputs[0].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[0].ExpressionIndex = Built.Roots[0];
	Input.IR.SurfaceRoot.Inputs[0].Type = EMaterialProgramValueType::Float3;
	auto First = MIR::Normalize(Input);
	ASSERT_TRUE(First) << (First.Diagnostics.empty() ? "no diagnostic"
		: FormatMaterialError(First.Diagnostics.front().Error));
	ASSERT_EQ(First.ActiveCollections.size(), 1u);
	const auto Generated = GenerateMaterialProgramSlang(
		First.IR, First.Layout, First.ActiveCollections);
	ASSERT_TRUE(Generated);
	EXPECT_NE(Generated.Source.find("MaterialCollection0.Value0"),
		std::string::npos);

	ASSERT_TRUE(Collection->SetDefaultValue(Value.Id,
		FVector4(0.0, 1.0, 0.0, 1.0)));
	auto Rebuilt = MIR::BuildGraph(Expressions, std::span(&Root, 1));
	ASSERT_TRUE(Rebuilt);
	Input.IR = Rebuilt.IR;
	Input.IR.SurfaceRoot.Inputs[0].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[0].ExpressionIndex = Rebuilt.Roots[0];
	Input.IR.SurfaceRoot.Inputs[0].Type = EMaterialProgramValueType::Float3;
	Input.Collections = Rebuilt.Collections;
	Input.Sources = Rebuilt.Sources;
	const auto DefaultsChanged = MIR::Normalize(Input);
	ASSERT_TRUE(DefaultsChanged);
	EXPECT_EQ(DefaultsChanged.Identity, First.Identity);
}

TEST_F(FMaterialParameterCollectionTests,
	DefaultEditReusesShaderIdentityAndCookedProgramRetainsFallbackLayout)
{
	Durin::Testing::FScopedRHIBackendOverride Backend("vulkan");
	auto* Collection = Make<DMaterialParameterCollection>("CollectionCook");
	auto Value = MakeDeclaration("Color", EMaterialParameterType::Vector,
		FVector4(1.0, 0.0, 0.0, 0.0));
	ASSERT_TRUE(Collection->SetDeclarations(std::array{Value}));
	auto* Expression = Make<DMaterialExpressionCollectionParameter>(
		"CollectionCookExpression");
	Expression->Id = FGuid::NewGuid();
	Expression->Collection = Collection;
	Expression->ParameterId = Value.Id;
	auto* Material = Make<DMaterial>("CollectionCookMaterial");
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor = FMaterialNumericInput(
		FMaterialExpressionInput{Expression->Id}, 3);
	const std::array<DMaterialExpression*, 1> Expressions{Expression};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	(void)FAssetCompilingManager::Get().FinishAllCompilation();
	const auto First = Material->GetAcceptedCompiledProgram();
	ASSERT_TRUE(First);
	ASSERT_EQ(First->ActiveCollections.size(), 1u);
	const auto FirstIdentity = First->Identity;
	const auto FirstDefaults = First->ActiveCollections.front().DefaultPayload;

	ASSERT_TRUE(Collection->SetDefaultValue(Value.Id,
		FVector4(0.0, 1.0, 0.0, 0.0)));
	(void)FAssetCompilingManager::Get().FinishAllCompilation();
	const auto Refreshed = Material->GetAcceptedCompiledProgram();
	ASSERT_TRUE(Refreshed);
	EXPECT_EQ(Refreshed->Identity, FirstIdentity);
	EXPECT_EQ(Material->GetMaterialCompileStatus().CacheOutcome,
		EMaterialCompileCacheOutcome::RetainedHit);
	ASSERT_EQ(Refreshed->ActiveCollections.size(), 1u);
	EXPECT_NE(Refreshed->ActiveCollections.front().DefaultPayload,
		FirstDefaults);

	FByteBuffer Bytes;
	FMaterialOperationResult Error;
	ASSERT_TRUE((Error = Testing::EncodeMaterialCookedProgramFamilyForTest(*Refreshed,
		Material->GetStaticProperties(), ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, Bytes))) << FormatMaterialError(Error.Error);
	FMaterialStaticProperties DecodedProperties;
	std::shared_ptr<const FMaterialCompilerResult> Decoded;
	ASSERT_TRUE((Error = Testing::DecodeMaterialCookedProgramFamilyForTest(Bytes,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game,
		*Refreshed, DecodedProperties, Decoded))) << FormatMaterialError(Error.Error);
	ASSERT_TRUE(Decoded);
	ASSERT_EQ(Decoded->ActiveCollections.size(), 1u);
	EXPECT_EQ(Decoded->Identity, FirstIdentity);
	EXPECT_EQ(Decoded->ActiveCollections.front(),
		Refreshed->ActiveCollections.front());
}

TEST_F(FMaterialParameterCollectionTests,
	ExpandedClosureRejectsMoreThanFourCollections)
{
	std::vector<DMaterialExpression*> Expressions;
	std::vector<FMaterialExpressionInput> Roots;
	for (uint32 Index = 0; Index < 5; ++Index)
	{
		auto* Collection = Make<DMaterialParameterCollection>(
			std::format("Collection{}", Index));
		auto Value = MakeDeclaration(std::format("Value{}", Index));
		ASSERT_TRUE(Collection->SetDeclarations(std::array{Value}));
		auto* Expression = Make<DMaterialExpressionCollectionParameter>(
			std::format("Expression{}", Index));
		Expression->Id = FGuid::NewGuid();
		Expression->Collection = Collection;
		Expression->ParameterId = Value.Id;
		Expressions.push_back(Expression);
		Roots.push_back({Expression->Id});
	}
	const auto Built = MIR::BuildGraph(Expressions, Roots);
	ASSERT_FALSE(Built);
	ASSERT_FALSE(Built.Diagnostics.empty());
	EXPECT_EQ(Built.Diagnostics.front().Error.Code,
		FMaterialError::FCode(EMaterialExpressionError::CollectionCountExceedsBound));
}

TEST_F(FMaterialParameterCollectionTests,
	FunctionExpansionPreservesCollectionDependencyAndSourceLocation)
{
	auto* Collection = Make<DMaterialParameterCollection>("FunctionCollection");
	auto Value = MakeDeclaration("Color", EMaterialParameterType::Vector,
		FVector4(0.25, 0.5, 0.75, 0.0));
	ASSERT_TRUE(Collection->SetDeclarations(std::array{Value}));
	auto* Parameter = Make<DMaterialExpressionCollectionParameter>(
		"FunctionCollectionParameter");
	Parameter->Id = FGuid::NewGuid();
	Parameter->Collection = Collection;
	Parameter->ParameterId = Value.Id;
	FMaterialFunctionSignature Signature;
	Signature.Outputs.push_back({.Id = FGuid::NewGuid(),
		.Type = EMaterialProgramValueType::Float3, .Name = "Color"});
	auto* Terminal = Make<DMaterialExpressionFunctionOutput>("FunctionTerminal");
	Terminal->Id = FGuid::NewGuid();
	Terminal->Port = Signature.Outputs.front();
	Terminal->Source = {Parameter->Id};
	auto* Function = Make<DMaterialFunction>("CollectionFunction");
	const std::array<DMaterialExpression*, 2> Body{Parameter, Terminal};
	ASSERT_TRUE(Function->SetFunctionExpressions(
		Testing::WithFunctionPorts(Signature, Body)));

	auto* Call = Make<DMaterialExpressionFunctionCall>("CollectionFunctionCall");
	Call->Id = FGuid::NewGuid();
	Call->Function = Function;
	Call->Outputs = {{Signature.Outputs.front().Id,
		EMaterialProgramValueType::Float3}};
	const std::array<DMaterialExpression*, 1> RootExpressions{Call};
	MIR::FGraphBuilder Builder(RootExpressions,
		{.FindFunction = [](const DMaterialFunctionInterface& Candidate)
			-> std::optional<MIR::FFunctionBody> {
				if (const auto* Concrete = Cast<DMaterialFunction>(&Candidate))
					return Concrete->GetExpressionBody();
				return std::nullopt;
			}});
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor.Connection = {.ExpressionId = Call->Id,
		.OutputId = Signature.Outputs.front().Id};
	const auto Built = Builder.FinishSurface(Outputs);
	ASSERT_TRUE(Built) << (Built.Diagnostics.empty() ? "no diagnostic"
		: FormatMaterialError(Built.Diagnostics.front().Error));
	ASSERT_EQ(Built.Collections.size(), 1u);
	EXPECT_EQ(Built.Collections.front().CollectionId,
		Collection->GetCollectionId());
	ASSERT_FALSE(Built.Sources.empty());
	EXPECT_FALSE(Built.Sources.back().FunctionAssetPath.empty());
}

TEST(FMaterialParameterCollectionPersistenceTests,
	AssetRoundTripPreservesIdentitySchemaDefaultsAndCanonicalLayout)
{
	using namespace Durin;
	Testing::FScopedMountRegistryFixture MountRegistry;
	InitializeDObjectSystem();
	const auto Root = Testing::GetTestWorkDirectory() / "MaterialCollections";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests(
		"/MaterialCollectionTests/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/MaterialCollectionTests/Environment", Path));
	DMaterialParameterCollection* Collection = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Collection));
	auto Exposure = MakeDeclaration("Exposure", EMaterialParameterType::Scalar,
		FVector4(2.0, 99.0, 98.0, 97.0));
	auto Tint = MakeDeclaration("Tint", EMaterialParameterType::Vector,
		FVector4(0.25, 0.5, 0.75, 42.0));
	ASSERT_TRUE(Collection->SetDeclarations(std::array{Tint, Exposure}));
	const FGuid CollectionId = Collection->GetCollectionId();
	const auto Layout = Collection->BuildLayout();
	ASSERT_TRUE(Layout);
	ASSERT_TRUE(SavePackage(Collection->GetPackage()));
	auto* AuthoringMaterial = NewObject<DMaterial>(nullptr,
		"CollectionGraphAuthoring");
	FMaterialGraphCreationRequest Request{
		.Action = MakeCollectionParameterCreationAction(
			Collection->GetObjectPath(), Tint.Id, "Tint"),
		.X = 120, .Y = 80};
	const auto Created = FMaterialGraphDocument(*AuthoringMaterial).Create(
		Request, nullptr);
	ASSERT_TRUE(Created) << FormatMaterialGraphCommandResult(Created);
	ASSERT_EQ(Created.GeneratedNodeIds.size(), 1u);
	const auto Authored = std::ranges::find_if(
		AuthoringMaterial->GetExpressionCollection().Expressions,
		[&](const auto& Expression) {
			return Expression->Id == Created.GeneratedNodeIds.front();
		});
	ASSERT_NE(Authored,
		AuthoringMaterial->GetExpressionCollection().Expressions.end());
	const auto* CollectionExpression =
		Cast<DMaterialExpressionCollectionParameter>(Authored->Get());
	ASSERT_NE(CollectionExpression, nullptr);
	EXPECT_EQ(CollectionExpression->Collection.Get(), Collection);
	EXPECT_EQ(CollectionExpression->ParameterId, Tint.Id);
	const auto View = FMaterialGraphDocument(*AuthoringMaterial).Inspect();
	const auto Read = std::ranges::find(View.Nodes, Created.GeneratedNodeIds.front(), [](const auto& Node) { return Node.Node.Id; });
	ASSERT_NE(Read, View.Nodes.end());
	EXPECT_EQ(Read->Node.Opcode, EMaterialProgramOpcode::CollectionParameter);
	EXPECT_EQ(Read->Node.ResultType, EMaterialProgramValueType::Float3);
	EXPECT_EQ(Read->PrimaryLabel, "Collection Parameter");
	ASSERT_EQ(Read->Outputs.size(), 1);
	EXPECT_EQ(Read->Outputs.front().Type, EMaterialProgramValueType::Float3);

	MarkObjectHierarchyAsGarbage(AuthoringMaterial);
	ASSERT_TRUE(UnloadPackage(Path));

	const auto LoadedResult = LoadObject<DMaterialParameterCollection>(
		Testing::MakePackageLeafAssetObjectPathForTests(Path));
	ASSERT_TRUE(LoadedResult);
	auto* Loaded = *LoadedResult;
	EXPECT_EQ(Loaded->GetCollectionId(), CollectionId);
	ASSERT_EQ(Loaded->GetDeclarations().size(), 2u);
	const auto* LoadedExposure = Loaded->FindDeclaration(Exposure.Id);
	const auto* LoadedTint = Loaded->FindDeclaration(Tint.Id);
	ASSERT_NE(LoadedExposure, nullptr);
	ASSERT_NE(LoadedTint, nullptr);
	EXPECT_EQ(LoadedExposure->DefaultValue, FVector4(2.0, 0.0, 0.0, 0.0));
	EXPECT_EQ(LoadedTint->DefaultValue, FVector4(0.25, 0.5, 0.75, 0.0));
	const auto LoadedLayout = Loaded->BuildLayout();
	ASSERT_TRUE(LoadedLayout);
	EXPECT_EQ(LoadedLayout->UniformLayout.Identity,
		Layout->UniformLayout.Identity);
	EXPECT_EQ(LoadedLayout->DefaultPayload, Layout->DefaultPayload);
	ASSERT_TRUE(UnloadPackage(Path));
}
