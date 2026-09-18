#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "MaterialProgramTestFixture.h"

TEST(FMaterialDiagnosticTests, ExistingDomainSuccessAndExternalProviderFailuresRemainDistinct)
{
	using namespace Durin;
	EXPECT_TRUE(FMaterialOperationResult{});
	EXPECT_FALSE(FMaterialError(EMaterialLayoutError::None).HasError());
	EXPECT_FALSE(FMaterialError(EMaterialParameterError::None).HasError());
	const FMaterialError Layout(EMaterialLayoutError::InvalidField, 7);
	EXPECT_TRUE(Layout.HasError());
	EXPECT_EQ(Layout.Index, 7u);
	EXPECT_TRUE(Layout.ExternalDiagnostic.empty());
	const FGuid ParameterId{1, 2, 3, 4};
	const FMaterialError LayoutContext(FMaterialLayoutValidationResult{
		.Error = EMaterialLayoutError::InvalidField, .ParameterId = ParameterId, .FieldIndex = 7});
	EXPECT_EQ(LayoutContext.ParameterId, ParameterId);
	EXPECT_EQ(LayoutContext.Index, 7u);
	const auto Archive = FMaterialError::FromArchive({
		.Code = EArchiveFailureCode::TruncatedPayload, .Path = "Program/Layout", .Message = "opaque archive text"});
	EXPECT_EQ(Archive.ArchiveCode, EArchiveFailureCode::TruncatedPayload);
	EXPECT_EQ(Archive.ArchivePath, "Program/Layout");
	EXPECT_TRUE(Archive.ExternalDiagnostic.empty());
	const auto Provider = FMaterialError::FromExternal(
		EMaterialCompileError::ShaderCompilerFailed, std::string(1024, 'x'));
	EXPECT_EQ(Provider.Code, FMaterialError::FCode(EMaterialCompileError::ShaderCompilerFailed));
	EXPECT_EQ(Provider.ExternalDiagnostic.size(), MaterialProgramMaxDiagnosticMessageBytes);
	EXPECT_FALSE(FormatMaterialError(Provider).empty());
}

TEST(FMaterialDiagnosticTests, FailedCodecsDoNotPublishPartialProducts)
{
	using namespace Durin;
	MIR::FModule Invalid;
	Invalid.Version = 0;
	FByteBuffer Bytes{std::byte{1}, std::byte{2}};
	const auto Encoded = MIR::EncodeCanonical(Invalid, Bytes);
	EXPECT_FALSE(Encoded);
	EXPECT_EQ(Encoded.Error.Code, FMaterialError::FCode(EMaterialIRError::InvalidStructure));
	EXPECT_TRUE(Bytes.empty());
	EXPECT_TRUE(Encoded.Error.ExternalDiagnostic.empty());
	const auto Generated = GenerateMaterialProgramSlang(Invalid);
	EXPECT_FALSE(Generated);
	EXPECT_TRUE(Generated.Source.empty());
	ASSERT_FALSE(Generated.Diagnostics.empty());
	EXPECT_EQ(Generated.Diagnostics.front().Error.Code, Encoded.Error.Code);

	FMaterialStaticProperties Properties;
	Properties.OpacityMaskThreshold = 0.25f;
	const auto Original = Properties;
	auto Previous = std::make_shared<const FMaterialCompilerResult>();
	auto Program = Previous;
	const auto Decoded = DecodeMaterialCookedProgram({}, ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, Properties, Program);
	EXPECT_FALSE(Decoded);
	EXPECT_EQ(Decoded.Error.Code, FMaterialError::FCode(EMaterialCookError::CookedProgramByteExtentInvalid));
	EXPECT_EQ(Properties, Original);
	EXPECT_EQ(Program, Previous);
}

TEST(FMaterialProgramSchemaTests,
	TypedExpressionsAreReflectedBoundedAndDeterministicallyValid)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto First = Testing::MakePBRMaterialExpressionsForTest();
	const auto Second = Testing::MakePBRMaterialExpressionsForTest();
	ASSERT_EQ(First.Expressions.size(), Second.Expressions.size());
	EXPECT_EQ(First.Outputs, Second.Outputs);
	EXPECT_FALSE(First.Expressions.empty());
	EXPECT_LE(First.Expressions.size(), MaterialProgramMaxNodeCount);
	std::unordered_set<FGuid> NodeIds;
	size_t LinkCount = 8;
	std::vector<DMaterialExpression*> Expressions;
	for (size_t Index = 0; Index < First.Expressions.size(); ++Index)
	{
		const auto& Node = First.Expressions[Index];
		EXPECT_TRUE(Node->Id.IsValid()); EXPECT_TRUE(NodeIds.insert(Node->Id).second);
		EXPECT_LE(Node->GetAuthoredInputCount(), MaterialProgramMaxNodeInputCount);
		LinkCount += Node->GetAuthoredInputCount();
		ASSERT_EQ(Node->GetClass(), Second.Expressions[Index]->GetClass());
		Node->GetClass()->ForEachProperty([&](FProperty* Property) {
			EXPECT_TRUE(ArePropertyValuesIdentical(Property, Node.Get(), 0, Second.Expressions[Index].Get(), 0));
		});
		Expressions.push_back(Node.Get());
	}
	EXPECT_LE(LinkCount, MaterialProgramMaxLinkCount);
	const auto Validation = MIR::FGraphBuilder::ValidateSurface(Expressions, First.Outputs);
	EXPECT_TRUE(Validation); EXPECT_TRUE(Validation.Diagnostics.empty());
	EXPECT_NE(FMaterialExpressionCollection::StaticStruct()->FindPropertyByName("Expressions"), nullptr);
	EXPECT_NE(FMaterialExpressionSurfaceOutputs::StaticStruct()->FindPropertyByName("BaseColor"), nullptr);
	EXPECT_NE(DMaterialExpressionScalarConstant::StaticClass()->FindPropertyByName("Value"), nullptr);
	DStruct* Presentation = FMaterialGraphPresentation::StaticStruct();
	ASSERT_NE(Presentation, nullptr);
	EXPECT_EQ(Presentation->FindPropertyByName("bHasMaterialOutputPosition"), nullptr);
	EXPECT_EQ(Presentation->FindPropertyByName("MaterialOutputX"), nullptr);
	EXPECT_EQ(Presentation->FindPropertyByName("MaterialOutputY"), nullptr);
	EXPECT_EQ(DMaterial::StaticClass()->FindPropertyByName("Program"), nullptr);
	EXPECT_NE(DMaterial::StaticClass()->FindPropertyByName("ExpressionCollection"), nullptr);
	EXPECT_EQ(DMaterialInstance::StaticClass()->FindPropertyByName("ExpressionCollection"), nullptr);
}

