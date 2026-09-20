#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "FunctionPortTestFixture.h"
#include "MaterialFunctionTestSupport.h"

TEST(FMaterialFunctionTests, StandardRecipesOwnTypedExpressionsAndPublishIndependentChildren)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	FStandardMaterialFunctions Functions;
	std::vector<TStrongObjectPtr<DMaterialFunction>> Owners;
	const std::array Slots{&Functions.UVTransform, &Functions.SampleNormal, &Functions.SampleORM,
		&Functions.StandardPBR, &Functions.StandardPBR_ORM, &Functions.ImportedSurfaceValues};
	for (uint32 Index = 0; Index < Slots.size(); ++Index)
	{
		auto Recipe = MakeStandardMaterialFunctionExpressions(static_cast<EStandardMaterialFunction>(Index + 1), Functions);
		ASSERT_FALSE(Recipe.Expressions.empty());
		const auto Interface = GetStandardMaterialFunctionInterface(static_cast<EStandardMaterialFunction>(Index + 1));
		EXPECT_TRUE(ValidateMaterialFunctionSignature(Interface));
		EXPECT_EQ(Recipe.GetSignature(), Interface);
		// Keep single-use bounds and flat normals inline. Shared emissive zero and
		// raw RGB masks remain meaningful; scalar sampling outputs need no mask.
		for (const auto& Expression : Recipe.Expressions)
		{
			EXPECT_FALSE(Expression->IsA<DMaterialExpressionScalarConstant>());
			if (const auto* Clamp = Cast<DMaterialExpressionClamp>(Expression.Get()))
			{
				EXPECT_FALSE(Clamp->Minimum.ExpressionId.IsValid());
				EXPECT_FALSE(Clamp->Maximum.ExpressionId.IsValid());
				EXPECT_EQ(Clamp->MinimumDefault, std::vector<float>{.045f});
				EXPECT_EQ(Clamp->MaximumDefault, std::vector<float>{1});
			}
			if (const auto* Lerp = Cast<DMaterialExpressionLerp>(Expression.Get()))
			{
				EXPECT_FALSE(Lerp->A.ExpressionId.IsValid());
				EXPECT_EQ(Lerp->ADefault, (std::vector<float>{0, 0, 1}));
			}
			if (const auto* Mask = Cast<DMaterialExpressionSwizzle>(Expression.Get()))
			{
				const auto Source = std::ranges::find_if(Recipe.Expressions, [&](const auto& N) { return N->Id == Mask->Input.ExpressionId; });
				ASSERT_NE(Source, Recipe.Expressions.end());
				if ((*Source)->IsA<DMaterialExpressionTextureSample2D>())
				{
					EXPECT_EQ(Mask->Input.OutputIndex, 0u);
					EXPECT_EQ(Mask->Components, (std::vector<uint8>{0, 1, 2}));
				}
			}
		}
		Owners.emplace_back(NewObject<DMaterialFunction>(nullptr, NAME_None));
		auto& Function = *Owners.back();
		const auto Result = Recipe.Apply(Function);
		ASSERT_TRUE(Result) << (Result.Diagnostics.empty() ? "" : Durin::FormatMaterialError(Result.Diagnostics.front().Error));
		*Slots[Index] = &Function;
		EXPECT_TRUE(Recipe.Matches(Function));
		const auto& Published = Function.GetExpressionCollection().Expressions;
		ASSERT_EQ(Published.size(), Recipe.Expressions.size());
		for (size_t Node = 0; Node < Published.size(); ++Node)
		{
			EXPECT_NE(Published[Node].Get(), Recipe.Expressions[Node].Get());
			EXPECT_EQ(Published[Node]->GetOuter(), &Function);
			EXPECT_EQ(Published[Node]->Id, Recipe.Expressions[Node]->Id);
		}
		CollectGarbage();
		EXPECT_TRUE(Recipe.Matches(Function));
		const auto Id = Published.front()->Id;
		Published.front()->Id = FGuid::NewGuid();
		EXPECT_FALSE(Recipe.Matches(Function));
		EXPECT_EQ(Recipe.Expressions.front()->Id, Id);
		ASSERT_TRUE(Recipe.Apply(Function));
		EXPECT_TRUE(Recipe.Matches(Function));
	}
	Owners.clear();
	CollectGarbage();
}

TEST(FMaterialFunctionTests, ExplicitMRTemplateRetainsIndependentInstanceParametersWithoutFunctions)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "PBRSurfaceMaterial_MR"));
	ASSERT_NE(Material.Get(), nullptr);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Recipe = AssetForge::Builtins::MakePBRSurfaceMaterialMRExpressions();
	ASSERT_TRUE(Recipe.Apply(*Material));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 48u);
	EXPECT_EQ(Recipe.Presentation.Nodes.size(), Recipe.Expressions.size());
	EXPECT_TRUE(Recipe.MatchesGraph(*Material));
	EXPECT_FALSE(Recipe.Outputs.Surface.ExpressionId.IsValid());
	for (const auto* Output : {&Recipe.Outputs.BaseColor, &Recipe.Outputs.Normal, &Recipe.Outputs.Metallic,
		&Recipe.Outputs.Roughness, &Recipe.Outputs.AmbientOcclusion, &Recipe.Outputs.Emissive,
		&Recipe.Outputs.Opacity, &Recipe.Outputs.OpacityMask}) EXPECT_TRUE(Output->ExpressionId.IsValid());
	CollectGarbage();
	EXPECT_TRUE(Recipe.MatchesGraph(*Material));
	for (size_t I = 0; I < Recipe.Expressions.size(); ++I)
	{
		EXPECT_NE(Recipe.Expressions[I].Get(), Material->GetExpressionCollection().Expressions[I].Get());
		EXPECT_EQ(Material->GetExpressionCollection().Expressions[I]->GetOuter(), Material.Get());
	}
	using Kind = Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterKind;
	for (uint32 Index = 0; Index < 8; ++Index)
	{
		const auto Role = static_cast<EMaterialSurfaceOutput>(Index);
		const auto Id = Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(Role, Kind::Texture);
		const auto Sample = std::ranges::find_if(Recipe.Expressions, [&](const auto& Expression) {
			const auto* Parameter = Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get());
			return Parameter && Parameter->Metadata.Id == Id;
		});
		ASSERT_NE(Sample, Recipe.Expressions.end());
		EXPECT_TRUE(Cast<DMaterialExpressionTextureSampleParameter2D>(Sample->Get())->UV.ExpressionId.IsValid());
	}
	auto InputCapture = SnapshotMaterialCompilerInput(*Material, {.CompilerIdentity = "ExplicitMRTemplate"});
	ASSERT_TRUE(InputCapture);
	auto& Input = InputCapture.Snapshot->Input;
	const auto Normalized = MIR::Normalize(Input);
	ASSERT_TRUE(Normalized);
	EXPECT_EQ(Normalized.Layout.ResourceFieldCount, 6u);
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, "MRInstance"));
	ASSERT_TRUE(Instance->SetParent(Material.Get()));
	const auto MetallicId = Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::Metallic, Kind::Value);
	const auto RoughnessId = Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::Roughness, Kind::Value);
	ASSERT_TRUE(Instance->SetParameterValue(MetallicId, FMaterialParameterValue::MakeScalar(.8f)));
	ASSERT_TRUE(Instance->SetParameterValue(RoughnessId, FMaterialParameterValue::MakeScalar(.2f)));
	FResolvedMaterialParameter Resolved;
	ASSERT_TRUE(Instance->ResolveParameterValue(MetallicId, Resolved));
	EXPECT_FLOAT_EQ(Resolved.Value.GetScalar(), .8f);
	ASSERT_TRUE(Instance->ResolveParameterValue(RoughnessId, Resolved));
	EXPECT_FLOAT_EQ(Resolved.Value.GetScalar(), .2f);
}

TEST(FMaterialFunctionTests, StructuralImportRecipesExposeOnlyRequiredOwners)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	std::array<FImportedSurfaceRole, 8> Roles;
	const FMaterialSurfaceOutputs Defaults;
	for (uint32 I = 0; I < 8; ++I)
		Roles[I].Value = GetMaterialSurfaceOutputDefault(Defaults, static_cast<EMaterialSurfaceOutput>(I));
	const auto Empty = MakeImportedSurfaceRecipe(Roles);
	EXPECT_TRUE(Empty.Graph.Expressions.empty());
	EXPECT_TRUE(Empty.Owners.empty());
	Roles[0].Sample = FImportedSurfaceSample{.ResourceIdentity = "first"};
	Roles[0].Value = {1, 1, 1};
	const auto Plain = MakeImportedSurfaceRecipe(Roles);
	ASSERT_EQ(Plain.Graph.Expressions.size(), 1u);
	EXPECT_EQ(Plain.Owners.size(), 1u);
	Roles[0].Sample->ResourceIdentity = "second";
	Roles[0].Sample->Sampler.AddressU = EMaterialSamplerAddressMode::ClampToEdge;
	const auto Renamed = MakeImportedSurfaceRecipe(Roles);
	EXPECT_EQ(Plain.CanonicalKey, Renamed.CanonicalKey);
	EXPECT_TRUE(Plain.Graph.MatchesGraph(Renamed.Graph));
	Roles[0].Value = {.2f, .3f, .4f};
	Roles[0].Sample->UVOffset = {.25f, .5f};
	const auto Transformed = MakeImportedSurfaceRecipe(Roles);
	EXPECT_EQ(Transformed.Graph.Expressions.size(), 8u);
	EXPECT_EQ(Transformed.Owners.size(), 3u);
	Roles[0].Value = {.6f, .7f, .8f};
	Roles[0].Sample->UVOffset = {.75f, .25f};
	const auto OtherValues = MakeImportedSurfaceRecipe(Roles);
	EXPECT_EQ(Transformed.CanonicalKey, OtherValues.CanonicalKey);
	EXPECT_TRUE(Transformed.Graph.MatchesGraph(OtherValues.Graph));
	auto* Material = NewObject<DMaterial>(nullptr, "StructuralImportRecipe");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(OtherValues.Graph.Apply(*Material));
	Roles[2].Value = {1};
	Roles[3].Value = {1};
	Roles[2].Sample = FImportedSurfaceSample{.ResourceIdentity = "packed", .Usage = ETextureUsage::DataMask, .OutputIndex = 4};
	Roles[3].Sample = FImportedSurfaceSample{.ResourceIdentity = "packed", .Usage = ETextureUsage::DataMask, .OutputIndex = 3};
	const auto Packed = MakeImportedSurfaceRecipe(Roles);
	EXPECT_EQ(Packed.Graph.Expressions.size(), 9u);
	EXPECT_EQ(Packed.Graph.Outputs.Metallic.ExpressionId, Packed.Graph.Outputs.Roughness.ExpressionId);
	ASSERT_TRUE(Packed.Graph.Apply(*Material));
	Roles[3].Sample->UVChannel = {1};
	const auto Split = MakeImportedSurfaceRecipe(Roles);
	EXPECT_NE(Packed.CanonicalKey, Split.CanonicalKey);
	EXPECT_NE(Split.Graph.Outputs.Metallic.ExpressionId, Split.Graph.Outputs.Roughness.ExpressionId);
	ASSERT_TRUE(Split.Graph.Apply(*Material));
	Roles[1].Sample = FImportedSurfaceSample{.ResourceIdentity = "normal", .Usage = ETextureUsage::Normal,
		.OutputIndex = 1};
	const auto Normal = MakeImportedSurfaceRecipe(Roles);
	EXPECT_EQ(Normal.Graph.Expressions.size(), Split.Graph.Expressions.size() + 1);
	ASSERT_TRUE(Normal.Graph.Apply(*Material));
	Roles[1].Sample.reset();
	EXPECT_TRUE(MakeImportedSurfaceRecipe(Roles).Graph.MatchesGraph(Split.Graph));
}

