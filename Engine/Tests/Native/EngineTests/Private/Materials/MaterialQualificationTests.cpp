#include "MaterialGraphDocument.h"
#include "ExplicitMaterialProgramTestFixture.h"
#include "MaterialVariantTestFixture.h"
#include "MaterialTestSupport.h"
#include "TypedMaterialGraphTestFixture.h"
#include "MaterialGraphOperations.h"
#include "Misc/MountPathTestSupport.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialCookedProgram.h"
#include "Hash/XxHash.h"
#include "ObjectCacheContext.h"

#include <iostream>

namespace
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	auto MakeSyntheticMaterialCompilerInput() -> Durin::MIR::FCompilerInput
	{
		InitializeDObjectSystem();
		auto Recipe = Durin::Testing::MakePBRMaterialExpressionsForTest();
		std::vector<Durin::DMaterialExpression*> Expressions;
		for (const auto& Expression : Recipe.Expressions) Expressions.push_back(Expression.Get());
		Durin::MIR::FGraphBuilder Context(Expressions);
		auto Built = Context.FinishSurface(Recipe.Outputs);
		check(Built);
		Durin::MIR::FCompilerInput Input{.IR = std::move(Built.IR), .Parameters = std::move(Built.Parameters),
			.Sources = std::move(Built.Sources)};
		Input.Environment.CompilerIdentity = "slang-test-build;target=spirv;profile=spirv_1_5";
		Input.Environment.Target = "vulkan-spirv-1.5";
		Input.Environment.Dependencies = {
			{"/Engine/MaterialTemplate.slang", {11, 12}},
			{"/Engine/StaticMeshBasePass.slang", {21, 22}}};
		return Input;
	}

}

TEST(FMaterialQualificationTests, MaximumGraphLayoutLatency)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "MaximumLayoutMaterial");
	ASSERT_NE(Material, nullptr);
	Testing::FTestMaterialExpressionGraph Graph;
	while (Graph.Expressions.size() < MaterialProgramMaxNodeCount)
		Graph.Expressions.emplace_back(Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>().Get());
	ASSERT_TRUE(Graph.Apply(*Material));
	const uint64 SemanticRevision =
		Material->GetMaterialCompileStatus().AuthoredRevision;

	const auto Begin = std::chrono::steady_clock::now();
	const FMaterialGraphCommandResult First =
		FMaterialGraphDocument(*Material).Layout();
	const auto Duration = std::chrono::steady_clock::now() - Begin;
	ASSERT_TRUE(First) << First.Message;
	EXPECT_LT(Duration, std::chrono::seconds(1));
	EXPECT_EQ(Material->GetMaterialGraphPresentation().Nodes.size(),
		MaterialProgramMaxNodeCount);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphPresentation FirstLayout =
		Material->GetMaterialGraphPresentation();
	const FMaterialGraphView LayoutView = FMaterialGraphDocument(*Material).Inspect();
	for (size_t A = 0; A < LayoutView.Nodes.size(); ++A)
		for (size_t B = A + 1; B < LayoutView.Nodes.size(); ++B)
		{
			const auto& PositionA = LayoutView.Nodes[A].Presentation;
			const auto& PositionB = LayoutView.Nodes[B].Presentation;
			// This fixture contains only header-only scalar constants.
			const float HeightA = FMaterialGraphGeometry::GetMetrics().HeaderHeight;
			const float HeightB = HeightA;
			const float Width = 112.0f;
			EXPECT_FALSE(PositionA.X < PositionB.X + Width
				&& PositionA.X + Width > PositionB.X
				&& PositionA.Y < PositionB.Y + HeightB
				&& PositionA.Y + HeightA > PositionB.Y);
		}
	const FMaterialGraphCommandResult Second =
		FMaterialGraphDocument(*Material).Layout();
	EXPECT_EQ(Second.Status, EMaterialGraphCommandStatus::NoChange);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), FirstLayout);
	std::vector<std::chrono::microseconds> Samples;
	Samples.reserve(100);
	for (uint32 Sample = 0; Sample < 100; ++Sample)
	{
		const auto SampleBegin = std::chrono::steady_clock::now();
		EXPECT_TRUE(FMaterialGraphDocument(*Material).Layout());
		Samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - SampleBegin));
	}
	std::ranges::sort(Samples);
	RecordProperty("LayoutMedianMicroseconds", Samples[50].count());
	RecordProperty("LayoutP95Microseconds", Samples[95].count());
	EXPECT_LT(Samples[50], std::chrono::milliseconds(25));
	EXPECT_LT(Samples[95], std::chrono::milliseconds(50));

	MarkAsGarbage(Material);
	CollectGarbage();
}