TEST(FMaterialProgramSchemaTests,
	TypedValidatorRejectsMalformedOwnersLinksAndBoundsDeterministically)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto ExpectFailure = [&](const Testing::FTestMaterialExpressionGraph& Graph, EMaterialExpressionError Code) {
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Graph.Expressions) Expressions.push_back(Expression.Get());
		auto Validation = MIR::FGraphBuilder::ValidateSurface(Expressions, Graph.Outputs);
		EXPECT_FALSE(Validation);
		EXPECT_FALSE(Validation.Diagnostics.empty());
		if (!Validation.Diagnostics.empty()) EXPECT_EQ(Validation.Diagnostics.front().Error.Code, FMaterialError::FCode(Code))
			<< Durin::FormatMaterialError(Validation.Diagnostics.front().Error);
		return Validation;
	};
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Expressions[1]->Id = Graph.Expressions[0]->Id;
		ExpectFailure(Graph, EMaterialExpressionError::CollectionContainsNullOwnerInvalidGUIDDuplicateGUID);
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Expressions.emplace_back();
		ExpectFailure(Graph, EMaterialExpressionError::CollectionContainsNullOwnerInvalidGUIDDuplicateGUID);
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Outputs.Metallic.ExpressionId = {1, 2, 3, 4};
		ExpectFailure(Graph, EMaterialExpressionError::InputDisconnectedRefersMissingExpression);
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Outputs.Metallic = Graph.Outputs.BaseColor;
		const auto Validation = ExpectFailure(Graph, EMaterialExpressionError::OutputSourceIncompatibleType);
		ASSERT_FALSE(Validation.Diagnostics.empty());
		EXPECT_EQ(Validation.Diagnostics.front().Category, EMaterialProgramDiagnosticCategory::Type);
		EXPECT_EQ(Validation.Diagnostics.front().LocationKind, EMaterialProgramDiagnosticLocationKind::SurfaceOutput);
		EXPECT_EQ(Validation.Diagnostics.front().LocationIndex, static_cast<uint32>(EMaterialSurfaceOutput::Metallic));
		EXPECT_EQ(Validation.Diagnostics.front().Error.ExpectedType, EMaterialProgramValueType::Float);
		EXPECT_EQ(Validation.Diagnostics.front().Error.ActualType, EMaterialProgramValueType::Float3);
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		TStrongObjectPtr<DMaterialExpressionScalarConstant> Constant(NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None));
		Constant->Id = FGuid::NewGuid(); Constant->Value = std::numeric_limits<float>::infinity();
		Graph.Expressions.emplace_back(Constant.Get());
		ExpectFailure(Graph, EMaterialExpressionError::NonFiniteConstant);
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		TStrongObjectPtr<DMaterialExpressionAdd> Invalid(NewObject<DMaterialExpressionAdd>(nullptr, NAME_None));
		Invalid->Id = FGuid::NewGuid(); Invalid->ResultType = static_cast<EMaterialProgramValueType>(255);
		Graph.Expressions.emplace_back(Invalid.Get());
		ExpectFailure(Graph, EMaterialExpressionError::NumericSignatureMismatch);
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		TStrongObjectPtr<DMaterialExpressionNegate> A(NewObject<DMaterialExpressionNegate>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionNegate> B(NewObject<DMaterialExpressionNegate>(nullptr, NAME_None));
		A->Id = FGuid::NewGuid(); B->Id = FGuid::NewGuid(); A->Input = {B->Id}; B->Input = {A->Id};
		Graph.Expressions.emplace_back(A.Get()); Graph.Expressions.emplace_back(B.Get());
		ExpectFailure(Graph, EMaterialExpressionError::InputsContainCycleExceedTraversalDepthBound);
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		FGuid Previous;
		for (uint32 Index = 0; Index <= MaterialProgramMaxDepth; ++Index)
		{
			TStrongObjectPtr<DMaterialExpressionNegate> Node(NewObject<DMaterialExpressionNegate>(nullptr, NAME_None));
			Node->Id = {0xde770001, 0, 0, Index + 1}; Node->Input = {Previous};
			Node->InputDefault = {0}; Previous = Node->Id;
			Graph.Expressions.emplace_back(Node.Get());
		}
		ExpectFailure(Graph, EMaterialExpressionError::BuildExceedsIRDepthBound);
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		while (Graph.Expressions.size() <= MaterialProgramMaxNodeCount)
		{
			TStrongObjectPtr<DMaterialExpressionScalarConstant> Node(NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None));
			Node->Id = FGuid::NewGuid(); Graph.Expressions.emplace_back(Node.Get());
		}
		ExpectFailure(Graph, EMaterialExpressionError::CollectionExceedsAuthoredNodeBound);
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		const auto Found = std::ranges::find_if(Graph.Expressions, [](const auto& Node) { return Cast<DMaterialExpressionParameter>(Node.Get()) != nullptr; });
		ASSERT_NE(Found, Graph.Expressions.end());
		auto* Parameter = Cast<DMaterialExpressionParameter>(Found->Get());
		Parameter->Metadata.Id = {};
		ExpectFailure(Graph, EMaterialExpressionError::ParameterExpressionsRequireValidParameterGUIDsMaterialOwner);
		Parameter->Metadata.Id = FGuid::NewGuid();
		Parameter->Metadata.DisplayName.assign(MaterialProgramMaxDisplayNameBytes + 1, 'x');
		const auto Forward = ExpectFailure(Graph, EMaterialExpressionError::ParameterExpressionMetadataDefaultInvalid);
		std::ranges::reverse(Graph.Expressions);
		const auto Reversed = ExpectFailure(Graph, EMaterialExpressionError::ParameterExpressionMetadataDefaultInvalid);
		EXPECT_EQ(Forward.Diagnostics, Reversed.Diagnostics);
	}
}

TEST(FMaterialProgramSchemaTests,
	BaseOwnsExpressionsAndInstancesShareWithoutDuplicatingThem)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Base(NewObject<DMaterial>(nullptr, "ExpressionOwningBase"));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, "ExpressionSharingInstance"));
	ASSERT_TRUE(Instance->SetParent(Base.Get()));
	auto Graph = Testing::MakePBRMaterialExpressionsForTest();
	std::ranges::reverse(Graph.Expressions);
	ASSERT_TRUE(Graph.Apply(*Base));
	const auto& Owned = Base->GetExpressionCollection().Expressions;
	ASSERT_EQ(Owned.size(), Graph.Expressions.size() + 1);
	for (size_t Index = 0; Index < Graph.Expressions.size(); ++Index)
	{
		EXPECT_EQ(Owned[Index]->Id, Graph.Expressions[Index]->Id);
		EXPECT_EQ(Owned[Index]->GetOuter(), Base.Get());
		EXPECT_NE(Owned[Index].Get(), Graph.Expressions[Index].Get());
	}
	EXPECT_EQ(Instance->GetParent(), Base.Get());
	EXPECT_TRUE(GDObjectArray.GetObjectsWithOuter(Instance.Get(), EObjectQueryScope::LiveOnly).empty());
	const auto BeforeChildren = Owned;
	const auto BeforeOutputs = Base->GetExpressionOutputs();
	Graph.Outputs.BaseColorDefault.x = std::numeric_limits<float>::quiet_NaN();
	EXPECT_FALSE(Graph.Apply(*Base));
	EXPECT_EQ(Base->GetExpressionCollection().Expressions, BeforeChildren);
	EXPECT_EQ(Base->GetExpressionOutputs(), BeforeOutputs);
}

TEST(FMaterialProgramNormalizationTests,
	SnapshotIsDetachedAndExcludesDynamicParameterValues)
{
	InitializeDObjectSystem();
	auto* Material = MakeExpandedMaterial("CompilerSnapshotMaterial");
	Durin::FMaterialCompilerEnvironment Environment =
		MakeSyntheticMaterialCompilerInput().Environment;
	Durin::MIR::FCompilerInput Before;
	auto Validation = Durin::SnapshotMaterialCompilerInput(
		*Material, Environment, Before);
	ASSERT_TRUE(Validation);
	ASSERT_TRUE(Material->SetScalarParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::MetallicName(), 0.87f));
	Durin::MIR::FCompilerInput After;
	ASSERT_TRUE((Validation = Durin::SnapshotMaterialCompilerInput(
		*Material, Environment, After)));
	EXPECT_EQ(Before.IR, After.IR);
	EXPECT_EQ(Before.Parameters, After.Parameters);
	EXPECT_EQ(Before.StaticProperties, After.StaticProperties);
	EXPECT_EQ(Before.Environment, After.Environment);
	for (const auto& Node : Before.IR.Nodes) EXPECT_TRUE(Node.HasValidPayload());
	EXPECT_EQ(Before.Parameters.size(),
		Material->GetParameterDefinitions().size());
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
	EXPECT_TRUE(Durin::MIR::Normalize(Before));
}