TEST(FMaterialFunctionTests, ExpandedAndFunctionRecipesPreserveCompilationAndIndependentOverrides)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Frozen = NewObject<DMaterial>(nullptr, "ExpandedBaseline");
	ASSERT_NE(Frozen, nullptr);
	ASSERT_TRUE(Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Frozen));
	ASSERT_EQ(Frozen->GetParameterDefinitions().size(), 48u);

	auto* Current = NewObject<DMaterial>(nullptr, "CurrentFunctionFixture");
	ASSERT_NE(Current, nullptr);
	Current->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(Testing::SetStandardMaterialExpressionsForTest(*Current));
	EXPECT_EQ(Current->GetExpressionCollection().Expressions.size(), 189u);
	for (const auto& Node : Current->GetExpressionCollection().Expressions)
	{
		EXPECT_FALSE(Node->IsA<DMaterialExpressionSaturate>());
		EXPECT_FALSE(Node->IsA<DMaterialExpressionClamp>());
	}
	EXPECT_FALSE(Current->GetExpressionOutputs().Surface.ExpressionId.IsValid());
	ASSERT_EQ(GetFunctionCalls(*Current).size(), 1u);
	EXPECT_EQ(GetFunctionCalls(*Current).front()->Function->GetName(), "StandardFunction2");
	const auto& Outputs = Current->GetExpressionOutputs();
	for (const auto& Output : {Outputs.BaseColor, Outputs.Normal, Outputs.Metallic, Outputs.Roughness,
		Outputs.AmbientOcclusion, Outputs.Emissive, Outputs.Opacity, Outputs.OpacityMask})
		EXPECT_TRUE(Output.ExpressionId.IsValid());
	const FMaterialCompilerEnvironment Environment{.CompilerIdentity = "FunctionFixtureParity"};
	auto FrozenInputCapture = SnapshotMaterialCompilerInput(*Frozen, Environment);
	ASSERT_TRUE(FrozenInputCapture);
	auto& FrozenInput = FrozenInputCapture.Snapshot->Input;
	auto CurrentInputCapture = SnapshotMaterialCompilerInput(*Current, Environment);
	ASSERT_TRUE(CurrentInputCapture);
	auto& CurrentInput = CurrentInputCapture.Snapshot->Input;
	const auto Baseline = MIR::Normalize(FrozenInput);
	const auto Candidate = MIR::Normalize(CurrentInput);
	ASSERT_TRUE(Baseline);
	ASSERT_TRUE(Candidate);
	EXPECT_NE(Baseline.CanonicalBytes, Candidate.CanonicalBytes);
	EXPECT_EQ(Baseline.Layout.Fields, Candidate.Layout.Fields);
	const auto BaselineSource = GenerateMaterialProgramSlang(Baseline.IR, Baseline.Layout);
	const auto CandidateSource = GenerateMaterialProgramSlang(Candidate.IR, Candidate.Layout);
	ASSERT_TRUE(BaselineSource);
	ASSERT_TRUE(CandidateSource);
	EXPECT_NE(BaselineSource.Source, CandidateSource.Source);
	EXPECT_NE(CandidateSource.Source.find("return EvaluateMaterialSurface(result)"), std::string::npos);
	ASSERT_EQ(Baseline.Layout.Fields.size(), 36u);
	const auto ArtifactRoot = Testing::GetTestWorkDirectory() / "MaterialAuthoringBaseline";
	std::filesystem::create_directories(ArtifactRoot);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Baseline.CanonicalBytes, ArtifactRoot / "canonical-ir.bin"));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(BaselineSource.Source)),
		ArtifactRoot / "generated.slang"));
	RecordProperty("BaselineExpressionCount", static_cast<int>(Baseline.IR.Nodes.size()));
	RecordProperty("BaselineParameterCount", static_cast<int>(Baseline.Layout.Fields.size()));

	// A nonidentity override on one map must never become a shared UV default.
	auto* Parent = NewObject<DMaterialInstance>(nullptr, "IndependentUVParent");
	auto* Child = NewObject<DMaterialInstance>(nullptr, "IndependentUVChild");
	ASSERT_TRUE(Parent->SetParent(Frozen));
	ASSERT_TRUE(Child->SetParent(Parent));
	using Kind = Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterKind;
	for (uint32 Index = 0; Index < 8; ++Index)
	{
		const auto Role = static_cast<EMaterialSurfaceOutput>(Index);
		for (const auto ParameterKind : {Kind::UVChannel, Kind::UVScale, Kind::UVOffset, Kind::UVRotation})
		{
			const auto Id = Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(Role, ParameterKind);
			const auto* Definition = Frozen->FindParameterDefinition(Id);
			ASSERT_NE(Definition, nullptr);
			const auto Value = ParameterKind == Kind::UVScale || ParameterKind == Kind::UVOffset
				? FMaterialParameterValue::MakeVector4({1.25f + Index, -.125f * Index, 0, 0})
				: FMaterialParameterValue::MakeScalar(ParameterKind == Kind::UVChannel
					? static_cast<float>(Index % 4) : .2f * (Index + 1));
			ASSERT_TRUE(Parent->SetParameterValue(Id, Value));
			FResolvedMaterialParameter Resolved;
			ASSERT_TRUE(Child->ResolveParameterValue(Id, Resolved));
			EXPECT_EQ(Resolved.Value, Value);
			EXPECT_FALSE(Child->IsParameterValueOrphan(Id));
		}
	}
	// Switching to the new recipe must preserve inherited values by GUID.
	ASSERT_TRUE(Parent->SetParent(Current));
	Parent->VisitLocalParameterValues([&](const FGuid& Id, const FMaterialParameterValue& Value) {
		FResolvedMaterialParameter Resolved;
		ASSERT_TRUE(Child->ResolveParameterValue(Id, Resolved));
		EXPECT_EQ(Resolved.Value, Value);
		EXPECT_FALSE(Parent->IsParameterValueOrphan(Id));
	});
	MarkAsGarbage(Child);
	MarkAsGarbage(Parent);
	MarkAsGarbage(Current);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, LiteralDefaultsMatchExplicitConstantsAndValidateTypes)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Constant = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
	auto* Product = NewObject<DMaterialExpressionMultiply>(nullptr, NAME_None);
	auto* Value = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
	Constant->Id = FGuid::NewGuid(); Constant->Value = .5f;
	Product->Id = FGuid::NewGuid(); Product->A = {Constant->Id}; Product->BDefault = {.25f};
	Value->Id = FGuid::NewGuid(); Value->Value = .25f;
	std::vector<DMaterialExpression*> Expressions{Constant, Product};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Roughness = {Product->Id};
	const auto Baseline = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Baseline);
	EXPECT_TRUE(Baseline.ActiveParameters.empty());
	Expressions.push_back(Value);
	Product->B = {Value->Id}; Product->BDefault.clear();
	const auto Expanded = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Expanded);
	EXPECT_EQ(Baseline.CanonicalBytes, Expanded.CanonicalBytes);
	EXPECT_EQ(Baseline.Layout, Expanded.Layout);
	Product->B = {}; Product->BDefault = {.25f, .5f};
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	Product->BDefault = {std::numeric_limits<float>::infinity()};
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	for (auto* Expression : Expressions) MarkAsGarbage(Expression);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, SamplingDefaultsToMeshUV0AndAcceptsSharedFloat2)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Owner = NewObject<DMaterialExpressionTextureSampleParameter2D>(nullptr, NAME_None);
	Owner->Id = FGuid::NewGuid(); Owner->Metadata = {.Id = FGuid::NewGuid(), .Name = "Texture"};
	auto* Sample = NewObject<DMaterialExpressionTextureSample2D>(nullptr, NAME_None);
	Sample->Id = FGuid::NewGuid(); Sample->Texture = {Owner->Id, 7};
	auto* UV = NewObject<DMaterialExpressionTextureCoordinates>(nullptr, NAME_None);
	UV->Id = FGuid::NewGuid();
	auto* Constant = NewObject<DMaterialExpressionVector2Constant>(nullptr, NAME_None);
	Constant->Id = FGuid::NewGuid(); Constant->Value = {.25, .75};
	auto* WrongType = NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None);
	WrongType->Id = FGuid::NewGuid();
	const std::array<DMaterialExpression*, 5> Expressions{Owner, Sample, UV, Constant, WrongType};
	const FMaterialExpressionSurfaceOutputs Outputs{.BaseColor = {Owner->Id, 1}, .Emissive = {Sample->Id, 1}};
	const auto Implicit = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Implicit);
	EXPECT_EQ(std::ranges::count(Implicit.IR.Nodes, EMaterialProgramOpcode::UVChannel, &MIR::FNode::Opcode), 2);
	EXPECT_EQ(std::ranges::count(Implicit.IR.Nodes, EMaterialProgramOpcode::Multiply, &MIR::FNode::Opcode), 0);
	const FMaterialExpressionSurfaceOutputs SingleOutput{.BaseColor = {Owner->Id, 1}};
	const auto ImplicitSingle = NormalizeTypedExpressions(Expressions, SingleOutput);
	ASSERT_TRUE(ImplicitSingle);
	Owner->UV = Sample->UV = {UV->Id};
	const auto Explicit = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Explicit);
	EXPECT_EQ(std::ranges::count(Explicit.IR.Nodes, EMaterialProgramOpcode::UVChannel, &MIR::FNode::Opcode), 1);
	const auto ExplicitSingle = NormalizeTypedExpressions(Expressions, SingleOutput);
	ASSERT_TRUE(ExplicitSingle);
	EXPECT_EQ(ImplicitSingle.CanonicalBytes, ExplicitSingle.CanonicalBytes);
	UV->ChannelDefault = {1};
	const auto ChannelOne = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(ChannelOne);
	EXPECT_NE(Implicit.CanonicalBytes, ChannelOne.CanonicalBytes);
	Owner->UV = Sample->UV = {Constant->Id};
	const auto Fixed = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Fixed);
	EXPECT_EQ(std::ranges::count(Fixed.IR.Nodes, EMaterialProgramOpcode::UVChannel, &MIR::FNode::Opcode), 0);
	Owner->UV = {WrongType->Id};
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	Owner->UV = {Constant->Id}; Sample->UV = {WrongType->Id};
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	for (auto* Expression : Expressions) MarkAsGarbage(Expression);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, CompactSamplingSharesFetchAndPreservesUVParameterReachability)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Sample = NewObject<DMaterialExpressionTextureSampleParameter2D>(nullptr, NAME_None);
	auto* Channel = NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None);
	auto* UV = NewObject<DMaterialExpressionTextureCoordinates>(nullptr, NAME_None);
	auto* Coordinates = NewObject<DMaterialExpressionVector2Constant>(nullptr, NAME_None);
	auto* Texture = NewObject<DMaterialExpressionTextureParameter>(nullptr, NAME_None);
	auto* ExplicitSample = NewObject<DMaterialExpressionTextureSample2D>(nullptr, NAME_None);
	Sample->Id = FGuid::NewGuid(); Sample->Metadata = {.Id = FGuid::NewGuid(), .Name = "Texture"};
	Channel->Id = FGuid::NewGuid(); Channel->Metadata = {.Id = FGuid::NewGuid(), .Name = "Channel"};
	UV->Id = FGuid::NewGuid(); UV->Channel = {Channel->Id}; Sample->UV = {UV->Id};
	Coordinates->Id = FGuid::NewGuid(); Coordinates->Value = FVector2{.25f, .75f};
	Texture->Id = FGuid::NewGuid(); Texture->Metadata = Sample->Metadata;
	ExplicitSample->Id = Sample->Id; ExplicitSample->Texture = {Texture->Id}; ExplicitSample->UV = {UV->Id};
	std::vector<DMaterialExpression*> Expressions{Sample, Channel, UV};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor = {Sample->Id, 1}; Outputs.Roughness = {Sample->Id, 3}; Outputs.Metallic = {Sample->Id, 4};
	const auto Baseline = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Baseline);
	EXPECT_EQ(std::ranges::count(Baseline.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &MIR::FNode::Opcode), 1);
	EXPECT_EQ(Baseline.ActiveParameters.size(), 2u);
	EXPECT_TRUE(GenerateMaterialProgramSlang(Baseline.IR, Baseline.Layout));
	const std::array<DMaterialExpression*, 4> Explicit{Texture, ExplicitSample, Channel, UV};
	const auto Expanded = NormalizeTypedExpressions(Explicit, Outputs);
	ASSERT_TRUE(Expanded);
	EXPECT_EQ(Baseline.CanonicalBytes, Expanded.CanonicalBytes);
	EXPECT_EQ(Baseline.Layout, Expanded.Layout);
	Expressions.push_back(Coordinates); Sample->UV = {Coordinates->Id};
	const auto Connected = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Connected);
	EXPECT_EQ(Connected.ActiveParameters.size(), 1u);
	Sample->UV = {UV->Id};
	EXPECT_EQ(NormalizeTypedExpressions(Expressions, Outputs).CanonicalBytes, Baseline.CanonicalBytes);
	Outputs.Metallic.OutputIndex = 7;
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	for (auto* Expression : Expressions) MarkAsGarbage(Expression);
	MarkAsGarbage(Texture); MarkAsGarbage(ExplicitSample);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, NormalRGBDecodesOnceAndRejectsRetiredSelectors)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Sample = NewObject<DMaterialExpressionTextureSampleParameter2D>(nullptr, NAME_None);
	Sample->Id = FGuid::NewGuid();
	Sample->Metadata = {.Id = FGuid::NewGuid(), .Name = "NormalTexture"};
	Sample->TextureUsage = ETextureUsage::Normal;
	std::vector<DMaterialExpression*> Expressions{Sample};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Normal = {Sample->Id, 1}; Outputs.Roughness = {Sample->Id, 3};
	const auto Normalized = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Normalized);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &MIR::FNode::Opcode), 1);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::DecodeNormalRG, &MIR::FNode::Opcode), 1);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::BlendNormalsRNM, &MIR::FNode::Opcode), 0);
	auto* Separate = NewObject<DMaterialExpressionTextureSample2D>(nullptr, NAME_None);
	Separate->Id = FGuid::NewGuid(); Separate->Texture = {Sample->Id, 7};
	Expressions.push_back(Separate); Outputs.Normal = {Separate->Id, 1};
	Outputs.Roughness = {Separate->Id, 3};
	const auto SeparateRGB = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(SeparateRGB);
	EXPECT_EQ(SeparateRGB.CanonicalBytes, Normalized.CanonicalBytes);
	for (const uint8 Index : {6, 7, 8, 9})
	{
		Outputs.Normal = {Separate->Id, Index};
		EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	}
	Expressions.pop_back(); MarkAsGarbage(Separate);
	Outputs.Roughness = {Sample->Id, 3};
	Sample->TextureUsage = ETextureUsage::Color; Outputs.Normal = {Sample->Id, 1};
	const auto ColorRGB = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(ColorRGB);
	EXPECT_EQ(std::ranges::count(ColorRGB.IR.Nodes, EMaterialProgramOpcode::DecodeNormalRG, &MIR::FNode::Opcode), 0);
	Sample->TextureUsage = ETextureUsage::Normal;
	for (const uint8 Index : {6, 8, 9})
	{
		Outputs.Normal = {Sample->Id, Index};
		EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	}
	for (auto* Expression : Expressions) MarkAsGarbage(Expression);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, ResourceOutputSkipsOwnerUVAndPreservesIndependentSamples)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Material = NewObject<DMaterial>(nullptr, "ResourceFanOut");
	auto* Texture = NewObject<DTexture2D>(nullptr, "OwnedTexture");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Channel = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	Channel->Metadata.Id = FGuid::NewGuid(); Channel->Metadata.Name = "UnusedOwnerUV";
	auto Coordinates = Testing::MakeGraphExpression<DMaterialExpressionTextureCoordinates>();
	Coordinates->Channel = {Channel->Id};
	auto Owner = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	Owner->Metadata.Id = FGuid::NewGuid(); Owner->Metadata.Name = "SharedTexture";
	Owner->DefaultValue.Texture = Texture; Owner->UV = {Coordinates->Id};
	auto UV1 = Testing::MakeGraphExpression<DMaterialExpressionVector2Constant>(); UV1->Value = {.1f, .2f};
	auto UV2 = Testing::MakeGraphExpression<DMaterialExpressionVector2Constant>(); UV2->Value = {.7f, .8f};
	auto Sample1 = Testing::MakeGraphExpression<DMaterialExpressionTextureSample2D>();
	auto Sample2 = Testing::MakeGraphExpression<DMaterialExpressionTextureSample2D>();
	Sample1->Texture = {Owner->Id, 7}; Sample1->UV = {UV1->Id};
	Sample2->Texture = {Owner->Id, 7}; Sample2->UV = {UV2->Id};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 7>{Channel.Get(), Coordinates.Get(), Owner.Get(), UV1.Get(), UV2.Get(), Sample1.Get(), Sample2.Get()},
		{.BaseColor = {Sample1->Id, 1}, .Emissive = {Sample2->Id, 1}}));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 2u);
	auto SnapshotCapture = SnapshotMaterialCompilerInput(*Material, {.CompilerIdentity = "ResourceFanOut"});
	ASSERT_TRUE(SnapshotCapture);
	auto& Snapshot = SnapshotCapture.Snapshot->Input;
	EXPECT_TRUE(std::ranges::any_of(Snapshot.Parameters, [&](const auto& Declaration) {
		return Declaration.Id == Owner->Metadata.Id && Declaration.Type == EMaterialParameterType::Texture;
	}));
	for (const auto& Node : Snapshot.IR.Nodes) EXPECT_TRUE(Node.HasValidPayload());
	const auto Normalized = MIR::Normalize(Snapshot);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "" : Durin::FormatMaterialError(Normalized.Diagnostics.front().Error));
	ASSERT_EQ(Normalized.ActiveParameters.size(), 1u);
	EXPECT_EQ(Normalized.ActiveParameters.front().Id, Owner->Metadata.Id);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &MIR::FNode::Opcode), 2);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::UVChannel, &MIR::FNode::Opcode), 0);
	EXPECT_EQ(Normalized.Layout.ResourceFieldCount, 1u);
	MarkAsGarbage(Material); MarkAsGarbage(Texture); CollectGarbage();
}

