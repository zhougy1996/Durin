#include "MaterialGraphDocument.h"
#include "MaterialCookedProgramTestSupport.h"
#include "ExplicitMaterialProgramTestFixture.h"
#include "MaterialVariantTestFixture.h"
#include "MaterialTestSupport.h"
#include "TypedMaterialGraphTestFixture.h"
#include "MaterialGraphOperations.h"
#include "Misc/MountPathTestSupport.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialCookedProgram.h"
#include "Hash/XxHash.h"
#include "Materials/ObjectCacheContext.h"
#include "NativeQualificationSupport.h"
#include "RenderingThread.h"

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

	auto MakeMeasuredValue(const Durin::FMaterialParameterDefinition& Definition,
		float Seed) -> Durin::FMaterialParameterValue
	{
		using namespace Durin;
		if (Definition.Type == EMaterialParameterType::Scalar)
			return FMaterialParameterValue::MakeScalar(Seed);
		return FMaterialParameterValue::MakeVector4(
			{Seed, Seed + 0.01f, Seed + 0.02f, Seed + 0.03f});
	}

}

TEST(FMaterialQualificationTests, MaximumGraphLayoutLatency)
{
	InitializeDObjectSystem();
	DMaterial* Material = NewObject<DMaterial>(nullptr, "MaximumLayoutMaterial");
	ASSERT_NE(Material, nullptr);
	Testing::FTestMaterialExpressionGraph Graph;
	// Applying a graph materializes its required output owner.
	while (Graph.Expressions.size() + 1 < MaterialProgramMaxNodeCount)
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
		EXPECT_EQ(Material->GetExpressionCollection().Expressions.size(), 66u);
		auto* Copy = Cast<DMaterial>(DuplicateObject(Material, nullptr, "GraphWithoutLedgerCopy").value());
		ASSERT_NE(Copy, nullptr);
		EXPECT_FALSE(Copy->HasAllocatedAuthoredOverrideLedger());
		EXPECT_EQ(Copy->GetExpressionCollection().Expressions.size(), 66u);
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
	ASSERT_EQ(Compiled.CompiledShaders.size(), 4u);
	EXPECT_EQ(Compiled.CompiledShaders[0].Reflection.ResourceBindings.size(), 21u);
	EXPECT_EQ(Compiled.CompiledShaders[1].Reflection.ResourceBindings.size(), 14u);
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
	ASSERT_TRUE((Error = Durin::Testing::EncodeMaterialCookedProgramFamilyForTest(Compiled, {},
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

TEST(FMaterialQualificationTests, BoundedStaticSelectorVariantGrowth)
{
	using namespace Durin;
	using Type = EMaterialProgramValueType;
	FModuleManager::Get().LoadModule("RenderCore");
	for (const uint32 DeclarationCount : {1u, 4u, 8u})
	{
		Testing::FTestMaterialExpressionGraph Graph;
		auto& Base = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
			{}, {}, {.X = .05f});
		DMaterialExpression* Chain = &Base;
		std::vector<FMaterialCompilerEnvironment::FStaticBoolValue> StaticBools;
		for (uint32 Index = 0; Index < DeclarationCount; ++Index)
		{
			auto& DeclarationNode = Graph.Add(EMaterialProgramOpcode::StaticBool,
				Type::StaticBool, {}, {}, {});
			auto* Declaration = Cast<DMaterialExpressionStaticBool>(&DeclarationNode);
			ASSERT_NE(Declaration, nullptr);
			Declaration->Name = FName(std::format("QualificationStatic{}", Index));
			Declaration->DefaultValue = false;
			StaticBools.push_back({Declaration->DeclarationId, false});
			auto& Enabled = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
				{}, {}, {.X = .1f + .01f * static_cast<float>(Index)});
			auto& SwitchNode = Graph.Add(EMaterialProgramOpcode::StaticSwitch,
				Type::Float, {}, {}, {});
			auto* Switch = Cast<DMaterialExpressionStaticSwitch>(&SwitchNode);
			ASSERT_NE(Switch, nullptr);
			Switch->Condition = Testing::MakeLink(DeclarationNode);
			Switch->FalseValue.Connection = Testing::MakeLink(*Chain);
			Switch->TrueValue.Connection = Testing::MakeLink(Enabled);
			Chain = &SwitchNode;
		}
		std::ranges::sort(StaticBools, {},
			&FMaterialCompilerEnvironment::FStaticBoolValue::DeclarationId);
		auto& Low = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
			{}, {}, {.X = .2f});
		auto& High = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
			{}, {}, {.X = .3f});
		auto& QualityNode = Graph.Add(EMaterialProgramOpcode::QualitySwitch,
			Type::Float, {}, {}, {});
		auto* Quality = Cast<DMaterialExpressionQualitySwitch>(&QualityNode);
		ASSERT_NE(Quality, nullptr);
		Quality->DefaultValue.Connection = Testing::MakeLink(Low);
		Quality->Low = Testing::MakeLink(Low);
		Quality->High = Testing::MakeLink(High);
		auto& ES31 = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
			{}, {}, {.X = .4f});
		auto& SM5 = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
			{}, {}, {.X = .5f});
		auto& SM6 = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
			{}, {}, {.X = .6f});
		auto& FeatureNode = Graph.Add(EMaterialProgramOpcode::FeatureLevelSwitch,
			Type::Float, {}, {}, {});
		auto* Feature = Cast<DMaterialExpressionFeatureLevelSwitch>(&FeatureNode);
		ASSERT_NE(Feature, nullptr);
		Feature->DefaultValue.Connection = Testing::MakeLink(ES31);
		Feature->ES3_1 = Testing::MakeLink(ES31);
		Feature->SM5 = Testing::MakeLink(SM5);
		Feature->SM6 = Testing::MakeLink(SM6);
		auto& QualityFeature = Graph.Add(EMaterialProgramOpcode::Add, Type::Float,
			{Testing::MakeLink(QualityNode), Testing::MakeLink(FeatureNode)}, {}, {});
		auto& Result = Graph.Add(EMaterialProgramOpcode::Add, Type::Float,
			{Testing::MakeLink(*Chain), Testing::MakeLink(QualityFeature)}, {}, {});
		Graph.Outputs.Roughness.Connection = Testing::MakeLink(Result);
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Graph.Expressions) Expressions.push_back(Expression.Get());
		ASSERT_TRUE(MIR::FGraphBuilder::ValidateSurface(Expressions, Graph.Outputs));
		FMaterialCompilerEnvironment BaseEnvironment;
		ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(BaseEnvironment));
		std::vector<FMaterialCompilerResult> Programs;
		uint64 MaximumCanonicalBytes = 0, MaximumGeneratedBytes = 0;
		for (const auto QualityLevel : {EMaterialQualityLevel::Low,
			EMaterialQualityLevel::High})
			for (const auto FeatureLevel : {ERHIFeatureLevel::ES3_1,
				ERHIFeatureLevel::SM5, ERHIFeatureLevel::SM6})
			{
				MIR::FGraphBuilder Builder(Expressions, {.Quality = QualityLevel,
					.FeatureLevel = FeatureLevel, .StaticBools = StaticBools});
				auto Built = Builder.FinishSurface(Graph.Outputs);
				ASSERT_TRUE(Built);
				MIR::FCompilerInput Input{.IR = std::move(Built.IR)};
				Input.Environment = BaseEnvironment;
				Input.Environment.Quality = QualityLevel;
				Input.Environment.FeatureLevel = FeatureLevel;
				Input.Environment.StaticBools = StaticBools;
				auto Normalized = MIR::Normalize(Input);
				ASSERT_TRUE(Normalized);
				MaximumCanonicalBytes = std::max<uint64>(MaximumCanonicalBytes,
					Normalized.CanonicalBytes.size());
				auto Program = MIR::Compile(Input);
				ASSERT_TRUE(Program);
				MaximumGeneratedBytes = std::max<uint64>(MaximumGeneratedBytes,
					Program.GeneratedSource.size());
				Programs.push_back(std::move(Program));
			}
		std::vector<const FMaterialCompilerResult*> ProgramPointers;
		std::unordered_set<FMaterialProgramIdentity> Identities;
		for (const auto& Program : Programs)
		{
			ProgramPointers.push_back(&Program);
			Identities.insert(Program.Identity);
		}
		FByteBuffer Cooked;
		ASSERT_TRUE(EncodeMaterialCookedProgramFamily(ProgramPointers, {},
			ECookTargetPlatform::Win64, ECookTargetProfile::Game, Cooked));
		EXPECT_EQ(Programs.size(), MaterialCookedProgramMaxConfigurations);
		EXPECT_LE(Identities.size(), Programs.size());
		EXPECT_LE(MaximumCanonicalBytes, 256ull * 1024ull);
		EXPECT_LE(MaximumGeneratedBytes, 256ull * 1024ull);
		EXPECT_LE(Cooked.size(), 2ull * 1024ull * 1024ull);
		std::cout << "[MaterialSelectorGrowth] declarations=" << DeclarationCount
			<< " requested_variants=" << Programs.size()
			<< " distinct_programs=" << Identities.size()
			<< " max_canonical_bytes=" << MaximumCanonicalBytes
			<< " max_generated_bytes=" << MaximumGeneratedBytes
			<< " family_cooked_bytes=" << Cooked.size() << '\n';
	}
}