TEST(FMaterialProgramNormalizationTests,
	DefaultProgramUsesLiteralSurfaceRootWithoutOrdinaryNodes)
{
	InitializeDObjectSystem();
	Durin::MIR::FCompilerInput Input = MakeSyntheticMaterialCompilerInput();
	Durin::MIR::FGraphBuilder Empty(std::span<Durin::DMaterialExpression* const>{});
	Input.IR = Empty.FinishSurface({}).IR;
	Input.Parameters.clear();
	Input.Sources.clear();
	const Durin::MIR::FNormalizationResult Normalized =
		Durin::MIR::Normalize(Input);
	ASSERT_TRUE(Normalized);
	EXPECT_TRUE(Input.IR.Nodes.empty());
	ASSERT_TRUE(Normalized.IR.Nodes.empty());
	EXPECT_TRUE(Normalized.ActiveParameters.empty());
	EXPECT_FALSE(Normalized.IR.SurfaceRoot.bAggregate);
	EXPECT_EQ(Normalized.IR.SurfaceRoot.Inputs[0].Literal,
		(Durin::FMaterialProgramLiteral{0.5f, 0.5f, 0.5f, 0.0f}));
	EXPECT_EQ(Normalized.IR.SurfaceRoot.Inputs[1].Literal,
		(Durin::FMaterialProgramLiteral{0.0f, 0.0f, 1.0f, 0.0f}));
	std::string Source;
	Durin::FMaterialOperationResult Error;
	const auto SourceGeneration = Durin::GenerateMaterialProgramSlang(Normalized.IR);
	ASSERT_TRUE(SourceGeneration);
	Source = SourceGeneration.Source;
	EXPECT_EQ(Source.find("BaseColorTexture.Sample"), std::string::npos);
	EXPECT_EQ(Source.find("NormalTexture.Sample"), std::string::npos);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	const Durin::FMaterialCompilerResult Compiled =
		Durin::MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty()
		? std::string("no diagnostic") : Durin::FormatMaterialError(Compiled.Diagnostics.front().Error));
	size_t ActiveBindings = 0;
	for (const Durin::FCompiledShader& Shader : Compiled.CompiledShaders)
		ActiveBindings += Shader.Reflection.ResourceBindings.size();
	std::cout << "[MaterialOutputDefaultBaseline] input_ir_nodes="
		<< Input.IR.Nodes.size()
		<< " ir_nodes=" << Normalized.IR.Nodes.size()
		<< " texture_samples=0 generated_bytes=" << Source.size()
		<< " source_hash=" << Durin::FXxHash128::HashBuffer(Source).ToString()
		<< " identity=" << Compiled.Identity.Digest.ToString()
		<< " compiled_stages=" << Compiled.CompiledShaders.size()
		<< " active_bindings=" << ActiveBindings << '\n';
}

TEST(FMaterialProgramSchemaTests, AggregateInputRequiresMaterialAttributesType)
{
	InitializeDObjectSystem();
	auto Graph = Durin::Testing::MakePBRMaterialExpressionsForTest();
	Graph.Outputs.Surface = Graph.Outputs.BaseColor; Graph.Outputs.bUseMaterialAttributes = true;
	std::vector<Durin::DMaterialExpression*> Expressions;
	for (const auto& Expression : Graph.Expressions) Expressions.push_back(Expression.Get());
	auto Validation = Durin::MIR::FGraphBuilder::ValidateSurface(Expressions, Graph.Outputs);
	EXPECT_FALSE(Validation);
	ASSERT_FALSE(Validation.Diagnostics.empty());
	EXPECT_EQ(Validation.Diagnostics.front().Error.Code, Durin::FMaterialError::FCode(Durin::EMaterialExpressionError::AggregateMaterialOutputRequiresSurfaceExpression));
	EXPECT_NE(std::ranges::find(Validation.Diagnostics,
		Durin::EMaterialProgramDiagnosticCategory::Type,
		&Durin::FMaterialProgramDiagnostic::Category),
		Validation.Diagnostics.end());
}

TEST(FMaterialProgramNormalizationTests,
	EquivalentTypedInputsProduceIdenticalCanonicalIdentity)
{
	const Durin::MIR::FCompilerInput BaselineInput =
		MakeSyntheticMaterialCompilerInput();
	const Durin::MIR::FNormalizationResult Baseline =
		Durin::MIR::Normalize(BaselineInput);
	ASSERT_TRUE(Baseline);
	EXPECT_FALSE(Baseline.CanonicalBytes.empty());
	EXPECT_LE(Baseline.CanonicalBytes.size(),
		Durin::MaterialProgramMaxCanonicalBytes);
	constexpr std::string_view Domain = "DurinMaterialProgramIR";
	ASSERT_GT(Baseline.CanonicalBytes.size(), Domain.size());
	EXPECT_EQ(std::memcmp(
		Baseline.CanonicalBytes.data(), Domain.data(), Domain.size()), 0);
	EXPECT_EQ(Baseline.CanonicalBytes[Domain.size()], std::byte{0});

	const auto ExpectEquivalent = [&](Durin::MIR::FCompilerInput Candidate) {
		const auto Result = Durin::MIR::Normalize(Candidate);
		ASSERT_TRUE(Result);
		EXPECT_EQ(Result.IR, Baseline.IR);
		EXPECT_EQ(Result.CanonicalBytes, Baseline.CanonicalBytes);
		EXPECT_EQ(Result.Identity, Baseline.Identity);
		EXPECT_EQ(Result.ActiveParameters, Baseline.ActiveParameters);
	};

	Durin::MIR::FCompilerInput Reordered = BaselineInput;
	ReorderIndependentMaterialIRNodes(Reordered);
	ExpectEquivalent(std::move(Reordered));

	Durin::MIR::FCompilerInput Reidentified = BaselineInput;
	for (auto& Source : Reidentified.Sources) Source.NodeId = Durin::FGuid::NewGuid();
	ExpectEquivalent(std::move(Reidentified));

	Durin::MIR::FCompilerInput PresentationOnly = BaselineInput;
	for (auto& Source : PresentationOnly.Sources)
		Source.FunctionAssetPath = "ignored diagnostic source location";
	const auto Float3Constant = std::ranges::find_if(
		PresentationOnly.IR.Nodes, [](const auto& Node) {
			return Node.Opcode == Durin::EMaterialProgramOpcode::Constant
				&& Node.ResultType
					== Durin::EMaterialProgramValueType::Float3;
		});
	ASSERT_NE(Float3Constant, PresentationOnly.IR.Nodes.end());
	std::get<Durin::FMaterialProgramLiteral>(Float3Constant->Payload).W = 123.0f;
	ExpectEquivalent(std::move(PresentationOnly));

	Durin::MIR::FCompilerInput SignedZero = BaselineInput;
	const auto ZeroConstant = std::ranges::find_if(
		SignedZero.IR.Nodes, [](const auto& Node) {
			return Node.Opcode == Durin::EMaterialProgramOpcode::Constant
				&& Node.GetLiteral().X == 0.0f;
		});
	ASSERT_NE(ZeroConstant, SignedZero.IR.Nodes.end());
	std::get<Durin::FMaterialProgramLiteral>(ZeroConstant->Payload).X = -0.0f;
	ExpectEquivalent(std::move(SignedZero));

	Durin::MIR::FCompilerInput Swapped = BaselineInput;
	const auto Commutative = std::ranges::find_if(
		Swapped.IR.Nodes, [](const auto& Node) {
			return (Node.Opcode == Durin::EMaterialProgramOpcode::Add
				|| Node.Opcode == Durin::EMaterialProgramOpcode::Multiply
				|| Node.Opcode == Durin::EMaterialProgramOpcode::Minimum
				|| Node.Opcode == Durin::EMaterialProgramOpcode::Maximum)
				&& Node.Inputs.size() == 2;
		});
	ASSERT_NE(Commutative, Swapped.IR.Nodes.end());
	std::swap(Commutative->Inputs[0], Commutative->Inputs[1]);
	ExpectEquivalent(std::move(Swapped));

	Durin::MIR::FCompilerInput WithDeadNode = BaselineInput;
	Durin::MIR::FNode DeadNode;
	DeadNode.Opcode = Durin::EMaterialProgramOpcode::Constant;
	DeadNode.ResultType = Durin::EMaterialProgramValueType::Float;
	DeadNode.Payload = Durin::FMaterialProgramLiteral{-0.0f};
	WithDeadNode.IR.Nodes.push_back(std::move(DeadNode));
	ExpectEquivalent(std::move(WithDeadNode));
}