TEST(FMaterialFunctionTests, InlineCallBindingsRespectFunctionDefaultsAndRootOwnership)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "InlineCallFunction");
	const auto In = FunctionPort(1, EMaterialProgramValueType::Float, "Value");
	const auto Out = FunctionPort(2, EMaterialProgramValueType::Float, "Result");
	FMaterialFunctionSignature Signature;
	Signature.Inputs = {In};
	Signature.Inputs[0].Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {.X = .75f}};
	Signature.Outputs = {Out};
	auto* Input = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
	auto* Output = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
	Input->Id = FGuid::NewGuid(); Input->Port.Id = In.Id;
	Output->Id = FGuid::NewGuid(); Output->Port.Id = Out.Id; Output->Source = {Input->Id};
	std::vector<DMaterialExpression*> Body{Input, Output};
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
	Call->Id = FGuid::NewGuid(); Call->Function = Function;
	Call->Inputs = {{In.Id, In.Type, {}, {.25f}}}; Call->Outputs = {{Out.Id, Out.Type}};
	const std::array<DMaterialExpression*, 1> Expressions{Call};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Roughness = {.ExpressionId = Call->Id, .OutputId = Out.Id};
	const auto Bound = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Bound);
	ASSERT_EQ(Bound.IR.Nodes.size(), 1u);
	EXPECT_EQ(Bound.IR.Nodes[0].GetLiteral().X, .25f);
	Call->Inputs[0].InputDefault.clear();
	const auto Defaulted = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Defaulted);
	EXPECT_EQ(Defaulted.IR.Nodes[0].GetLiteral().X, .75f);
	Signature.Inputs[0].bRequired = true; Signature.Inputs[0].Default = {};
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	auto* Forbidden = NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None);
	Forbidden->Id = FGuid::NewGuid();
	Forbidden->Metadata = {.Id = FGuid::NewGuid(), .Name = "ForbiddenOwner"};
	Body.push_back(Forbidden);
	EXPECT_FALSE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	for (auto* Expression : Body) MarkAsGarbage(Expression);
	MarkAsGarbage(Call); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialFunctionTests, DefaultsAreTypedAndInputReferencesRemainStable)
{
	using namespace Durin;
	FMaterialFunctionSignature Signature;
	Signature.Inputs = {FunctionPort(1, EMaterialProgramValueType::Float2, "UV"),
		FunctionPort(2, EMaterialProgramValueType::Float2, "MapUV")};
	Signature.Outputs = {FunctionPort(3, EMaterialProgramValueType::Float2, "Result")};
	Signature.Inputs[0].Default.Kind = EMaterialFunctionDefaultKind::UV0;
	Signature.Inputs[1].Default = {.Kind = EMaterialFunctionDefaultKind::Input,
		.InputId = Signature.Inputs[0].Id};
	ASSERT_TRUE(ValidateMaterialFunctionSignature(Signature));
	std::ranges::reverse(Signature.Inputs);
	Signature.Inputs[1].Name = "RenamedCommonUV";
	Signature.Inputs[1].DisplayOrder = 17;
	ASSERT_TRUE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs[1].Default = {.Kind = EMaterialFunctionDefaultKind::Input,
		.InputId = Signature.Inputs[0].Id};
	const auto Cycle = ValidateMaterialFunctionSignature(Signature);
	EXPECT_FALSE(Cycle);
	ASSERT_FALSE(Cycle.Diagnostics.empty());
	EXPECT_TRUE(Cycle.Diagnostics[0].PortId.IsValid());
	Signature.Inputs[1].Default.Kind = EMaterialFunctionDefaultKind::UV0;
	Signature.Inputs.erase(Signature.Inputs.begin() + 1);
	EXPECT_FALSE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs[0].Default.Kind = EMaterialFunctionDefaultKind::Texture;
	EXPECT_FALSE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs[0].Type = EMaterialProgramValueType::Texture2D;
	Signature.Inputs[0].Default.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
	ASSERT_TRUE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs[0].bRequired = true;
	EXPECT_FALSE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs[0].Default.Kind = EMaterialFunctionDefaultKind::None;
	EXPECT_TRUE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs.resize(MaterialFunctionMaxInputs + 1);
	EXPECT_FALSE(ValidateMaterialFunctionSignature(Signature));
}

TEST(FMaterialFunctionTests, GraphValidationRejectsCyclesParametersAndInvalidTerminals)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "FunctionValidation"));
	auto Graph = CaptureFunctionExpressions(*Function);
	ASSERT_TRUE(Graph.Validate());
	Cast<DMaterialExpressionFunctionOutput>(Graph.Expressions.back().Get())->Port.Id = {};
	EXPECT_FALSE(Graph.Validate());
	Graph = CaptureFunctionExpressions(*Function);
	auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	Parameter->Metadata.Id = FGuid::NewGuid(); Parameter->Metadata.Name = "ForbiddenRootParameter";
	Graph.Expressions.emplace_back(Parameter.Get());
	EXPECT_FALSE(Graph.Validate());
	Graph = CaptureFunctionExpressions(*Function);
	Cast<DMaterialExpressionFunctionOutput>(Graph.Expressions.back().Get())->Source.OutputId = Graph.Signature.Outputs[0].Id;
	EXPECT_FALSE(Graph.Validate());
	Graph = CaptureFunctionExpressions(*Function);
	Graph.Expressions.emplace_back(Graph.Expressions.back().Get());
	EXPECT_FALSE(Graph.Validate());
	Graph = CaptureFunctionExpressions(*Function);
	auto Cycle = Testing::MakeGraphExpression<DMaterialExpressionAdd>({9, 8, 7, 6});
	Cycle->A = {Cycle->Id}; Cycle->B = {Cycle->Id};
	Graph.Expressions.emplace_back(Cycle.Get());
	EXPECT_FALSE(Graph.Validate());
}