TEST(FMaterialQualificationTests, DynamicInstanceUpdateWorkloads)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModule("RenderCore");
	struct FScopedRenderingThread
	{
		bool bOwns = GetRenderCommandAdmissionState()
			== ERenderCommandAdmissionState::Stopped;
		FScopedRenderingThread() { if (bOwns) InitRenderingThread(); }
		~FScopedRenderingThread() { if (bOwns) ShutdownRenderingThread(); }
	} RenderingThread;
	auto* Base = NewObject<DMaterial>(nullptr, "DynamicUpdateWorkloadBase");
	ASSERT_TRUE(Testing::MakePBRMaterialExpressionsForTest().Apply(*Base));
	ASSERT_TRUE(Base->GetAcceptedCompiledProgram());
	AddToRoot(Base);
	std::vector<const FMaterialParameterDefinition*> Numeric;
	for (const auto& Active : Base->GetAcceptedCompiledProgram()->ActiveParameters)
		if (const auto* Definition = Base->FindParameterDefinition(Active.Id);
			Definition && (Definition->Type == EMaterialParameterType::Scalar
				|| Definition->Type == EMaterialParameterType::Vector4))
			Numeric.push_back(Definition);
	ASSERT_GE(Numeric.size(), 16u);

	using FClock = std::chrono::steady_clock;
	const auto Measure = [&](auto&& Work) {
		std::array<double, 3> Samples{};
		Work(0);
		for (uint32 Sample = 0; Sample < Samples.size(); ++Sample)
		{
			const auto Begin = FClock::now();
			Work(Sample + 1);
			Samples[Sample] = std::chrono::duration<double, std::micro>(
				FClock::now() - Begin).count();
		}
		std::ranges::sort(Samples);
		return std::pair{Samples[1], Samples[2]};
	};

	for (const uint32 InstanceCount : {1000u, 10000u})
	{
		std::vector<DMaterialInstance*> Instances;
		Instances.reserve(InstanceCount);
		for (uint32 Index = 0; Index < InstanceCount; ++Index)
		{
			auto* Instance = DMaterialInstance::CreateDynamic(Base, nullptr,
				FName(std::format("MeasuredDynamic{}", Index).c_str()));
			ASSERT_TRUE(Instance);
			Instances.push_back(Instance);
		}
		FlushRenderingCommands();
		for (const uint32 ParameterCount : {1u, 4u, 16u})
		{
			ResetMaterialDynamicParameterCounters();
			ResetMaterialRenderProxyCounters();
			Testing::FQualificationAllocationSampler IndividualAllocationSampler;
			const auto Individual = Measure([&](uint32 Sample) {
				const float Seed = (Sample & 1) ? 0.21f : 0.41f;
				for (auto* Instance : Instances)
					for (uint32 Parameter = 0; Parameter < ParameterCount; ++Parameter)
						ASSERT_TRUE(Instance->SetParameterValue(Numeric[Parameter]->Id,
							MakeMeasuredValue(*Numeric[Parameter], Seed + Parameter * 0.001f)));
			});
			FlushRenderingCommands();
			const auto IndividualAllocation = IndividualAllocationSampler.Finish();
			const auto IndividualCommits = GetMaterialDynamicParameterCounters();
			const auto IndividualProxy = GetMaterialRenderProxyCounters();
			EXPECT_EQ(IndividualCommits.ChangedCommitCount,
				static_cast<uint64>(InstanceCount) * ParameterCount * 4);
			EXPECT_EQ(IndividualCommits.OwnerPublicationCount,
				IndividualCommits.ChangedCommitCount);

			ResetMaterialDynamicParameterCounters();
			ResetMaterialRenderProxyCounters();
			Testing::FQualificationAllocationSampler BatchAllocationSampler;
			const auto Batched = Measure([&](uint32 Sample) {
				const float Seed = (Sample & 1) ? 0.61f : 0.81f;
				std::vector<FMaterialDynamicParameterUpdate> Updates;
				Updates.reserve(ParameterCount);
				for (uint32 Parameter = 0; Parameter < ParameterCount; ++Parameter)
					Updates.push_back(FMaterialDynamicParameterUpdate::Set(
						Numeric[Parameter]->Id,
						MakeMeasuredValue(*Numeric[Parameter], Seed + Parameter * 0.001f)));
				for (auto* Instance : Instances)
					ASSERT_TRUE(Instance->ApplyDynamicParameterUpdates(Updates));
			});
			FlushRenderingCommands();
			const auto BatchAllocation = BatchAllocationSampler.Finish();
			const auto BatchCommits = GetMaterialDynamicParameterCounters();
			const auto BatchProxy = GetMaterialRenderProxyCounters();
			EXPECT_EQ(BatchCommits.ChangedCommitCount,
				static_cast<uint64>(InstanceCount) * 4);
			EXPECT_EQ(BatchCommits.OwnerPublicationCount,
				BatchCommits.ChangedCommitCount);
			const float FinalSeed = 0.61f;
			std::vector<FMaterialDynamicParameterUpdate> NoOpUpdates;
			for (uint32 Parameter = 0; Parameter < ParameterCount; ++Parameter)
				NoOpUpdates.push_back(FMaterialDynamicParameterUpdate::Set(
					Numeric[Parameter]->Id,
					MakeMeasuredValue(*Numeric[Parameter], FinalSeed + Parameter * 0.001f)));
			const auto NoOp = Measure([&](uint32) {
				for (auto* Instance : Instances)
					ASSERT_TRUE(Instance->ApplyDynamicParameterUpdates(NoOpUpdates));
			});
			std::array RejectedUpdates{
				FMaterialDynamicParameterUpdate::Set(Numeric[0]->Id,
					MakeMeasuredValue(*Numeric[0], 0.17f)),
				FMaterialDynamicParameterUpdate::Clear(Numeric[0]->Id),
			};
			const auto Rejected = Measure([&](uint32) {
				for (auto* Instance : Instances)
					ASSERT_FALSE(Instance->ApplyDynamicParameterUpdates(RejectedUpdates));
			});
			std::cout << "MATERIAL_DYNAMIC_UPDATE_MEASUREMENT instances=" << InstanceCount
				<< " parameters=" << ParameterCount
				<< " individual_median_us=" << Individual.first
				<< " individual_p95_us=" << Individual.second
				<< " batch_median_us=" << Batched.first
				<< " batch_p95_us=" << Batched.second
				<< " noop_median_us=" << NoOp.first
				<< " noop_p95_us=" << NoOp.second
				<< " rejected_median_us=" << Rejected.first
				<< " rejected_p95_us=" << Rejected.second
				<< " individual_commits=" << IndividualCommits.ChangedCommitCount
				<< " batch_commits=" << BatchCommits.ChangedCommitCount
				<< " individual_waves=" << IndividualProxy.QueuedPublicationWaveCount
				<< " batch_waves=" << BatchProxy.QueuedPublicationWaveCount
				<< " individual_payload_bytes=" << IndividualProxy.CopiedMaterialPayloadBytes
				<< " batch_payload_bytes=" << BatchProxy.CopiedMaterialPayloadBytes
				<< " individual_peak_allocation_increase="
				<< IndividualAllocation.GetPeakIncrease()
				<< " batch_peak_allocation_increase="
				<< BatchAllocation.GetPeakIncrease()
				<< " allocation_samples=" << BatchAllocation.SampleCount << '\n';
		}
		for (auto* Instance : Instances) MarkAsGarbage(Instance);
		CollectGarbage();
	}
	RemoveFromRoot(Base);
	MarkAsGarbage(Base);
	CollectGarbage();
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