TEST(FMaterialProgramNormalizationTests, MaximumExpandedGraphPreservesCanonicalOrdering)
{
	using namespace Durin;
	auto Input = MakeSyntheticMaterialCompilerInput();
	Input.IR = MakeDefaultMaterialCompilerIR();
	Input.Parameters.clear(); Input.Sources.clear();
	// A balanced tree reaches the expanded-node bound without exceeding depth.
	for (uint32 Index = 0; Index < MaterialFunctionMaxExpandedNodes / 2; ++Index)
		Input.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
			.Payload = FMaterialProgramLiteral{static_cast<float>(Index % 17) / 17.f}});
	for (uint32 Index = 0; Input.IR.Nodes.size() < MaterialFunctionMaxExpandedNodes - 1; Index += 2)
		Input.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Add, .Inputs = {Index, Index + 1}});
	Input.IR.SurfaceRoot.Inputs[2].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[2].ExpressionIndex = static_cast<uint32>(Input.IR.Nodes.size() - 1);
	Input.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
		.Payload = FMaterialProgramLiteral{99.f}}); // Unreachable.
	const auto Baseline = MIR::Normalize(Input);
	ASSERT_TRUE(Baseline);
	EXPECT_EQ(Baseline.IR.Nodes.size(), MaterialFunctionMaxExpandedNodes - 1);
	for (auto& Node : Input.IR.Nodes)
		if (Node.Opcode == EMaterialProgramOpcode::Add) std::swap(Node.Inputs[0], Node.Inputs[1]);
	ReorderIndependentMaterialIRNodes(Input);
	const auto Reordered = MIR::Normalize(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Reordered.CanonicalBytes, Baseline.CanonicalBytes);
	EXPECT_EQ(Reordered.Identity, Baseline.Identity);
}

TEST(FMaterialProgramNormalizationTests, SharedDagKeysRemainBoundedAtMaximumDepth)
{
	using namespace Durin;
	auto Input = MakeSyntheticMaterialCompilerInput();
	MIR::FGraphBuilder Empty(std::span<DMaterialExpression* const>{});
	Input.IR = Empty.FinishSurface({}).IR;
	Input.Parameters.clear(); Input.Sources.clear();
	// Independent equal DAGs force structural comparisons at the depth boundary.
	std::array<uint32, 2> Roots;
	for (uint32 Branch = 0; Branch < 2; ++Branch)
	{
		uint32 Previous = 0;
		for (uint32 Level = 0; Level < MaterialProgramMaxDepth - 1; ++Level)
		{
			MIR::FNode Node{.Opcode = Level == 0 ? EMaterialProgramOpcode::Constant : EMaterialProgramOpcode::Add};
			if (Level == 0) Node.Payload = FMaterialProgramLiteral{.25f};
			else Node.Inputs = {Previous, Previous};
			Previous = static_cast<uint32>(Input.IR.Nodes.size());
			Input.IR.Nodes.push_back(std::move(Node));
		}
		Roots[Branch] = Previous;
	}
	Input.IR.SurfaceRoot.Inputs[2].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[2].ExpressionIndex = static_cast<uint32>(Input.IR.Nodes.size());
	Input.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Add, .Inputs = {Roots[0], Roots[1]}});
	const auto Baseline = MIR::Normalize(Input);
	ASSERT_TRUE(Baseline);
	EXPECT_EQ(Baseline.IR.Nodes.size(), Input.IR.Nodes.size());
	EXPECT_LT(Baseline.CanonicalBytes.size(), MaterialProgramMaxCanonicalBytes);
	std::swap(Input.IR.Nodes.back().Inputs[0], Input.IR.Nodes.back().Inputs[1]);
	ReorderIndependentMaterialIRNodes(Input);
	const auto Reordered = MIR::Normalize(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Reordered.Identity, Baseline.Identity);
	EXPECT_EQ(Reordered.CanonicalBytes, Baseline.CanonicalBytes);
	for (auto& Node : Input.IR.Nodes)
		if (Node.Opcode == EMaterialProgramOpcode::Constant) Node.Payload = FMaterialProgramLiteral{.5f};
	const auto Changed = MIR::Normalize(Input);
	ASSERT_TRUE(Changed);
	EXPECT_NE(Changed.Identity, Baseline.Identity);
}

TEST(FMaterialProgramNormalizationTests,
	CodeAffectingInputsParticipateInIdentityAndRuntimeStateDoesNot)
{
	const Durin::MIR::FCompilerInput BaselineInput =
		MakeSyntheticMaterialCompilerInput();
	const auto Baseline = Durin::MIR::Normalize(BaselineInput);
	ASSERT_TRUE(Baseline);
	const auto ExpectDifferent = [&](Durin::MIR::FCompilerInput Candidate) {
		const auto Result = Durin::MIR::Normalize(Candidate);
		ASSERT_TRUE(Result);
		EXPECT_NE(Result.Identity, Baseline.Identity);
	};

	Durin::MIR::FCompilerInput ProgramChange = BaselineInput;
	const auto Constant = std::ranges::find_if(
		ProgramChange.IR.Nodes, [](const auto& Node) {
			return Node.Opcode == Durin::EMaterialProgramOpcode::Constant
				&& Node.ResultType == Durin::EMaterialProgramValueType::Float;
		});
	ASSERT_NE(Constant, ProgramChange.IR.Nodes.end());
	std::get<Durin::FMaterialProgramLiteral>(Constant->Payload).X += 0.125f;
	ExpectDifferent(std::move(ProgramChange));

	Durin::MIR::FCompilerInput DependencyChange = BaselineInput;
	DependencyChange.Environment.Dependencies.front().ContentHash.HashLow++;
	ExpectDifferent(std::move(DependencyChange));
	Durin::MIR::FCompilerInput CompilerChange = BaselineInput;
	CompilerChange.Environment.CompilerIdentity += ";revision=2";
	ExpectDifferent(std::move(CompilerChange));
	Durin::MIR::FCompilerInput TargetChange = BaselineInput;
	TargetChange.Environment.Target = "vulkan-spirv-1.6";
	ExpectDifferent(std::move(TargetChange));
	Durin::MIR::FCompilerInput PassChange = BaselineInput;
	PassChange.Environment.PassContractVersion++;
	ExpectDifferent(std::move(PassChange));

	Durin::MIR::FCompilerInput BlendChange = BaselineInput;
	BlendChange.StaticProperties.BlendMode =
		Durin::EMaterialBlendMode::Masked;
	ExpectDifferent(std::move(BlendChange));
	Durin::MIR::FCompilerInput ShadingChange = BaselineInput;
	ShadingChange.StaticProperties.ShadingModel =
		Durin::EMaterialShadingModel::Unlit;
	ExpectDifferent(std::move(ShadingChange));
	Durin::MIR::FCompilerInput ThresholdChange = BaselineInput;
	ThresholdChange.StaticProperties.OpacityMaskThreshold = 0.5f;
	EXPECT_EQ(Durin::MIR::Normalize(ThresholdChange).Identity, Baseline.Identity);
	ThresholdChange.StaticProperties.BlendMode = Durin::EMaterialBlendMode::Masked;
	const auto MaskedIdentity = Durin::MIR::Normalize(ThresholdChange).Identity;
	ThresholdChange.StaticProperties.OpacityMaskThreshold = 0.75f;
	EXPECT_NE(Durin::MIR::Normalize(ThresholdChange).Identity, MaskedIdentity);
	ThresholdChange.StaticProperties.OpacityMaskThreshold = -0.0f;
	const auto ZeroIdentity = Durin::MIR::Normalize(ThresholdChange).Identity;
	ThresholdChange.StaticProperties.OpacityMaskThreshold = 0.0f;
	EXPECT_EQ(Durin::MIR::Normalize(ThresholdChange).Identity, ZeroIdentity);

	Durin::MIR::FCompilerInput RuntimeOnly = BaselineInput;
	RuntimeOnly.StaticProperties.bTwoSided = true;
	RuntimeOnly.StaticProperties.DepthWritePolicy =
		Durin::EMaterialDepthWritePolicy::Enabled;
	const auto RuntimeOnlyResult =
		Durin::MIR::Normalize(RuntimeOnly);
	ASSERT_TRUE(RuntimeOnlyResult);
	EXPECT_EQ(RuntimeOnlyResult.Identity, Baseline.Identity);

	Durin::MIR::FCompilerInput Invalid = BaselineInput;
	Invalid.Environment.Dependencies.push_back(
		Invalid.Environment.Dependencies.front());
	const auto InvalidResult = Durin::MIR::Normalize(Invalid);
	EXPECT_FALSE(InvalidResult);
	ASSERT_FALSE(InvalidResult.Diagnostics.empty());
	EXPECT_EQ(InvalidResult.Diagnostics.front().Category,
		Durin::EMaterialProgramDiagnosticCategory::Normalization);
}