TEST(FMaterialFunctionTests, CallBindingsUseGuidAndDiagnoseRemovedRetypedOrRequiredPorts)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FMaterialFunctionSignature Signature;
	Signature.Inputs = {FunctionPort(1, EMaterialProgramValueType::Float, "Factor"),
		FunctionPort(2, EMaterialProgramValueType::Texture2D, "Map")};
	Signature.Inputs[0].bRequired = true;
	Signature.Inputs[1].Default.Kind = EMaterialFunctionDefaultKind::Texture;
	Signature.Outputs = {FunctionPort(3, EMaterialProgramValueType::Surface, "Surface"),
		FunctionPort(4, EMaterialProgramValueType::Float, "Value")};
	auto Call = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>({4, 3, 2, 1});
	Call->Inputs = {{Signature.Inputs[0].Id, EMaterialProgramValueType::Float, {{9, 8, 7, 6}}}};
	Call->Outputs = {{Signature.Outputs[0].Id, EMaterialProgramValueType::Surface}};
	ASSERT_TRUE(ValidateMaterialFunctionCallSignature(*Call, Signature));
	std::ranges::reverse(Signature.Inputs);
	std::ranges::reverse(Signature.Outputs);
	Signature.Inputs[1].Name = "RenamedFactor";
	ASSERT_TRUE(ValidateMaterialFunctionCallSignature(*Call, Signature));
	Signature.Outputs[1].Type = EMaterialProgramValueType::Float3;
	const auto Retyped = ValidateMaterialFunctionCallSignature(*Call, Signature);
	EXPECT_FALSE(Retyped);
	ASSERT_FALSE(Retyped.Diagnostics.empty());
	EXPECT_EQ(Retyped.Diagnostics[0].NodeId, Call->Id);
	EXPECT_EQ(Retyped.Diagnostics[0].PortId, Call->Outputs[0].OutputId);
	Signature.Outputs.pop_back();
	EXPECT_FALSE(ValidateMaterialFunctionCallSignature(*Call, Signature));
	Call->Outputs.clear();
	Call->Inputs.clear();
	EXPECT_FALSE(ValidateMaterialFunctionCallSignature(*Call, Signature));
}

TEST(FMaterialFunctionTests, BaseTypedCallsRoundTripAndSnapshotsDoNotRetainOwners)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("MaterialFunctionAssets");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/FunctionTests/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath CalleePath, CallerPath, AbstractPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionTests/Callee", CalleePath));
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionTests/Caller", CallerPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionTests/Abstract", AbstractPath));
	EXPECT_FALSE(IAssetTools::Get().CreatePackageLeafAssetForTesting(AbstractPath,
		DMaterialFunctionInterface::StaticClass()));
	DMaterialFunction* Callee = nullptr;
	DMaterialFunction* Caller = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(CalleePath, Callee));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(CallerPath, Caller));
	DMaterialFunctionInterface* Interface = Callee;
	ASSERT_TRUE(Interface->GetFunctionDependencies().empty());
	const auto OutputId = Interface->GetFunctionSignature().Outputs[0].Id;
	const FGuid CallId{0x341558ff, 1, 2, 3};
	auto Graph = CaptureFunctionExpressions(*Caller);
	auto Call = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>(CallId);
	Call->Function = Interface; Call->Outputs = {{OutputId, EMaterialProgramValueType::Surface}};
	const FGuid GetId{0x341558ff, 1, 2, 4}, SetId{0x341558ff, 1, 2, 5};
	auto Get = Testing::MakeGraphExpression<DMaterialExpressionGetSurfaceAttributes>(GetId);
	Get->Surface = {.ExpressionId = CallId, .OutputId = OutputId};
	Get->AttributeMask = 1u << static_cast<uint8>(EMaterialSurfaceOutput::Metallic);
	auto Set = Testing::MakeGraphExpression<DMaterialExpressionSetSurfaceAttributes>(SetId);
	Set->Surface = {.ExpressionId = CallId, .OutputId = OutputId};
	Set->Attributes = {{EMaterialSurfaceOutput::Metallic, {GetId, static_cast<uint8>(EMaterialSurfaceOutput::Metallic)}}};
	Cast<DMaterialExpressionFunctionOutput>(Graph.Expressions[1].Get())->Source = {SetId};
	Graph.Expressions.emplace_back(Call.Get()); Graph.Expressions.emplace_back(Get.Get()); Graph.Expressions.emplace_back(Set.Get());
	ASSERT_TRUE(Graph.Apply(*Caller));
	const auto Capture = [](DMaterialFunction& Function) {
		auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
		Call->Id = {0x341558ff, 9, 9, 9}; Call->Function = &Function;
		const auto& Output = Function.GetFunctionSignature().Outputs[0];
		Call->Outputs = {{Output.Id, Output.Type}};
		const std::array<DMaterialExpression*, 1> Expressions{Call};
		FMaterialExpressionSurfaceOutputs Outputs;
		Outputs.Surface = {.ExpressionId = Call->Id, .OutputId = Output.Id}; Outputs.bUseMaterialAttributes = true;
		auto Result = BuildTypedExpressions(Expressions, Outputs);
		MarkAsGarbage(Call);
		return Result;
	};
	const auto BeforeSignature = Caller->GetFunctionSignature();
	const auto Before = Capture(*Caller);
	ASSERT_TRUE(Before);
	const auto Revision = Caller->GetFunctionRevision();
	ASSERT_TRUE(Caller->SetFunctionPresentation({.Nodes = {{CallId, 140, 240}}}));
	EXPECT_EQ(Caller->GetFunctionRevision(), Revision);
	ASSERT_EQ(Caller->GetFunctionDependencies().size(), 1u);
	ASSERT_TRUE(SavePackage(Callee->GetPackage()));
	ASSERT_TRUE(SavePackage(Caller->GetPackage()));
	const auto CallerFile = FindAssetExact(CallerPath);
	ASSERT_TRUE(CallerFile);
	FAssetPackageInspection Inspection;
	ASSERT_TRUE(InspectAssetPackage(CallerFile->PhysicalPath, CallerPath, Inspection));
	std::vector<FAssetReferenceEdge> References;
	ASSERT_TRUE(ExtractAssetReferences(CallerPath, Inspection, References));
	ASSERT_EQ(References.size(), 1u);
	EXPECT_EQ(References[0].ExpectedClass, "Durin::DMaterialFunctionInterface");
	EXPECT_EQ(References[0].TargetPath.ToString(), Callee->GetObjectPath());
	Graph.Expressions.clear(); Call.Reset(); Get.Reset(); Set.Reset();
	ASSERT_TRUE(UnloadPackage(CallerPath));
	ASSERT_TRUE(UnloadPackage(CalleePath));
	CollectGarbage();
	Caller = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(CallerPath), Caller));
	const auto After = Capture(*Caller);
	ASSERT_TRUE(After);
	EXPECT_EQ(Before.IR, After.IR);
	EXPECT_EQ(Before.Parameters, After.Parameters);
	ASSERT_EQ(Before.Sources.size(), After.Sources.size());
	for (size_t Index = 0; Index < Before.Sources.size(); ++Index)
	{
		const auto& A = Before.Sources[Index]; const auto& B = After.Sources[Index];
		EXPECT_EQ(std::tie(A.ExpressionIndex, A.NodeId, A.PortId, A.FunctionAssetPath, A.CallPath, A.InputIndex, A.UVFieldIndex),
			std::tie(B.ExpressionIndex, B.NodeId, B.PortId, B.FunctionAssetPath, B.CallPath, B.InputIndex, B.UVFieldIndex));
	}
	EXPECT_EQ(BeforeSignature, Caller->GetFunctionSignature());
	std::vector<FMaterialFunctionOutputBinding> CapturedCallOutputs;
	for (const auto& Expression : Caller->GetExpressionCollection().Expressions)
		if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get())) CapturedCallOutputs = Call->Outputs;
	EXPECT_EQ(Caller->GetFunctionPresentation().Nodes.size(), 1u);
	ASSERT_EQ(Caller->GetFunctionDependencies().size(), 1u);
	EXPECT_EQ(Caller->GetFunctionDependencies()[0]->GetFunctionSignature().Outputs[0].Id, OutputId);
	ASSERT_TRUE(UnloadPackage(CallerPath));
	ASSERT_TRUE(UnloadPackage(CalleePath));
	CollectGarbage();
	ASSERT_EQ(CapturedCallOutputs.size(), 1u);
	EXPECT_EQ(CapturedCallOutputs[0].OutputId, OutputId);
}

