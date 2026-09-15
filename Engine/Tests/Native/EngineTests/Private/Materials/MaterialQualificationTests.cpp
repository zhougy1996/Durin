#include "ExplicitMaterialProgramTestFixture.h"
#include "MaterialVariantTestFixture.h"
#include "MaterialTestSupport.h"
#include "TypedMaterialGraphTestFixture.h"
#include "MaterialGraphOperations.h"
#include "Misc/MountPathTestSupport.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialCookedProgram.h"
#include "Hash/XxHash.h"

#include <iostream>

namespace
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	auto MakeSyntheticMaterialCompilerInput() -> Durin::FMaterialIRCompilerInput
	{
		InitializeDObjectSystem();
		auto Recipe = Durin::Testing::MakePBRMaterialExpressionsForTest();
		std::vector<Durin::DMaterialExpression*> Expressions;
		for (const auto& Expression : Recipe.Expressions) Expressions.push_back(Expression.Get());
		Durin::FMaterialExpressionBuildContext Context(Expressions);
		auto Built = Context.FinishSurface(Recipe.Outputs);
		check(Built);
		Durin::FMaterialIRCompilerInput Input{.IR = std::move(Built.IR), .Parameters = std::move(Built.Parameters),
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
		FMaterialGraphOperations::Layout(*Material);
	const auto Duration = std::chrono::steady_clock::now() - Begin;
	ASSERT_TRUE(First) << First.Message;
	EXPECT_LT(Duration, std::chrono::seconds(1));
	EXPECT_EQ(Material->GetMaterialGraphPresentation().Nodes.size(),
		MaterialProgramMaxNodeCount);
	EXPECT_EQ(Material->GetMaterialCompileStatus().AuthoredRevision,
		SemanticRevision);
	const FMaterialGraphPresentation FirstLayout =
		Material->GetMaterialGraphPresentation();
	const FMaterialGraphView LayoutView = FMaterialGraphOperations::Inspect(*Material);
	for (size_t A = 0; A < LayoutView.Nodes.size(); ++A)
		for (size_t B = A + 1; B < LayoutView.Nodes.size(); ++B)
		{
			const auto& PositionA = LayoutView.Nodes[A].Presentation;
			const auto& PositionB = LayoutView.Nodes[B].Presentation;
			const float HeightA = FMaterialGraphGeometry::GetNodeHeight(
				static_cast<uint32>(LayoutView.Nodes[A].Inputs.size()));
			const float HeightB = FMaterialGraphGeometry::GetNodeHeight(
				static_cast<uint32>(LayoutView.Nodes[B].Inputs.size()));
			const float Width = FMaterialGraphGeometry::GetMetrics().NodeWidth;
			EXPECT_FALSE(PositionA.X < PositionB.X + Width
				&& PositionA.X + Width > PositionB.X
				&& PositionA.Y < PositionB.Y + HeightB
				&& PositionA.Y + HeightA > PositionB.Y);
		}
	const FMaterialGraphCommandResult Second =
		FMaterialGraphOperations::Layout(*Material);
	EXPECT_EQ(Second.Status, EMaterialGraphCommandStatus::NoChange);
	EXPECT_EQ(Material->GetMaterialGraphPresentation(), FirstLayout);
	std::vector<std::chrono::microseconds> Samples;
	Samples.reserve(100);
	for (uint32 Sample = 0; Sample < 100; ++Sample)
	{
		const auto SampleBegin = std::chrono::steady_clock::now();
		EXPECT_TRUE(FMaterialGraphOperations::Layout(*Material));
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
		const auto LoadResult = LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Material);
		ASSERT_TRUE(LoadResult) << LoadResult.Message;
		const auto LoadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Begin).count();
		std::cout << "[ MATERIAL BASELINE ] nodes=65 round=" << Round
			<< " complete_bytes=" << CompleteBytes << " delta_bytes=" << DeltaBytes
			<< " load_ms=" << LoadMs << " ledger_allocated=" << Material->HasAllocatedAuthoredOverrideLedger() << '\n';
		ASSERT_NE(Material, nullptr);
		EXPECT_FALSE(Material->HasAllocatedAuthoredOverrideLedger());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 65u);
		auto* Copy = Cast<DMaterial>(DuplicateObject(Material, nullptr, "GraphWithoutLedgerCopy"));
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
	Durin::FMaterialIRCompilerInput Input = MakeSyntheticMaterialCompilerInput();
	std::string EnvironmentError;
	ASSERT_TRUE(Durin::BuildDefaultMaterialCompilerEnvironment(
		Input.Environment, EnvironmentError)) << EnvironmentError;
	ASSERT_EQ(Input.Environment.Dependencies.size(), 1u);
	EXPECT_EQ(Input.Environment.Dependencies.front().VirtualPath,
		"/Engine/MaterialCompilerEnvironment");
	EXPECT_FALSE(Input.Environment.Dependencies.front().ContentHash.IsZero());
	const auto Normalized = Durin::NormalizeMaterialIR(Input);
	ASSERT_TRUE(Normalized);
	std::string FirstSource;
	std::string SecondSource;
	std::string Error;
	ASSERT_TRUE(Durin::GenerateMaterialProgramSlang(
		Normalized.IR, FirstSource, Error)) << Error;
	ASSERT_TRUE(Durin::GenerateMaterialProgramSlang(
		Normalized.IR, SecondSource, Error)) << Error;
	EXPECT_EQ(FirstSource, SecondSource);
	EXPECT_LE(FirstSource.size(), Durin::MaterialProgramMaxCanonicalBytes);
	EXPECT_NE(FirstSource.find("module DurinGeneratedMaterial"),
		std::string::npos);
	EXPECT_EQ(FirstSource.find(Input.Environment.Dependencies.front().VirtualPath),
		std::string::npos);

	const Durin::FMaterialCompilerResult Compiled =
		Durin::CompileMaterialIR(Input, true);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty()
		? "missing diagnostic"
		: Compiled.Diagnostics.front().Message);
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
		Durin::CompileMaterialIR(Input);
	ASSERT_TRUE(Warm) << (Warm.Diagnostics.empty()
		? "missing diagnostic" : Warm.Diagnostics.front().Message);
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
	ASSERT_TRUE(Durin::EncodeMaterialCookedProgram(Compiled, {},
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, CookedBytes, Error)) << Error;
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
			Normalized.IR.Nodes, [](const Durin::FMaterialIRNode& Node) {
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

	Durin::FMaterialIR InvalidIR = Normalized.IR;
	InvalidIR.Version++;
	std::string InvalidSource;
	EXPECT_FALSE(Durin::GenerateMaterialProgramSlang(
		InvalidIR, InvalidSource, Error));
	EXPECT_TRUE(InvalidSource.empty());
	Durin::FMaterialIRCompilerInput InvalidInput = Input;
	InvalidInput.Environment.Target.clear();
	const auto Failed = Durin::CompileMaterialIR(InvalidInput);
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