TEST(FMaterialProgramCompilerTests,
	CanonicalIRGeneratesStableBoundedSourceAndCompleteStages)
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
	EXPECT_EQ(Compiled.CompiledShaders[0].Reflection.ResourceBindings.size(), 20u);
	EXPECT_EQ(Compiled.CompiledShaders[1].Reflection.ResourceBindings.size(), 13u);
	EXPECT_TRUE(Compiled.CompiledShaders[2].Reflection.ResourceBindings.empty());
	std::vector CorruptedStages = Compiled.CompiledShaders;
	CorruptedStages[1].Reflection.ResourceBindings.back().BindingIndex = 99;
	std::string ReflectionError;
	EXPECT_FALSE(Durin::ValidateMaterialCompiledStages(
		CorruptedStages, Compiled.Layout));

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

TEST(FMaterialProgramCompilerTests, NormalizedIRRecompilationPreservesSourceAndStages)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto Input = MakeSyntheticMaterialCompilerInput();
	Durin::FMaterialOperationResult Error;
	ASSERT_TRUE((Error = BuildDefaultMaterialCompilerEnvironment(Input.Environment))) << Durin::FormatMaterialError(Error.Error);
	const auto Baseline = MIR::Compile(Input);
	ASSERT_TRUE(Baseline);
	MIR::FCompilerInput Direct{.IR = Baseline.IR, .Parameters = Input.Parameters,
		.StaticProperties = Input.StaticProperties, .Environment = Input.Environment};
	const auto Compiled = MIR::Compile(Direct);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "Missing diagnostic" : Durin::FormatMaterialError(Compiled.Diagnostics.front().Error));
	EXPECT_EQ(Compiled.GeneratedSource, Baseline.GeneratedSource);
	EXPECT_EQ(Compiled.Layout, Baseline.Layout);
	ASSERT_EQ(Compiled.CompiledShaders.size(), Baseline.CompiledShaders.size());
	for (size_t Index = 0; Index < Compiled.CompiledShaders.size(); ++Index)
	{
		EXPECT_EQ(Compiled.CompiledShaders[Index].Frequency, Baseline.CompiledShaders[Index].Frequency);
		EXPECT_EQ(Compiled.CompiledShaders[Index].SourceEntryPoint, Baseline.CompiledShaders[Index].SourceEntryPoint);
		EXPECT_EQ(Compiled.CompiledShaders[Index].Hash, Baseline.CompiledShaders[Index].Hash);
	}
}

TEST(FMaterialProgramCompilerTests, DirectIRNormalizationPrunesDeadCodeAndRemapsSourcesDeterministically)
{
	using namespace Durin;
	MIR::FCompilerInput Input;
	Input.Environment = MakeSyntheticMaterialCompilerInput().Environment;
	for (uint32 Index = 0; Index < Input.IR.SurfaceRoot.Inputs.size(); ++Index)
		Input.IR.SurfaceRoot.Inputs[Index].Type = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index));
	Input.IR.Nodes = {
		{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.25f}},
		{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.5f}},
		{.Opcode = EMaterialProgramOpcode::Add, .ResultType = EMaterialProgramValueType::Float, .Inputs = {0, 1}},
		{.Opcode = EMaterialProgramOpcode::Splat3, .ResultType = EMaterialProgramValueType::Float3, .Inputs = {2}},
		{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{9.f}}};
	Input.IR.SurfaceRoot.Inputs[0].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[0].ExpressionIndex = 3;
	Input.Sources = {{.ExpressionIndex = 0, .NodeId = {1, 0, 0, 1}}, {.ExpressionIndex = 1, .NodeId = {1, 0, 0, 2}},
		{.ExpressionIndex = 4, .NodeId = {1, 0, 0, 3}}};
	for (uint32 Index = 0; Index < 32; ++Index) Input.Parameters.push_back({{2, 0, 0, Index + 1}, EMaterialParameterType::Texture});
	const auto Baseline = MIR::Normalize(Input);
	ASSERT_TRUE(Baseline);
	EXPECT_EQ(Baseline.IR.Nodes.size(), 4u);
	EXPECT_EQ(Baseline.Sources.size(), 2u);
	EXPECT_TRUE(Baseline.ActiveParameters.empty());
	std::swap(Input.IR.Nodes[0], Input.IR.Nodes[1]);
	Input.IR.Nodes[2].Inputs = {1, 0};
	Input.Sources[0].ExpressionIndex = 1; Input.Sources[1].ExpressionIndex = 0;
	Input.IR.SurfaceRoot.Inputs[0].Literal = {42, 43, 44};
	std::get<FMaterialProgramLiteral>(Input.IR.Nodes[0].Payload).W = 88;
	std::get<FMaterialProgramLiteral>(Input.IR.Nodes[4].Payload).X = 99;
	const auto Reordered = MIR::Normalize(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Reordered.Identity, Baseline.Identity);
	EXPECT_EQ(Reordered.IR, Baseline.IR);
	EXPECT_EQ(Reordered.CanonicalBytes, Baseline.CanonicalBytes);
	for (const auto& Source : Baseline.Sources)
	{
		const auto Found = std::ranges::find(Reordered.Sources, Source.NodeId, &MIR::FSource::NodeId);
		ASSERT_NE(Found, Reordered.Sources.end()); EXPECT_EQ(Found->ExpressionIndex, Source.ExpressionIndex);
	}
	Input.IR.Nodes[4].Payload = FMaterialProgramLiteral{std::numeric_limits<float>::quiet_NaN()};
	EXPECT_FALSE(MIR::Normalize(Input));
}