TEST(FMaterialFunctionTests, DependencyStampsAreDetachedAndRejectRecursionAndMissingDependencies)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Caller = NewObject<DMaterialFunction>(nullptr, "ClosureCaller");
	auto* Callee = NewObject<DMaterialFunction>(nullptr, "ClosureCallee");
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Caller, *Callee));
	const std::array<DMaterialFunctionInterface*, 1> Roots{Caller};
	std::vector<FMaterialFunctionOwnerStamp> Closure;
	ASSERT_TRUE(ValidateMaterialFunctionDependencies(Roots, Closure));
	ASSERT_EQ(Closure.size(), 2u);
	const auto Original = Closure;
	auto Graph = CaptureFunctionExpressions(*Callee);
	Graph.Signature.Inputs[0].Default.Surface.EmissiveDefault.X = 2;
	ASSERT_TRUE(Graph.Apply(*Callee));
	std::vector<FMaterialFunctionOwnerStamp> Edited;
	ASSERT_TRUE(ValidateMaterialFunctionDependencies(Roots, Edited));
	EXPECT_NE(Original, Edited);
	EXPECT_EQ(Original, Closure);
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Callee, *Caller));
	const auto Recursive = ValidateMaterialFunctionDependencies(Roots, Closure);
	EXPECT_FALSE(Recursive);
	ASSERT_FALSE(Recursive.Diagnostics.empty());
	EXPECT_EQ(Recursive.Diagnostics[0].CallPath.size(), 2u);
	EXPECT_EQ(Closure, Original);
	Graph = CaptureFunctionExpressions(*Caller);
	for (const auto& Expression : Graph.Expressions)
		if (auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get())) Call->Function = nullptr;
	ASSERT_TRUE(Graph.Apply(*Caller));
	EXPECT_FALSE(ValidateMaterialFunctionDependencies(Roots, Closure));
	EXPECT_EQ(Closure, Original);
	MarkAsGarbage(Caller);
	MarkAsGarbage(Callee);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, ClosureBoundsIncludeRepeatedSharedSubtrees)
{
	using namespace Durin;
	InitializeDObjectSystem();
	std::vector<DMaterialFunction*> Chain;
	for (uint32 Index = 0; Index <= MaterialFunctionMaxCallDepth; ++Index)
		Chain.push_back(NewObject<DMaterialFunction>(nullptr, FName(std::format("DepthFunction{}", Index))));
	for (size_t Index = 1; Index < Chain.size(); ++Index)
		ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Chain[Index], *Chain[Index - 1]));
	// The valid subtree is visited first so memoization must retain its depth.
	const std::array<DMaterialFunctionInterface*, 2> Roots{Chain[1], Chain.back()};
	std::vector<FMaterialFunctionOwnerStamp> Closure;
	const auto Result = ValidateMaterialFunctionDependencies(Roots, Closure);
	EXPECT_FALSE(Result);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics[0].Category, EMaterialProgramDiagnosticCategory::Bounds);
	EXPECT_TRUE(Closure.empty());
	const std::array<DMaterialFunctionInterface*, 1> ValidRoots{Chain[MaterialFunctionMaxCallDepth - 1]};
	ASSERT_TRUE(ValidateMaterialFunctionDependencies(ValidRoots, Closure));
	EXPECT_EQ(Closure.size(), MaterialFunctionMaxCallDepth);
	const std::vector<DMaterialFunctionInterface*> RepeatedRoots(MaterialFunctionMaxDependencies + 1, Chain[0]);
	ASSERT_TRUE(ValidateMaterialFunctionDependencies(RepeatedRoots, Closure));
	EXPECT_EQ(Closure.size(), 1u);
	for (auto* Function : Chain) MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, ExpansionPreservesIndependentInputsMultipleOutputsAndEquivalentKeys)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "ArithmeticFunction");
	FMaterialFunctionSignature Signature;
	Signature.Inputs = {FunctionPort(1, EMaterialProgramValueType::Float, "Value"),
		FunctionPort(2, EMaterialProgramValueType::Float, "Offset")};
	Signature.Inputs[0].bRequired = true;
	Signature.Inputs[1].Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {1}};
	Signature.Outputs = {FunctionPort(3, EMaterialProgramValueType::Float, "Sum"),
		FunctionPort(4, EMaterialProgramValueType::Float, "Original")};
	const FGuid ValueId{11, 1, 1, 1}, OffsetId{11, 1, 1, 2}, SumId{11, 1, 1, 3};
	auto* Value = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
	auto* Offset = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
	auto* Sum = NewObject<DMaterialExpressionAdd>(nullptr, NAME_None);
	auto* SumOutput = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
	auto* OriginalOutput = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
	Value->Id = ValueId; Value->Port.Id = Signature.Inputs[0].Id;
	Offset->Id = OffsetId; Offset->Port.Id = Signature.Inputs[1].Id;
	Sum->Id = SumId; Sum->A = {ValueId}; Sum->B = {OffsetId};
	SumOutput->Id = FGuid::NewGuid(); SumOutput->Port.Id = Signature.Outputs[0].Id; SumOutput->Source = {SumId};
	OriginalOutput->Id = FGuid::NewGuid(); OriginalOutput->Port.Id = Signature.Outputs[1].Id; OriginalOutput->Source = {ValueId};
	std::vector<DMaterialExpression*> Body{Value, Offset, Sum, SumOutput, OriginalOutput};
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	const FGuid FirstValue{12, 1, 1, 1}, SecondValue{12, 1, 1, 2};
	const FGuid FirstCall{12, 1, 1, 3}, SecondCall{12, 1, 1, 4};
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Pair : {std::pair{FirstCall, FirstValue}, std::pair{SecondCall, SecondValue}})
	{
		auto* Constant = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
		Constant->Id = Pair.second; Constant->Value = Pair.first == FirstCall ? 2.f : 9.f;
		auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
		Call->Id = Pair.first; Call->Function = Function;
		Call->Inputs = {{Signature.Inputs[0].Id, EMaterialProgramValueType::Float, {Pair.second}}};
		Call->Outputs = {{Signature.Outputs[0].Id, EMaterialProgramValueType::Float},
			{Signature.Outputs[1].Id, EMaterialProgramValueType::Float}};
		Expressions.push_back(Constant); Expressions.push_back(Call);
	}
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Metallic = {.ExpressionId = FirstCall, .OutputId = Signature.Outputs[0].Id};
	Outputs.Roughness = {.ExpressionId = SecondCall, .OutputId = Signature.Outputs[0].Id};
	Outputs.AmbientOcclusion = {.ExpressionId = FirstCall, .OutputId = Signature.Outputs[1].Id};
	const auto Normalized = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "" : Durin::FormatMaterialError(Normalized.Diagnostics[0].Error));
	const auto& Metal = Normalized.IR.Nodes[Normalized.IR.SurfaceRoot.Inputs[2].ExpressionIndex];
	const auto& Rough = Normalized.IR.Nodes[Normalized.IR.SurfaceRoot.Inputs[3].ExpressionIndex];
	ASSERT_EQ(Metal.Opcode, EMaterialProgramOpcode::Add);
	ASSERT_EQ(Rough.Opcode, EMaterialProgramOpcode::Add);
	const auto HasConstant = [&](const MIR::FNode& Node, float Value) {
		return std::ranges::any_of(Node.Inputs, [&](uint32 Index) { return Normalized.IR.Nodes[Index].GetLiteral().X == Value; });
	};
	EXPECT_TRUE(HasConstant(Metal, 2));
	EXPECT_TRUE(HasConstant(Metal, 1));
	EXPECT_TRUE(HasConstant(Rough, 9));
	EXPECT_EQ(Normalized.IR.Nodes[Normalized.IR.SurfaceRoot.Inputs[4].ExpressionIndex].GetLiteral().X, 2);
	EXPECT_TRUE(std::ranges::all_of(Normalized.IR.Nodes, [](const auto& Node) { return Node.Opcode < EMaterialProgramOpcode::FunctionInput; }));
	EXPECT_TRUE(std::ranges::any_of(Normalized.Sources, [&](const auto& Source) {
		return Source.NodeId == SumId && Source.FunctionAssetPath == Function->GetObjectPath()
			&& Source.CallPath == std::vector<FGuid>{FirstCall};
	}));
	std::ranges::reverse(Body);
	std::ranges::reverse(Signature.Inputs);
	Signature.Outputs[0].Name = "RenamedSum";
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	std::ranges::reverse(Expressions);
	const auto Reordered = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Normalized.CanonicalBytes, Reordered.CanonicalBytes);
	EXPECT_EQ(Normalized.Identity, Reordered.Identity);
	EXPECT_TRUE(GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout));
	for (auto* Expression : Body) MarkAsGarbage(Expression);
	for (auto* Expression : Expressions) MarkAsGarbage(Expression);
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, NestedTextureDefaultsYieldToConnectedRootResource)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "TextureLeaf");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "TextureWrapper");
	FFunctionTestExpressions Graph;
	Graph.Signature.Inputs = {FunctionPort(1, EMaterialProgramValueType::Texture2D, "Texture"),
		FunctionPort(2, EMaterialProgramValueType::Float2, "UV")};
	Graph.Signature.Inputs[0].Default = {.Kind = EMaterialFunctionDefaultKind::Texture,
		.TextureFallback = EMaterialTextureFallback::Black};
	Graph.Signature.Inputs[1].Default.Kind = EMaterialFunctionDefaultKind::UV0;
	Graph.Signature.Outputs = {FunctionPort(3, EMaterialProgramValueType::Float3, "Color")};
	const FGuid TextureId{31, 1, 1, 1}, UVId{31, 1, 1, 2}, SampleId{31, 1, 1, 3}, ColorId{31, 1, 1, 4};
	auto TextureInput = Testing::MakeGraphExpression<DMaterialExpressionFunctionInput>(TextureId);
	TextureInput->Port.Id = Graph.Signature.Inputs[0].Id;
	auto UVInput = Testing::MakeGraphExpression<DMaterialExpressionFunctionInput>(UVId);
	UVInput->Port.Id = Graph.Signature.Inputs[1].Id;
	auto Sample = Testing::MakeGraphExpression<DMaterialExpressionTextureSample2D>(SampleId);
	Sample->Texture = {TextureId}; Sample->UV = {UVId};
	auto Color = Testing::MakeGraphExpression<DMaterialExpressionSwizzle>(ColorId);
	Color->Input = {SampleId}; Color->Components = {0, 1, 2};
	auto Terminal = Testing::MakeGraphExpression<DMaterialExpressionFunctionOutput>({31, 1, 1, 5});
	Terminal->Port.Id = Graph.Signature.Outputs[0].Id; Terminal->Source = {ColorId};
	for (DMaterialExpression* Expression : std::array<DMaterialExpression*, 5>{TextureInput.Get(), UVInput.Get(), Sample.Get(), Color.Get(), Terminal.Get()}) Graph.Expressions.emplace_back(Expression);
	ASSERT_TRUE(Graph.Apply(*Leaf));
	FFunctionTestExpressions Outer;
	Outer.Signature.Inputs = {Graph.Signature.Inputs[0]};
	Outer.Signature.Inputs[0].Default.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
	Outer.Signature.Outputs = Graph.Signature.Outputs;
	const FGuid CallId{32, 1, 1, 1};
	Outer.Expressions.emplace_back(DuplicateObject(TextureInput.Get(), nullptr, NAME_None).Object);
	auto InnerCall = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>(CallId);
	InnerCall->Function = Leaf;
	InnerCall->Inputs = {{Graph.Signature.Inputs[0].Id, EMaterialProgramValueType::Texture2D, {TextureId}}};
	InnerCall->Outputs = {{Graph.Signature.Outputs[0].Id, EMaterialProgramValueType::Float3}};
	auto OuterTerminal = Testing::MakeGraphExpression<DMaterialExpressionFunctionOutput>({32, 1, 1, 2});
	OuterTerminal->Port.Id = Graph.Signature.Outputs[0].Id;
	OuterTerminal->Source = {.ExpressionId = CallId, .OutputId = Graph.Signature.Outputs[0].Id};
	Outer.Expressions.emplace_back(InnerCall.Get()); Outer.Expressions.emplace_back(OuterTerminal.Get());
	ASSERT_TRUE(Outer.Apply(*Wrapper));
	const FGuid RootCall{33, 1, 1, 1}, ParameterNode{33, 1, 1, 2}, ParameterId{33, 1, 1, 3};
	auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
	Call->Id = RootCall; Call->Function = Wrapper;
	Call->Outputs = {{Graph.Signature.Outputs[0].Id, EMaterialProgramValueType::Float3}};
	std::vector<DMaterialExpression*> Expressions{Call};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor = {.ExpressionId = RootCall, .OutputId = Graph.Signature.Outputs[0].Id};
	const auto Default = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Default) << (Default.Diagnostics.empty() ? "" : Durin::FormatMaterialError(Default.Diagnostics[0].Error));
	EXPECT_EQ(Default.Layout.ResourceFieldCount, 0u);
	EXPECT_TRUE(std::ranges::any_of(Default.IR.Nodes, [](const auto& Node) {
		return Node.Opcode == EMaterialProgramOpcode::Constant && Node.ResultType == EMaterialProgramValueType::Float4
			&& Node.GetLiteral() == FMaterialProgramLiteral{0.5f, 0.5f, 1, 1};
	}));
	auto* Parameter = NewObject<DMaterialExpressionTextureParameter>(nullptr, NAME_None);
	Parameter->Id = ParameterNode; Parameter->Metadata = {.Id = ParameterId, .Name = "RootTexture"};
	Expressions.push_back(Parameter);
	Call->Inputs.push_back({Graph.Signature.Inputs[0].Id, EMaterialProgramValueType::Texture2D, {ParameterNode}});
	const auto Connected = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Connected) << (Connected.Diagnostics.empty() ? "" : Durin::FormatMaterialError(Connected.Diagnostics[0].Error));
	EXPECT_EQ(Connected.Layout.ResourceFieldCount, 1u);
	EXPECT_EQ(std::ranges::count(Connected.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &MIR::FNode::Opcode), 1);
	ASSERT_EQ(Connected.ActiveParameters.size(), 1u);
	EXPECT_EQ(Connected.ActiveParameters[0].Id, ParameterId);
	EXPECT_TRUE(GenerateMaterialProgramSlang(Connected.IR, Connected.Layout));
	// Compose the sampled texture into a Surface function and compile real shaders.
	Graph.Signature.Inputs.push_back(FunctionPort(4, EMaterialProgramValueType::Surface, "BaseSurface"));
	Graph.Signature.Inputs.back().Default.Kind = EMaterialFunctionDefaultKind::Surface;
	Graph.Signature.Outputs[0].Type = EMaterialProgramValueType::Surface;
	const FGuid SurfaceInput{34, 1, 1, 1}, SurfaceSet{34, 1, 1, 2};
	Terminal->Source = {SurfaceSet};
	auto SurfaceParameter = Testing::MakeGraphExpression<DMaterialExpressionFunctionInput>(SurfaceInput);
	SurfaceParameter->Port.Id = Graph.Signature.Inputs.back().Id;
	auto SurfaceOverride = Testing::MakeGraphExpression<DMaterialExpressionSetSurfaceAttributes>(SurfaceSet);
	SurfaceOverride->Surface = {SurfaceInput};
	SurfaceOverride->Attributes = {{EMaterialSurfaceOutput::BaseColor, {ColorId}}};
	Graph.Expressions.emplace_back(SurfaceParameter.Get()); Graph.Expressions.emplace_back(SurfaceOverride.Get());
	ASSERT_TRUE(Graph.Apply(*Leaf));
	Call->Function = Leaf;
	Call->Outputs[0].ExpectedType = EMaterialProgramValueType::Surface;
	Outputs = {}; Outputs.Surface = {.ExpressionId = RootCall, .OutputId = Graph.Signature.Outputs[0].Id}; Outputs.bUseMaterialAttributes = true;
	auto Built = BuildTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Built);
	MIR::FCompilerInput Input;
	Input.IR = std::move(Built.IR); Input.Parameters = std::move(Built.Parameters); Input.Sources = std::move(Built.Sources);
	FModuleManager::Get().LoadModule("RenderCore");
	Durin::FMaterialOperationResult Error;
	ASSERT_TRUE((Error = BuildDefaultMaterialCompilerEnvironment(Input.Environment))) << Durin::FormatMaterialError(Error.Error);
	const auto Compiled = MIR::Compile(Input, true);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "" : Durin::FormatMaterialError(Compiled.Diagnostics[0].Error));
	EXPECT_EQ(Compiled.Layout.ResourceFieldCount, 1u);
	EXPECT_FALSE(Compiled.CompiledShaders.empty());
	for (auto* Expression : Expressions) MarkAsGarbage(Expression);
	MarkAsGarbage(Wrapper);
	MarkAsGarbage(Leaf);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, RootCallsCommitAtomicallyAndSnapshotThroughInstances)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "RootSurfaceFunction");
	auto* Material = NewObject<DMaterial>(nullptr, "RootFunctionMaterial");
	auto* Instance = NewObject<DMaterialInstance>(nullptr, "RootFunctionInstance");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(Instance->SetParent(Material));
	const auto& Output = Function->GetFunctionSignature().Outputs[0];
	const FGuid CallId{41, 1, 1, 1};
	auto Call = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>(CallId);
	Call->Function = Function;
	Call->Outputs = {{Output.Id, Output.Type}};
	const std::array<DMaterialExpression*, 1> Expressions{Call.Get()};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Surface = {.ExpressionId = CallId, .OutputId = Output.Id}; Outputs.bUseMaterialAttributes = true;
	const auto Before = Material->GetExpressionOutputs();
	const auto BeforeRevision = Material->GetMaterialProgramRevision();
	auto InvalidOutputs = Outputs;
	InvalidOutputs.Surface.ExpressionId = FGuid::NewGuid();
	EXPECT_FALSE(Material->SetMaterialExpressions(Expressions, InvalidOutputs));
	EXPECT_EQ(Material->GetExpressionOutputs(), Before);
	EXPECT_EQ(Material->GetMaterialProgramRevision(), BeforeRevision);
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	ASSERT_EQ(GetFunctionCalls(*Material).size(), 1u);
	EXPECT_EQ(GetFunctionCalls(*Material)[0]->Function.Get(), Function);
	auto InputCapture = SnapshotMaterialCompilerInput(*Instance, {.CompilerIdentity = "RootFunctionTest"});
	ASSERT_TRUE(InputCapture);
	auto& Input = InputCapture.Snapshot->Input;
	const auto& Owners = InputCapture.Snapshot->FunctionOwners;
	ASSERT_EQ(Owners.size(), 1u);
	EXPECT_EQ(Owners[0].Owner, FObjectKey(Function));
	EXPECT_TRUE(std::ranges::any_of(Input.Sources, [&](const auto& Source) {
		return !Source.CallPath.empty() && Source.CallPath.front() == CallId;
	}));
	const auto Normalized = MIR::Normalize(Input);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "" : Durin::FormatMaterialError(Normalized.Diagnostics[0].Error));
	EXPECT_TRUE(GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout));
	Call->Function = nullptr;
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	const auto OldIR = Input.IR;
	const auto OldOwners = Owners;
	const auto Missing = SnapshotMaterialCompilerInput(*Material, {});
	EXPECT_FALSE(Missing.Snapshot.has_value());
	EXPECT_FALSE(Missing);
	ASSERT_FALSE(Missing.Diagnostics.empty());
	EXPECT_EQ(Missing.Diagnostics[0].NodeId, CallId);
	EXPECT_EQ(Input.IR, OldIR);
	EXPECT_EQ(Owners, OldOwners);
	MarkAsGarbage(Instance);
	MarkAsGarbage(Material);
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, ImportProvenanceRoundtripsWithoutChangingGraphOrCompileRevision)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("ImportProvenance");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/ImportProvenance/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath ParentPath, InstancePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ImportProvenance/Parent", ParentPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ImportProvenance/Instance", InstancePath));
	DMaterial* Parent = nullptr;
	DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(ParentPath, Parent));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(InstancePath, Instance));
	Parent->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(Instance->SetParent(Parent));
	const auto Outputs = Parent->GetExpressionOutputs();
	const auto ExpressionCount = Parent->GetExpressionCollection().Expressions.size();
	const auto Revision = Parent->GetMaterialCompileStatus().AuthoredRevision;
	const FMaterialImportProvenance ParentReceipt{.RecipeId = "Durin.ImportedSurface", .RecipeVersion = 1,
		.StructuralKey = "Durin.ImportedSurface:1;d;d;d;d;d;d;d;d"};
	auto InstanceReceipt = ParentReceipt;
	InstanceReceipt.SourceIdentity = "source.gltf";
	InstanceReceipt.OutputIdentity = "scene:material:stable";
	ASSERT_TRUE(Parent->SetImportProvenance(ParentReceipt));
	ASSERT_TRUE(Instance->SetImportProvenance(InstanceReceipt));
	EXPECT_EQ(Parent->GetMaterialCompileStatus().AuthoredRevision, Revision);
	EXPECT_EQ(Parent->GetExpressionOutputs(), Outputs);
	EXPECT_EQ(Parent->GetExpressionCollection().Expressions.size(), ExpressionCount);
	EXPECT_TRUE(Parent->GetParameterDefinitions().empty());
	auto Invalid = InstanceReceipt;
	Invalid.SourceIdentity.assign(4097, 'x');
	EXPECT_FALSE(Instance->SetImportProvenance(Invalid));
	EXPECT_EQ(Instance->GetImportProvenance(), InstanceReceipt);
	ASSERT_TRUE(SavePackage(Parent->GetPackage()));
	ASSERT_TRUE(SavePackage(Instance->GetPackage()));
	ASSERT_TRUE(UnloadPackage(InstancePath));
	ASSERT_TRUE(UnloadPackage(ParentPath));
	CollectGarbage();
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(InstancePath), Instance));
	Parent = Cast<DMaterial>(Instance->GetParent());
	ASSERT_NE(Parent, nullptr);
	EXPECT_EQ(Parent->GetImportProvenance(), ParentReceipt);
	EXPECT_EQ(Instance->GetImportProvenance(), InstanceReceipt);
	EXPECT_EQ(Parent->GetExpressionOutputs(), Outputs);
	EXPECT_EQ(Parent->GetExpressionCollection().Expressions.size(), ExpressionCount);
	ASSERT_TRUE(UnloadPackage(InstancePath));
	ASSERT_TRUE(UnloadPackage(ParentPath));
	CollectGarbage();
}