TEST(FMaterialQualificationTests, LargeGraphLoadBaseline)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "GraphWithoutLedger";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/GraphWithoutLedger/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/GraphWithoutLedger/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	Testing::FTestMaterialExpressionGraph Graph;
	for (int Index = 0; Index < 65; ++Index)
	{
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid();
		Definition.Name = std::format("Tint{}", Index);
		Definition.Type = EMaterialParameterType::Vector4;
		Definition.Value = FMaterialParameterValue::MakeVector4({0.1, 0.0, 0.3, 1.0});
		auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
		Parameter->Metadata.Id = Definition.Id; Parameter->Metadata.Name = Definition.Name;
		Parameter->DefaultValue = Definition.Value.GetVector4();
		Graph.Expressions.emplace_back(Parameter.Get());
	}
	ASSERT_TRUE(Graph.Apply(*Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage(), EAssetPackageSaveMode::Complete));
	const auto CompleteBytes = std::filesystem::file_size(Root / "Base.dasset");
	for (int Round = 0; Round < 2; ++Round)
	{
		ASSERT_TRUE(SavePackage(Material->GetPackage()));
		const auto DeltaBytes = std::filesystem::file_size(Root / "Base.dasset");
		EXPECT_LE(DeltaBytes, CompleteBytes);
		ASSERT_TRUE(UnloadPackage(Path));
		const auto Begin = std::chrono::steady_clock::now();
		const auto LoadResult = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
		Material = LoadResult.value_or(nullptr);
		ASSERT_TRUE(LoadResult) << (LoadResult ? std::string{} : LoadResult.error().Message);
		const auto LoadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Begin).count();
		std::cout << "[ MATERIAL BASELINE ] nodes=65 round=" << Round
			<< " complete_bytes=" << CompleteBytes << " delta_bytes=" << DeltaBytes
			<< " load_ms=" << LoadMs << " ledger_allocated=" << Material->HasAllocatedAuthoredOverrideLedger() << '\n';
		ASSERT_NE(Material, nullptr);
		EXPECT_FALSE(Material->HasAllocatedAuthoredOverrideLedger());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 65u);
		auto* Copy = Cast<DMaterial>(DuplicateObject(Material, nullptr, "GraphWithoutLedgerCopy").value());
		ASSERT_NE(Copy, nullptr);
		EXPECT_FALSE(Copy->HasAllocatedAuthoredOverrideLedger());
		EXPECT_EQ(Copy->GetExpressionCollection().Expressions.size(), 65u);
		MarkObjectHierarchyAsGarbage(Copy);
	}
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage();
}