TEST(FMaterialProgramCompilerTests, DetachedIRRejectsMalformedInputsWithoutAuthoredGraphReconstruction)
{
	using namespace Durin;
	const auto Layout = CompileMaterialLayout({});
	ASSERT_TRUE(Layout);
	MIR::FModule IR;
	for (uint32 Index = 0; Index < IR.SurfaceRoot.Inputs.size(); ++Index)
		IR.SurfaceRoot.Inputs[Index].Type = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index));
	IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
		.ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.25f}});
	IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Saturate,
		.ResultType = EMaterialProgramValueType::Float, .Inputs = {0}});
	IR.SurfaceRoot.Inputs[3].bExpression = true;
	IR.SurfaceRoot.Inputs[3].ExpressionIndex = 1;
	ASSERT_TRUE(GenerateMaterialProgramSlang(IR, Layout.Layout));
	const auto Good = IR;
	const auto Reject = [&] {
		const auto Result = GenerateMaterialProgramSlang(IR, Layout.Layout);
		EXPECT_FALSE(Result);
		EXPECT_TRUE(Result.Source.empty());
		EXPECT_FALSE(Result.Diagnostics.empty());
		IR = Good;
	};
	IR.Nodes[1].Inputs = {1}; Reject();
	IR.Nodes[1].Inputs = {0xffffffffu}; Reject();
	IR.Nodes[1].Inputs.clear(); Reject();
	IR.Nodes[1].Opcode = EMaterialProgramOpcode::TextureCoordinates; Reject();
	IR.Nodes[0].Payload = std::monostate{}; Reject();
	IR.Nodes[1].Payload = FGuid{1, 2, 3, 4}; Reject();
	IR.Nodes[0].ResultType = EMaterialProgramValueType::Float2; Reject();
	IR.Nodes[0].Payload = FMaterialProgramLiteral{std::numeric_limits<float>::infinity()}; Reject();
	IR.Nodes[0].Opcode = EMaterialProgramOpcode::Parameter;
	IR.Nodes[0].Payload = FGuid{1, 2, 3, 4}; Reject();
	IR.Nodes[1].Opcode = EMaterialProgramOpcode::Swizzle;
	IR.Nodes[1].Payload = MIR::FSwizzle{1, {1}}; Reject();
	for (uint32 Index = 2; Index <= MaterialProgramMaxDepth; ++Index)
		IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Saturate,
			.ResultType = EMaterialProgramValueType::Float, .Inputs = {Index - 1}});
	Reject();
	IR.SurfaceRoot.Inputs[2].Literal.X = std::numeric_limits<float>::quiet_NaN(); Reject();
	IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
		.ResultType = EMaterialProgramValueType::Float3, .Payload = FMaterialProgramLiteral{}});
	for (uint32 Index = 0; Index < MaterialFunctionMaxExpandedLinks / 8; ++Index)
		IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::MakeSurface,
			.ResultType = EMaterialProgramValueType::Surface, .Inputs = {2, 2, 0, 0, 0, 2, 0, 0}});
	Reject();
}

TEST(FMaterialProgramCompilerTests, CompiledLayoutsAreTypedDeterministicAndDeviceBounded)
{
	using namespace Durin;
	std::vector<FMaterialCompilerParameterDeclaration> Parameters{
		{{0, 0, 0, 5}, EMaterialParameterType::Texture},
		{{0, 0, 0, 4}, EMaterialParameterType::Vector4},
		{{0, 0, 0, 3}, EMaterialParameterType::Vector},
		{{0, 0, 0, 2}, EMaterialParameterType::Vector2},
		{{0, 0, 0, 1}, EMaterialParameterType::Scalar}};
	const auto Built = CompileMaterialLayout(Parameters);
	ASSERT_TRUE(Built);
	EXPECT_TRUE(ValidateCompiledMaterialLayout(Built.Layout));
	EXPECT_EQ(Built.Layout.Identity.Version, 4u);
	EXPECT_EQ(Built.Layout.UniformPayloadSize, 80u);
	EXPECT_EQ(Built.Layout.UniformFieldCount, 4u);
	EXPECT_EQ(Built.Layout.ResourceFieldCount, 1u);
	for (uint32 Index = 0; Index < 4; ++Index)
	{
		EXPECT_EQ(Built.Layout.Fields[Index].Offset, 16u * (Index + 1));
		EXPECT_EQ(Built.Layout.Fields[Index].Size, 4u * (Index + 1));
	}
	std::ranges::reverse(Parameters);
	EXPECT_EQ(CompileMaterialLayout(Parameters).Layout, Built.Layout);
	auto Broken = Built.Layout;
	Broken.Fields[1].Offset = Broken.Fields[0].Offset;
	EXPECT_EQ(ValidateCompiledMaterialLayout(Broken).Error, EMaterialLayoutError::InvalidField);
	Broken = Built.Layout;
	Broken.Fields[1].Type = EMaterialRenderValueType::Vector4;
	EXPECT_FALSE(ValidateCompiledMaterialLayout(Broken));
	Broken = Built.Layout;
	Broken.Identity.Id = FGuid::NewGuid();
	EXPECT_EQ(ValidateCompiledMaterialLayout(Broken).Error, EMaterialLayoutError::InvalidIdentity);
	Parameters.push_back(Parameters.front());
	EXPECT_EQ(CompileMaterialLayout(Parameters).Validation.Error, EMaterialLayoutError::DuplicateParameter);
	Parameters.pop_back();
	FMaterialCompilerResourceLimits Limits;
	Limits.Samplers = 2;
	EXPECT_EQ(CompileMaterialLayout(Parameters, Limits).Validation.Error, EMaterialLayoutError::ResourceLimit);
	Limits = {};
	Limits.UniformBufferBytes = 64;
	EXPECT_EQ(CompileMaterialLayout(Parameters, Limits).Validation.Error, EMaterialLayoutError::ResourceLimit);
	Parameters.clear();
	for (uint32 Index = 0; Index < MaterialMaxParameterDefinitionCount; ++Index)
		Parameters.push_back({{0, 0, 0, Index + 1}, EMaterialParameterType::Vector4});
	EXPECT_TRUE(CompileMaterialLayout(Parameters));
	Parameters.push_back({FGuid::NewGuid(), EMaterialParameterType::Scalar});
	EXPECT_FALSE(CompileMaterialLayout(Parameters));
}