TEST(FMaterialFunctionTests, TypedFieldsRoundtripAndRejectInvalidCoordinateDefaults)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("CompactRoundtrip");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/CompactRoundtrip/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath MaterialPath, FunctionPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/CompactRoundtrip/Material", MaterialPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/CompactRoundtrip/Function", FunctionPath));
	DMaterial* Material = nullptr;
	DMaterialFunction* Function = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FunctionPath, Function));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material), FunctionDocument(*Function);
	ASSERT_TRUE(Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::TextureSampleParameter2D, EMaterialProgramValueType::Float4));
	ASSERT_TRUE(Testing::CreateGraphCatalogNode(FunctionDocument, EMaterialProgramOpcode::Multiply));
	const auto Call = Document.InsertFunctionCall(*Function, 300, 0);
	ASSERT_TRUE(Call);
	ASSERT_TRUE(Document.Connect(FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, std::nullopt), FMaterialGraphPinAddress::Output({Call.GeneratedNodeIds[0], 0, Function->GetFunctionSignature().Outputs[0].Id}), true));
	const auto CaptureFields = [](const auto& Owner) {
		std::vector<FPropertyValueSnapshotPayload> Fields;
		for (const auto& Expression : Owner.GetExpressionCollection().Expressions)
			Expression->GetClass()->ForEachProperty([&](FProperty* Property) {
				// The callee is checked by asset path separately across package lifetimes.
				if (Property == DMaterialExpressionFunctionCall::StaticClass()->FindPropertyByName("Function")) return;
				Fields.emplace_back();
				EXPECT_TRUE(CapturePropertyValuePayload(Property, Expression.Get(), 0, Fields.back()));
			});
		return Fields;
	};
	const auto Expected = CaptureFields(*Material);
	const auto ExpectedFunction = CaptureFields(*Function);
	const auto ExpectedOutputs = Material->GetExpressionOutputs();
	const auto ExpectedSignature = Function->GetFunctionSignature();
	const auto FunctionObjectPath = Function->GetObjectPath();
	const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Material->GetExpressionCollection().Expressions.front().Get());
	ASSERT_NE(Sample, nullptr);
	const auto TextureId = Sample->Metadata.Id;
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	const auto SaveMaterial = SavePackage(Material->GetPackage());
	ASSERT_TRUE(SaveMaterial) << SaveMaterial.Message;
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(FunctionPath));
	CollectGarbage();
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Material));
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(FunctionPath), Function));
	EXPECT_EQ(CaptureFields(*Material), Expected);
	EXPECT_EQ(CaptureFields(*Function), ExpectedFunction);
	EXPECT_EQ(Material->GetExpressionOutputs(), ExpectedOutputs);
	EXPECT_EQ(Function->GetFunctionSignature(), ExpectedSignature);
	EXPECT_NE(Material->FindParameterDefinition(TextureId), nullptr);
	ASSERT_EQ(GetFunctionCalls(*Material).size(), 1u);
	EXPECT_EQ(GetFunctionCalls(*Material)[0]->Function.Get(), Function);
	EXPECT_EQ(Function->GetObjectPath(), FunctionObjectPath);
	auto Invalid = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	Invalid->Metadata.Id = TextureId;
	Invalid->Metadata.Name = "InvalidUV";
	Invalid->UV.OutputIndex = 1;
	const std::array<DMaterialExpression*, 1> InvalidExpressions{Invalid.Get()};
	const auto Rejected = Material->SetMaterialExpressions(InvalidExpressions, {});
	EXPECT_FALSE(Rejected);
	ASSERT_FALSE(Rejected.Diagnostics.empty());
	EXPECT_EQ(Rejected.Diagnostics.front().NodeId, Invalid->Id);
	EXPECT_EQ(Rejected.Diagnostics.front().Error.Code, FMaterialError::FCode(EMaterialExpressionError::DisconnectedUVInputOutputSelector));
	EXPECT_EQ(CaptureFields(*Material), Expected);
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(FunctionPath));
	CollectGarbage();
}