TEST(FMaterialQualificationTests, ColdAndWarmCompilerBaseline)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::MIR::FCompilerInput Input = MakeSyntheticMaterialCompilerInput();
	Durin::FMaterialOperationResult EnvironmentError;
	ASSERT_TRUE((EnvironmentError = Durin::BuildDefaultMaterialCompilerEnvironment(
		Input.Environment))) << Durin::FormatMaterialError(EnvironmentError.Error);
	ASSERT_EQ(Input.Environment.Dependencies.size(), 1u);
	EXPECT_EQ(Input.Environment.Dependencies.front().VirtualPath,
		"/Engine/MaterialCompilerEnvironment");
	EXPECT_FALSE(Input.Environment.Dependencies.front().ContentHash.IsZero());
	const auto Normalized = Durin::MIR::Normalize(Input);
	ASSERT_TRUE(Normalized);
	std::string FirstSource;
	std::string SecondSource;
	Durin::FMaterialOperationResult Error;
	const auto FirstSourceGeneration = Durin::GenerateMaterialProgramSlang(Normalized.IR);
	ASSERT_TRUE(FirstSourceGeneration);
	FirstSource = FirstSourceGeneration.Source;
	const auto SecondSourceGeneration = Durin::GenerateMaterialProgramSlang(Normalized.IR);
	ASSERT_TRUE(SecondSourceGeneration);
	SecondSource = SecondSourceGeneration.Source;
	EXPECT_EQ(FirstSource, SecondSource);
	EXPECT_LE(FirstSource.size(), Durin::MaterialProgramMaxCanonicalBytes);
	EXPECT_NE(FirstSource.find("module DurinGeneratedMaterial"),
		std::string::npos);
	EXPECT_EQ(FirstSource.find(Input.Environment.Dependencies.front().VirtualPath),
		std::string::npos);

	const Durin::FMaterialCompilerResult Compiled =
		Durin::MIR::Compile(Input, true);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty()
		? "missing diagnostic"
		: Durin::FormatMaterialError(Compiled.Diagnostics.front().Error));
	EXPECT_EQ(Compiled.Identity, Normalized.Identity);
	ASSERT_EQ(Compiled.CompiledShaders.size(), 3u);
	EXPECT_EQ(Compiled.CompiledShaders[0].Reflection.ResourceBindings.size(), 24u);
	EXPECT_EQ(Compiled.CompiledShaders[1].Reflection.ResourceBindings.size(), 17u);
	EXPECT_TRUE(Compiled.CompiledShaders[2].Reflection.ResourceBindings.empty());
	std::vector CorruptedStages = Compiled.CompiledShaders;
	CorruptedStages[1].Reflection.ResourceBindings.back().BindingIndex = 99;
	std::string ReflectionError;
	EXPECT_FALSE(Durin::ValidateMaterialCompiledStages(
		CorruptedStages, Compiled.Layout));
	const Durin::FMaterialCompilerResult Warm =
		Durin::MIR::Compile(Input);
	ASSERT_TRUE(Warm) << (Warm.Diagnostics.empty()
		? "missing diagnostic" : Durin::FormatMaterialError(Warm.Diagnostics.front().Error));
	ASSERT_EQ(Warm.CompiledShaders.size(), Compiled.CompiledShaders.size());
	uint64 SpirvBytes = 0;
	for (size_t Index = 0; Index < Compiled.CompiledShaders.size(); ++Index)
	{
		EXPECT_EQ(Warm.CompiledShaders[Index].Hash,
			Compiled.CompiledShaders[Index].Hash);
		ASSERT_TRUE(Compiled.CompiledShaders[Index].Code);
		SpirvBytes += Compiled.CompiledShaders[Index].Code->size();
	}
	Durin::FByteBuffer CookedBytes;
	ASSERT_TRUE((Error = Durin::EncodeMaterialCookedProgram(Compiled, {},
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, CookedBytes))) << Durin::FormatMaterialError(Error.Error);
	RecordProperty("GeneratedSourceBytes", Compiled.GeneratedSource.size());
	RecordProperty("DependencyCount", Compiled.Dependencies.size());
	RecordProperty("SpirvBytes", SpirvBytes);
	RecordProperty("NormalizationMicroseconds",
		Compiled.Timings.NormalizationMicroseconds);
	RecordProperty("GenerationMicroseconds",
		Compiled.Timings.GenerationMicroseconds);
	RecordProperty("ColdCompilationMicroseconds",
		Compiled.Timings.CompilationMicroseconds);
	RecordProperty("WarmCompilationMicroseconds",
		Warm.Timings.CompilationMicroseconds);
	std::cout << "[MaterialIRCompilerBaseline] input_ir_nodes="
		<< Input.IR.Nodes.size()
		<< " ir_nodes=" << Normalized.IR.Nodes.size()
		<< " texture_samples=" << std::ranges::count_if(
			Normalized.IR.Nodes, [](const Durin::MIR::FNode& Node) {
				return Node.Opcode == Durin::EMaterialProgramOpcode::TextureSample2D;
			})
		<< " canonical_bytes=" << Normalized.CanonicalBytes.size()
		<< " generated_bytes=" << Compiled.GeneratedSource.size()
		<< " source_hash="
		<< Durin::FXxHash128::HashBuffer(Compiled.GeneratedSource).ToString()
		<< " identity=" << Compiled.Identity.Digest.ToString()
		<< " dependencies=" << Compiled.Dependencies.size()
		<< " spirv_bytes=" << SpirvBytes
		<< " cooked_bytes=" << CookedBytes.size()
		<< " normalize_us=" << Compiled.Timings.NormalizationMicroseconds
		<< " generate_us=" << Compiled.Timings.GenerationMicroseconds
		<< " cold_compile_us=" << Compiled.Timings.CompilationMicroseconds
		<< " warm_compile_us=" << Warm.Timings.CompilationMicroseconds << '\n';

	Durin::MIR::FModule InvalidIR = Normalized.IR;
	InvalidIR.Version++;
	std::string InvalidSource;
	EXPECT_FALSE(Durin::GenerateMaterialProgramSlang(InvalidIR));
	EXPECT_TRUE(InvalidSource.empty());
	Durin::MIR::FCompilerInput InvalidInput = Input;
	InvalidInput.Environment.Target.clear();
	const auto Failed = Durin::MIR::Compile(InvalidInput);
	EXPECT_FALSE(Failed);
	EXPECT_TRUE(Failed.CompiledShaders.empty());
	EXPECT_FALSE(Failed.Diagnostics.empty());
}