TEST(FMaterialProgramCompilerTests, CustomNumericTextureAndResourceFreeProgramsCompileWithTheirLayouts)
{
	using namespace Durin;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input;
	Input.IR = MakeDefaultMaterialCompilerIR();
	Durin::FMaterialOperationResult Error;
	ASSERT_TRUE((Error = BuildDefaultMaterialCompilerEnvironment(Input.Environment))) << Durin::FormatMaterialError(Error.Error);
	Input.StaticProperties.BlendMode = EMaterialBlendMode::Masked;
	const FGuid Tint{0, 0, 1, 1}, UV{0, 0, 1, 2}, Texture{0, 0, 1, 3}, Amount{0, 0, 1, 4};
	Input.Parameters = {{Tint, EMaterialParameterType::Vector4}, {UV, EMaterialParameterType::Vector4},
		{Texture, EMaterialParameterType::Texture}, {Amount, EMaterialParameterType::Scalar}};
	auto Add = [&](EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		FGuid Parameter, std::vector<uint32> Links = {}) {
		MIR::FNode Node{.Opcode = Opcode, .ResultType = Type, .Inputs = std::move(Links)};
		if (Parameter.IsValid()) Node.Payload = Parameter;
		const auto Index = static_cast<uint32>(Input.IR.Nodes.size());
		Input.IR.Nodes.push_back(std::move(Node));
		return Index;
	};
	const auto TintValue = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float4, Tint);
	const auto UVParameter = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float4, UV);
	const auto UVValue = Add(EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float2, {}, {UVParameter});
	Input.IR.Nodes.back().Payload = MIR::FSwizzle{2, {0, 1}};
	const auto TextureValue = Add(EMaterialProgramOpcode::TextureParameter, EMaterialProgramValueType::Texture2D, Texture);
	const auto Sample = Add(EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4, {}, {TextureValue, UVValue});
	const auto Product = Add(EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float4, {}, {TintValue, Sample});
	Input.IR.SurfaceRoot.Inputs[0].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[0].ExpressionIndex = Add(EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float3, {}, {Product});
	Input.IR.Nodes.back().Payload = MIR::FSwizzle{3, {0, 1, 2}};
	Input.IR.SurfaceRoot.Inputs[7].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[7].ExpressionIndex = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float, Amount);
	const auto Compiled = MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic" : Durin::FormatMaterialError(Compiled.Diagnostics.front().Error));
	EXPECT_EQ(Compiled.Layout.Identity.Version, 4u);
	EXPECT_EQ(Compiled.Layout.Fields.size(), 4u);
	EXPECT_EQ(Compiled.Layout.ResourceFieldCount, 1u);
	EXPECT_TRUE(ValidateMaterialCompiledStages(Compiled.CompiledShaders, Compiled.Layout));
	EXPECT_TRUE(ValidateMaterialCompilerResult(Compiled));
	FByteBuffer CookedBytes;
	ASSERT_TRUE((Error = EncodeMaterialCookedProgram(Compiled, Input.StaticProperties,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game, CookedBytes))) << Durin::FormatMaterialError(Error.Error);
	FMaterialStaticProperties CookedProperties;
	std::shared_ptr<const FMaterialCompilerResult> Cooked;
	ASSERT_TRUE((Error = DecodeMaterialCookedProgram(CookedBytes, ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, CookedProperties, Cooked))) << Durin::FormatMaterialError(Error.Error);
	ASSERT_NE(Cooked, nullptr);
	EXPECT_EQ(Cooked->Layout, Compiled.Layout);
	EXPECT_EQ(Cooked->ActiveParameters, Compiled.ActiveParameters);
	EXPECT_TRUE(Cooked->IR.Nodes.empty());
	EXPECT_TRUE(Cooked->GeneratedSource.empty());
	FByteBuffer Reencoded;
	ASSERT_TRUE((Error = EncodeMaterialCookedProgram(*Cooked, CookedProperties,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game, Reencoded)));
	EXPECT_EQ(Reencoded, CookedBytes);
	for (uint32 Mutation = 0; Mutation < 5; ++Mutation)
	{
		auto Invalid = Compiled;
		if (Mutation == 0) Invalid.Layout.Fields.front().Offset += 4;
		if (Mutation == 1) Invalid.Layout.Identity.Id = FGuid::NewGuid();
		if (Mutation == 2) Invalid.Layout.UniformPayloadSize += 16;
		if (Mutation == 3) Invalid.ActiveParameters.pop_back();
		if (Mutation == 4) Invalid.CompiledShaders.front().BinaryEntryPoint.clear();
		EXPECT_FALSE((Error = EncodeMaterialCookedProgram(Invalid, Input.StaticProperties,
			ECookTargetPlatform::Win64, ECookTargetProfile::Game, Reencoded)));
	}
	const auto AcceptedCooked = Cooked;
	for (size_t Position : {size_t{4}, CookedBytes.size() / 2, CookedBytes.size() - 1})
	{
		auto Broken = CookedBytes; Broken[Position] ^= std::byte{1};
		EXPECT_FALSE((Error = DecodeMaterialCookedProgram(Broken, ECookTargetPlatform::Win64,
			ECookTargetProfile::Game, CookedProperties, Cooked)));
		EXPECT_EQ(Cooked, AcceptedCooked);
	}
	auto LegacyCooked = CookedBytes; LegacyCooked[4] = std::byte{3};
	EXPECT_FALSE((Error = DecodeMaterialCookedProgram(LegacyCooked, ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, CookedProperties, Cooked)));
	EXPECT_EQ(Error.Error.Code, FMaterialError::FCode(EMaterialCookError::IncompatiblePayloadFormat));

	auto InvalidResult = Compiled;
	InvalidResult.ActiveParameters.pop_back();
	EXPECT_FALSE(ValidateMaterialCompilerResult(InvalidResult));
	InvalidResult = Compiled;
	InvalidResult.CompiledShaders.pop_back();
	EXPECT_FALSE(ValidateMaterialCompilerResult(InvalidResult));
	InvalidResult = Compiled;
	InvalidResult.bSucceeded = false;
	EXPECT_FALSE(ValidateMaterialCompilerResult(InvalidResult));
	EXPECT_NE(Compiled.GeneratedSource.find("MaterialTexture0.Sample(MaterialSampler0"), std::string::npos);
	EXPECT_EQ(Compiled.GeneratedSource.find("GetMaterialUV"), std::string::npos);
	ReorderIndependentMaterialIRNodes(Input);
	std::ranges::reverse(Input.Parameters);
	const auto Reordered = MIR::Normalize(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Reordered.Identity, Compiled.Identity);
	EXPECT_EQ(Reordered.Layout, Compiled.Layout);
	auto Corrupted = Compiled.CompiledShaders;
	ASSERT_FALSE(Corrupted.front().Reflection.ResourceBindings.empty());
	Corrupted.front().Reflection.ResourceBindings.front().Type = ERHIBindingType::StorageBuffer;
	EXPECT_FALSE(ValidateMaterialCompiledStages(Corrupted, Compiled.Layout));
	Corrupted = Compiled.CompiledShaders;
	Corrupted.front().Reflection.ResourceBindings.push_back(Corrupted.front().Reflection.ResourceBindings.front());
	EXPECT_FALSE(ValidateMaterialCompiledStages(Corrupted, Compiled.Layout));
	Corrupted.pop_back();
	EXPECT_FALSE(ValidateMaterialCompiledStages(Corrupted, Compiled.Layout));
	auto InvalidIR = Compiled.IR;
	InvalidIR.Nodes.front().Inputs = {0xffffffffu};
	EXPECT_FALSE(GenerateMaterialProgramSlang(InvalidIR, Compiled.Layout));

	Input.IR = MakeDefaultMaterialCompilerIR();
	Input.Parameters.clear();
	Input.StaticProperties = {.ShadingModel = EMaterialShadingModel::Unlit};
	const auto ResourceFree = MIR::Compile(Input);
	ASSERT_TRUE(ResourceFree) << (ResourceFree.Diagnostics.empty() ? "missing diagnostic" : Durin::FormatMaterialError(ResourceFree.Diagnostics.front().Error));
	EXPECT_TRUE(ResourceFree.Layout.Fields.empty());
	for (const auto& Stage : ResourceFree.CompiledShaders)
		EXPECT_TRUE(Stage.Reflection.ResourceBindings.empty()) << Stage.SourceEntryPoint;
}