TEST(FMaterialFunctionTests, RejectsOldRootSchemaAndPreservesCurrentFunctionReferences)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("RootFunctionAssets");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/RootFunctionTests/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath MaterialPath, FunctionPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/RootFunctionTests/Material", MaterialPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/RootFunctionTests/Function", FunctionPath));
	DMaterial* Material = nullptr;
	DMaterialFunction* Function = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FunctionPath, Function));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const FGuid CallId{42, 1, 1, 1};
	ASSERT_TRUE(PublishRootFunction(*Material, *Function, CallId));
	const auto Outputs = Material->GetExpressionOutputs();
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	FAssetPackageInspection Inspection;
	const auto File = FindAssetExact(MaterialPath);
	ASSERT_TRUE(File);
	ASSERT_TRUE(InspectAssetPackage(File->PhysicalPath, MaterialPath, Inspection));
	std::vector<FAssetReferenceEdge> References;
	ASSERT_TRUE(ExtractAssetReferences(MaterialPath, Inspection, References));
	EXPECT_TRUE(std::ranges::any_of(References, [&](const auto& Reference) {
		return Reference.ExpectedClass == "Durin::DMaterialFunctionInterface"
			&& Reference.TargetPath.ToString() == Function->GetObjectPath();
	}));
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(FunctionPath));
	CollectGarbage();
	Material = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Material));
	EXPECT_EQ(Material->GetExpressionOutputs(), Outputs);
	ASSERT_EQ(GetFunctionCalls(*Material).size(), 1u);
	EXPECT_EQ(GetFunctionCalls(*Material)[0]->Id, CallId);
	auto InputCapture = SnapshotMaterialCompilerInput(*Material, {.CompilerIdentity = "RootRoundTrip"});
	ASSERT_TRUE(InputCapture);
	auto& Input = InputCapture.Snapshot->Input;
	EXPECT_TRUE(MIR::Normalize(Input));
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(FunctionPath));
	CollectGarbage();
}

TEST(FMaterialFunctionTests, NestedSurfaceOverridesAndSelectedOutputsPreserveAttributes)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "SurfaceOverrideLeaf");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "SurfaceOverrideWrapper");
	auto Graph = CaptureFunctionExpressions(*Leaf);
	const FGuid ValueId{51, 1, 1, 1}, SetId{51, 1, 1, 2};
	auto Value = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>(ValueId);
	Value->Value = 0.75f;
	auto Set = Testing::MakeGraphExpression<DMaterialExpressionSetSurfaceAttributes>(SetId);
	Set->Surface = {Graph.Expressions[0]->Id};
	Set->Attributes = {{EMaterialSurfaceOutput::Metallic, {ValueId}}};
	Cast<DMaterialExpressionFunctionOutput>(Graph.Expressions[1].Get())->Source = {SetId};
	Graph.Expressions.emplace_back(Value.Get());
	Graph.Expressions.emplace_back(Set.Get());
	ASSERT_TRUE(Graph.Apply(*Leaf));
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Wrapper, *Leaf));
	const FGuid CallId{52, 1, 1, 1}, GetId{52, 1, 1, 2};
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
	auto* Get = NewObject<DMaterialExpressionGetSurfaceAttributes>(nullptr, NAME_None);
	Call->Id = CallId; Call->Function = Wrapper; Call->Outputs = {{Output.Id, Output.Type}};
	Get->Id = GetId; Get->Surface = {.ExpressionId = CallId, .OutputId = Output.Id};
	Get->AttributeMask = (1u << static_cast<uint8>(EMaterialSurfaceOutput::Normal))
		| (1u << static_cast<uint8>(EMaterialSurfaceOutput::Metallic));
	const std::array<DMaterialExpression*, 2> Expressions{Call, Get};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor = {GetId, static_cast<uint8>(EMaterialSurfaceOutput::Normal)};
	Outputs.Roughness = {GetId, static_cast<uint8>(EMaterialSurfaceOutput::Metallic)};
	const auto Normalized = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "" : Durin::FormatMaterialError(Normalized.Diagnostics[0].Error));
	EXPECT_EQ(Normalized.IR.Nodes[Normalized.IR.SurfaceRoot.Inputs[0].ExpressionIndex].GetLiteral(), (FMaterialProgramLiteral{0, 0, 1}));
	EXPECT_EQ(Normalized.IR.Nodes[Normalized.IR.SurfaceRoot.Inputs[3].ExpressionIndex].GetLiteral().X, 0.75f);
	EXPECT_TRUE(std::ranges::all_of(Normalized.IR.Nodes, [](const auto& Node) { return Node.Opcode < EMaterialProgramOpcode::FunctionInput; }));
	EXPECT_TRUE(GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout));
	Get->AttributeMask |= 1u << static_cast<uint8>(EMaterialSurfaceOutput::Roughness);
	const auto MoreVisible = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(MoreVisible);
	EXPECT_EQ(MoreVisible.Identity, Normalized.Identity);
	Get->AttributeMask = 1u << static_cast<uint8>(EMaterialSurfaceOutput::Normal);
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	Set->Attributes[0].Source = {SetId};
	EXPECT_FALSE(Graph.Apply(*Leaf));
	MarkAsGarbage(Call); MarkAsGarbage(Get);
	MarkAsGarbage(Wrapper);
	MarkAsGarbage(Leaf);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, SurfaceOverridesSupportAllEightAttributesAndRejectInvalidBindings)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Base = NewObject<DMaterialExpressionMakeSurface>(nullptr, NAME_None);
	auto* Set = NewObject<DMaterialExpressionSetSurfaceAttributes>(nullptr, NAME_None);
	Base->Id = FGuid::NewGuid(); Set->Id = FGuid::NewGuid(); Set->Surface = {Base->Id};
	const std::array<FMaterialExpressionInput*, 8> Inputs{&Base->BaseColor, &Base->Normal,
		&Base->Metallic, &Base->Roughness, &Base->AmbientOcclusion, &Base->Emissive,
		&Base->Opacity, &Base->OpacityMask};
	std::vector<DMaterialExpression*> Expressions{Base, Set};
	for (uint8 Index = 0; Index < 8; ++Index)
	{
		const auto Attribute = static_cast<EMaterialSurfaceOutput>(Index);
		DMaterialExpression* Constant;
		if (GetMaterialSurfaceOutputType(Attribute) == EMaterialProgramValueType::Float3)
		{
			auto* Vector = NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None);
			Vector->Value = FVector3{.125f * Index, .25f, .5f}; Constant = Vector;
		}
		else
		{
			auto* Scalar = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
			Scalar->Value = .125f * Index; Constant = Scalar;
		}
		Constant->Id = FGuid::NewGuid(); Expressions.push_back(Constant);
		*Inputs[Index] = {Constant->Id}; Set->Attributes.push_back({Attribute, *Inputs[Index]});
	}
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Surface = {Set->Id}; Outputs.bUseMaterialAttributes = true;
	const auto Normalized = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Normalized);
	EXPECT_TRUE(GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout));
	EXPECT_EQ(Set->GetAuthoredInputCount(), 9u);
	std::ranges::reverse(Set->Attributes);
	const auto Reordered = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Normalized.Identity, Reordered.Identity);
	Set->Attributes.clear();
	const auto Passthrough = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Passthrough);
	EXPECT_EQ(Normalized.Identity, Passthrough.Identity);
	Set->Attributes = {{EMaterialSurfaceOutput::Metallic, *Inputs[0]}};
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	Set->Attributes = {{EMaterialSurfaceOutput::Metallic, *Inputs[2]}, {EMaterialSurfaceOutput::Metallic, *Inputs[2]}};
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	Set->Attributes = {{static_cast<EMaterialSurfaceOutput>(255), *Inputs[2]}};
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, Outputs));
	for (auto* Expression : Expressions) MarkAsGarbage(Expression);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, NestedDiagnosticsIdentifyOwningDocumentAndRootInvocation)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "DiagnosticLeaf");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "DiagnosticWrapper");
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Wrapper, *Leaf));
	auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
	Call->Id = FGuid::NewGuid(); Call->Function = Wrapper;
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	Call->Outputs = {{Output.Id, Output.Type}};
	DMaterialExpressionFunctionCall* Nested = nullptr;
	DMaterialExpressionFunctionOutput* WrapperTerminal = nullptr;
	for (const auto& Expression : Wrapper->GetExpressionCollection().Expressions)
	{
		if (auto* Found = Cast<DMaterialExpressionFunctionCall>(Expression.Get())) Nested = Found;
		if (auto* Found = Cast<DMaterialExpressionFunctionOutput>(Expression.Get())) WrapperTerminal = Found;
	}
	DMaterialExpressionFunctionOutput* Terminal = nullptr;
	for (const auto& Expression : Leaf->GetExpressionCollection().Expressions)
		if (auto* Found = Cast<DMaterialExpressionFunctionOutput>(Expression.Get())) Terminal = Found;
	ASSERT_NE(Nested, nullptr); ASSERT_NE(Terminal, nullptr); ASSERT_NE(WrapperTerminal, nullptr);
	const auto SavedSource = Terminal->Source;
	Terminal->Source.ExpressionId = {99, 99, 99, 99};
	const std::array<DMaterialExpression*, 1> Expressions{Call};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Surface = {.ExpressionId = Call->Id, .OutputId = Output.Id}; Outputs.bUseMaterialAttributes = true;
	const auto Broken = NormalizeTypedExpressions(Expressions, Outputs);
	EXPECT_FALSE(Broken);
	EXPECT_TRUE(std::ranges::any_of(Broken.Diagnostics, [&](const auto& Diagnostic) {
		return Diagnostic.NodeId == Terminal->Id && Diagnostic.FunctionAssetPath == Leaf->GetObjectPath()
			&& Diagnostic.CallPath == std::vector<FGuid>{Call->Id, Nested->Id};
	}));
	Terminal->Source = SavedSource;
	Nested->Outputs[0].OutputId = {98, 98, 98, 98};
	WrapperTerminal->Source.OutputId = Nested->Outputs[0].OutputId;
	const auto InvalidPort = NormalizeTypedExpressions(Expressions, Outputs);
	EXPECT_FALSE(InvalidPort);
	ASSERT_FALSE(InvalidPort.Diagnostics.empty());
	EXPECT_EQ(InvalidPort.Diagnostics[0].NodeId, Nested->Id);
	EXPECT_EQ(InvalidPort.Diagnostics[0].FunctionAssetPath, Wrapper->GetObjectPath());
	EXPECT_EQ(InvalidPort.Diagnostics[0].CallPath, (std::vector<FGuid>{Call->Id}));
	MarkAsGarbage(Call); MarkAsGarbage(Wrapper); MarkAsGarbage(Leaf); CollectGarbage();
}