TEST(FMaterialQualificationTests, InstanceVariantPayloadBaseline)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	Durin::Testing::CheckInstanceVariantsForTest(true);
	Durin::ShutdownAssetCompilingManager();
}

TEST(FMaterialQualificationTests, ObjectQueryCacheCandidateCost)
{
	InitializeDObjectSystem();
	CollectGarbage();
	auto* Base = NewObject<DMaterial>(nullptr, "MeasuredQueryBase");
	std::vector<DMaterialInterface*> Materials{Base};
	for (uint32 I = 1; I < 32; ++I)
	{
		auto* Child = NewObject<DMaterialInstance>(nullptr, FName(std::format("MeasuredQuery{}", I).c_str()));
		auto* Parent = static_cast<FObjectProperty*>(Child->GetClass()->FindPropertyByName("Parent"));
		Parent->SetObjectPropertyValue(Child, Materials.back());
		Materials.push_back(Child);
	}
	std::vector<DObject*> Unrelated;
	using FClock = std::chrono::steady_clock;
	const auto Micros = [](auto Start) {
		return std::chrono::duration<double, std::micro>(FClock::now() - Start).count();
	};
	for (const uint32 Count : {0u, 2000u, 20000u})
	{
		while (Unrelated.size() < Count) Unrelated.push_back(NewObject<DObject>(nullptr, NAME_None));
		std::vector<double> LegacyTimes, ColdTimes, WarmTimes;
		uint64 ScannedObjects = 0, ScannedMaterials = 0;
		// One warm-up and sixteen recorded samples. Diagnostic CPU measurements,
		// deliberately without a machine-dependent latency pass/fail threshold.
		for (int32 Sample = -1; Sample < 16; ++Sample)
		{
			auto Start = FClock::now();
			std::vector<FObjectKey> Expected;
			for (auto* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
				if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material) && Material->IsDependent(Base))
					Expected.emplace_back(Material);
			std::ranges::sort(Expected);
			const double Legacy = Micros(Start);
			Start = FClock::now();
			FObjectCacheContext Context;
			std::vector<FObjectKey> Actual;
			for (auto* Material : Context.GetMaterialsAffectedByMaterial(Base)) Actual.emplace_back(Material);
			const double Cold = Micros(Start);
			EXPECT_EQ(Actual, Expected);
			EXPECT_EQ(Actual.size(), Materials.size());
			Start = FClock::now();
			uint64 Consumed = 0;
			for (uint32 Query = 0; Query < 8; ++Query)
				for (auto* Material : Context.GetMaterialsAffectedByMaterial(Base))
					Consumed += Material != nullptr;
			const double Warm = Micros(Start) / 8;
			EXPECT_EQ(Consumed, 8 * Materials.size());
			const auto& Stats = Context.GetDiagnostics();
			EXPECT_EQ(Stats.SnapshotCount, 1u);
			EXPECT_EQ(Stats.ParentTableBuildCount, 1u);
			EXPECT_EQ(Stats.QueryCount, 9u);
			ScannedObjects = Stats.ScannedObjectCount;
			ScannedMaterials = Stats.ScannedMaterialCount;
			if (Sample >= 0) { LegacyTimes.push_back(Legacy); ColdTimes.push_back(Cold); WarmTimes.push_back(Warm); }
		}
		std::ranges::sort(LegacyTimes);
		std::ranges::sort(ColdTimes);
		std::ranges::sort(WarmTimes);
		std::cout << "OBJECT_CACHE_MEASUREMENT unrelated=" << Count
			<< " candidates=" << ScannedObjects << " materials=" << ScannedMaterials
			<< " legacy_median_us=" << LegacyTimes[8] << " cold_median_us=" << ColdTimes[8]
			<< " warm_median_us=" << WarmTimes[8] << " cold_p95_us=" << ColdTimes[15] << '\n';
	}
	for (auto* Object : Unrelated) MarkAsGarbage(Object);
	for (auto* Material : Materials) MarkAsGarbage(Material);
	CollectGarbage();
}