TEST(FMaterialProgramCompilerTests, ExplicitUVAndSurfaceCompositionUseOnlyAuthoredInputs)
{
	using namespace Durin;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input;
	Input.IR = MakeDefaultMaterialCompilerIR();
	Durin::FMaterialOperationResult Error;
	ASSERT_TRUE((Error = BuildDefaultMaterialCompilerEnvironment(Input.Environment))) << Durin::FormatMaterialError(Error.Error);
	const FGuid Channel = FGuid::NewGuid(), Angle = FGuid::NewGuid();
	Input.Parameters = {{Channel, EMaterialParameterType::Scalar}, {Angle, EMaterialParameterType::Scalar}};
	auto Add = [&](EMaterialProgramOpcode Op, EMaterialProgramValueType Type,
		std::vector<uint32> Links = {}, FGuid Parameter = {}, FMaterialProgramLiteral Literal = {}) {
		MIR::FNode Node{.Opcode = Op, .ResultType = Type, .Inputs = std::move(Links)};
		if (Op == EMaterialProgramOpcode::Constant) Node.Payload = Literal;
		else if (Parameter.IsValid()) Node.Payload = Parameter;
		const auto Index = static_cast<uint32>(Input.IR.Nodes.size());
		Input.IR.Nodes.push_back(std::move(Node));
		return Index;
	};
	const auto ChannelValue = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float, {}, Channel);
	const auto AngleValue = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float, {}, Angle);
	const auto UV = Add(EMaterialProgramOpcode::UVChannel, EMaterialProgramValueType::Float2, {ChannelValue});
	const auto Sin = Add(EMaterialProgramOpcode::Sine, EMaterialProgramValueType::Float, {AngleValue});
	const auto Cos = Add(EMaterialProgramOpcode::Cosine, EMaterialProgramValueType::Float, {AngleValue});
	const auto Factor = Add(EMaterialProgramOpcode::MakeFloat2, EMaterialProgramValueType::Float2, {Sin, Cos});
	const auto Product = Add(EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float2, {UV, Factor});
	const auto X = Add(EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float, {Product});
	Input.IR.Nodes[X].Payload = MIR::FSwizzle{1, {0}};
	const auto Color = Add(EMaterialProgramOpcode::MakeFloat3, EMaterialProgramValueType::Float3, {X, Sin, Cos});
	const auto Normal = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float3, {}, {}, {0, 0, 1, 0});
	const auto Zero = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float);
	const auto One = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float, {}, {}, {1, 0, 0, 0});
	Input.IR.SurfaceRoot.bAggregate = true;
	Input.IR.SurfaceRoot.AggregateExpressionIndex = Add(EMaterialProgramOpcode::MakeSurface, EMaterialProgramValueType::Surface,
		{Color, Normal, Zero, One, One, Color, One, One});
	const auto Compiled = MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic" : Durin::FormatMaterialError(Compiled.Diagnostics.front().Error));
	ASSERT_TRUE(ValidateMaterialCompilerResult(Compiled));
	EXPECT_EQ(Compiled.Layout.Fields.size(), 2u);
	EXPECT_EQ(Compiled.Layout.ResourceFieldCount, 0u);
	EXPECT_NE(Compiled.GeneratedSource.find("SelectAuthoredUV(input,"), std::string::npos);
	EXPECT_EQ(Compiled.GeneratedSource.find("GetMaterialUV"), std::string::npos);
	EXPECT_EQ(Compiled.GeneratedSource.find("EvaluateStandardSurface"), std::string::npos);
	Input.IR.Nodes.back().Inputs.pop_back();
	EXPECT_FALSE(MIR::Normalize(Input));
	Input.IR.Nodes.back().Inputs.push_back(One);
	Input.IR.Nodes[2].Inputs[0] = Color;
	EXPECT_FALSE(MIR::Normalize(Input));


}

TEST(FMaterialProgramSchemaTests, RetiredIROpcodesAreRejectedWithoutMutatingInput)
{
	using namespace Durin;
	const auto Original = MakeSyntheticMaterialCompilerInput();
	for (uint8 Opcode : {uint8(3), uint8(30), uint8(255)})
	{
		auto Input = Original;
		ASSERT_FALSE(Input.IR.Nodes.empty());
		Input.IR.Nodes.front().Opcode = static_cast<EMaterialProgramOpcode>(Opcode);
		const auto Before = Input.IR;
		EXPECT_FALSE(MIR::Normalize(Input));
		EXPECT_EQ(Input.IR, Before);
	}
}

TEST(FMaterialProgramSchemaTests, EnvironmentInputsCompileWithoutMaterialParameters)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialExpressionWorldPosition> Position(NewObject<DMaterialExpressionWorldPosition>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialExpressionTime> Time(NewObject<DMaterialExpressionTime>(nullptr, NAME_None));
	Position->Id = FGuid::NewGuid(); Time->Id = FGuid::NewGuid();
	const std::array<DMaterialExpression*, 2> Expressions{Position.Get(), Time.Get()};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor = {Position->Id}; Outputs.Roughness = {Time->Id};
	Outputs.OpacityMask = {Time->Id};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	FMaterialCompilerEnvironment Environment;
	Durin::FMaterialOperationResult Error;
	ASSERT_TRUE((Error = BuildDefaultMaterialCompilerEnvironment(Environment))) << Durin::FormatMaterialError(Error.Error);
	MIR::FCompilerInput Input;
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, Environment, Input));
	const auto Normalized = MIR::Normalize(Input);
	ASSERT_TRUE(Normalized);
	const auto Source = GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout);
	ASSERT_TRUE(Source);
	EXPECT_NE(Source.Source.find("input.worldPosition"), std::string::npos);
	EXPECT_NE(Source.Source.find("Material.SurfaceParams.x"), std::string::npos);
	EXPECT_EQ(Source.Source.find("materialTime :"), std::string::npos);
	Input.StaticProperties.BlendMode = EMaterialBlendMode::Masked;
	const auto Compiled = MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic" : Durin::FormatMaterialError(Compiled.Diagnostics.front().Error));
	EXPECT_TRUE(ValidateMaterialCompilerResult(Compiled));
	for (const auto& Stage : Compiled.CompiledShaders)
	{
		const auto Uniform = std::ranges::find(Stage.Reflection.ResourceBindings, 2u,
			[](const auto& Binding) { return Binding.BindingIndex; });
		ASSERT_NE(Uniform, Stage.Reflection.ResourceBindings.end()) << Stage.SourceEntryPoint;
		EXPECT_EQ(Uniform->Name, "Material");
	}

	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::WorldPosition, EMaterialProgramValueType::Float4));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Time, EMaterialProgramValueType::Float3));
}

TEST(FMaterialProgramNormalizationTests, PackedAndIndividualInputsSharePropertyActivation)
{
	using namespace Durin;
	auto Individual = MakeSyntheticMaterialCompilerInput();
	auto Packed = Individual;
	std::vector<uint32> Attributes;
	for (const auto& Root : Packed.IR.SurfaceRoot.Inputs)
	{
		if (Root.bExpression) Attributes.push_back(Root.ExpressionIndex);
		else
		{
			Attributes.push_back(static_cast<uint32>(Packed.IR.Nodes.size()));
			Packed.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
				.ResultType = Root.Type, .Payload = Root.Literal});
		}
	}
	Individual.IR.Nodes = Packed.IR.Nodes;
	for (size_t Index = 0; Index < 8; ++Index)
	{
		Individual.IR.SurfaceRoot.Inputs[Index].bExpression = true;
		Individual.IR.SurfaceRoot.Inputs[Index].ExpressionIndex = Attributes[Index];
	}
	Packed.IR.SurfaceRoot.bAggregate = true;
	Packed.IR.SurfaceRoot.AggregateExpressionIndex = static_cast<uint32>(Packed.IR.Nodes.size());
	Packed.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::MakeSurface,
		.ResultType = EMaterialProgramValueType::Surface, .Inputs = Attributes});
	for (const auto Shading : {EMaterialShadingModel::Lit, EMaterialShadingModel::Unlit})
		for (const auto Blend : {EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent})
		{
			Individual.StaticProperties.ShadingModel = Packed.StaticProperties.ShadingModel = Shading;
			Individual.StaticProperties.BlendMode = Packed.StaticProperties.BlendMode = Blend;
			const auto A = MIR::Normalize(Individual), B = MIR::Normalize(Packed);
			ASSERT_TRUE(A); ASSERT_TRUE(B);
			EXPECT_FALSE(A.IR.SurfaceRoot.bAggregate); EXPECT_FALSE(B.IR.SurfaceRoot.bAggregate);
			for (size_t Index = 0; Index < 8; ++Index)
			{
				const bool bActive = IsMaterialSurfaceOutputActive(static_cast<EMaterialSurfaceOutput>(Index), Individual.StaticProperties);
				if (!bActive)
				{
					EXPECT_FALSE(A.IR.SurfaceRoot.Inputs[Index].bExpression);
					EXPECT_FALSE(B.IR.SurfaceRoot.Inputs[Index].bExpression);
				}
			}
			// Every inactive property's exclusive source is absent in both normalized graphs.
			EXPECT_EQ(A.IR.Nodes.size(), B.IR.Nodes.size());
			FByteBuffer BytesA, BytesB;
			Durin::FMaterialOperationResult Error;
			ASSERT_TRUE((Error = MIR::EncodeCanonical(A.IR, BytesA)));
			ASSERT_TRUE((Error = MIR::EncodeCanonical(B.IR, BytesB)));
			EXPECT_EQ(BytesA, BytesB);
		}
}