TEST(FMaterialFunctionTests, DependencyEditsPreserveAcceptedContractsAndOwnerSourceMaps)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "LifecycleFunctionLeaf");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "LifecycleFunctionWrapper");
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Wrapper, *Leaf));
	auto* First = NewObject<DMaterial>(nullptr, "FirstFunctionCaller");
	auto* Second = NewObject<DMaterial>(nullptr, "SecondFunctionCaller");
	auto* Child = NewObject<DMaterialInstance>(nullptr, "FunctionCallerChild");
	First->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Second->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(Child->SetParent(First));
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	const FGuid FirstCall{55, 1, 1, 1}, SecondCall{55, 1, 1, 2};
	for (const auto& Pair : {std::pair{First, FirstCall}, std::pair{Second, SecondCall}})
	{
		ASSERT_TRUE(PublishRootFunction(*Pair.first, *Wrapper, Pair.second));
		ASSERT_TRUE(Pair.first->CompileEdits());
	}
	const auto Accepted = First->GetAcceptedCompiledProgram();
	ASSERT_TRUE(Accepted);
	EXPECT_EQ(Second->GetAcceptedCompiledProgram(), Accepted);
	EXPECT_EQ(Child->GetAcceptedCompiledProgram(), Accepted);
	ASSERT_FALSE(First->GetAcceptedExpressionSources().empty());
	ASSERT_FALSE(Second->GetAcceptedExpressionSources().empty());
	EXPECT_EQ(First->GetAcceptedExpressionSources()[0].CallPath[0], FirstCall);
	EXPECT_EQ(Second->GetAcceptedExpressionSources()[0].CallPath[0], SecondCall);
	const auto Before = First->GetMaterialCompileStatus();
	ASSERT_TRUE(Leaf->SetFunctionPresentation({.Nodes = {{Leaf->GetExpressionCollection().Expressions[0]->Id, 70, 80}}}));
	EXPECT_EQ(First->GetMaterialCompileStatus().AuthoredRevision, Before.AuthoredRevision);
	auto Graph = CaptureFunctionExpressions(*Leaf);
	Graph.Signature.Inputs[0].Default.Surface.RoughnessDefault.X = 0.21f;
	ASSERT_TRUE(Graph.Apply(*Leaf));
	for (const DMaterialInterface* Caller : {static_cast<DMaterialInterface*>(First), static_cast<DMaterialInterface*>(Second),
		static_cast<DMaterialInterface*>(Child)})
	{
		EXPECT_EQ(Caller->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);
		EXPECT_EQ(Caller->GetAcceptedCompiledProgram(), Accepted);
	}
	FMaterialCompileResult Stale{.Owner = FWeakObjectPtr(First), .AuthoredRevision = Before.AuthoredRevision,
		.Generation = Before.RequestGeneration, .DependencyRevision = Before.DependencyRevision,
		.ParentChainRevision = Before.ParentChainRevision, .ProgramIdentity = Before.RequestedIdentity,
		.StaticProperties = First->GetStaticProperties(), .Target = Before.Target,
		.State = EMaterialCompileState::Ready, .CompiledProgram = Accepted};
	EXPECT_FALSE(Private::FMaterialCompilationLifecycle::Admit(*First, Stale));
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), Accepted);
	ASSERT_TRUE(First->CompileEdits());
	ASSERT_TRUE(Second->CompileEdits());
	EXPECT_NE(First->GetAcceptedCompiledProgram()->Identity, Accepted->Identity);
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), Child->GetAcceptedCompiledProgram());
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), Second->GetAcceptedCompiledProgram());
	const auto OriginalWrapper = CaptureFunctionExpressions(*Wrapper);
	auto BrokenWrapper = CaptureFunctionExpressions(*Wrapper);
	for (const auto& Expression : BrokenWrapper.Expressions)
		if (auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get())) Call->Function = nullptr;
	ASSERT_TRUE(BrokenWrapper.Apply(*Wrapper));
	EXPECT_FALSE(First->CompileEdits());
	EXPECT_FALSE(First->GetAcceptedCompiledProgram());
	EXPECT_FALSE(Child->GetAcceptedCompiledProgram());
	EXPECT_TRUE(First->GetAcceptedExpressionSources().empty());
	ASSERT_TRUE(OriginalWrapper.Apply(*Wrapper));
	ASSERT_TRUE(First->CompileEdits());
	EXPECT_TRUE(First->GetAcceptedCompiledProgram());
	const auto Current = First->GetMaterialCompileStatus();
	const auto CurrentProgram = First->GetAcceptedCompiledProgram();
	// Simulate a dependency changing across a missed external notification. The
	// publication boundary must compare captured versions independently of events.
	auto* FirstBinding = Cast<DMaterialExpressionFunctionCall>(First->GetExpressionCollection().Expressions[0].Get());
	auto* SecondBinding = Cast<DMaterialExpressionFunctionCall>(Second->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(FirstBinding, nullptr);
	ASSERT_NE(SecondBinding, nullptr);
	FirstBinding->Function = nullptr;
	SecondBinding->Function = nullptr;
	Graph.Signature.Inputs[0].Default.Surface.RoughnessDefault.X = 0.37f;
	ASSERT_TRUE(Graph.Apply(*Leaf));
	FirstBinding->Function = Wrapper;
	SecondBinding->Function = Wrapper;
	EXPECT_EQ(First->GetMaterialCompileStatus().AuthoredRevision, Current.AuthoredRevision);
	FMaterialCompileResult StaleClosure{.Owner = FWeakObjectPtr(First), .AuthoredRevision = Current.AuthoredRevision,
		.Generation = Current.RequestGeneration, .DependencyRevision = Current.DependencyRevision,
		.ParentChainRevision = Current.ParentChainRevision, .ProgramIdentity = Current.RequestedIdentity,
		.StaticProperties = First->GetStaticProperties(), .Target = Current.Target,
		.State = EMaterialCompileState::Ready, .CompiledProgram = CurrentProgram};
	EXPECT_FALSE(Private::FMaterialCompilationLifecycle::Admit(*First, std::move(StaleClosure)));
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), CurrentProgram);
	EXPECT_EQ(First->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);
	ASSERT_TRUE(First->CompileEdits());
	EXPECT_NE(First->GetAcceptedCompiledProgram()->Identity, CurrentProgram->Identity);
	MarkAsGarbage(Child);
	MarkAsGarbage(Second);
	MarkAsGarbage(First);
	MarkAsGarbage(Wrapper);
	MarkAsGarbage(Leaf);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, RelocationRefreshesNestedCallersAndDeletionHonorsReferences)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("FunctionMutationAssets");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/FunctionMutationTests/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath WrapperPath, LeafPath, MovedPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionMutationTests/Wrapper", WrapperPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionMutationTests/Leaf", LeafPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionMutationTests/Moved", MovedPath));
	DMaterialFunction* Wrapper = nullptr;
	DMaterialFunction* Leaf = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(WrapperPath, Wrapper));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(LeafPath, Leaf));
	auto OriginalGraph = CaptureFunctionExpressions(*Wrapper);
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Wrapper, *Leaf));
	ASSERT_TRUE(SavePackage(Leaf->GetPackage()));
	ASSERT_TRUE(SavePackage(Wrapper->GetPackage()));
	FAssetDeletionOperation Deletion;
	EXPECT_FALSE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {LeafPath}}, Deletion));
	EXPECT_TRUE(std::ranges::any_of(Deletion.GetBlockers(), [](const auto& Blocker) {
		return Blocker.Kind == EAssetDeletionBlocker::ExternalPersistentReference
			|| Blocker.Kind == EAssetDeletionBlocker::ExternalLoadedReference;
	}));
	auto* Material = NewObject<DMaterial>(nullptr, "FunctionRelocationCaller");
	TStrongObjectPtr<DMaterial> MaterialRoot(Material);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	const FGuid CallId{64, 1, 1, 1};
	ASSERT_TRUE(PublishRootFunction(*Material, *Wrapper, CallId));
	ASSERT_TRUE(Material->CompileEdits());
	const auto Accepted = Material->GetAcceptedCompiledProgram();
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	const auto OriginalLeafPath = Leaf->GetObjectPath();
	const std::array Mappings{FAssetRelocationMapping{LeafPath, MovedPath}};
	FAssetRelocationSummary Summary;
	FAssetMutationJob Job;
	ASSERT_TRUE(PrepareAssetRelocationJob(Mappings, Summary, Job));
	ASSERT_TRUE(Job.ResumeForward());
	EXPECT_EQ(Wrapper->GetFunctionDependencies()[0].Get(), Leaf);
	EXPECT_GT(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	EXPECT_EQ(Material->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);
	EXPECT_EQ(Material->GetAcceptedCompiledProgram(), Accepted);
	ASSERT_TRUE(Material->CompileEdits());
	EXPECT_EQ(Material->GetAcceptedCompiledProgram()->Identity, Accepted->Identity);
	EXPECT_TRUE(std::ranges::any_of(Material->GetAcceptedExpressionSources(), [&](const auto& Source) {
		return Source.FunctionAssetPath == Leaf->GetObjectPath();
	}));
	EXPECT_FALSE(std::ranges::any_of(Material->GetAcceptedExpressionSources(), [&](const auto& Source) {
		return Source.FunctionAssetPath == OriginalLeafPath;
	}));
	ASSERT_TRUE(OriginalGraph.Apply(*Wrapper));
	ASSERT_TRUE(SavePackage(Wrapper->GetPackage()));
	ASSERT_TRUE(Material->CompileEdits());
	const std::array Removed{LeafPath, MovedPath};
	FAssetDeletionOperation UnreferencedDeletion;
	const auto PreparedDeletion = IAssetTools::Get().PrepareDeletion(
		{.AssetPaths = {LeafPath, MovedPath}}, UnreferencedDeletion);
	ASSERT_TRUE(PreparedDeletion) << PreparedDeletion.Message;
	ASSERT_TRUE(Testing::RemoveAssetPackagesForTests(Removed));
	EXPECT_TRUE(Material->GetAcceptedCompiledProgram());
	MaterialRoot.Reset();
	MarkAsGarbage(Material);
	CollectGarbage();
	ASSERT_TRUE(UnloadPackage(WrapperPath));
	CollectGarbage();
}

TEST(FMaterialFunctionTests, ExpandedBoundsApplyBeforePruningWithoutRaisingAuthoredBounds)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "BoundedExpansionFunction");
	FMaterialFunctionSignature Signature;
	Signature.Outputs = {FunctionPort(1, EMaterialProgramValueType::Float, "Result")};
	std::vector<DMaterialExpression*> Body;
	for (uint32 Index = 0; Index < 199; ++Index)
	{
		auto* Value = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
		Value->Id = FGuid::NewGuid(); Value->Value = static_cast<float>(Index); Body.push_back(Value);
	}
	auto* Terminal = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
	Terminal->Id = FGuid::NewGuid(); Terminal->Port.Id = Signature.Outputs[0].Id;
	Terminal->Source = {Body.front()->Id}; Body.push_back(Terminal);
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	std::vector<DMaterialExpression*> Expressions;
	const auto AddCall = [&] {
		auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
		Call->Id = FGuid::NewGuid(); Call->Function = Function;
		Call->Outputs = {{Signature.Outputs[0].Id, EMaterialProgramValueType::Float}};
		Expressions.push_back(Call);
	};
	for (uint32 Index = 0; Index < 20; ++Index) AddCall();
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Metallic = {.ExpressionId = Expressions.front()->Id, .OutputId = Signature.Outputs[0].Id};
	const auto Valid = NormalizeTypedExpressions(Expressions, Outputs);
	ASSERT_TRUE(Valid);
	EXPECT_EQ(Valid.IR.Nodes.size(), 1u);
	AddCall();
	const auto Excessive = NormalizeTypedExpressions(Expressions, Outputs);
	EXPECT_FALSE(Excessive);
	ASSERT_FALSE(Excessive.Diagnostics.empty());
	EXPECT_EQ(Excessive.Diagnostics[0].Category, EMaterialProgramDiagnosticCategory::Bounds);
	for (auto* Expression : Expressions) MarkAsGarbage(Expression);
	Expressions.clear();
	for (uint32 Index = 0; Index <= MaterialProgramMaxNodeCount; ++Index)
	{
		auto* Value = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
		Value->Id = FGuid::NewGuid(); Expressions.push_back(Value);
	}
	EXPECT_FALSE(NormalizeTypedExpressions(Expressions, {}));
	for (auto* Expression : Expressions) MarkAsGarbage(Expression);
	for (auto* Expression : Body) MarkAsGarbage(Expression);
	MarkAsGarbage(Function); CollectGarbage();
}
