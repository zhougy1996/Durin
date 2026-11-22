#include "Shader/MaterialShader.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionArithmetic.h"
#include "Materials/MaterialExpressionSwitch.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionScene.h"
#include "DerivedDataRequestOwner.h"
#include "BuildServiceBackendPrivate.h"
#include "BuildCacheRecordPrivate.h"
#include "CacheDispatchPrivate.h"
#include "Shader/ShaderCompilingManager.h"
#include "MetalShaderFormat.h"
#include "Materials/MaterialShaderMap.h"
#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "MaterialCookedProgramTestSupport.h"
#include "MaterialProgramTestFixture.h"
#include "Materials/MaterialPreparedProgram.h"
#include "Materials/MaterialTranslation.h"
#include "Materials/MaterialTranslationCodec.h"
#include "Threading/Task.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Threading/ThreadEvent.h"
#include "Shader/ShaderDiagnostics.h"
#include "NativeRHIBackendTestSupport.h"
#include "Components/StaticMeshComponent.h"
#include "Shader/IShaderBuildModule.h"
#include "ShaderBuildTestSupport.h"

namespace
{
	using namespace Durin;
    class FM15VFParameterOwner {
    public:
        DURIN_BEGIN_SHADER_PARAMETERS(FM15VFParameterOwner)
#include "Tests/M15VFParameters.inl"
        DURIN_END_SHADER_PARAMETERS();
        static auto GetParametersMetadata() -> const FShaderParametersMetadata*
        { return GetShaderParametersMetadataOrNull<FM15VFParameterOwner>(); }
    };
	// A registered extension reuses Local geometry code without changing compiler selection.
	auto GetM15TestFactory() -> const Durin::FVertexFactoryType*
	{
		using namespace Durin;
		static const FVertexFactoryType Type("M15TestVertexFactory", EVertexFactoryFlags::None, nullptr,
			{.ShaderModules = {"VertexFactory.LocalVertexFactory"}, .MaterialEntryInclude = "Tests/M15MaterialEntry.slang",
			 .ShouldCompile = [](const FShaderPermutationParameters& P) { return P.Target == VulkanShaderTarget && P.PermutationId == 37; },
			 .ModifyEnvironment = [](const FShaderPermutationParameters& P, FShaderCompileOptions& O) {
				O.Macros.emplace_back("DURIN_M15_VF_PROOF", std::to_string(P.PermutationId));
			 }, .ParametersMetadata = FM15VFParameterOwner::GetParametersMetadata()});
		return &Type;
	}
	class FM15TestVertexShader final : public Durin::FMeshMaterialShader
	{
	public:
		static auto GetMaterialCompilationDescription() -> Durin::FMaterialShaderEntryDescriptor
		{ return {.Entry = Durin::EMaterialShaderEntry::LocalVertex, .GeometryUsage = 8, .PassUsage = 1,
			.PermutationId = 37, .VertexFactory = &GetM15TestFactory}; }
		static auto ShouldCompileMaterialPermutation(const Durin::FMaterialShaderPermutationParameters& P) -> bool
		{
			return P.Domain == Durin::EMaterialDomain::Surface && P.Quality == Durin::EMaterialQualityLevel::Low
				&& P.FeatureLevel == Durin::ERHIFeatureLevel::SM5 && P.PermutationId == 37
				&& (P.Properties.GeometryUsage & 8) && P.VertexFactoryType == GetM15TestFactory();
		}
		static auto ModifyMaterialCompilationEnvironment(const Durin::FMaterialShaderPermutationParameters& P,
			Durin::FShaderCompileOptions& O) -> void
		{ O.Macros.emplace_back("DURIN_M15_POLICY_PROOF", std::to_string(uint8(P.Quality))); }
        DURIN_BEGIN_SHADER_PARAMETERS(FM15TestVertexShader)
#include "Tests/M15VFParameters.inl"
#include "Material/ViewParameters.inl"
#include "Material/GPUSceneParameters.inl"
        DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_MESH_MATERIAL_SHADER(FM15TestVertexShader, Durin::FMeshMaterialShader,
			"/Engine/StaticMeshBasePass", Durin::EShaderFrequency::Vertex, "VertexMain");
	};
	DURIN_IMPLEMENT_MESH_MATERIAL_SHADER(FM15TestVertexShader);

	auto FindCompiledMaterialStage(const Durin::FMaterialShaderMap& Map, std::string_view Name) -> const Durin::FShaderCode&
	{
		return *std::ranges::find(Map.CompiledShaders, Name, &Durin::FShaderCode::SourceEntryPoint);
	}
}

TEST(FMaterialAttributeContractTests, FrozenDefinitionsOwnTypesStagesDefaultsAndBlendPolicy)
{
	using namespace Durin;
	const auto Definitions = FMaterialAttributeDefinitionMap::GetDefinitions();
	ASSERT_EQ(Definitions.size(), 12u);
	std::unordered_set<FGuid> IDs;
	for (const auto& Definition : Definitions)
	{
		EXPECT_TRUE(IDs.insert(Definition.AttributeID).second);
		EXPECT_EQ(FMaterialAttributeDefinitionMap::Find(Definition.Property), &Definition);
		EXPECT_EQ(FMaterialAttributeDefinitionMap::Find(Definition.AttributeID), &Definition);
	}
	EXPECT_EQ(FMaterialAttributeDefinitionMap::GetID(EMaterialProperty::MP_WorldPositionOffset), (FGuid{0x4d415454,0x52505250,1,9}));
	const auto* Normal = FMaterialAttributeDefinitionMap::Find(EMaterialProperty::MP_Normal);
	EXPECT_EQ(Normal->DefaultValue, (FMaterialProgramLiteral{0,0,1}));
	EXPECT_EQ(Normal->Blend, EMaterialAttributeBlend::NormalizedNormal);
	EXPECT_EQ(FMaterialAttributeDefinitionMap::Find(EMaterialProperty::MP_WorldPositionOffset)->Semantics.Stages, EMaterialEvaluationStage::Vertex);
	const auto Unlit = FMaterialAttributeDefinitionMap::GetActiveProperties(EMaterialDomain::Surface, EMaterialShadingModel::Unlit, EMaterialBlendMode::Opaque);
	EXPECT_FALSE(std::ranges::contains(Unlit, EMaterialProperty::MP_BaseColor));
	EXPECT_TRUE(std::ranges::contains(Unlit, EMaterialProperty::MP_EmissiveColor));
}

TEST(FMaterialAttributeContractTests, DetachedRootsRejectMalformedSchemaBeforeIndexedAccess)
{
	using namespace Durin;
	const auto Base = MakeSyntheticMaterialCompilerInput();
	for (uint32 Case = 0; Case < 5; ++Case)
	{
		auto Input = Base;
		if (Case == 0) Input.IR.AttributeRoots.pop_back();
		if (Case == 1) Input.IR.AttributeRoots.back().AttributeID = Input.IR.AttributeRoots.front().AttributeID;
		if (Case == 2) Input.IR.AttributeRoots.front().AttributeID = {99,98,97,96};
		if (Case == 3) Input.IR.AttributeRoots.front().Type = EMaterialProgramValueType::Float;
		if (Case == 4) Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).LegalStages = EMaterialEvaluationStage::Pixel;
		const auto Before = Input.IR;
		const auto Invalid = MIR::Normalize(Input);
		EXPECT_FALSE(Invalid);
		EXPECT_FALSE(Invalid.Diagnostics.empty());
		EXPECT_TRUE(Invalid.CanonicalBytes.empty());
		EXPECT_EQ(Input.IR, Before);
	}
}

TEST(FMaterialAttributeContractTests, GuidRootOrderDoesNotChangeCanonicalIdentity)
{
	using namespace Durin;
	auto Input = MakeSyntheticMaterialCompilerInput();
	const auto First = MIR::Normalize(Input);
	ASSERT_TRUE(First);
	std::ranges::reverse(Input.IR.AttributeRoots);
	const auto Reordered = MIR::Normalize(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(First.CanonicalBytes, Reordered.CanonicalBytes);
	EXPECT_EQ(First.Identity, Reordered.Identity);
}

TEST(FMaterialShaderStageTests, RegisteredFactoryPolicyCompilesAndRejectsEntryOnlySubstitution)
{
	using namespace Durin;
	auto Input = MakeSyntheticMaterialCompilerInput();
	Input.StaticProperties.GeometryUsage = 8;
	Input.StaticProperties.PassUsage = 1;
	const auto Environment = BuildDefaultMaterialCompilerEnvironment(Input.Environment, VulkanShaderTarget);
	ASSERT_TRUE(Environment) << Environment.error();
	Input.Environment.Quality = EMaterialQualityLevel::Low;
	const auto Translation = MaterialBuildPrivate::TranslateMaterialInput(Input, false);
	ASSERT_TRUE(Translation);
	ASSERT_EQ(Translation.RequiredEntries.size(), 2u);
	EXPECT_TRUE(std::ranges::contains(Translation.RequiredEntries, std::string("FM15TestVertexShader")));
	const auto Requests = MakeMaterialShaderStageRequests(Translation, false);
	const auto Vertex = std::ranges::find_if(Requests, [](const auto& R) { return R.Frequencies[0] == EShaderFrequency::Vertex; });
	ASSERT_NE(Vertex, Requests.end());
	EXPECT_TRUE(std::ranges::any_of(Vertex->Macros, [](const auto& M) { return M.Name == "DURIN_M15_VF_PROOF" && M.Value == "37"; }));
	EXPECT_TRUE(std::ranges::any_of(Vertex->Macros, [](const auto& M) { return M.Name == "DURIN_M15_POLICY_PROOF"; }));
	ASSERT_FALSE(Vertex->AdditionalSourceTrees.empty());
	auto CorruptSource = *Vertex;
	CorruptSource.AdditionalSourceTrees[0].ContentHash.HashLow ^= 1;
	const auto RejectedSource = FShaderCompilingManager::Get().CompileGeneratedSync(CorruptSource);
	EXPECT_FALSE(RejectedSource);
	const auto Compiled = MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "Missing diagnostic" : Compiled.Diagnostics.front().Message);
	const auto& Map = Compiled.GetShaderMap();
	const auto Entry = std::ranges::find_if(Map.Entries, [](const auto& E) { return E.Shader.ShaderType == "FM15TestVertexShader"; });
	ASSERT_NE(Entry, Map.Entries.end());
	ASSERT_TRUE(Entry->Mesh);
	EXPECT_EQ(Entry->Mesh->VertexFactoryType, "M15TestVertexFactory");
	const FShaderType* Type = &FM15TestVertexShader::StaticType();
	FMeshMaterialShaderMapCompileInput Admission{
		.Identity = Entry->Shader.Material, .Target = Map.Target, .LocalPermutationId = 37,
		.VertexFactoryType = GetM15TestFactory(), .ShaderTypes = std::span(&Type, 1),
		.GeneratedStages = Map.CompiledShaders, .CompiledProgramIdentity = Map.ShaderMapId,
		.CompiledTarget = Map.Target, .GeneratedEntries = Map.Entries, .CompiledParameters = Compiled.Resource.ShaderMap, .bCreateRHIShaders = false};
	Admission.CompileOptions.Target = VulkanShaderTarget;
	FMeshMaterialShaderMap Accepted;
	const auto Admitted = FMeshMaterialShaderMap::TryCompileSync(Admission, Accepted);
	ASSERT_TRUE(Admitted) << Admitted.Error;
    ASSERT_TRUE(Accepted);
    const auto* Runtime = TMaterialShaderRef<FM15TestVertexShader>(Accepted).GetShader();
    ASSERT_NE(Runtime, nullptr);
    const auto Offset = std::ranges::find(Runtime->GetParameterBindings(), std::string("M15Offset"), &FShaderParameterBinding::Name);
    ASSERT_NE(Offset, Runtime->GetParameterBindings().end());
    EXPECT_FALSE(std::ranges::contains(Runtime->GetParameterBindings(), std::string("M15Unused"), &FShaderParameterBinding::Name));
    auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create("VFExtension", 64, 16, EBufferUsageFlags::UniformBuffer));
    FM15TestVertexShader::FParameters Values;
    Values.M15Offset = {Buffer.GetReference(), 16, 16};
    const auto Submitted = ResolveShaderParameterResources(*FM15TestVertexShader::GetParametersMetadata(), std::span(&*Offset, 1), &Values);
    ASSERT_EQ(Submitted.size(), 1u);
    EXPECT_EQ(Submitted[0].Resource, Buffer.GetReference());
    EXPECT_EQ(Submitted[0].SetIndex, Offset->SetIndex); EXPECT_EQ(Submitted[0].BindingIndex, Offset->BindingIndex);
    EXPECT_EQ(Submitted[0].Offset, 16u);
    Admission.LocalPermutationId = 38;
	EXPECT_FALSE(FMeshMaterialShaderMap::TryCompileSync(Admission, Accepted));
	EXPECT_FALSE(Accepted);
	FByteBuffer CookedBytes;
	ASSERT_TRUE(Testing::EncodeMaterialCookedProgramFamilyForTest(Compiled.Resource, Input.StaticProperties,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game, CookedBytes));
	FMaterialStaticProperties CookedProperties;
	std::shared_ptr<const FMaterialProgram> Cooked;
	ASSERT_TRUE(Testing::DecodeMaterialCookedProgramFamilyForTest(CookedBytes, ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, Compiled.Resource, CookedProperties, Cooked));
	EXPECT_EQ(Cooked->GetShaderMap().Quality, EMaterialQualityLevel::Low);
	EXPECT_EQ(Cooked->GetShaderMap().Entries, Map.Entries);
	Admission.LocalPermutationId = 37;
	Admission.VertexFactoryType = nullptr;
	EXPECT_FALSE(FMeshMaterialShaderMap::TryCompileSync(Admission, Accepted));
	Admission.VertexFactoryType = GetM15TestFactory();
	auto WrongEntries = Map.Entries;
	for (auto& E : WrongEntries) if (E.Shader.ShaderType == "FM15TestVertexShader") E.Shader.ShaderType = "DifferentTypeWithSameEntry";
	Admission.GeneratedEntries = WrongEntries;
	EXPECT_FALSE(FMeshMaterialShaderMap::TryCompileSync(Admission, Accepted));
	Input.Environment.Quality = EMaterialQualityLevel::High;
	EXPECT_FALSE(std::ranges::contains(SelectMaterialShaderEntries(Input.StaticProperties, true, Input.IR.Domain, &Input.Environment), std::string("FM15TestVertexShader")));
	Input.Environment.Quality = EMaterialQualityLevel::Low;
	Input.StaticProperties.GeometryUsage = 9;
	const auto SameEntry = MIR::Compile(Input);
	ASSERT_TRUE(SameEntry) << (SameEntry.Diagnostics.empty() ? "Missing diagnostic" : SameEntry.Diagnostics.front().Message);
	EXPECT_EQ(std::ranges::count(SameEntry.GetShaderMap().CompiledShaders, std::string("VertexMain"), &FShaderCode::SourceEntryPoint), 2);
	const auto& Both = SameEntry.GetShaderMap();
	Admission.Identity = std::ranges::find_if(Both.Entries, [](const auto& E) { return E.Shader.ShaderType == "FM15TestVertexShader"; })->Shader.Material;
	Admission.GeneratedEntries = Both.Entries; Admission.GeneratedStages = Both.CompiledShaders;
    Admission.CompiledParameters = SameEntry.Resource.ShaderMap;
	Admission.CompiledProgramIdentity = Both.ShaderMapId;
	EXPECT_TRUE(FMeshMaterialShaderMap::TryCompileSync(Admission, Accepted));
	auto Swapped = Both.CompiledShaders;
	std::swap(Swapped[0], Swapped[2]);
	EXPECT_FALSE(ValidateMaterialCompilationOutput(Both, Both.ShaderMapId, Both.Target, Swapped));
}

TEST(FMaterialShaderStageTests, CanonicalEntryContractUsesStableShaderTypes)
{
	using namespace Durin;
	constexpr std::array<std::string_view, 6> Names{
		"FragmentMain", "GeometryFragmentMain", "ShadowFragmentMain", "HitProxyFragmentMain",
		"VertexMain", "SplineVertexMain"};
	const auto EntryPoints = GetMaterialCompiledEntryPoints();
	ASSERT_GE(EntryPoints.size(), 6u);
	EXPECT_TRUE(std::ranges::equal(std::span(EntryPoints).first(6), Names));
	EXPECT_EQ(GetMaterialCompiledEntryPoints().back(), "PostProcessMaterialMain");
	EXPECT_EQ(GetDefaultMaterialShaderEntries().size(), 6u);
	const auto Options = MakeMaterialShaderCompileRequest(FMaterialTranslatedProgram{.Status = EMaterialProgramStatus::Ok}, false);
	ASSERT_EQ(Options.EntryPoints.size(), 6u);
	ASSERT_EQ(Options.Frequencies.size(), 6u);
	auto OrderedEntries = GetDefaultMaterialShaderEntries();
	std::ranges::sort(OrderedEntries);
	for (size_t Index = 0; Index < Names.size(); ++Index)
	{
		const auto* Entry = FindMaterialShaderEntry(OrderedEntries[Index]);
		ASSERT_NE(Entry, nullptr);
		EXPECT_EQ(Options.EntryPoints[Index], Entry->Name);
		EXPECT_EQ(Options.Frequencies[Index], Entry->Frequency);
	}
}

TEST(FMaterialShaderStageTests, UnrelatedPixelViewReadsDoNotMakeCoverageOrVertexEntriesViewDependent)
{
	using namespace Durin;
	MIR::FCompilerInput Input{.IR = MakeDefaultMaterialCompilerIR()};
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	Input.IR.Nodes = {{.Opcode = EMaterialProgramOpcode::Time, .ResultType = EMaterialProgramValueType::Float},
		{.Opcode = EMaterialProgramOpcode::Splat3, .ResultType = EMaterialProgramValueType::Float3, .Inputs = {0}}};
	auto& Root = Input.IR.GetAttribute(EMaterialProperty::MP_EmissiveColor); Root.bExpression = true; Root.ExpressionIndex = 1;
	const auto Translation = TranslatePreparedMaterialProgram({.Normalized = MIR::Normalize(Input),
		.StaticProperties = Input.StaticProperties, .Environment = Input.Environment});
	ASSERT_TRUE(Translation) << FormatMaterialProgramDiagnostics(Translation.Diagnostics);
	EXPECT_TRUE(Translation.Requirements.bMaterialView);
	for (const auto& Entry : Translation.Entries)
		EXPECT_EQ(Entry.Requirements.Context.bMaterialView,
			Entry.Shader.ShaderType == "FSurfaceFragmentShader" || Entry.Shader.ShaderType == "FGBufferFragmentShader");
}

TEST(FMaterialProgramOwnershipTests, ConfigurationCopiesShareArtifactsAndEditorData)
{
	using namespace Durin;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input{.IR = MakeDefaultMaterialCompilerIR()};
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	const auto First = MaterialBuildPrivate::CompileMaterialInput(Input, false);
	ASSERT_TRUE(First);
	const auto& Original = First.Output;
	Input.Environment.Quality = EMaterialQualityLevel::Low;
	const auto Configured = MaterialBuildPrivate::CompileMaterialInput(Input, false);
	ASSERT_TRUE(Configured);
	EXPECT_TRUE(Configured.RetainedHit);
	EXPECT_EQ(Configured.Output.GetShaderMap().ShaderMapId, Original.GetShaderMap().ShaderMapId);
	EXPECT_EQ(Configured.Output.Resource.ShaderMap, Original.Resource.ShaderMap);
	EXPECT_EQ(Configured.Output.GetShaderMap().DebugData, Original.GetShaderMap().DebugData);
	EXPECT_EQ(Configured.Output.Resource.Quality, EMaterialQualityLevel::Low);
	EXPECT_EQ(Original.Resource.Quality, EMaterialQualityLevel::High);
	EXPECT_TRUE(ValidateMaterialCompilerResult(Configured.Output));
	const auto Value = MIR::Compile(Input);
	EXPECT_EQ(Value.Resource.ShaderMap, Original.Resource.ShaderMap);
	EXPECT_EQ(Value.GetShaderMap().DebugData, Original.GetShaderMap().DebugData);
	EXPECT_EQ(Value.Resource.Quality, EMaterialQualityLevel::Low);
}

TEST(FMaterialProgramOwnershipTests, ResourcePresenceValidationAndCompletionAreIndependent)
{
	using namespace Durin;
	static_assert(!std::is_base_of_v<FMaterialProgram, FMaterialCompilerResult>);
	static_assert(!std::is_convertible_v<FMaterialCompilerResult, FMaterialProgram>);
	static_assert(!std::is_constructible_v<bool, FMaterialProgram>);
	FMaterialProgram Empty;
	EXPECT_FALSE(Empty.HasShaderMap());
	EXPECT_FALSE(Empty.IsValid());
	Empty.ShaderMap = std::make_shared<FMaterialShaderMap>();
	EXPECT_TRUE(Empty.HasShaderMap());
	EXPECT_FALSE(Empty.IsValid());

	InitializeDObjectSystem();
	MIR::FCompilerInput Input{.IR = MakeDefaultMaterialCompilerIR()};
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	auto Completed = MIR::Compile(Input);
	ASSERT_TRUE(Completed);
	ASSERT_TRUE(Completed.Resource.IsValid());
	Completed.Status = EMaterialProgramStatus::Error;
	EXPECT_FALSE(ValidateMaterialCompilerResult(Completed));
	EXPECT_TRUE(Completed.Resource.IsValid());
}

TEST(FMaterialCustomDataTests, NumericRangesAndStableInstanceSnapshotsAreAtomic)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DStaticMeshComponent> Component(NewObject<DStaticMeshComponent>(nullptr, NAME_None));
	const std::array<float, 4> Values{1.f, 2.f, 3.f, 4.f};
	ASSERT_TRUE(Component->SetCustomPrimitiveData(28, Values));
	const auto Before = Component->GetCustomPrimitiveData();
	EXPECT_FALSE(Component->SetCustomPrimitiveData(29, Values));
	const std::array<float, 2> NonFinite{9.f, std::numeric_limits<float>::infinity()};
	EXPECT_FALSE(Component->SetCustomPrimitiveData(0, NonFinite));
	EXPECT_EQ(Component->GetCustomPrimitiveData(), Before);
	std::array<FStaticMeshRenderInstance, 2> Instances{{{.Id = 11}, {.Id = 22}}};
	ASSERT_TRUE(Component->SetRenderInstances(Instances));
	const auto Original = Component->GetRenderInstances();
	ASSERT_TRUE(Component->SetInstanceCustomData(22, 3, Values));
	EXPECT_EQ((*Original)[1].CustomData.Values[3], 0.f);
	EXPECT_EQ((*Component->GetRenderInstances())[1].CustomData.Values[3], 1.f);
	Instances[0].Id = 22;
	const auto Accepted = Component->GetRenderInstances();
	EXPECT_FALSE(Component->SetRenderInstances(Instances));
	EXPECT_EQ(Component->GetRenderInstances(), Accepted);
	Instances = {(*Accepted)[1], (*Accepted)[0]};
	ASSERT_TRUE(Component->SetRenderInstances(Instances));
	ASSERT_TRUE(Component->RemoveRenderInstance(11));
	EXPECT_EQ(Component->GetRenderInstances()->front().Id, 22u);
	EXPECT_EQ(Component->GetRenderInstances()->front().CustomData.Values[3], 1.f);
	EXPECT_FALSE(Component->SetInstanceCustomData(11, 0, Values));
	Instances[0].LocalToPrimitive = FMatrix(0.0);
	EXPECT_FALSE(Component->SetRenderInstances(Instances));
	Instances[0].LocalToPrimitive = FMatrix(1.0);
	Instances[0].LocalToPrimitive[0][3] = 1.0;
	EXPECT_FALSE(Component->SetRenderInstances(Instances));
	ASSERT_TRUE(Component->SetRenderInstances({}));
	EXPECT_TRUE(Component->GetRenderInstances()->empty());
	Component->ClearRenderInstances();
	EXPECT_FALSE(Component->GetRenderInstances());
}

TEST(FMaterialCustomDataTests, TypedCrossSlotReadsCompileAndRoundTripDetachedTranslation)
{
	using namespace Durin;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input;
	Input.IR = MakeDefaultMaterialCompilerIR();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	auto Add = [&](MIR::FNode Node) {
		Input.IR.Nodes.push_back(Node);
		return uint32(Input.IR.Nodes.size() - 1);
	};
	const auto Color = Add({.Opcode = EMaterialProgramOpcode::InstanceCustomData,
		.ResultType = EMaterialProgramValueType::Float3, .Payload = MIR::FCustomDataRead{3}});
	const auto Offset = Add({.Opcode = EMaterialProgramOpcode::PrimitiveCustomData,
		.ResultType = EMaterialProgramValueType::Float3, .Payload = MIR::FCustomDataRead{29}});
	Input.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<size_t>(EMaterialProperty::MP_EmissiveColor))).bExpression = true;
	Input.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<size_t>(EMaterialProperty::MP_EmissiveColor))).ExpressionIndex = Color;
	Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).bExpression = true;
	Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).ExpressionIndex = Offset;
	const auto Encoded = MaterialBuildPrivate::EncodeTranslationInput(Input);
	ASSERT_TRUE(Encoded);
	const auto Decoded = MaterialBuildPrivate::DecodeTranslationInput(Encoded->GetBytes());
	ASSERT_TRUE(Decoded);
	EXPECT_EQ(Decoded->IR, Input.IR);
	const auto Compiled = MIR::Compile(*Decoded);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "no diagnostic" : Compiled.Diagnostics.front().Message);
	EXPECT_EQ(Compiled.GetShaderMap().CompiledShaders.size(), GetDefaultMaterialShaderEntries().size());
	EXPECT_TRUE(Compiled.GetShaderMap().ActiveParameters.empty());
	EXPECT_NE(Compiled.GetDebugData().GeneratedSource.find("nointerpolation float4 customInstanceData"), std::string::npos);
	for (uint32 Width = 0; Width < 4; ++Width)
	{
		MIR::FNode Read{.Opcode = EMaterialProgramOpcode::InstanceCustomData,
			.ResultType = static_cast<EMaterialProgramValueType>(Width), .Payload = MIR::FCustomDataRead{31 - Width}};
		EXPECT_TRUE(Read.HasValidPayload());
		Read.Payload = MIR::FCustomDataRead{32 - Width};
		EXPECT_FALSE(Read.HasValidPayload());
	}
	auto Changed = Input;
	Changed.IR.Nodes[Color].Payload = MIR::FCustomDataRead{4};
	const auto First = MIR::Normalize(Input), Second = MIR::Normalize(Changed);
	ASSERT_TRUE(First && Second);
	EXPECT_NE(First.Identity, Second.Identity);
}

TEST(FMaterialShaderStageTests, GeometryPassSelectionAndDetachedRoundTripAreExact)
{
	using namespace Durin;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input{.IR = MakeDefaultMaterialCompilerIR()};
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	for (uint8 Geometry = 1; Geometry <= 7; ++Geometry)
		for (uint8 Passes = 1; Passes <= 15; ++Passes)
			for (bool Editor : {false, true})
			{
				SCOPED_TRACE(std::format("geometry={} passes={} editor={}", Geometry, Passes, Editor));
				Input.StaticProperties.GeometryUsage = Geometry;
				Input.StaticProperties.PassUsage = Passes;
				Input.Environment.bIncludeEditorPasses = Editor;
				if ((Geometry & 4) && !(Geometry & 1))
				{
					EXPECT_FALSE(ValidateMaterialStaticProperties(Input.StaticProperties));
					continue;
				}
				ASSERT_TRUE(ValidateMaterialStaticProperties(Input.StaticProperties));
				const uint8 EffectivePasses = Passes & (Editor ? 15 : 7);
				const auto Encoded = MaterialBuildPrivate::EncodeTranslationInput(Input);
				ASSERT_TRUE(Encoded);
				const auto Decoded = MaterialBuildPrivate::DecodeTranslationInput(Encoded->GetBytes());
				ASSERT_TRUE(Decoded);
				EXPECT_EQ(Decoded->Environment.bIncludeEditorPasses, Editor);
				const auto Translation = TranslatePreparedMaterialProgram({.Normalized = MIR::Normalize(Input),
					.StaticProperties = Input.StaticProperties, .Environment = Input.Environment});
				ASSERT_TRUE(Translation);
				const auto Requests = MakeMaterialShaderStageRequests(Translation, false);
				size_t Expected = std::popcount(uint8((Geometry & 3) ? EffectivePasses : (EffectivePasses & 2)));
				if (EffectivePasses)
				{
					Expected += bool(Geometry & 1) + bool(Geometry & 2);
				}
				EXPECT_EQ(Requests.size(), Expected);
				for (const auto& Request : Requests)
				{
					ASSERT_EQ(Request.EntryPoints.size(), 1u);
					EXPECT_EQ(Request.Frequencies.size(), 1u);
					if (!Editor) EXPECT_NE(Request.EntryPoints.front(), "HitProxyFragmentMain");
				}
			}
	Input.StaticProperties.GeometryUsage = 0;
	EXPECT_FALSE(ValidateMaterialStaticProperties(Input.StaticProperties));
	Input.StaticProperties.GeometryUsage = 128;
	EXPECT_FALSE(ValidateMaterialStaticProperties(Input.StaticProperties));
	Input.StaticProperties = {}; Input.StaticProperties.PassUsage = 16;
	EXPECT_FALSE(ValidateMaterialStaticProperties(Input.StaticProperties));
}

TEST(FMaterialShaderStageTests, PixelEditsShareVertexProductsAndRequiredSetsAdmitAtomically)
{
	using namespace Durin;
	Testing::FScopedRHIBackendOverride Backend("vulkan");
	InitializeDObjectSystem();
	MIR::FCompilerInput Input{.IR = MakeDefaultMaterialCompilerIR()};
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	const auto Before = GetShaderBuildStats();
	const auto Cold = MIR::Compile(Input, true);
	ASSERT_TRUE(Cold) << FormatMaterialProgramDiagnostics(Cold.Diagnostics);
	const auto AfterCold = GetShaderBuildStats();
	EXPECT_EQ(AfterCold.Compilations - Before.Compilations, 6u);
	const auto Warm = MIR::Compile(Input);
	ASSERT_TRUE(Warm);
	EXPECT_EQ(GetShaderBuildStats().Compilations, AfterCold.Compilations);
	Input.IR.GetAttribute(EMaterialProperty::MP_EmissiveColor).Literal.X = 0.271828f;
	const auto Pixel = MIR::Compile(Input);
	ASSERT_TRUE(Pixel) << FormatMaterialProgramDiagnostics(Pixel.Diagnostics);
	const auto AfterPixel = GetShaderBuildStats();
	EXPECT_NE(Cold.GetShaderMap().ShaderMapId, Pixel.GetShaderMap().ShaderMapId);
	EXPECT_GE(AfterPixel.MemoryHits - AfterCold.MemoryHits, 2u);
	EXPECT_LE(AfterPixel.Compilations - AfterCold.Compilations, 4u);
	uint64 TotalBytes = 0, SharedVertexBytes = 0;
	for (size_t Index = 0; Index < Cold.GetShaderMap().CompiledShaders.size(); ++Index)
	{
		const auto& A = Cold.GetShaderMap().CompiledShaders[Index]; const auto& B = Pixel.GetShaderMap().CompiledShaders[Index];
		TotalBytes += A.Code.GetRetainedCapacityBytes() + B.Code.GetRetainedCapacityBytes();
		if (A.Frequency == EShaderFrequency::Vertex)
		{
			EXPECT_EQ(A.CodeHash, B.CodeHash);
			EXPECT_TRUE(A.Code.SharesStorageWith(B.Code));
			SharedVertexBytes += A.Code.GetRetainedCapacityBytes();
		}
	}
	auto MakeRequests = [](const MIR::FCompilerInput& Value) {
		return MakeMaterialShaderStageRequests(TranslatePreparedMaterialProgram({.Normalized = MIR::Normalize(Value),
			.StaticProperties = Value.StaticProperties, .Environment = Value.Environment}), false);
	};
	const auto OriginalRequests = MakeRequests(Input);
	auto VertexChange = Input; VertexChange.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).Literal.X = 0.125f;
	const auto VertexRequests = MakeRequests(VertexChange);
	for (size_t Index = 0; Index < OriginalRequests.size(); ++Index)
		if (OriginalRequests[Index].Frequencies.front() == EShaderFrequency::Vertex) EXPECT_NE(OriginalRequests[Index].VirtualPath, VertexRequests[Index].VirtualPath);
		else EXPECT_EQ(OriginalRequests[Index].VirtualPath, VertexRequests[Index].VirtualPath);
	auto InterfaceChange = Input;
	InterfaceChange.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::InstanceCustomData,
		.ResultType = EMaterialProgramValueType::Float3, .Payload = MIR::FCustomDataRead{3}});
	auto& Emissive = InterfaceChange.IR.GetAttribute(EMaterialProperty::MP_EmissiveColor);
	Emissive.bExpression = true; Emissive.ExpressionIndex = uint32(InterfaceChange.IR.Nodes.size() - 1);
	const auto InterfaceRequests = MakeRequests(InterfaceChange);
	ASSERT_EQ(InterfaceRequests.size(), 6u);
	for (size_t Index = 0; Index < OriginalRequests.size(); ++Index)
		if (OriginalRequests[Index].Frequencies.front() == EShaderFrequency::Vertex) EXPECT_NE(OriginalRequests[Index].VirtualPath, InterfaceRequests[Index].VirtualPath);
	auto LayoutChange = Input;
	const FGuid ParameterId(0, 0, 0, 905);
	LayoutChange.Parameters.push_back({ParameterId, EMaterialParameterType::Scalar});
	LayoutChange.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Parameter,
		.ResultType = EMaterialProgramValueType::Float, .Payload = ParameterId});
	auto& ParameterRoot = LayoutChange.IR.GetAttribute(EMaterialProperty::MP_Roughness);
	ParameterRoot.bExpression = true; ParameterRoot.ExpressionIndex = uint32(LayoutChange.IR.Nodes.size() - 1);
	const auto LayoutRequests = MakeRequests(LayoutChange);
	ASSERT_EQ(LayoutRequests.size(), 6u);
	for (size_t Index = 0; Index < OriginalRequests.size(); ++Index)
		if (OriginalRequests[Index].Frequencies.front() == EShaderFrequency::Vertex) EXPECT_NE(OriginalRequests[Index].VirtualPath, LayoutRequests[Index].VirtualPath);
	Input.Environment.bIncludeEditorPasses = false;
	const auto Game = MIR::Compile(Input);
	ASSERT_TRUE(Game);
	EXPECT_EQ(Game.GetShaderMap().CompiledShaders.size(), 5u);
	EXPECT_FALSE(std::ranges::contains(Game.GetShaderMap().RequiredEntries, std::string("FHitProxyFragmentShader")));
	EXPECT_EQ(GetShaderBuildStats().Compilations, AfterPixel.Compilations);
	Input.StaticProperties.GeometryUsage = 1; Input.StaticProperties.PassUsage = 1;
	const auto Selected = MIR::Compile(Input);
	ASSERT_TRUE(Selected);
	ASSERT_EQ(Selected.GetShaderMap().CompiledShaders.size(), 2u);
	EXPECT_EQ(Selected.GetShaderMap().CompiledShaders[0].SourceEntryPoint, "VertexMain");
	EXPECT_EQ(Selected.GetShaderMap().CompiledShaders[1].SourceEntryPoint, "FragmentMain");
	EXPECT_TRUE(FindCompiledMaterialStage(Selected.GetShaderMap(), "FragmentMain").Code.SharesStorageWith(FindCompiledMaterialStage(Pixel.GetShaderMap(), "FragmentMain").Code));
	EXPECT_TRUE(FindCompiledMaterialStage(Selected.GetShaderMap(), "VertexMain").Code.SharesStorageWith(FindCompiledMaterialStage(Pixel.GetShaderMap(), "VertexMain").Code));
	EXPECT_TRUE(ValidateMaterialCompilerResult(Selected));
	for (uint32 Kind = 0; Kind < 4; ++Kind)
	{
		auto Broken = Selected;
		auto BrokenArtifact = std::make_shared<FMaterialShaderMap>(Selected.GetShaderMap());
		Broken.Resource.ShaderMap = BrokenArtifact;
		if (Kind == 0) BrokenArtifact->CompiledShaders.pop_back();
		if (Kind == 1) BrokenArtifact->CompiledShaders.push_back(Cold.GetShaderMap().CompiledShaders[5]);
		if (Kind == 2) BrokenArtifact->Entries[0].Shader.OutputLayout.Version++;
		if (Kind == 3) BrokenArtifact->CompiledShaders[0].Frequency = EShaderFrequency::Fragment;
		EXPECT_FALSE(ValidateMaterialCompilerResult(Broken));
	}
	FByteBuffer Bytes;
	ASSERT_TRUE(Testing::EncodeMaterialCookedProgramFamilyForTest(Selected.Resource, Input.StaticProperties,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes));
	FMaterialStaticProperties LoadedProperties; std::shared_ptr<const FMaterialProgram> Loaded;
	ASSERT_TRUE(Testing::DecodeMaterialCookedProgramFamilyForTest(Bytes, ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, Selected.Resource, LoadedProperties, Loaded));
	ASSERT_TRUE(Loaded);
	EXPECT_EQ(LoadedProperties, Input.StaticProperties);
	EXPECT_EQ(Loaded->GetShaderMap().RequiredEntries, Selected.GetShaderMap().RequiredEntries);
	EXPECT_EQ(Loaded->GetShaderMap().CompiledShaders.size(), 2u);
	EXPECT_TRUE(Loaded->GetDebugData().GeneratedSource.empty());
	EXPECT_TRUE(Loaded->GetDebugData().IR.Nodes.empty());
	RecordProperty("cold_stage_compilations_count", AfterCold.Compilations - Before.Compilations);
	RecordProperty("pixel_edit_stage_compilations_count", AfterPixel.Compilations - AfterCold.Compilations);
	RecordProperty("shared_vertex_products_count", 2);
	RecordProperty("shared_vertex_capacity_bytes", SharedVertexBytes);
	RecordProperty("pair_code_capacity_upper_bound_bytes", TotalBytes - SharedVertexBytes);
	RecordProperty("authority", "structural_debug_observation");
	std::cout << "[StageProducts] cold_compiles=" << AfterCold.Compilations - Before.Compilations
		<< " warm_compiles=0 pixel_edit_compiles=" << AfterPixel.Compilations - AfterCold.Compilations
		<< " shared_vertex_products=2 pair_retained_capacity_bytes=" << TotalBytes
		<< " shared_vertex_bytes=" << SharedVertexBytes << " pair_capacity_after_vertex_sharing_bytes=" << TotalBytes - SharedVertexBytes
		<< " game_stages=5 local_forward_stages=2\n";
}

TEST(FMaterialProgramCompilerTests, DiagnosticSummaryPreservesOrderAndBoundsPresentation)
{
	using namespace Durin;
	const std::array Diagnostics{
		FMaterialProgramDiagnostic{.Message = "first"},
		FMaterialProgramDiagnostic{.Message = "second"}};
	EXPECT_EQ(FormatMaterialProgramDiagnostics(Diagnostics),
		Diagnostics[0].Message + "\n" + Diagnostics[1].Message);
	EXPECT_EQ(FormatMaterialProgramDiagnostics({}, "fallback"), "fallback");
	EXPECT_TRUE(FormatMaterialProgramDiagnostics({}).empty());
	const std::vector<FMaterialProgramDiagnostic> Many(1000, Diagnostics.front());
	EXPECT_EQ(FormatMaterialProgramDiagnostics(Many).size(), 4096u);
	EXPECT_EQ(FormatMaterialProgramDiagnostics({}, std::string(8192, 'x')).size(), 4096u);
}

TEST(FMaterialProgramCompilerTests, ExplicitTargetDoesNotFollowHostBackend)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FMaterialCompilerEnvironment First, Second;
	{
		Testing::FScopedRHIBackendOverride Backend("vulkan");
		ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(First, VulkanShaderTarget));
	}
	{
		Testing::FScopedRHIBackendOverride Backend("metal");
		ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Second, VulkanShaderTarget));
	}
	EXPECT_EQ(First.Target, Second.Target);
	EXPECT_EQ(First.CompilerIdentity, Second.CompilerIdentity);
	EXPECT_EQ(First.Dependencies, Second.Dependencies);
	auto Invalid = VulkanShaderTarget;
	Invalid.OutputFormat = EShaderCodeFormat::Msl20Source;
	EXPECT_FALSE(BuildDefaultMaterialCompilerEnvironment(Second, Invalid));
}

TEST(FMaterialProgramCompilerTests, CookVariantsAreBoundedAndCanonical)
{
	using namespace Durin;
	const auto All = ResolveMaterialCookConfigurations({});
	ASSERT_TRUE(All);
	EXPECT_EQ(All->size(), MaterialSupportedConfigurationCount);
	const FMaterialCookConfiguration Low{EMaterialQualityLevel::Low, ERHIFeatureLevel::SM5};
	const FMaterialCookConfiguration High{EMaterialQualityLevel::High, ERHIFeatureLevel::SM5};
	const std::array Requested{High, Low};
	const auto Selected = ResolveMaterialCookConfigurations(Requested);
	ASSERT_TRUE(Selected);
	EXPECT_EQ(*Selected, (std::vector<FMaterialCookConfiguration>{Low, High}));
	EXPECT_FALSE(ResolveMaterialCookConfigurations(std::array{High, High}));
	EXPECT_FALSE(ResolveMaterialCookConfigurations(std::array{FMaterialCookConfiguration{static_cast<EMaterialQualityLevel>(255)}}));
}

namespace
{
	auto InitializeSurfaceRootSemantics(Durin::MIR::FModule& IR) -> void
	{
		for (const auto& Definition : Durin::FMaterialAttributeDefinitionMap::GetDefinitions())
		{
			const auto Semantics = Durin::GetMaterialSurfaceOutputSemantics(
				Definition.Property);
			auto& Input = IR.GetAttribute(Definition.Property);
			Input.Type = Semantics.Type;
			Input.LegalStages = Semantics.Stages;
			Input.SpatialKind = Semantics.Kind;
			Input.CoordinateSpace = Semantics.Space;
		}
	}

	auto ResolveNodeSemantics(Durin::MIR::FModule& IR, Durin::MIR::FNode& Node) -> void
	{
		std::vector<Durin::FMaterialValueSemantics> Inputs;
		for (size_t Slot = 0; Slot < Node.Inputs.size(); ++Slot)
		{
			auto Value = IR.Nodes.at(Node.Inputs[Slot]).GetSemantics();
			if (Node.ScalarBroadcastMask & (1u << Slot)) Value.Type = Durin::EMaterialProgramValueType::Float;
			Inputs.push_back(Value);
		}
		const auto* Transform = Node.Opcode == Durin::EMaterialProgramOpcode::TransformPosition
			|| Node.Opcode == Durin::EMaterialProgramOpcode::TransformDirection
			|| Node.Opcode == Durin::EMaterialProgramOpcode::TransformNormal
			? &std::get<Durin::FMaterialTransformPayload>(Node.Payload) : nullptr;
		const auto Result = Durin::ResolveMaterialProgramNodeSemantics(
			Node.Opcode, Node.ResultType, Inputs, Transform, Node.GetTextureReadMode());
		if (!Result) return;
		if (Node.Opcode == Durin::EMaterialProgramOpcode::Constant
			&& Node.SpatialKind != Durin::EMaterialSpatialKind::None) return;
		Node.LegalStages = Result->Stages;
		Node.SpatialKind = Result->Kind;
		Node.CoordinateSpace = Result->Space;
	}
}

TEST(FMaterialShaderStageTests, VertexClosureOrderIgnoresPixelReachabilityAndNodeRenumbering)
{
	using namespace Durin;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input{.IR = MakeDefaultMaterialCompilerIR()};
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	auto Add = [&](MIR::FNode Node) {
		ResolveNodeSemantics(Input.IR, Node);
		Input.IR.Nodes.push_back(std::move(Node));
		return uint32(Input.IR.Nodes.size() - 1);
	};
	const auto Data = Add({.Opcode = EMaterialProgramOpcode::PrimitiveCustomData,
		.ResultType = EMaterialProgramValueType::Float3, .Payload = MIR::FCustomDataRead{0}});
	const auto Value = Add({.Opcode = EMaterialProgramOpcode::Constant,
		.ResultType = EMaterialProgramValueType::Float3, .Payload = FMaterialProgramLiteral{.1f, .2f, .3f}});
	const auto Sum = Add({.Opcode = EMaterialProgramOpcode::Add,
		.ResultType = EMaterialProgramValueType::Float3, .Inputs = {Data, Value}});
	Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).bExpression = true; Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).ExpressionIndex = Sum;
	auto& Pixel = Input.IR.GetAttribute(EMaterialProperty::MP_EmissiveColor);
	Pixel.bExpression = true; Pixel.ExpressionIndex = Data;
	const auto First = TranslatePreparedMaterialProgram({.Normalized = MIR::Normalize(Input),
		.StaticProperties = Input.StaticProperties, .Environment = Input.Environment});
	ASSERT_TRUE(First);
	// The same vertex DAG previously encountered through a pixel root is now
	// first emitted through WPO, reversing normalized dependency numbering.
	Pixel.bExpression = false; Pixel.Literal = {.17f, .23f, .31f};
	const auto Second = TranslatePreparedMaterialProgram({.Normalized = MIR::Normalize(Input),
		.StaticProperties = Input.StaticProperties, .Environment = Input.Environment});
	ASSERT_TRUE(Second);
	EXPECT_NE(First.Identity, Second.Identity);
	const auto A = MakeMaterialShaderStageRequests(First, false);
	const auto B = MakeMaterialShaderStageRequests(Second, false);
	ASSERT_EQ(A.size(), 6u); ASSERT_EQ(B.size(), 6u);
	for (size_t Index = 0; Index < A.size(); ++Index)
	{
		if (A[Index].Frequencies.front() != EShaderFrequency::Vertex) continue;
		EXPECT_EQ(A[Index].Source, B[Index].Source);
		EXPECT_EQ(A[Index].VirtualPath, B[Index].VirtualPath);
	}
}

TEST(FMaterialTranslationTests, PersistentHitsSkipTransformsAndPayloadResolution)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	auto Input = MakeSyntheticMaterialCompilerInput();
	const auto Previous = FPaths::DerivedDataCacheDir();
	const auto Root = Testing::GetTestWorkDirectory() / "MaterialTranslationWarm";
	FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
	struct FRestore { std::string Path; ~FRestore() { FPaths::SetDerivedDataCacheDirForTests(Path); } } Restore{Previous};
	const auto Before = GetTranslationStats();
	const auto Cold = TranslateMaterialInput(Input, false);
	ASSERT_TRUE(Cold);
	const auto Produced = GetTranslationStats();
	EXPECT_EQ(Produced.Producers, Before.Producers + 1);
	EXPECT_EQ(Produced.Resolutions, Before.Resolutions + 1);
	for (auto& Source : Input.Sources) { Source.NodeId = FGuid::NewGuid(); Source.FunctionAssetPath = "RequestingAsset"; }
	Input.Environment.CompilerIdentity = "OtherCompiler";
	const auto Warm = TranslateMaterialInput(Input, false);
	ASSERT_TRUE(Warm);
	EXPECT_EQ(Warm.GeneratedSource, Cold.GeneratedSource);
	EXPECT_NE(Warm.Identity, Cold.Identity);
	EXPECT_EQ(GetTranslationStats().Producers, Produced.Producers);
	EXPECT_EQ(GetTranslationStats().Resolutions, Produced.Resolutions);
	EXPECT_EQ(GetTranslationStats().Hits, Produced.Hits + 1);
	for (const auto& Source : Warm.Sources)
	{
		EXPECT_EQ(Source.Source.FunctionAssetPath, "RequestingAsset");
		EXPECT_TRUE(std::ranges::contains(Input.Sources, Source.Source.NodeId, &MIR::FSource::NodeId));
		ASSERT_LT(Source.Ordinal, Input.Sources.size());
		EXPECT_EQ(Source.Source.NodeId, Input.Sources[Source.Ordinal].NodeId);
	}
	ASSERT_TRUE(TranslateMaterialInput(Input, true));
	EXPECT_EQ(GetTranslationStats().Producers, Produced.Producers + 1);
}

TEST(FMaterialTranslationTests, CanonicalInputAndTypedOutputHaveIndependentConsumerIdentity)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	auto Input = MakeSyntheticMaterialCompilerInput();
	auto Bytes = EncodeTranslationInput(Input);
	ASSERT_TRUE(Bytes);
	auto Other = Input;
	std::ranges::reverse(Other.Parameters);
	Other.Environment.CompilerIdentity = "DifferentCompiler";
	Other.Environment.Dependencies.clear();
	for (auto& Source : Other.Sources) { Source.NodeId = FGuid::NewGuid(); Source.FunctionAssetPath = "AnotherAsset"; }
	auto SameBytes = EncodeTranslationInput(Other);
	ASSERT_TRUE(SameBytes);
	EXPECT_TRUE(std::ranges::equal(Bytes->GetBytes(), SameBytes->GetBytes()));
	Other.Environment.Target = "metal-msl-2.0";
	const auto TargetBytes = EncodeTranslationInput(Other);
	ASSERT_TRUE(TargetBytes);
	EXPECT_NE(FXxHash128::HashBuffer(Bytes->GetBytes()), FXxHash128::HashBuffer(TargetBytes->GetBytes()));
	const auto TargetInput = DecodeTranslationInput(TargetBytes->GetBytes());
	ASSERT_TRUE(TargetInput);
	EXPECT_EQ(TargetInput->Environment.Target, Other.Environment.Target);
	Other.Environment.Target = Input.Environment.Target;
	Other.Environment.ResourceLimits.SampledImages += 1;
	auto Changed = EncodeTranslationInput(Other);
	ASSERT_TRUE(Changed);
	EXPECT_NE(FXxHash128::HashBuffer(Bytes->GetBytes()), FXxHash128::HashBuffer(Changed->GetBytes()));
	auto Decoded = DecodeTranslationInput(Bytes->GetBytes());
	ASSERT_TRUE(Decoded);
	FMaterialPreparedProgram Prepared{.Normalized = MIR::Normalize(*Decoded), .StaticProperties = Decoded->StaticProperties, .Environment = Decoded->Environment};
	auto Product = TranslatePreparedMaterialProgram(Prepared);
	ASSERT_TRUE(Product);
	const auto Identity = FXxHash128::HashBuffer(Bytes->GetBytes());
	auto Encoded = EncodeTranslationProduct(Product, Identity);
	ASSERT_TRUE(Encoded);
	const auto Restored = DecodeTranslationProduct(Encoded->GetBytes(), Identity, Input);
	ASSERT_TRUE(Restored);
	EXPECT_EQ(Restored->Diagnostics, Product.Diagnostics);
	EXPECT_FALSE(DecodeTranslationProduct(Encoded->GetBytes(), {1, 2}, Input));
	EXPECT_FALSE(DecodeTranslationProduct(Encoded->GetBytes().first(Encoded->size() - 1), Identity, Input));
	FByteBuffer Wrong(Encoded->begin(), Encoded->end());
	Wrong.front() = std::byte{TranslationSchema + 1};
	EXPECT_FALSE(DecodeTranslationProduct(Wrong, Identity, Input));
	Wrong.front() = std::byte{TranslationSchema - 1};
	EXPECT_FALSE(DecodeTranslationProduct(Wrong, Identity, Input));
	Product.Status = EMaterialProgramStatus::Error;
	Product.Diagnostics = {{.Message = "cached translation failure"}};
	const auto Negative = EncodeTranslationProduct(Product, Identity);
	ASSERT_TRUE(Negative);
	const auto RestoredNegative = DecodeTranslationProduct(Negative->GetBytes(), Identity, Input);
	ASSERT_TRUE(RestoredNegative);
	EXPECT_EQ(RestoredNegative->Diagnostics, Product.Diagnostics);
	auto OversizedMessage = Product;
	OversizedMessage.Diagnostics.front().Message.assign(MaterialProgramMaxDiagnosticMessageBytes + 1, 'x');
	EXPECT_FALSE(EncodeTranslationProduct(OversizedMessage, Identity));
	Product.Status = EMaterialProgramStatus::Ok;
	Product.Diagnostics.clear();
	Product.Layout.UniformPayloadSize += 16;
	auto BadLayout = EncodeTranslationProduct(Product, Identity);
	ASSERT_TRUE(BadLayout);
	EXPECT_FALSE(DecodeTranslationProduct(BadLayout->GetBytes(), Identity, Input));
}

TEST(FMaterialTranslationTests, DeterministicNegativeHitsPreserveMessagesAndLocations)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	auto Input = MakeSyntheticMaterialCompilerInput();
	Input.IR.Nodes.front().Inputs = {UINT32_MAX};
	const auto Previous = FPaths::DerivedDataCacheDir();
	FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "MaterialTranslationNegative").generic_string());
	struct FRestore { std::string Path; ~FRestore() { FPaths::SetDerivedDataCacheDirForTests(Path); } } Restore{Previous};
	const auto Cold = TranslateMaterialInput(Input, false);
	ASSERT_FALSE(Cold);
	ASSERT_FALSE(Cold.Diagnostics.empty());
	const auto Produced = GetTranslationStats();
	for (auto& Source : Input.Sources) { Source.NodeId = FGuid::NewGuid(); Source.FunctionAssetPath = "NegativeRequestAsset"; Source.PortId = FGuid::NewGuid(); }
	const auto Warm = TranslateMaterialInput(Input, false);
	EXPECT_FALSE(Warm);
	EXPECT_EQ(Warm.Diagnostics.front().Message, Cold.Diagnostics.front().Message);
	for (const auto& Diagnostic : Warm.Diagnostics) if (Diagnostic.NodeId.IsValid())
	{
		const auto Source = std::ranges::find(Input.Sources, Diagnostic.NodeId, &MIR::FSource::NodeId); ASSERT_NE(Source, Input.Sources.end());
		EXPECT_EQ(Diagnostic.FunctionAssetPath, "NegativeRequestAsset"); EXPECT_EQ(Diagnostic.PortId, Source->PortId);
	}
	EXPECT_EQ(GetTranslationStats().Producers, Produced.Producers);
	EXPECT_EQ(GetTranslationStats().Resolutions, Produced.Resolutions);
	EXPECT_EQ(GetTranslationStats().Hits, Produced.Hits + 1);
}

TEST(FMaterialTranslationTests, LateNormalizationFailureRetainsItsNegativeProduct)
{
	using namespace Durin; using namespace MaterialBuildPrivate;
	auto Input = MakeSyntheticMaterialCompilerInput(); Input.Environment.ResourceLimits.SampledImages = 0;
	const auto Previous = FPaths::DerivedDataCacheDir(); FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "MaterialResourceNegative").generic_string());
	struct FRestore { std::string Path; ~FRestore() { FPaths::SetDerivedDataCacheDirForTests(Path); } } Restore{Previous};
	const auto Cold = TranslateMaterialInput(Input, false); ASSERT_FALSE(Cold); ASSERT_FALSE(Cold.Diagnostics.empty());
	EXPECT_EQ(Cold.Diagnostics.front().Message, "The material layout exceeds the target uniform or descriptor budget. [index 0]");
	const auto Before = GetTranslationStats(); const auto Warm = TranslateMaterialInput(Input, false); ASSERT_FALSE(Warm);
	EXPECT_EQ(Warm.Diagnostics, Cold.Diagnostics); EXPECT_EQ(GetTranslationStats().Producers, Before.Producers); EXPECT_EQ(GetTranslationStats().Hits, Before.Hits + 1);
}

TEST(FMaterialTranslationTests, MaterialCacheWaitReleasesTheOnlyAvailableWorker)
{
	using namespace Durin;
	using namespace DerivedData;
	using namespace MaterialBuildPrivate;
	InitializeDObjectSystem();
	struct FGate final : ICacheBackend
	{
		mutable FThreadEvent Entered, Release;
		auto Get(const FCacheGetBackendRequest&) const -> FCacheGetResult
		{
			Entered.Trigger();
			Release.WaitFor(10);
			return std::optional<FCacheRecord>{};
		}
		auto Put(const FCachePutBackendRequest&) const -> FCachePutResult { return {}; }
	};
	auto Cache = std::make_shared<FGate>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(MakeTranslationFunction()));
	SetTranslationServiceForTests(Service);
	FThreadEvent Hold, Held; std::atomic_uint32_t Holding = 0;
	std::vector<Tasks::FTask> Blockers;
	const auto Count = GetTaskSchedulerDiagnostics().WorkerCount;
	struct FRestore { std::shared_ptr<FGate> Cache; FThreadEvent& Hold; std::vector<Tasks::FTask>& Blockers; ~FRestore() { Cache->Release.Trigger(); Hold.Trigger(); for (auto& Task : Blockers) WaitTask(Task); SetTranslationServiceForTests({}); } } Restore{Cache, Hold, Blockers};
	for (uint32 Index = 1; Index < Count; ++Index) Blockers.push_back(Tasks::LaunchTask("HoldOtherMaterialWorkers", [&] { if (Holding.fetch_add(1) + 1 == Count - 1) Held.Trigger(); Hold.WaitFor(10); }));
	if (Count > 1) ASSERT_TRUE(Held.WaitFor(5));
	auto Input = std::make_shared<const MIR::FCompilerInput>(MakeSyntheticMaterialCompilerInput());
	auto& Manager = FShaderCompilingManager::Get(); std::atomic_uint32_t Completed = 0;
	auto Compile = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {}, [&](auto Result) { EXPECT_TRUE(Result); ++Completed; });
	ASSERT_TRUE(Compile);
	ASSERT_TRUE(Cache->Entered.WaitFor(5));
	FThreadEvent Progress; auto Unrelated = Tasks::LaunchTask("UnrelatedMaterialProgress", [&] { Progress.Trigger(); });
	ASSERT_TRUE(Progress.WaitFor(5));
	Cache->Release.Trigger(); Manager.FinishCompilation(Compile); EXPECT_FALSE(Manager.IsCompiling(Compile)); EXPECT_EQ(Completed, 1u);
}

TEST(FMaterialTranslationTests, RejectedHitsRebuildOnceAndStoreFailureKeepsLocalProduct)
{
	using namespace Durin;
	using namespace DerivedData;
	using namespace MaterialBuildPrivate;
	InitializeDObjectSystem();
	struct FCache final : ICacheBackend
	{
		mutable std::optional<FCacheRecord> Record;
		mutable bool Corrupt = false, FailStore = false;
		auto Get(const FCacheGetBackendRequest& Request) const -> FCacheGetResult override
		{
			if (!Record) return std::optional<FCacheRecord>{};
			if (!Corrupt) return Record;
			const auto* Value = Record->FindValue(FValueId("Product"));
			EXPECT_NE(Value, nullptr);
			if (!Value) return std::unexpected(FCacheError{ECacheError::Corrupt, "Missing fixture product."});
			const auto Data = Value->GetData(); EXPECT_FALSE(Data.IsEmpty());
			if (Data.IsEmpty()) return std::unexpected(FCacheError{ECacheError::Corrupt, "Empty fixture product."});
			FByteBuffer Bytes(Data.begin(), Data.end());
			Bytes.front() = std::byte{2};
			FBuildOutputBuilder Output("Material.Translation", 1); Output.AddValue(FValueId("Product"), FSharedByteBuffer::Take(std::move(Bytes)));
			return std::optional{Durin::DerivedData::Private::MakeBuildCacheRecord(Request.Key, std::move(Output).Build().value()).value()};
		}
		auto Put(const FCachePutBackendRequest& Request) const -> FCachePutResult override
		{ if (FailStore) return std::unexpected(FCacheError{ECacheError::StorageFailure, "fixture"}); Record = Request.Record; return Request.Record; }
	};
	auto Cache = std::make_shared<FCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(MakeTranslationFunction()));
	SetTranslationServiceForTests(Service);
	struct FRestore { ~FRestore() { SetTranslationServiceForTests({}); } } Restore;
	auto Input = std::make_shared<const MIR::FCompilerInput>(MakeSyntheticMaterialCompilerInput());
	auto Run = [&](FMaterialTranslatedProgram& Product, bool Force = false) {
		FRequestOwner Owner; SubmitTranslation(Input, Force, Owner, [&](auto Value) { Product = std::move(Value); });
		Owner.Wait();
	};
	FMaterialTranslatedProgram Cold; Run(Cold); ASSERT_TRUE(Cold);
	const auto Before = GetTranslationStats(); Cache->Corrupt = true;
	FMaterialTranslatedProgram Rebuilt; Run(Rebuilt); ASSERT_TRUE(Rebuilt);
	EXPECT_EQ(GetTranslationStats().Producers, Before.Producers + 1);
	Cache->FailStore = true;
	FMaterialTranslatedProgram Local; Run(Local, true); ASSERT_TRUE(Local);
	EXPECT_EQ(Local.GeneratedSource, Cold.GeneratedSource);
	FRequestOwner Canceled; Canceled.Cancel(); uint32 Completions = 0;
	SubmitTranslation(Input, true, Canceled, [&](auto Product) { EXPECT_FALSE(Product); EXPECT_EQ(Product.Status, EMaterialProgramStatus::Canceled); EXPECT_TRUE(Product.Diagnostics.empty()); ++Completions; });
	Canceled.Wait(); EXPECT_EQ(Completions, 1u);
}

TEST(FMaterialTranslationTests, PreservesExecutionDiagnostics)
{
	using namespace Durin; using namespace DerivedData; using namespace MaterialBuildPrivate;
	InitializeDObjectSystem();
	struct FFailingTranslation final : IBuildFunction
	{
		std::shared_ptr<const IBuildFunction> Inner = MakeTranslationFunction();
		auto GetName() const -> std::string_view override { return Inner->GetName(); }
		auto GetVersion() const -> uint32 override { return Inner->GetVersion(); }
		auto Configure(FBuildConfigContext& Context) const -> void override { Inner->Configure(Context); }
		auto Build(FBuildContext& Context) const -> void override { Context.FailExecution("translation producer diagnostic"); }
	};
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({});
	ASSERT_TRUE(Service->Register(std::make_shared<FFailingTranslation>()));
	SetTranslationServiceForTests(Service);
	struct FRestore { ~FRestore() { SetTranslationServiceForTests({}); } } Restore;
	auto Input = std::make_shared<const MIR::FCompilerInput>(MakeSyntheticMaterialCompilerInput());
	FMaterialTranslatedProgram Product; FRequestOwner Owner;
	SubmitTranslation(Input, true, Owner, [&](auto Value) { Product = std::move(Value); });
	Owner.Wait();
	EXPECT_EQ(Product.Status, EMaterialProgramStatus::Error);
	ASSERT_FALSE(Product.Diagnostics.empty());
	EXPECT_EQ(Product.Diagnostics.front().Message, "translation producer diagnostic");

}

TEST(FMaterialTranslationTests, QueuedRequestsExceedFormerAdmissionLimitAndDrainOnce)
{
	using namespace Durin; using namespace MaterialBuildPrivate;
	auto Captured = std::make_shared<const MIR::FCompilerInput>(MakeSyntheticMaterialCompilerInput());
	struct FGate final : DerivedData::ICacheBackend
	{
		mutable FThreadEvent Entered, Release;
		auto Get(const DerivedData::FCacheGetBackendRequest&) const -> DerivedData::FCacheGetResult override
		{ Entered.Trigger(); Release.WaitFor(10); return std::optional<DerivedData::FCacheRecord>{}; }
		auto Put(const DerivedData::FCachePutBackendRequest&) const -> DerivedData::FCachePutResult override { return {}; }
	};
	auto Cache = std::make_shared<FGate>();
	auto Service = DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(MakeTranslationFunction()));
	SetTranslationServiceForTests(Service);
	auto& Manager = FShaderCompilingManager::Get();
	std::vector<FMaterialProgramCompileHandle> Handles;
	Tasks::FCancellationToken Cancellation;
	struct FRestore {
		std::shared_ptr<FGate> Cache; std::vector<FMaterialProgramCompileHandle>& Handles; Tasks::FCancellationToken& Cancellation;
		~FRestore() {
			Cancellation.Cancel(); Cache->Release.Trigger();
			auto& Manager = FShaderCompilingManager::Get();
			for (const auto& Handle : Handles) Manager.CancelCompilation(Handle);
			for (const auto& Handle : Handles) Manager.FinishCompilation(Handle);
			Handles.clear(); SetTranslationServiceForTests({});
		}
	} Restore{Cache, Handles, Cancellation};
	std::atomic_uint32_t Completions = 0;
	for (uint32 Index = 0; Index < 40; ++Index) {
		auto Compile = MaterialBuildPrivate::FMaterialCompilation::Compile(Captured, {.IsCancellationRequested = [&Cancellation] { return Cancellation.IsCanceled(); }}, [&](auto) { ++Completions; });
		ASSERT_TRUE(Compile); Handles.push_back(std::move(Compile));
	}
	ASSERT_TRUE(Cache->Entered.WaitFor(5));
	Cancellation.Cancel(); Cache->Release.Trigger();
	for (const auto& Handle : Handles) Manager.CancelCompilation(Handle);
	for (const auto& Handle : Handles) Manager.FinishCompilation(Handle);
	EXPECT_EQ(Completions, 40u); Handles.clear();
	{
		auto Recovered = MaterialBuildPrivate::FMaterialCompilation::Compile(Captured, {.IsCancellationRequested = [&Cancellation] { return Cancellation.IsCanceled(); }}, [](auto Result) {
			EXPECT_EQ(Result.Output.Status, EMaterialProgramStatus::Canceled);
		});
		ASSERT_TRUE(Recovered); Manager.FinishCompilation(Recovered);
	}
	auto Input = MakeSyntheticMaterialCompilerInput(); Input.IR.Nodes.reserve(TranslationInputBytes / sizeof(MIR::FNode) + 1);
	const auto Before = GetTranslationStats();
	// Unused vector capacity is not serialized content and does not invalidate input.
	EXPECT_TRUE(EncodeTranslationInput(Input));
	EXPECT_EQ(GetTranslationStats().Producers, Before.Producers);
	FByteBuffer Oversized(TranslationOutputBytes + 1); EXPECT_FALSE(DecodeTranslationProduct(Oversized, {}, Input));
}

TEST(FMaterialTranslationTests, CancellationDuringCacheIOCompletesAndRetiresWithoutPublicationPump)
{
	using namespace Durin; using namespace DerivedData; using namespace MaterialBuildPrivate;
	struct FGate final : ICacheBackend
	{
		mutable FThreadEvent Entered, Release;
		auto Get(const FCacheGetBackendRequest&) const -> FCacheGetResult override
		{
			Entered.Trigger();
			Release.WaitFor(10);
			return std::optional<FCacheRecord>{};
		}
		auto Put(const FCachePutBackendRequest& Request) const -> FCachePutResult override { return Request.Record; }
	};
	auto Cache = std::make_shared<FGate>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(MakeTranslationFunction()));
	SetTranslationServiceForTests(Service);
	struct FRestore { std::shared_ptr<FGate> Cache; ~FRestore() { Cache->Release.Trigger(); SetTranslationServiceForTests({}); } } Restore{Cache};
	auto& Manager = FShaderCompilingManager::Get(); std::atomic_uint32_t Calls = 0;
	auto Compile = MaterialBuildPrivate::FMaterialCompilation::Compile(std::make_shared<const MIR::FCompilerInput>(MakeSyntheticMaterialCompilerInput()), {}, [&](auto Result) { EXPECT_FALSE(Result); EXPECT_TRUE(Result.Output.Diagnostics.empty()); EXPECT_EQ(Result.Output.Status, EMaterialProgramStatus::Canceled); ++Calls; });
	ASSERT_TRUE(Compile);
	ASSERT_TRUE(Cache->Entered.WaitFor(5)); const auto Begin = std::chrono::steady_clock::now(); Manager.CancelCompilation(Compile);
	Cache->Release.Trigger(); Manager.FinishCompilation(Compile); EXPECT_FALSE(Manager.IsCompiling(Compile)); EXPECT_EQ(Calls, 1u);
	RecordProperty("CacheCancelRetirementMicroseconds", std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Begin).count());
}

TEST(FMaterialProgramOwnershipTests, ManagerAndSynchronousCallersShareReuseAndForcePolicy)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	InitializeDObjectSystem();
	FMaterialShaderMap::ClearCache();
	struct FRestore { ~FRestore() { FMaterialShaderMap::ClearCache(); } } Restore;
	auto Input = std::make_shared<MIR::FCompilerInput>();
	Input->IR = MakeDefaultMaterialCompilerIR();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input->Environment));
	const auto Build = [&](bool Force) {
		FMaterialBuildResult Result;
		auto& Manager = FShaderCompilingManager::Get();
		auto Compile = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.bForceRecompile = Force}, [&](auto Value) { Result = std::move(Value); });
		if (Compile) Manager.FinishCompilation(Compile);
		return Result;
	};
	const auto First = Build(false);
	ASSERT_TRUE(First);
	EXPECT_FALSE(First.RetainedHit);
	const auto Warm = CompileMaterialInput(*Input, false);
	ASSERT_TRUE(Warm);
	EXPECT_TRUE(Warm.RetainedHit);
	EXPECT_EQ(Warm.Output.Resource.ShaderMap, First.Output.Resource.ShaderMap);
	EXPECT_EQ(FMaterialShaderMap::FindId(First.Output.GetShaderMap().ShaderMapId), First.Output.Resource.ShaderMap);
	auto LowInput = *Input; LowInput.Environment.Quality = EMaterialQualityLevel::Low;
	const auto Low = CompileMaterialInput(LowInput, false);
	ASSERT_TRUE(Low); EXPECT_TRUE(Low.RetainedHit);
	EXPECT_EQ(Low.Output.Resource.ShaderMap, First.Output.Resource.ShaderMap);
	EXPECT_EQ(Low.Output.Resource.Quality, EMaterialQualityLevel::Low);
	EXPECT_EQ(First.Output.Resource.Quality, Input->Environment.Quality);
	const auto BeforeForce = GetTranslationStats();
	const auto Forced = Build(true);
	ASSERT_TRUE(Forced);
	EXPECT_FALSE(Forced.RetainedHit);
	EXPECT_EQ(GetTranslationStats().Producers, BeforeForce.Producers + 1);
	EXPECT_NE(Forced.Output.Resource.ShaderMap, First.Output.Resource.ShaderMap);
	const auto AfterForce = CompileMaterialInput(*Input, false);
	ASSERT_TRUE(AfterForce);
	EXPECT_TRUE(AfterForce.RetainedHit);
	EXPECT_EQ(AfterForce.Output.Resource.ShaderMap, Forced.Output.Resource.ShaderMap);
}

TEST(FMaterialProgramOwnershipTests, RetainedMapWithMissingCollectionReturnsRequestDiagnostic)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FMaterialShaderMap::ClearCache();
	struct FRestore { ~FRestore() { FMaterialShaderMap::ClearCache(); } } Restore;
	MIR::FCompilerInput Input{.IR = MakeDefaultMaterialCompilerIR()};
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	const auto Original = MaterialBuildPrivate::CompileMaterialInput(Input, true);
	ASSERT_TRUE(Original);
	auto Map = std::make_shared<FMaterialShaderMap>(Original.Output.GetShaderMap());
	const auto Missing = FGuid::NewGuid();
	Map->CollectionLayouts.push_back({.CollectionId = Missing});
	ASSERT_EQ(FMaterialShaderMap::Register(Map, true), Map);
	const auto Rejected = MaterialBuildPrivate::CompileMaterialInput(Input, false);
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.Output.Status, EMaterialProgramStatus::Error);
	EXPECT_FALSE(Rejected.Output.Resource.HasShaderMap());
	ASSERT_FALSE(Rejected.Output.Diagnostics.empty());
	EXPECT_NE(Rejected.Output.Diagnostics.back().Message.find(Missing.ToString()), std::string::npos);
	EXPECT_NE(Rejected.Output.Diagnostics.back().Message.find("missing from compile request"), std::string::npos);
	EXPECT_TRUE(Original.Output.Resource.ActiveCollections.empty());
}

TEST(FMaterialProgramOwnershipTests, SharedProgramSubscriptionsCancelIndependentlyAndAssembleInCanonicalOrder)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	InitializeDObjectSystem();
	auto& Manager = FShaderCompilingManager::Get();
	for (bool Observe : {false, true}) for (int Canceled = -1; Canceled < 3; ++Canceled) {
		SCOPED_TRACE(std::format("observer={}, canceled={}", Observe, Canceled));
		std::array<Tasks::FCancellationToken, 2> Cancellation;
		struct FGate final : DerivedData::ICacheBackend
		{
			mutable FThreadEvent FirstEntered, SecondEntered, FirstRelease, OthersRelease;
			mutable std::atomic_uint32_t Stores = 0;
			auto Get(const DerivedData::FCacheGetBackendRequest&) const -> DerivedData::FCacheGetResult override { return std::optional<DerivedData::FCacheRecord>{}; }
			auto Put(const DerivedData::FCachePutBackendRequest& Request) const -> DerivedData::FCachePutResult override
			{
				if (Stores.fetch_add(1) == 0) { FirstEntered.Trigger(); FirstRelease.WaitFor(15); }
				else { SecondEntered.Trigger(); }
				return DerivedData::GetCacheBackend().Put(Request);
			}
		};
		auto Cache = std::make_shared<FGate>();
		FMaterialShaderMap::ClearCache(); ShaderBuildPrivate::ResetServiceForTests({.Cache = Cache});
		struct FRestore { std::shared_ptr<FGate> Cache; ~FRestore() { Cache->FirstRelease.Trigger(); Cache->OthersRelease.Trigger(); FShaderCompilingManager::Get().FinishAllCompilation(); ShaderBuildPrivate::ResetServiceForTests(); FMaterialShaderMap::ClearCache(); } } Restore{Cache};
		auto Input = std::make_shared<MIR::FCompilerInput>(); Input->IR = MakeDefaultMaterialCompilerIR();
		Input->IR.Nodes = {
			{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.25f}},
			{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.5f}},
			{.Opcode = EMaterialProgramOpcode::Add, .ResultType = EMaterialProgramValueType::Float, .Inputs = {0, 1}},
			{.Opcode = EMaterialProgramOpcode::Splat3, .ResultType = EMaterialProgramValueType::Float3, .Inputs = {2}}};
		Input->IR.GetAttribute(EMaterialProperty::MP_BaseColor).bExpression = true;
		Input->IR.GetAttribute(EMaterialProperty::MP_BaseColor).ExpressionIndex = 3;
		ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input->Environment));
		for (uint32 Node = 0; Node < Input->IR.Nodes.size(); ++Node)
			Input->Sources.push_back({.ExpressionIndex = Node, .NodeId = FGuid::NewGuid(), .FunctionAssetPath = "ProducerAsset"});
		std::ranges::reverse(Input->Sources);
		auto Other = std::make_shared<MIR::FCompilerInput>(*Input);
		for (auto& Source : Other->Sources) { Source.NodeId = FGuid::NewGuid(); Source.FunctionAssetPath = "SubscriberAsset"; }
		Other->IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.739f}});
		ASSERT_EQ(MIR::Normalize(*Input).Identity, MIR::Normalize(*Other).Identity);
		const auto Before = GetMaterialProgramCompileStatsForTests();
		std::array<FMaterialBuildResult, 2> Results;
		std::array<std::atomic_uint32_t, 2> Calls{};
		auto First = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.bForceRecompile = true, .IsCancellationRequested = [&] { return Observe && Cancellation[0].IsCanceled(); }}, [&](auto Result) { Results[0] = std::move(Result); ++Calls[0]; });
		ASSERT_TRUE(First); ASSERT_TRUE(Cache->FirstEntered.WaitFor(10)); ASSERT_TRUE(Cache->SecondEntered.WaitFor(10));
		auto Second = MaterialBuildPrivate::FMaterialCompilation::Compile(Other, {.bForceRecompile = true, .IsCancellationRequested = [&] { return Observe && Cancellation[1].IsCanceled(); }}, [&](auto Result) { Results[1] = std::move(Result); ++Calls[1]; });
		ASSERT_TRUE(Second);
		std::shared_ptr<DerivedData::FRequestOwner> FirstOwner, FirstBuildOwner, SecondOwner;
		{ std::lock_guard Lock((First)->Mutex); FirstOwner = (First)->Owner; FirstBuildOwner = (First)->BuildOwner; }
		{ std::lock_guard Lock((Second)->Mutex); SecondOwner = (Second)->Owner; }
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (GetMaterialProgramCompileStatsForTests().Joins == Before.Joins && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
		ASSERT_EQ(GetMaterialProgramCompileStatsForTests().Joins, Before.Joins + 1);
		if (Observe) {
			if (Canceled == 2) { Cancellation[0].Cancel(); Cancellation[1].Cancel(); }
			else if (Canceled >= 0) Cancellation[Canceled].Cancel();
			Manager.ProcessAsyncTasks({});
		}
		else {
			if (Canceled == 2) { Manager.CancelCompilation(First); Manager.CancelCompilation(Second); }
			else if (Canceled >= 0) Manager.CancelCompilation(Canceled == 0 ? First : Second);
		}
		EXPECT_FALSE(FirstOwner->IsCanceled());
		EXPECT_FALSE(FirstBuildOwner->IsCanceled());
		// Cancellation may finish inline or on the completion worker. A terminal
		// record can release its owner; a live subscription must keep its own.
		const auto CheckOwner = [](const auto& Request, const auto& Expected) {
			std::lock_guard Lock(Request->Mutex);
			EXPECT_TRUE(Request->Owner == Expected || (Request->ResultReady && !Request->Owner));
		};
		CheckOwner(First, FirstOwner); CheckOwner(Second, SecondOwner); EXPECT_NE(FirstOwner, SecondOwner);
		EXPECT_EQ((First)->BuildCancellation.IsCanceled(), Canceled == 2);
		Cache->OthersRelease.Trigger();
		const auto DrainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		if (Canceled != 2) {
			while (Cache->Stores.load() < GetDefaultMaterialShaderEntries().size() && std::chrono::steady_clock::now() < DrainDeadline) std::this_thread::yield();
			EXPECT_EQ(Cache->Stores.load(), GetDefaultMaterialShaderEntries().size());
		}
		Cache->FirstRelease.Trigger(); Manager.FinishCompilation(Second); Manager.FinishCompilation(First);
		for (size_t Index = 0; Index < Results.size(); ++Index) {
			EXPECT_EQ(Calls[Index], 1u);
			if (Canceled == 2 || static_cast<int>(Index) == Canceled) { EXPECT_EQ(Results[Index].Output.Status, EMaterialProgramStatus::Canceled); continue; }
			ASSERT_TRUE(Results[Index]);
			const auto& Consumer = Index == 0 ? *Input : *Other;
			ASSERT_FALSE(Results[Index].Sources.empty());
			for (const auto& Location : Results[Index].Sources) {
				ASSERT_LT(Location.Ordinal, Consumer.Sources.size());
				EXPECT_EQ(Location.Source.NodeId, Consumer.Sources[Location.Ordinal].NodeId);
				EXPECT_EQ(Location.Source.FunctionAssetPath, Consumer.Sources[Location.Ordinal].FunctionAssetPath);
			}
			const auto& Shaders = Results[Index].Output.GetShaderMap().CompiledShaders;
			ASSERT_EQ(Shaders.size(), GetDefaultMaterialShaderEntries().size());
			const std::array<std::string_view, 6> CanonicalNames{"GeometryFragmentMain", "HitProxyFragmentMain", "SplineVertexMain", "VertexMain", "FragmentMain", "ShadowFragmentMain"};
			for (size_t Stage = 0; Stage < Shaders.size(); ++Stage) EXPECT_EQ(Shaders[Stage].SourceEntryPoint, CanonicalNames[Stage]);
		}
		if (Canceled == -1) EXPECT_EQ(Results[0].Output.Resource.ShaderMap, Results[1].Output.Resource.ShaderMap);
		if (Canceled != 2) EXPECT_EQ(GetShaderBuildStats().Compilations, GetDefaultMaterialShaderEntries().size());
		EXPECT_EQ(FShaderCompilingManager::Get().GetPeakJobStats().first, 2u);
		EXPECT_LE(FShaderCompilingManager::Get().GetPeakJobStats().second, 2 * 256ull * 1024 * 1024);
	}
}

TEST(FMaterialProgramOwnershipTests, FailedShaderRequestsDrainWithoutPublicationPump)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	InitializeDObjectSystem();
	auto Input = std::make_shared<MIR::FCompilerInput>(); Input->IR = MakeDefaultMaterialCompilerIR();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input->Environment));
	auto Dependency = std::ranges::find(Input->Environment.Dependencies, "/Engine/MaterialCompilerEnvironment", &FMaterialCompilerDependency::VirtualPath);
	ASSERT_NE(Dependency, Input->Environment.Dependencies.end());
	Dependency->ContentHash = FXxHash128::HashBuffer("MaterialParallelFailure");
	auto& Manager = FShaderCompilingManager::Get();
	std::atomic_uint32_t Calls = 0;
	FMaterialBuildResult Result;
	auto Compile = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.bForceRecompile = true}, [&](auto Value) { Result = std::move(Value); ++Calls; });
	ASSERT_TRUE(Compile); Manager.FinishCompilation(Compile);
	EXPECT_FALSE(Manager.IsCompiling(Compile)); EXPECT_EQ(Calls, 1u);
	ASSERT_FALSE(Result);
	ASSERT_FALSE(Result.Output.Diagnostics.empty());
	EXPECT_TRUE(Result.Output.GetShaderMap().CompiledShaders.empty());
}

TEST(FMaterialProgramOwnershipTests, SuccessfulSubmissionSupportsInlineShaderCallbacks)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	// Deliberately do not bootstrap Core: standalone execution exercises all
	// seven successful Shader callbacks before SubmitJobs returns.
	const bool Inline = !IsTaskSchedulerRunning();
	auto Input = std::make_shared<MIR::FCompilerInput>(); Input->IR = MakeDefaultMaterialCompilerIR();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input->Environment));
	FMaterialShaderMap::ClearCache();
	struct FRestore { ~FRestore() { FShaderCompilingManager::Get().FinishAllCompilation(); FMaterialShaderMap::ClearCache(); } } Restore;
	auto& Manager = FShaderCompilingManager::Get();
	std::atomic_uint32_t Calls = 0;
	FMaterialBuildResult Result;
	auto Compile = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.bForceRecompile = true}, [&](auto Value) { Result = std::move(Value); ++Calls; });
	ASSERT_TRUE(Compile);
	if (Inline) EXPECT_EQ(Calls, 1u);
	Manager.FinishCompilation(Compile); EXPECT_EQ(Calls, 1u);
	ASSERT_TRUE(Result); EXPECT_EQ(Result.Output.GetShaderMap().CompiledShaders.size(), GetDefaultMaterialShaderEntries().size());
	RecordProperty("InlineExecution", Inline ? 1 : 0);
}

TEST(FMaterialProgramOwnershipTests, CacheHandoffAllowsLookupBeforeDeliveryRetires)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	InitializeDObjectSystem(); FMaterialShaderMap::ClearCache();
	FThreadEvent Ready, Release;
	std::atomic_bool Blocked = false;
	struct FRestore { FThreadEvent& Release; ~FRestore() { Release.Trigger(); FShaderCompilingManager::Get().FinishAllCompilation(); SetMaterialBuildCompletionHookForTests({}); FMaterialShaderMap::ClearCache(); } } Restore{Release};
	SetMaterialBuildCompletionHookForTests([&](EMaterialQualityLevel, bool Before) {
		if (Before && !Blocked.exchange(true)) { Ready.Trigger(); EXPECT_TRUE(Release.WaitFor(10)); }
	});
	auto Input = std::make_shared<MIR::FCompilerInput>(); Input->IR = MakeDefaultMaterialCompilerIR();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input->Environment));
	auto& Manager = FShaderCompilingManager::Get();
	FMaterialBuildResult First, Second;
	auto Producer = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.bForceRecompile = true}, [&](auto Result) { First = std::move(Result); });
	ASSERT_TRUE(Producer); ASSERT_TRUE(Ready.WaitFor(10));
	const auto Before = GetShaderBuildStats();
	auto Lookup = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {}, [&](auto Result) { Second = std::move(Result); });
	ASSERT_TRUE(Lookup); Manager.FinishCompilation(Lookup);
	ASSERT_TRUE(Second); EXPECT_TRUE(Second.RetainedHit);
	EXPECT_TRUE(Manager.IsCompiling(Producer));
	EXPECT_EQ(GetShaderBuildStats().Compilations, Before.Compilations);
	EXPECT_EQ(GetShaderBuildStats().MemoryHits, Before.MemoryHits);
	Release.Trigger(); Manager.FinishCompilation(Producer);
	ASSERT_TRUE(First); EXPECT_EQ(First.Output.Resource.ShaderMap, Second.Output.Resource.ShaderMap);
}

TEST(FMaterialProgramOwnershipTests, StableOwnersDoNotJoinOtherSubscriberCallbacks)
{
	using namespace Durin; using namespace MaterialBuildPrivate;
	InitializeDObjectSystem(); FMaterialShaderMap::ClearCache();
	struct FGate final : DerivedData::ICacheBackend
	{
		mutable FThreadEvent Entered, Release;
		mutable std::atomic_uint32_t Stores = 0;
		auto Get(const DerivedData::FCacheGetBackendRequest&) const -> DerivedData::FCacheGetResult override { return std::optional<DerivedData::FCacheRecord>{}; }
		auto Put(const DerivedData::FCachePutBackendRequest& Request) const -> DerivedData::FCachePutResult override
		{ if (Stores.fetch_add(1) == 0) { Entered.Trigger(); Release.WaitFor(10); } return Request.Record; }
	};
	auto Cache = std::make_shared<FGate>(); FThreadEvent PeerEntered, PeerRelease;
	ShaderBuildPrivate::ResetServiceForTests({.Cache = Cache});
	struct FRestore { std::shared_ptr<FGate> Cache; FThreadEvent& Peer; ~FRestore() { Cache->Release.Trigger(); Peer.Trigger(); FShaderCompilingManager::Get().FinishAllCompilation(); ShaderBuildPrivate::ResetServiceForTests(); FMaterialShaderMap::ClearCache(); } } Restore{Cache, PeerRelease};
	auto Input = std::make_shared<MIR::FCompilerInput>(); Input->IR = MakeDefaultMaterialCompilerIR();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input->Environment));
	auto& Manager = FShaderCompilingManager::Get(); const auto Before = GetMaterialProgramCompileStatsForTests();
	auto First = FMaterialCompilation::Compile(Input, {.bForceRecompile = true}, [](auto Result) { EXPECT_TRUE(Result); });
	ASSERT_TRUE(First); ASSERT_TRUE(Cache->Entered.WaitFor(10));
	auto Second = FMaterialCompilation::Compile(Input, {.bForceRecompile = true}, [&](auto Result) {
		EXPECT_TRUE(Result); Manager.FinishCompilation(First);
		PeerEntered.Trigger(); EXPECT_TRUE(PeerRelease.WaitFor(10));
	});
	ASSERT_TRUE(Second);
	auto FirstOwner = (First)->Owner; auto SecondOwner = (Second)->Owner;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (GetMaterialProgramCompileStatsForTests().Joins == Before.Joins && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
	ASSERT_EQ(GetMaterialProgramCompileStatsForTests().Joins, Before.Joins + 1);
	EXPECT_EQ((First)->Owner, FirstOwner); EXPECT_EQ((Second)->Owner, SecondOwner); EXPECT_NE(FirstOwner, SecondOwner);
	Cache->Release.Trigger(); ASSERT_TRUE(PeerEntered.WaitFor(10));
	Manager.FinishCompilation(First); EXPECT_TRUE(FirstOwner->Poll()); EXPECT_TRUE(Manager.IsCompiling(Second));
	PeerRelease.Trigger(); Manager.FinishCompilation(Second);
}

TEST(FMaterialProgramOwnershipTests, CacheRetainsShaderMapWithoutRetainingRequestResult)
{
	using namespace Durin; using namespace MaterialBuildPrivate;
	InitializeDObjectSystem(); FMaterialShaderMap::ClearCache();
	struct FRestore { ~FRestore() { FShaderCompilingManager::Get().FinishAllCompilation(); FMaterialShaderMap::ClearCache(); } } Restore;
	auto Input = std::make_shared<MIR::FCompilerInput>(); Input->IR = MakeDefaultMaterialCompilerIR();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input->Environment));
	auto First = CompileMaterialInput(*Input, true); ASSERT_TRUE(First);
	const auto Identity = First.Output.GetShaderMap().ShaderMapId;
	const auto ShaderMap = First.Output.Resource.ShaderMap;
	First = {};
	EXPECT_EQ(FMaterialShaderMap::FindId(Identity), ShaderMap);
	EXPECT_EQ(FMaterialShaderMap::GetCacheStats().second, ShaderMap->GetRetainedBytes());
	const auto Warm = CompileMaterialInput(*Input, false);
	ASSERT_TRUE(Warm); EXPECT_TRUE(Warm.RetainedHit);
	EXPECT_EQ(Warm.Output.Resource.ShaderMap, ShaderMap);
	EXPECT_EQ(Warm.Output.GetShaderMap().Target, ShaderMap->Target);
	EXPECT_EQ(Warm.Output.GetShaderMap().CompilerIdentity, ShaderMap->CompilerIdentity);
}

TEST(FMaterialProgramOwnershipTests, ShaderMapOwnsCompiledContentAndCachedMapsDoNotConsumeCompileCapacity)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	InitializeDObjectSystem(); FMaterialShaderMap::ClearCache();
	struct FRestore { ~FRestore() { FShaderCompilingManager::Get().FinishAllCompilation(); FMaterialShaderMap::ClearCache(); } } Restore;
	auto Input = std::make_shared<MIR::FCompilerInput>(); Input->IR = MakeDefaultMaterialCompilerIR();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input->Environment));
	auto& Manager = FShaderCompilingManager::Get();
	FMaterialBuildResult First;
	auto Producer = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.bForceRecompile = true}, [&](auto Result) { First = std::move(Result); });
	ASSERT_TRUE(Producer); Manager.FinishCompilation(Producer);
	ASSERT_TRUE(First);
	static_assert(!std::is_same_v<FMaterialProgramCompileHandle, std::shared_ptr<FMaterialShaderMap>>);
	EXPECT_EQ((Producer)->GetShaderMapId(), First.Output.GetShaderMap().ShaderMapId);
	const FMaterialShaderMap Copy(First.Output.GetShaderMap());
	EXPECT_EQ(Copy.CompiledShaders.size(), GetDefaultMaterialShaderEntries().size());
	std::vector<FMaterialProgramCompileHandle> Kept{Producer};
	for (uint32 Index = 0; Index < 40; ++Index) {
		FMaterialBuildResult Next;
		auto Compile = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {}, [&](auto Result) { Next = std::move(Result); });
		ASSERT_TRUE(Compile) << Index;
		Manager.FinishCompilation(Compile);
		ASSERT_TRUE(Next); EXPECT_TRUE(Next.RetainedHit);
		EXPECT_EQ(Next.Output.Resource.ShaderMap, First.Output.Resource.ShaderMap);
		EXPECT_NE(Compile, Producer);
		Kept.push_back(Compile);
	}
	ASSERT_NE(FMaterialShaderMap::FindId(First.Output.GetShaderMap().ShaderMapId), nullptr);
	EXPECT_FALSE(Manager.IsCompiling(Producer));
	Manager.PrioritizeCompilation(Producer);
	Manager.CancelCompilation(Producer);
	Manager.FinishCompilation(Producer);
	EXPECT_EQ(First.Output.GetShaderMap().CompiledShaders.size(), GetDefaultMaterialShaderEntries().size());
}

TEST(FMaterialProgramOwnershipTests, CancellationBeforeSubmissionRemainsLatchedAndRetires)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	auto Input = std::make_shared<const MIR::FCompilerInput>(MakeSyntheticMaterialCompilerInput());
	auto& Manager = FShaderCompilingManager::Get();
	Tasks::FCancellationToken Cancellation; Cancellation.Cancel();
	EXPECT_TRUE(Cancellation.IsCanceled());
	const auto Before = GetTranslationStats();
	std::atomic_uint32_t Calls = 0;
	auto Request = std::make_shared<FMaterialCompilation>();
	auto Compile = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.IsCancellationRequested = [&Cancellation] { return Cancellation.IsCanceled(); }, .Request = Request}, [&](auto Result) {
		EXPECT_EQ(Result.Output.Status, EMaterialProgramStatus::Canceled);
		EXPECT_FALSE(Result.RetainedHit);
		EXPECT_FALSE(Request->Owner->IsCanceled());
		EXPECT_FALSE(Request->BuildOwner->IsCanceled());
		++Calls;
	});
	ASSERT_TRUE(Compile); Manager.FinishCompilation(Compile);
	EXPECT_FALSE(Manager.IsCompiling(Compile));
	EXPECT_TRUE(Cancellation.IsCanceled());
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(GetTranslationStats().Producers, Before.Producers);
	EXPECT_TRUE((Compile)->BuildCancellation.IsCanceled());
}

TEST(FMaterialProgramOwnershipTests, FinishAllCompilationDrainsCallbackSubmissions)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	auto Input = std::make_shared<const MIR::FCompilerInput>(MakeSyntheticMaterialCompilerInput());
	auto& Manager = FShaderCompilingManager::Get();
	Tasks::FCancellationToken Cancellation; Cancellation.Cancel();
	std::atomic_uint32_t Calls = 0;
	std::function<void(FMaterialBuildResult)> Completion;
	Completion = [&](auto Result) {
		EXPECT_EQ(Result.Output.Status, EMaterialProgramStatus::Canceled);
		if (Calls.fetch_add(1) + 1 < 8) {
			auto Followup = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.IsCancellationRequested = [&Cancellation] { return Cancellation.IsCanceled(); }}, Completion);
			EXPECT_TRUE(Followup);
		}
	};
	auto First = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.IsCancellationRequested = [&Cancellation] { return Cancellation.IsCanceled(); }}, Completion);
	ASSERT_TRUE(First);
	Manager.FinishAllCompilation();
	const auto CallsAtReturn = Calls.load();
	// Keep callback captures alive even if a broken finish only drains one generation.
	for (uint32 Index = 0; Index < 8; ++Index) Manager.FinishAllCompilation();
	EXPECT_EQ(CallsAtReturn, 8u);
	EXPECT_FALSE(Manager.IsCompiling(First));
}

TEST(FMaterialProgramOwnershipTests, CanceledSubmissionSupportsInlineAndTaskExecution)
{
	using namespace Durin;
	using namespace MaterialBuildPrivate;
	// This case runs without bootstrap when isolated, and with the suite's scheduler otherwise.
	const bool Inline = !IsTaskSchedulerRunning();
	auto& Manager = FShaderCompilingManager::Get();
	Tasks::FCancellationToken Cancellation; Cancellation.Cancel();
	std::atomic_uint32_t Calls = 0;
	auto Compile = MaterialBuildPrivate::FMaterialCompilation::Compile(std::make_shared<const MIR::FCompilerInput>(), {.IsCancellationRequested = [&Cancellation] { return Cancellation.IsCanceled(); }}, [&](auto Result) {
		EXPECT_EQ(Result.Output.Status, EMaterialProgramStatus::Canceled); ++Calls;
	});
	ASSERT_TRUE(Compile);
	if (Inline) EXPECT_EQ(Calls, 1u);
	Manager.FinishCompilation(Compile);
	EXPECT_EQ(Calls, 1u);
	EXPECT_FALSE(Manager.IsCompiling(Compile));
	RecordProperty("InlineExecution", Inline ? 1 : 0);
}

TEST(FMaterialTranslationTests, FailedNormalizationNeverConstructsShaderWork)
{
	using namespace Durin;
	auto Input = MakeSyntheticMaterialCompilerInput();
	Input.IR.Version = 0;
	FMaterialPreparedProgram Prepared{.Normalized = MIR::Normalize(Input),
		.StaticProperties = Input.StaticProperties, .Environment = Input.Environment};
	ASSERT_FALSE(Prepared.Normalized);
	const auto Translation = TranslatePreparedMaterialProgram(Prepared);
	EXPECT_FALSE(Translation);
	EXPECT_EQ(Translation.Diagnostics, Prepared.Normalized.Diagnostics);
	EXPECT_TRUE(MakeMaterialShaderCompileRequest(Translation, false).Source.empty());
	const auto Result = MaterialBuildPrivate::FinalizeMaterialShaderMap(Translation, {});
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Diagnostics, Prepared.Normalized.Diagnostics);
	EXPECT_TRUE(Result.GetShaderMap().CompiledShaders.empty());
}

TEST(FMaterialTranslationTests, SplitCompilationPreservesArtifactsAndRejectsMismatchedStages)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto Input = MakeSyntheticMaterialCompilerInput();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	FMaterialPreparedProgram Prepared{.Normalized = MIR::Normalize(Input),
		.StaticProperties = Input.StaticProperties, .Environment = Input.Environment};
	ASSERT_TRUE(Prepared.Normalized);
	const auto Translation = TranslatePreparedMaterialProgram(Prepared);
	ASSERT_TRUE(Translation);
	static_assert(!std::same_as<FMaterialTranslatedProgram, FMaterialCompilerResult>);
	const auto Request = MakeMaterialShaderCompileRequest(Translation, false);
	ASSERT_TRUE(Request.ExpectedSourceTree);
	EXPECT_EQ(Request.ExpectedSourceTree->EntryPoints, Request.EntryPoints);
	EXPECT_EQ(Request.ExpectedSourceTree->Frequencies, Request.Frequencies);
	EXPECT_TRUE(Request.ExpectedSourceTree->bCompleteClosure);
	const auto Threshold = std::ranges::find(Request.ExpectedSourceTree->Macros,
		"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", &FShaderMacroDefinition::Name);
	ASSERT_NE(Threshold, Request.ExpectedSourceTree->Macros.end());
	EXPECT_EQ(Threshold->Value, "1056964608");
	FShaderCompilerOutput Output{.Status = EShaderBuildStatus::Ok};
	for (const auto& StageRequest : MakeMaterialShaderStageRequests(Translation, false))
	{
		auto Stage = FShaderCompilingManager::Get().CompileGeneratedSync(StageRequest);
		ASSERT_TRUE(Stage) << FormatShaderCompilerErrors(Stage.Errors);
		for (auto& Code : Stage.CompiledShaders) Output.CompiledShaders.push_back(std::move(Code));
	}
	const auto Split = MaterialBuildPrivate::FinalizeMaterialShaderMap(Translation, Output);
	const auto Synchronous = MIR::Compile(Input);
	ASSERT_TRUE(Split);
	ASSERT_TRUE(Synchronous);
	EXPECT_EQ(Split.GetShaderMap().ShaderMapId, Synchronous.GetShaderMap().ShaderMapId);
	EXPECT_EQ(Split.GetDebugData().IR, Synchronous.GetDebugData().IR);
	EXPECT_EQ(Split.GetShaderMap().Layout, Synchronous.GetShaderMap().Layout);
	EXPECT_EQ(Split.GetShaderMap().ActiveParameters, Synchronous.GetShaderMap().ActiveParameters);
	EXPECT_EQ(Split.GetShaderMap().Requirements, Synchronous.GetShaderMap().Requirements);
	EXPECT_EQ(Split.GetDebugData().GeneratedSource, Synchronous.GetDebugData().GeneratedSource);
	EXPECT_EQ(Split.Diagnostics, Synchronous.Diagnostics);
	ASSERT_EQ(Split.GetShaderMap().CompiledShaders.size(), Synchronous.GetShaderMap().CompiledShaders.size());
	for (size_t Index = 0; Index < Split.GetShaderMap().CompiledShaders.size(); ++Index)
	{
		EXPECT_EQ(Split.GetShaderMap().CompiledShaders[Index].CodeHash, Synchronous.GetShaderMap().CompiledShaders[Index].CodeHash);
		EXPECT_EQ(Split.GetShaderMap().CompiledShaders[Index].SourceEntryPoint,
			Synchronous.GetShaderMap().CompiledShaders[Index].SourceEntryPoint);
	}
	// Warnings belong to this request; retained maps keep only their original compile time.
	auto WarnedOutput = Output;
	WarnedOutput.Errors.emplace_back("material compiler warning");
	auto TimedTranslation = Translation;
	TimedTranslation.CompileTime = .031f;
	const auto Warned = MaterialBuildPrivate::FinalizeMaterialShaderMap(std::move(TimedTranslation), std::move(WarnedOutput));
	ASSERT_TRUE(Warned);
	ASSERT_FALSE(Warned.Diagnostics.empty());
	EXPECT_FLOAT_EQ(Warned.GetShaderMap().CompileTime, .031f);
	FMaterialShaderMap::ClearCache();
	ASSERT_EQ(FMaterialShaderMap::Register(Warned.Resource.ShaderMap), Warned.Resource.ShaderMap);
	const auto Before = GetShaderBuildStats();
	const auto Warm = MaterialBuildPrivate::CompileMaterialInput(Input, false);
	ASSERT_TRUE(Warm);
	EXPECT_TRUE(Warm.RetainedHit);
	EXPECT_EQ(Warm.Output.Resource.ShaderMap, Warned.Resource.ShaderMap);
	EXPECT_TRUE(Warm.Output.Diagnostics.empty());
	EXPECT_FLOAT_EQ(Warm.Output.GetShaderMap().CompileTime, Warned.GetShaderMap().CompileTime);
	EXPECT_EQ(GetShaderBuildStats().Compilations, Before.Compilations);
	const FMaterialShaderMap Copy(*Warned.Resource.ShaderMap);
	EXPECT_FLOAT_EQ(Copy.CompileTime, Warned.GetShaderMap().CompileTime);
	FMaterialShaderMap::ClearCache();
	for (uint32 Variant = 0; Variant < 3; ++Variant)
	{
		SCOPED_TRACE(Variant);
		auto Wrong = Output;
		if (Variant == 0) Wrong.CompiledShaders.pop_back();
		else if (Variant == 1) Wrong.CompiledShaders.front().SourceEntryPoint = "WrongEntry";
		else Wrong.CompiledShaders.front().Target = Request.Target == VulkanShaderTarget
			? MetalShaderTarget : VulkanShaderTarget;
		const auto Rejected = MaterialBuildPrivate::FinalizeMaterialShaderMap(Translation, std::move(Wrong));
		EXPECT_FALSE(Rejected);
		EXPECT_TRUE(Rejected.GetShaderMap().CompiledShaders.empty());
		ASSERT_FALSE(Rejected.Diagnostics.empty());
	}
	auto InvalidBinding = Output;
	const auto Stage = std::ranges::find_if(InvalidBinding.CompiledShaders, [](const FShaderCode& Code) {
		return !Code.Reflection.ResourceBindings.empty();
	});
	ASSERT_NE(Stage, InvalidBinding.CompiledShaders.end());
	Stage->Reflection.ResourceBindings.front().StageFlags = EShaderStageFlags::None;
	const auto Slot = std::distance(InvalidBinding.CompiledShaders.begin(), Stage);
	const auto BindingFailure = MaterialBuildPrivate::FinalizeMaterialShaderMap(Translation, std::move(InvalidBinding));
	ASSERT_FALSE(BindingFailure);
	EXPECT_TRUE(BindingFailure.GetShaderMap().CompiledShaders.empty());
	ASSERT_FALSE(BindingFailure.Diagnostics.empty());
	EXPECT_EQ(BindingFailure.Diagnostics.back().Message, std::format("{}: Material parameter stage is incompatible",
		Translation.Entries[Slot].Shader.ShaderType));
	const auto Failed = MaterialBuildPrivate::FinalizeMaterialShaderMap(Translation,
		{.Status = EShaderBuildStatus::Canceled});
	EXPECT_FALSE(Failed);
	EXPECT_TRUE(Failed.GetShaderMap().CompiledShaders.empty());
	EXPECT_TRUE(Failed.Diagnostics.empty());
	EXPECT_EQ(Failed.Status, EMaterialProgramStatus::Canceled);
}

TEST(FMaterialVertexEvaluationTests, InterpolationRequiresVertexInputsAndPreservesSpatialMeaning)
{
	using namespace Durin;
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	using Stage = EMaterialEvaluationStage;
	const FMaterialValueSemantics VertexNormal{Type::Float3, Stage::Vertex,
		EMaterialSpatialKind::Normal, EMaterialCoordinateSpace::World};
	const auto Interpolated = ResolveMaterialProgramNodeSemantics(Op::VertexInterpolator,
		Type::Float3, std::array{VertexNormal});
	ASSERT_TRUE(Interpolated);
	EXPECT_EQ(Interpolated->Stages, Stage::Pixel);
	EXPECT_EQ(Interpolated->Kind, VertexNormal.Kind);
	EXPECT_EQ(Interpolated->Space, VertexNormal.Space);
	EXPECT_FALSE(ResolveMaterialProgramNodeSemantics(Op::VertexInterpolator,
		Type::Float3, std::array{*Interpolated}));
	EXPECT_TRUE(IsMaterialWorldPositionOffsetSemantics(VertexNormal));
	EXPECT_FALSE(IsMaterialWorldPositionOffsetSemantics(*Interpolated));
	auto Position = VertexNormal;
	Position.Kind = EMaterialSpatialKind::Position;
	EXPECT_FALSE(IsMaterialWorldPositionOffsetSemantics(Position));
	Position.Kind = EMaterialSpatialKind::Direction;
	Position.Space = EMaterialCoordinateSpace::Object;
	EXPECT_FALSE(IsMaterialWorldPositionOffsetSemantics(Position));
}

TEST(FMaterialVertexEvaluationTests, VertexResourcesAndPixelInterpolationCompileAsOneCompleteProgram)
{
	using namespace Durin;
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input;
	Input.IR = MakeDefaultMaterialCompilerIR();

	const auto CompilerEnvironmentResult = BuildDefaultMaterialCompilerEnvironment(Input.Environment);
	ASSERT_TRUE(CompilerEnvironmentResult) << CompilerEnvironmentResult.error();
	const FGuid ScaleId = FGuid::NewGuid();
	Input.Parameters = {{ScaleId, EMaterialParameterType::Scalar}};
	auto Add = [&](MIR::FNode Node) {
		ResolveNodeSemantics(Input.IR, Node);
		Input.IR.Nodes.push_back(std::move(Node));
		return static_cast<uint32>(Input.IR.Nodes.size() - 1);
	};
	const auto Scale = Add({.Opcode = Op::Parameter, .ResultType = Type::Float, .Payload = ScaleId});
	const auto Scale3 = Add({.Opcode = Op::Splat3, .ResultType = Type::Float3, .Inputs = {Scale}});
	const auto Normal = Add({.Opcode = Op::VertexNormal, .ResultType = Type::Float3});
	const auto Offset = Add({.Opcode = Op::Multiply, .ResultType = Type::Float3,
		.ScalarBroadcastMask = 2, .Inputs = {Normal, Scale3}});
	const auto Time = Add({.Opcode = Op::Time, .ResultType = Type::Float});
	const auto Interpolator = Add({.Opcode = Op::VertexInterpolator, .ResultType = Type::Float, .Inputs = {Time}});
	const auto Color = Add({.Opcode = Op::Splat3, .ResultType = Type::Float3, .Inputs = {Interpolator}});
	Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).bExpression = true;
	Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).ExpressionIndex = Offset;
	auto& Emissive = Input.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<size_t>(EMaterialProperty::MP_EmissiveColor)));
	Emissive.bExpression = true; Emissive.ExpressionIndex = Color;
	const auto Compiled = MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic" : Compiled.Diagnostics.front().Message);
	ASSERT_EQ(Compiled.GetShaderMap().CompiledShaders.size(), GetDefaultMaterialShaderEntries().size());
	ASSERT_EQ(Compiled.GetShaderMap().ActiveParameters.size(), 1u);
	EXPECT_EQ(Compiled.GetShaderMap().ActiveParameters.front().Id, ScaleId);
	for (size_t Index = 0; Index < Compiled.GetShaderMap().CompiledShaders.size(); ++Index)
	{
		const auto& Shader = Compiled.GetShaderMap().CompiledShaders[Index];
		EXPECT_EQ(Shader.SourceEntryPoint, Compiled.GetShaderMap().Entries[Index].Shader.EntryPoint);
		EXPECT_EQ(Shader.Frequency, Compiled.GetShaderMap().Entries[Index].Shader.Frequency);
		const auto& Bindings = Shader.Reflection.ResourceBindings;
		EXPECT_EQ(std::ranges::contains(Bindings, "Material", &FShaderResourceBinding::Name), Shader.Frequency == EShaderFrequency::Vertex);
		if (Shader.Frequency == EShaderFrequency::Vertex) EXPECT_TRUE(std::ranges::contains(Bindings, "MaterialView", &FShaderResourceBinding::Name));
	}
	auto InvalidStages = Compiled.GetShaderMap().CompiledShaders;
	InvalidStages.front().Frequency = InvalidStages.front().Frequency == EShaderFrequency::Vertex ? EShaderFrequency::Fragment : EShaderFrequency::Vertex;
	EXPECT_FALSE(ValidateMaterialCompiledStages(InvalidStages, Compiled.GetShaderMap().Layout));
	auto PixelOffset = Input;
	PixelOffset.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).ExpressionIndex = Color;
	EXPECT_FALSE(MIR::Normalize(PixelOffset));
	auto NoOffset = Input;
	NoOffset.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).bExpression = false;
	const auto Normalized = MIR::Normalize(NoOffset);
	ASSERT_TRUE(Normalized);
	EXPECT_TRUE(Normalized.ActiveParameters.empty());
	EXPECT_NE(Normalized.Identity, Compiled.GetShaderMap().ShaderMapId);
}

// Match ShaderBuilder's host target policy; unsupported hosts still test rejection.
#if defined(__APPLE__)
TEST(FMaterialCompilerTests, MetalMaterialProgramCompilesAndCooks)
{
	using namespace Durin;
	auto Input = MakeSyntheticMaterialCompilerInput();
	FMaterialCompilerEnvironment CurrentEnvironment;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(CurrentEnvironment));
	Input.Environment.CompilerIdentity = CurrentEnvironment.CompilerIdentity;
	Input.Environment.Target = "metal-library-1";
	const auto Compiled = MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic"
		: Compiled.Diagnostics.front().Message);
	ASSERT_EQ(Compiled.GetShaderMap().CompiledShaders.size(), GetDefaultMaterialShaderEntries().size());
	for (const auto& Shader : Compiled.GetShaderMap().CompiledShaders)
	{
		EXPECT_EQ(Shader.Target, MetalLibraryShaderTarget);
		EXPECT_EQ(Shader.CodeFormat, EShaderCodeFormat::MetalLibrary);
		EXPECT_NE(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Identity, FXxHash128{});
	}
	FByteBuffer Bytes;
	const std::array Programs{&Compiled.Resource};
	auto Error = EncodeMaterialCookedProgramFamily(Programs,
		Input.StaticProperties, ECookTargetPlatform::MacOS,
		ECookTargetProfile::Game, Bytes);
	ASSERT_TRUE(Error) << Error.error();
	FMaterialStaticProperties Properties;
	std::shared_ptr<const FMaterialProgram> Decoded;
	Error = DecodeMaterialCookedProgramFamily(Bytes,
		ECookTargetPlatform::MacOS, ECookTargetProfile::Game,
		Compiled.Resource.Quality, Compiled.Resource.FeatureLevel, {}, Properties, Decoded);
	ASSERT_TRUE(Error) << Error.error();
	ASSERT_TRUE(Decoded);
	EXPECT_EQ(Decoded->GetShaderMap().ShaderMapId, Compiled.GetShaderMap().ShaderMapId);
}

#else
TEST(FMaterialCompilerTests, MetalMaterialProgramIsRejectedOnUnsupportedHosts)
{
	using namespace Durin;
	auto Input = MakeSyntheticMaterialCompilerInput();
	FMaterialCompilerEnvironment CurrentEnvironment;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(CurrentEnvironment));
	Input.Environment.CompilerIdentity = CurrentEnvironment.CompilerIdentity;
	Input.Environment.Target = "metal-msl-2.0";
	const auto Compiled = MIR::Compile(Input);
	ASSERT_FALSE(Compiled);
	EXPECT_TRUE(Compiled.GetShaderMap().CompiledShaders.empty());
	ASSERT_FALSE(Compiled.Diagnostics.empty());
	for (const auto& Diagnostic : Compiled.Diagnostics) EXPECT_FALSE(Diagnostic.Message.empty());
}
#endif

TEST(FMaterialVertexEvaluationTests, ReachableInterpolatorsHaveABoundedInterface)
{
	using namespace Durin;
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input;
	Input.IR = MakeDefaultMaterialCompilerIR();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	auto Add = [&](MIR::FNode Node) {
		ResolveNodeSemantics(Input.IR, Node);
		Input.IR.Nodes.push_back(std::move(Node));
		return static_cast<uint32>(Input.IR.Nodes.size() - 1);
	};
	uint32 Sum = 0;
	for (uint32 Index = 0; Index <= MaterialProgramMaxVertexInterpolators; ++Index)
	{
		const auto Value = Add({.Opcode = Op::Constant, .ResultType = Type::Float,
			.Payload = FMaterialProgramLiteral{static_cast<float>(Index)}});
		const auto Interpolated = Add({.Opcode = Op::VertexInterpolator, .ResultType = Type::Float, .Inputs = {Value}});
		Sum = Index ? Add({.Opcode = Op::Add, .ResultType = Type::Float, .Inputs = {Sum, Interpolated}}) : Interpolated;
		if (Index + 1 == MaterialProgramMaxVertexInterpolators)
		{
			auto& Root = Input.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<size_t>(EMaterialProperty::MP_Roughness)));
			Root.bExpression = true; Root.ExpressionIndex = Sum;
			const auto Normalized = MIR::Normalize(Input);
			ASSERT_TRUE(Normalized);
			EXPECT_TRUE(GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout));
		}
	}
	Input.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<size_t>(EMaterialProperty::MP_Roughness))).ExpressionIndex = Sum;
	const auto Normalized = MIR::Normalize(Input);
	ASSERT_TRUE(Normalized);
	const auto Rejected = GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout);
	EXPECT_FALSE(Rejected);
	EXPECT_TRUE(Rejected.Source.empty());
}

TEST(FMaterialVertexEvaluationTests, LiteralOffsetsParticipateInCanonicalIdentity)
{
	using namespace Durin;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input;
	Input.IR = MakeDefaultMaterialCompilerIR();
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment));
	const auto Zero = MIR::Normalize(Input);
	ASSERT_TRUE(Zero);
	Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).Literal = {1.f, 2.f, 3.f};
	const auto Offset = MIR::Normalize(Input);
	ASSERT_TRUE(Offset);
	EXPECT_NE(Zero.Identity, Offset.Identity);
	EXPECT_NE(Zero.CanonicalBytes, Offset.CanonicalBytes);
}

TEST(FMaterialDiagnosticTests, SynchronousResultsRequireExplicitCompletedSuccess)
{
	using namespace Durin;
	static_assert(!std::is_convertible_v<MIR::FBuildResult, bool>);
	static_assert(!std::is_convertible_v<FMaterialProgramValidationResult, bool>);
	static_assert(!std::is_convertible_v<MIR::FNormalizationResult, bool>);
	static_assert(!std::is_convertible_v<FMaterialSourceGenerationResult, bool>);
	static_assert(!std::is_convertible_v<FMaterialCompilerResult, bool>);
	EXPECT_FALSE(MIR::FBuildResult{});
	EXPECT_FALSE(FMaterialProgramValidationResult{});
	EXPECT_FALSE(MIR::FNormalizationResult{});
	EXPECT_FALSE(FMaterialSourceGenerationResult{});
	EXPECT_FALSE(FMaterialCompilerResult{});

	// An empty detached graph is valid, unlike a result with no completed stage.
	const auto Built = MIR::BuildGraph({}, {});
	ASSERT_TRUE(Built);
	EXPECT_TRUE(Built.IR.Nodes.empty());
	EXPECT_TRUE(Built.Roots.empty());
	MIR::FModule Invalid;
	Invalid.Version = 0;
	auto Failed = GenerateMaterialProgramSlang(Invalid);
	ASSERT_FALSE(Failed);
	ASSERT_FALSE(Failed.Diagnostics.empty());
	Failed.Diagnostics.clear();
	Failed.Source = "retained diagnostic context";
	EXPECT_FALSE(Failed);
	FMaterialCompilerResult Unusable;
	auto DiagnosticMap = std::make_shared<FMaterialShaderMap>();
	DiagnosticMap->DebugData = std::make_shared<const FMaterialProgramDebugData>(FMaterialProgramDebugData{.GeneratedSource = "retained diagnostic context"});
	Unusable.Resource.ShaderMap = DiagnosticMap;
	EXPECT_FALSE(Unusable);
	EXPECT_FALSE(ValidateMaterialCompilerResult(Unusable));
}

TEST(FMaterialProgramSemanticTests, TransformNormalIsCanonicalAndRejectsInvalidSpacePairs)
{
	using namespace Durin;
	MIR::FCompilerInput Input;
	InitializeSurfaceRootSemantics(Input.IR);
	Input.Environment = MakeSyntheticMaterialCompilerInput().Environment;
	Input.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
		.ResultType = EMaterialProgramValueType::Float3,
		.SpatialKind = EMaterialSpatialKind::Normal,
		.CoordinateSpace = EMaterialCoordinateSpace::World,
		.Payload = FMaterialProgramLiteral{0, 0, 1, 0}});
	Input.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::TransformNormal,
		.ResultType = EMaterialProgramValueType::Float3,
		.LegalStages = EMaterialEvaluationStage::Pixel,
		.SpatialKind = EMaterialSpatialKind::Normal,
		.CoordinateSpace = EMaterialCoordinateSpace::Tangent,
		.Inputs = {0},
		.Payload = FMaterialTransformPayload{EMaterialCoordinateSpace::World,
			EMaterialCoordinateSpace::Tangent}});
	Input.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<size_t>(EMaterialProperty::MP_Normal))).bExpression = true;
	Input.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<size_t>(EMaterialProperty::MP_Normal))).ExpressionIndex = 1;
	const auto Baseline = MIR::Normalize(Input);
	ASSERT_TRUE(Baseline);
	auto Repeated = MIR::Normalize(Input);
	ASSERT_TRUE(Repeated);
	EXPECT_EQ(Repeated.Identity, Baseline.Identity);
	EXPECT_EQ(Repeated.CanonicalBytes, Baseline.CanonicalBytes);

	auto Invalid = Input;
	Invalid.IR.Nodes[1].Payload = FMaterialTransformPayload{
		EMaterialCoordinateSpace::Screen, EMaterialCoordinateSpace::Tangent};
	EXPECT_FALSE(MIR::Normalize(Invalid));
	Invalid = Input;
	Invalid.IR.Nodes[1].Payload = FMaterialTransformPayload{
		EMaterialCoordinateSpace::World, EMaterialCoordinateSpace::World};
	EXPECT_FALSE(MIR::Normalize(Invalid));
}

TEST(FMaterialProgramSemanticTests, SpatialTransformsImplementTheFrozenSpaceMatrix)
{
	using namespace Durin;
	using Op = EMaterialProgramOpcode;
	using Kind = EMaterialSpatialKind;
	using Space = EMaterialCoordinateSpace;
	const std::array Spaces{Space::Object, Space::World, Space::View, Space::Tangent};
	for (const auto Opcode : {Op::TransformPosition, Op::TransformDirection, Op::TransformNormal})
	{
		const auto ExpectedKind = Opcode == Op::TransformPosition ? Kind::Position
			: Opcode == Op::TransformDirection ? Kind::Direction : Kind::Normal;
		for (const auto Source : Spaces)
			for (const auto Destination : Spaces)
			{
				const std::array Inputs{FMaterialValueSemantics{EMaterialProgramValueType::Float3,
					EMaterialEvaluationStage::Both, ExpectedKind, Source}};
				const FMaterialTransformPayload Payload{Source, Destination};
				const auto Resolved = ResolveMaterialProgramNodeSemantics(Opcode,
					EMaterialProgramValueType::Float3, Inputs, &Payload);
				const bool bPositionTangent = Opcode == Op::TransformPosition
					&& (Source == Space::Tangent || Destination == Space::Tangent);
				const bool bExpected = Source != Destination && !bPositionTangent;
				EXPECT_EQ(static_cast<bool>(Resolved), bExpected)
					<< static_cast<uint32>(Opcode) << " " << static_cast<uint32>(Source)
					<< " -> " << static_cast<uint32>(Destination);
				if (Resolved)
				{
					EXPECT_EQ(Resolved->Kind, ExpectedKind);
					EXPECT_EQ(Resolved->Space, Destination);
				}
			}
	}

	const FMaterialTransformPayload WorldToView{Space::World, Space::View};
	const std::array WrongKind{FMaterialValueSemantics{EMaterialProgramValueType::Float3,
		EMaterialEvaluationStage::Pixel, Kind::None, Space::None}};
	EXPECT_FALSE(ResolveMaterialProgramNodeSemantics(Op::TransformDirection,
		EMaterialProgramValueType::Float3, WrongKind, &WorldToView));
}

TEST(FMaterialProgramSemanticTests, GeometricOperationsEnforceWidthKindSpaceAndStage)
{
	using namespace Durin;
	using Type = EMaterialProgramValueType;
	using Stage = EMaterialEvaluationStage;
	using Kind = EMaterialSpatialKind;
	using Space = EMaterialCoordinateSpace;
	const FMaterialValueSemantics WorldDirection{Type::Float3, Stage::Both, Kind::Direction, Space::World};
	const FMaterialValueSemantics WorldNormal{Type::Float3, Stage::Pixel, Kind::Normal, Space::World};
	const FMaterialValueSemantics ViewNormal{Type::Float3, Stage::Pixel, Kind::Normal, Space::View};
	const FMaterialValueSemantics WorldPosition{Type::Float3, Stage::Pixel, Kind::Position, Space::World};
	const FMaterialValueSemantics Generic3{Type::Float3, Stage::Both, Kind::None, Space::None};

	std::array Pair{WorldDirection, WorldNormal};
	auto Result = ResolveMaterialProgramNodeSemantics(EMaterialProgramOpcode::Dot, Type::Float, Pair);
	ASSERT_TRUE(Result); EXPECT_EQ(Result->Type, Type::Float); EXPECT_EQ(Result->Kind, Kind::None);
	EXPECT_EQ(Result->Stages, Stage::Pixel);
	Result = ResolveMaterialProgramNodeSemantics(EMaterialProgramOpcode::Cross, Type::Float3, Pair);
	ASSERT_TRUE(Result); EXPECT_EQ(Result->Kind, Kind::Direction); EXPECT_EQ(Result->Space, Space::World);
	Pair[1] = ViewNormal;
	EXPECT_FALSE(ResolveMaterialProgramNodeSemantics(EMaterialProgramOpcode::Dot, Type::Float, Pair));
	const std::array PositionInput{WorldPosition};
	EXPECT_FALSE(ResolveMaterialProgramNodeSemantics(EMaterialProgramOpcode::Length, Type::Float, PositionInput));
	const std::array Positions{WorldPosition, WorldPosition};
	EXPECT_TRUE(ResolveMaterialProgramNodeSemantics(EMaterialProgramOpcode::Distance, Type::Float, Positions));
	Pair = {WorldDirection, WorldNormal};
	Result = ResolveMaterialProgramNodeSemantics(EMaterialProgramOpcode::Reflect, Type::Float3, Pair);
	ASSERT_TRUE(Result); EXPECT_EQ(Result->Kind, Kind::Direction); EXPECT_EQ(Result->Space, Space::World);
	const std::array GenericPair{Generic3, Generic3};
	EXPECT_TRUE(ResolveMaterialProgramNodeSemantics(EMaterialProgramOpcode::Reflect, Type::Float3, GenericPair));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Reflect, Type::Float));
}

TEST(FMaterialProgramGeneratorTests, PureMathAndGeometryEmitDeterministicIntrinsics)
{
	using namespace Durin;
	using Type = EMaterialProgramValueType;
	MIR::FModule IR = MakeDefaultMaterialCompilerIR();
	auto Add = [&](EMaterialProgramOpcode Opcode, Type ResultType,
		std::vector<uint32> Inputs = {}, FMaterialProgramLiteral Literal = {}) {
		MIR::FNode Node{.Opcode = Opcode, .ResultType = ResultType, .Inputs = std::move(Inputs)};
		if (Opcode == EMaterialProgramOpcode::Constant) Node.Payload = Literal;
		ResolveNodeSemantics(IR, Node);
		const uint32 Index = static_cast<uint32>(IR.Nodes.size());
		IR.Nodes.push_back(std::move(Node));
		return Index;
	};
	const auto A = Add(EMaterialProgramOpcode::Constant, Type::Float3, {}, {1, 2, 3});
	const auto B = Add(EMaterialProgramOpcode::Constant, Type::Float3, {}, {4, 5, 6});
	const auto Zero = Add(EMaterialProgramOpcode::Constant, Type::Float3, {}, {0, 0, 0});
	std::vector<uint32> Terms;
	const auto Splat = [&](uint32 Scalar) {
		return Add(EMaterialProgramOpcode::Splat3, Type::Float3, {Scalar});
	};
	Terms.push_back(Splat(Add(EMaterialProgramOpcode::Dot, Type::Float, {A, B})));
	Terms.push_back(Add(EMaterialProgramOpcode::Cross, Type::Float3, {A, B}));
	Terms.push_back(Splat(Add(EMaterialProgramOpcode::Length, Type::Float, {A})));
	Terms.push_back(Splat(Add(EMaterialProgramOpcode::Distance, Type::Float, {A, B})));
	Terms.push_back(Add(EMaterialProgramOpcode::Pow, Type::Float3, {A, B}));
	for (const auto Opcode : {EMaterialProgramOpcode::Sqrt, EMaterialProgramOpcode::Exp,
		EMaterialProgramOpcode::Log, EMaterialProgramOpcode::Floor,
		EMaterialProgramOpcode::Ceil, EMaterialProgramOpcode::Round,
		EMaterialProgramOpcode::Frac, EMaterialProgramOpcode::Sign})
		Terms.push_back(Add(Opcode, Type::Float3, {A}));
	Terms.push_back(Add(EMaterialProgramOpcode::Fmod, Type::Float3, {A, B}));
	Terms.push_back(Add(EMaterialProgramOpcode::Step, Type::Float3, {A, B}));
	Terms.push_back(Add(EMaterialProgramOpcode::SmoothStep, Type::Float3, {Zero, B, A}));
	Terms.push_back(Add(EMaterialProgramOpcode::Reflect, Type::Float3, {A, B}));
	uint32 Aggregate = Terms.front();
	for (size_t Index = 1; Index < Terms.size(); ++Index)
		Aggregate = Add(EMaterialProgramOpcode::Add, Type::Float3, {Aggregate, Terms[Index]});
	auto& BaseColor = IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<size_t>(EMaterialProperty::MP_BaseColor)));
	BaseColor.bExpression = true; BaseColor.ExpressionIndex = Aggregate;
	const auto Generated = GenerateMaterialProgramSlang(IR);
	ASSERT_TRUE(Generated) << (Generated.Diagnostics.empty() ? "missing diagnostic"
		: Generated.Diagnostics.front().Message);
	for (const std::string_view Intrinsic : {"dot(", "cross(", "length(", "distance(",
		"pow(", "sqrt(", "exp(", "log(", "floor(", "ceil(", "round(",
		"frac(", "fmod(", "step(", "smoothstep(", "sign(", "reflect("})
		EXPECT_NE(Generated.Source.find(Intrinsic), std::string::npos) << Intrinsic;
	const auto Repeated = GenerateMaterialProgramSlang(IR);
	ASSERT_TRUE(Repeated); EXPECT_EQ(Repeated.Source, Generated.Source);
	MIR::FCompilerInput Input;
	Input.IR = IR;
	Input.Environment = MakeSyntheticMaterialCompilerInput().Environment;
	const auto Compiled = MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic"
		: Compiled.Diagnostics.front().Message);
	EXPECT_TRUE(ValidateMaterialCompilerResult(Compiled));
}

TEST(FMaterialProgramGeneratorTests, ContextAndSpatialTransformsCompileWithExactReachableBindings)
{
	using namespace Durin;
	using Type = EMaterialProgramValueType;
	using Op = EMaterialProgramOpcode;
	MIR::FModule IR = MakeDefaultMaterialCompilerIR();
	auto Add = [&](MIR::FNode Node) {
		ResolveNodeSemantics(IR, Node);
		const uint32 Index = static_cast<uint32>(IR.Nodes.size());
		IR.Nodes.push_back(std::move(Node));
		return Index;
	};
	const auto WorldPosition = Add({.Opcode = Op::WorldPosition, .ResultType = Type::Float3});
	const auto CameraPosition = Add({.Opcode = Op::CameraPosition, .ResultType = Type::Float3});
	const auto CameraVector = Add({.Opcode = Op::CameraVector, .ResultType = Type::Float3});
	const auto ObjectPosition = Add({.Opcode = Op::ObjectPosition, .ResultType = Type::Float3});
	const auto ScreenPosition = Add({.Opcode = Op::ScreenPosition, .ResultType = Type::Float2});
	const auto ViewSize = Add({.Opcode = Op::ViewSize, .ResultType = Type::Float2});
	const auto ViewPosition = Add({.Opcode = Op::TransformPosition, .ResultType = Type::Float3,
		.Inputs = {WorldPosition}, .Payload = FMaterialTransformPayload{
			EMaterialCoordinateSpace::World, EMaterialCoordinateSpace::View}});
	const auto ObjectLocalPosition = Add({.Opcode = Op::TransformPosition, .ResultType = Type::Float3,
		.Inputs = {WorldPosition}, .Payload = FMaterialTransformPayload{
			EMaterialCoordinateSpace::World, EMaterialCoordinateSpace::Object}});
	const auto TangentDirection = Add({.Opcode = Op::TransformDirection, .ResultType = Type::Float3,
		.Inputs = {CameraVector}, .Payload = FMaterialTransformPayload{
			EMaterialCoordinateSpace::World, EMaterialCoordinateSpace::Tangent}});
	const auto WorldNormal = Add({.Opcode = Op::Constant, .ResultType = Type::Float3,
		.LegalStages = EMaterialEvaluationStage::Pixel,
		.SpatialKind = EMaterialSpatialKind::Normal,
		.CoordinateSpace = EMaterialCoordinateSpace::World,
		.Payload = FMaterialProgramLiteral{0, 0, 1, 0}});
	const auto TangentNormal = Add({.Opcode = Op::TransformNormal, .ResultType = Type::Float3,
		.Inputs = {WorldNormal}, .Payload = FMaterialTransformPayload{
			EMaterialCoordinateSpace::World, EMaterialCoordinateSpace::Tangent}});
	auto Clear3 = [&](uint32 Input) {
		return Add({.Opcode = Op::Swizzle, .ResultType = Type::Float3, .Inputs = {Input},
			.Payload = MIR::FSwizzle{3, {0, 1, 0, 0}}});
	};
	auto Expand2 = [&](uint32 Input) {
		const auto X = Add({.Opcode = Op::Swizzle, .ResultType = Type::Float, .Inputs = {Input},
			.Payload = MIR::FSwizzle{1, {0, 0, 0, 0}}});
		const auto Y = Add({.Opcode = Op::Swizzle, .ResultType = Type::Float, .Inputs = {Input},
			.Payload = MIR::FSwizzle{1, {1, 0, 0, 0}}});
		return Add({.Opcode = Op::MakeFloat3, .ResultType = Type::Float3,
			.Inputs = {X, Y, X}});
	};
	std::vector<uint32> Terms{Clear3(CameraPosition), Clear3(ObjectPosition),
		Clear3(ViewPosition), Clear3(ObjectLocalPosition), Clear3(TangentDirection), Clear3(TangentNormal),
		Expand2(ScreenPosition), Expand2(ViewSize)};
	uint32 Aggregate = Terms.front();
	for (size_t Index = 1; Index < Terms.size(); ++Index)
		Aggregate = Add({.Opcode = Op::Add, .ResultType = Type::Float3,
			.Inputs = {Aggregate, Terms[Index]}});
	auto& BaseColor = IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<size_t>(EMaterialProperty::MP_BaseColor)));
	BaseColor.bExpression = true;
	BaseColor.ExpressionIndex = Aggregate;

	MIR::FCompilerInput Input;
	Input.IR = IR;
	Input.Environment = MakeSyntheticMaterialCompilerInput().Environment;
	const auto Compiled = MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic"
		: Compiled.Diagnostics.front().Message);
	EXPECT_TRUE(ValidateMaterialCompilerResult(Compiled));
	for (const std::string_view Text : {"MaterialView.CameraWorldPosition", "MaterialPrimitive.BoundsCenter",
		"MaterialView.Viewport", "MaterialPrimitive.WorldToLocal", "MaterialTangentToWorld(input)"})
		EXPECT_NE(Compiled.GetDebugData().GeneratedSource.find(Text), std::string::npos) << Text;
	EXPECT_TRUE(Compiled.GetShaderMap().Requirements.bMaterialView);
	EXPECT_TRUE(Compiled.GetShaderMap().Requirements.bMaterialPrimitive);
	EXPECT_TRUE(Compiled.GetShaderMap().Requirements.bCameraPosition);
	EXPECT_TRUE(Compiled.GetShaderMap().Requirements.bViewport);
	EXPECT_TRUE(Compiled.GetShaderMap().Requirements.bViewTransforms);
	EXPECT_TRUE(Compiled.GetShaderMap().Requirements.bObjectTransforms);
	EXPECT_TRUE(Compiled.GetShaderMap().Requirements.bBoundsCenter);
	EXPECT_TRUE(Compiled.GetShaderMap().Requirements.bTangentFrame);
	for (const auto& Stage : Compiled.GetShaderMap().CompiledShaders | std::views::filter([](const auto& S) { return S.Frequency == EShaderFrequency::Vertex; }))
	{
		EXPECT_NE(std::ranges::find(Stage.Reflection.ResourceBindings, "MaterialView",
			&FShaderResourceBinding::Name), Stage.Reflection.ResourceBindings.end());
		EXPECT_EQ(std::ranges::find(Stage.Reflection.ResourceBindings, "MaterialPrimitive",
			&FShaderResourceBinding::Name), Stage.Reflection.ResourceBindings.end());
		EXPECT_NE(std::ranges::find(Stage.Reflection.ResourceBindings, "GPUSceneInstanceSceneData",
			&FShaderResourceBinding::Name), Stage.Reflection.ResourceBindings.end());
	}

	auto VertexOnly = MakeDefaultMaterialCompilerIR();
	VertexOnly.Nodes.push_back({.Opcode = Op::VertexNormal, .ResultType = Type::Float3,
		.LegalStages = EMaterialEvaluationStage::Vertex,
		.SpatialKind = EMaterialSpatialKind::Normal,
		.CoordinateSpace = EMaterialCoordinateSpace::World});
	auto& Normal = VertexOnly.GetAttribute(static_cast<EMaterialProperty>(static_cast<size_t>(EMaterialProperty::MP_Normal)));
	Normal.bExpression = true;
	Normal.ExpressionIndex = 0;
	MIR::FCompilerInput Rejected;
	Rejected.IR = std::move(VertexOnly);
	Rejected.Environment = Input.Environment;
	EXPECT_FALSE(MIR::Normalize(Rejected));
}

TEST(FMaterialProgramSemanticTests, StaticSelectorsPruneBeforeNormalizationAndResourceDiscovery)
{
	using namespace Durin;
	using Type = EMaterialProgramValueType;
	Testing::FTestMaterialExpressionGraph Graph;
	FMaterialParameterDefinition SelectedParameter;
	SelectedParameter.Id = FGuid::NewGuid();
	SelectedParameter.Name = "SelectedVector";
	SelectedParameter.Type = EMaterialParameterType::Scalar;
	SelectedParameter.Value = FMaterialParameterValue::MakeScalar(0.8f);
	const std::array Definitions{SelectedParameter};
	auto& SelectorNode = Graph.Add(EMaterialProgramOpcode::StaticBool, Type::StaticBool,
		{}, {}, {}, Definitions);
	auto* Selector = Cast<DMaterialExpressionStaticBool>(&SelectorNode);
	ASSERT_NE(Selector, nullptr);
	Selector->Name = "UseParameter";
	Selector->DefaultValue = false;
	auto& Fallback = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
		{}, {}, {0.1f}, Definitions);
	auto& Parameter = Graph.Add(EMaterialProgramOpcode::Parameter, Type::Float,
		{}, SelectedParameter.Id, {}, Definitions);
	auto& SwitchNode = Graph.Add(EMaterialProgramOpcode::StaticSwitch, Type::Float,
		{},
		{}, {}, Definitions);
	auto* Switch = Cast<DMaterialExpressionStaticSwitch>(&SwitchNode);
	ASSERT_NE(Switch, nullptr);
	Switch->Condition = Testing::MakeLink(SelectorNode);
	Switch->FalseValue.Connection = Testing::MakeLink(Fallback);
	Switch->TrueValue.Connection = Testing::MakeLink(Parameter);
	Graph.Outputs.Get(EMaterialProperty::MP_Roughness).Connection = Testing::MakeLink(SwitchNode);

	auto NormalizeSelection = [&](bool bUseParameter) {
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Graph.Expressions) Expressions.push_back(Expression.Get());
		MIR::FBuildEnvironment BuildEnvironment;
		BuildEnvironment.StaticBools.push_back({Selector->DeclarationId, bUseParameter});
		MIR::FGraphBuilder Builder(Expressions, BuildEnvironment);
		auto Built = Builder.FinishSurface(Graph.Outputs);
		EXPECT_TRUE(Built) << (Built.Diagnostics.empty() ? "missing diagnostic"
			: Built.Diagnostics.front().Message);
		MIR::FCompilerInput Input{.IR = std::move(Built.IR),
			.Parameters = std::move(Built.Parameters)};
		Input.Environment = MakeSyntheticMaterialCompilerInput().Environment;
		Input.Environment.StaticBools = BuildEnvironment.StaticBools;
		return MIR::Normalize(Input);
	};
	const auto Disabled = NormalizeSelection(false);
	ASSERT_TRUE(Disabled);
	EXPECT_TRUE(Disabled.ActiveParameters.empty());
	const auto Enabled = NormalizeSelection(true);
	ASSERT_TRUE(Enabled);
	ASSERT_EQ(Enabled.ActiveParameters.size(), 1u);
	EXPECT_EQ(Enabled.ActiveParameters.front().Id, SelectedParameter.Id);
	EXPECT_NE(Enabled.Identity, Disabled.Identity);
	for (const auto& Node : Enabled.IR.Nodes)
	{
		EXPECT_NE(Node.Opcode, EMaterialProgramOpcode::StaticBool);
		EXPECT_NE(Node.Opcode, EMaterialProgramOpcode::StaticSwitch);
	}
}

TEST(FMaterialProgramSemanticTests, QualityAndFeatureSelectorsUseExactBranchOrDefault)
{
	using namespace Durin;
	using Type = EMaterialProgramValueType;
	Testing::FTestMaterialExpressionGraph Graph;
	auto& QualityDefault = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
		{}, {}, {.X = .25f});
	auto& QualityLow = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
		{}, {}, {.X = .1f});
	auto& QualityNode = Graph.Add(EMaterialProgramOpcode::QualitySwitch, Type::Float,
		{} , {}, {});
	auto* Quality = Cast<DMaterialExpressionQualitySwitch>(&QualityNode);
	ASSERT_NE(Quality, nullptr);
	Quality->DefaultValue.Connection = Testing::MakeLink(QualityDefault);
	Quality->Low = Testing::MakeLink(QualityLow);
	auto& FeatureDefault = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
		{}, {}, {.X = .4f});
	auto& FeatureSM5 = Graph.Add(EMaterialProgramOpcode::Constant, Type::Float,
		{}, {}, {.X = .5f});
	auto& FeatureNode = Graph.Add(EMaterialProgramOpcode::FeatureLevelSwitch, Type::Float,
		{}, {}, {});
	auto* Feature = Cast<DMaterialExpressionFeatureLevelSwitch>(&FeatureNode);
	ASSERT_NE(Feature, nullptr);
	Feature->DefaultValue.Connection = Testing::MakeLink(FeatureDefault);
	Feature->SM5 = Testing::MakeLink(FeatureSM5);
	Graph.Outputs.Get(EMaterialProperty::MP_Roughness).Connection = Testing::MakeLink(QualityNode);
	Graph.Outputs.Get(EMaterialProperty::MP_Metallic).Connection = Testing::MakeLink(FeatureNode);
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Expression : Graph.Expressions) Expressions.push_back(Expression.Get());
	ASSERT_TRUE(MIR::FGraphBuilder::ValidateSurface(Expressions, Graph.Outputs));
	auto Build = [&](EMaterialQualityLevel InQuality, ERHIFeatureLevel InFeature) {
		MIR::FGraphBuilder Builder(Expressions,
			{.Quality = InQuality, .FeatureLevel = InFeature});
		return Builder.FinishSurface(Graph.Outputs);
	};
	const auto LowSM5 = Build(EMaterialQualityLevel::Low, ERHIFeatureLevel::SM5);
	ASSERT_TRUE(LowSM5);
	EXPECT_FLOAT_EQ(LowSM5.IR.Nodes[
		LowSM5.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<uint32>(EMaterialProperty::MP_Roughness))).ExpressionIndex]
		.GetLiteral().X, .1f);
	EXPECT_FLOAT_EQ(LowSM5.IR.Nodes[
		LowSM5.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<uint32>(EMaterialProperty::MP_Metallic))).ExpressionIndex]
		.GetLiteral().X, .5f);
	const auto HighSM6 = Build(EMaterialQualityLevel::High, ERHIFeatureLevel::SM6);
	ASSERT_TRUE(HighSM6);
	EXPECT_FLOAT_EQ(HighSM6.IR.Nodes[
		HighSM6.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<uint32>(EMaterialProperty::MP_Roughness))).ExpressionIndex]
		.GetLiteral().X, .25f);
	EXPECT_FLOAT_EQ(HighSM6.IR.Nodes[
		HighSM6.IR.GetAttribute(static_cast<EMaterialProperty>(static_cast<uint32>(EMaterialProperty::MP_Metallic))).ExpressionIndex]
		.GetLiteral().X, .4f);
}

TEST(FMaterialProgramSemanticTests, StaticSelectorDisconnectedBranchesUseRetainedValues)
{
	using namespace Durin;
	using Type = EMaterialProgramValueType;
	Testing::FTestMaterialExpressionGraph Graph;
	auto& SelectorNode = Graph.Add(EMaterialProgramOpcode::StaticBool,
		Type::StaticBool, {}, {}, {});
	auto* Selector = Cast<DMaterialExpressionStaticBool>(&SelectorNode);
	ASSERT_NE(Selector, nullptr);
	auto& SwitchNode = Graph.Add(EMaterialProgramOpcode::StaticSwitch,
		Type::Float, {}, {}, {});
	auto* Switch = Cast<DMaterialExpressionStaticSwitch>(&SwitchNode);
	ASSERT_NE(Switch, nullptr);
	Switch->Condition = Testing::MakeLink(SelectorNode);
	Switch->FalseValue.SetConstant({.2f});
	Switch->TrueValue.SetConstant({.8f});
	Graph.Outputs.Get(EMaterialProperty::MP_Roughness).Connection = Testing::MakeLink(SwitchNode);
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Expression : Graph.Expressions)
		Expressions.push_back(Expression.Get());
	ASSERT_TRUE(MIR::FGraphBuilder::ValidateSurface(Expressions, Graph.Outputs));
	auto Build = [&](bool bSelected) {
		MIR::FBuildEnvironment Environment;
		Environment.StaticBools.push_back({Selector->DeclarationId, bSelected});
		MIR::FGraphBuilder Builder(Expressions, std::move(Environment));
		return Builder.FinishSurface(Graph.Outputs);
	};
	const auto FalseBranch = Build(false);
	ASSERT_TRUE(FalseBranch);
	EXPECT_FLOAT_EQ(FalseBranch.IR.Nodes[FalseBranch.IR.GetAttribute(static_cast<EMaterialProperty>(
		static_cast<uint32>(EMaterialProperty::MP_Roughness))).ExpressionIndex]
		.GetLiteral().X, .2f);
	const auto TrueBranch = Build(true);
	ASSERT_TRUE(TrueBranch);
	EXPECT_FLOAT_EQ(TrueBranch.IR.Nodes[TrueBranch.IR.GetAttribute(static_cast<EMaterialProperty>(
		static_cast<uint32>(EMaterialProperty::MP_Roughness))).ExpressionIndex]
		.GetLiteral().X, .8f);
}

TEST(FMaterialDiagnosticTests, FailedCodecsDoNotPublishPartialProducts)
{
	using namespace Durin;
	MIR::FModule Invalid;
	Invalid.Version = 0;
	FByteBuffer Bytes{std::byte{1}, std::byte{2}};
	const auto Encoded = MIR::EncodeCanonical(Invalid, Bytes);
	ASSERT_FALSE(Encoded);
	EXPECT_FALSE(Encoded.error().empty());
	EXPECT_TRUE(Bytes.empty());
	const auto Generated = GenerateMaterialProgramSlang(Invalid);
	EXPECT_FALSE(Generated);
	EXPECT_TRUE(Generated.Source.empty());
	ASSERT_FALSE(Generated.Diagnostics.empty());
	EXPECT_EQ(Generated.Diagnostics.front().Message, Encoded.error());

	// Reject after writing the canonical header without exposing partial bytes.
	Invalid.Version = MIR::CurrentVersion;
	Invalid.bAggregateAttributes = true;
	Invalid.AggregateAttributesIndex = 0;
	Bytes = {std::byte{3}};
	const auto InvalidRoot = MIR::EncodeCanonical(Invalid, Bytes);
	ASSERT_FALSE(InvalidRoot);
	EXPECT_FALSE(InvalidRoot.error().empty());
	EXPECT_TRUE(Bytes.empty());

	FMaterialStaticProperties Properties;
	Properties.OpacityMaskThreshold = 0.25f;
	const auto Original = Properties;
	auto Previous = std::make_shared<const FMaterialProgram>();
	auto Program = Previous;
	const auto Decoded = DecodeMaterialCookedProgramFamily({},
		ECookTargetPlatform::Win64, ECookTargetProfile::Game,
		EMaterialQualityLevel::High, ERHIFeatureLevel::SM5, {}, Properties, Program);
	ASSERT_FALSE(Decoded);
	EXPECT_EQ(Decoded.error(), "Material cooked program byte extent is invalid.");
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
	EXPECT_EQ(CaptureMaterialOutputBindings(First.Outputs), CaptureMaterialOutputBindings(Second.Outputs));
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
		EXPECT_TRUE(HaveSameMaterialExpressionFields(*Node, *Second.Expressions[Index]));
		Expressions.push_back(Node.Get());
	}
	EXPECT_LE(LinkCount, MaterialProgramMaxLinkCount);
	const auto Validation = MIR::FGraphBuilder::ValidateSurface(Expressions, First.Outputs);
	EXPECT_TRUE(Validation) << (Validation.Diagnostics.empty() ? std::string{}
		: std::format("{} at {}", Validation.Diagnostics.front().Message,
			Validation.Diagnostics.front().NodeId.ToString()));
	EXPECT_TRUE(Validation.Diagnostics.empty());
	EXPECT_NE(FMaterialExpressionCollection::StaticStruct()->FindPropertyByName("Expressions"), nullptr);
	EXPECT_NE(DMaterial::StaticClass()->FindPropertyByName("BaseColor"), nullptr);
	EXPECT_EQ(DMaterial::StaticClass()->FindPropertyByName("PropertyInputs_DEPRECATED"), nullptr);
	EXPECT_EQ(DMaterialExpressionMakeMaterialAttributes::StaticClass()->FindPropertyByName("Values_DEPRECATED"), nullptr);
	EXPECT_EQ(FMaterialExpressionAttributeBinding::StaticStruct()->FindPropertyByName("Source_DEPRECATED"), nullptr);
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
	const auto ExpectFailure = [&](const Testing::FTestMaterialExpressionGraph& Graph, std::string_view Message) {
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Graph.Expressions) Expressions.push_back(Expression.Get());
		auto Validation = MIR::FGraphBuilder::ValidateSurface(Expressions, Graph.Outputs);
		EXPECT_FALSE(Validation);
		EXPECT_FALSE(Validation.Diagnostics.empty());
		if (!Validation.Diagnostics.empty()) EXPECT_EQ(Validation.Diagnostics.front().Message, Message)
			<< Validation.Diagnostics.front().Message;
		return Validation;
	};
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Expressions[1]->Id = Graph.Expressions[0]->Id;
		ExpectFailure(Graph, "Expression collection contains a null owner, invalid GUID, or duplicate GUID.");
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Expressions.emplace_back();
		ExpectFailure(Graph, "Expression collection contains a null owner, invalid GUID, or duplicate GUID.");
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Outputs.Get(EMaterialProperty::MP_Metallic).Connection.Expression = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
		ExpectFailure(Graph, "Expression input is disconnected or refers to a missing expression.");
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		auto* Vector = NewObject<DMaterialExpressionVector2Constant>(nullptr, NAME_None);
		Vector->Id = FGuid::NewGuid();
		Vector->Value = {1, 2};
		Graph.Expressions.emplace_back(Vector);
		Graph.Outputs.Get(EMaterialProperty::MP_BaseColor).Connection = {Vector};
		const auto Validation = ExpectFailure(Graph, std::format(
			"Material output source has an incompatible type. [expected type {}] [actual type {}]",
			static_cast<uint8>(EMaterialProgramValueType::Float3), static_cast<uint8>(EMaterialProgramValueType::Float2)));
		ASSERT_FALSE(Validation.Diagnostics.empty());
		EXPECT_EQ(Validation.Diagnostics.front().LocationKind, EMaterialProgramDiagnosticLocationKind::SurfaceOutput);
		EXPECT_EQ(Validation.Diagnostics.front().LocationIndex, static_cast<uint32>(EMaterialProperty::MP_BaseColor));
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		TStrongObjectPtr<DMaterialExpressionScalarConstant> Constant(NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None));
		Constant->Id = FGuid::NewGuid(); Constant->Value = std::numeric_limits<float>::infinity();
		Graph.Expressions.emplace_back(Constant.Get());
		ExpectFailure(Graph, "Expression constant must be finite.");
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		TStrongObjectPtr<DMaterialExpressionAdd> Invalid(NewObject<DMaterialExpressionAdd>(nullptr, NAME_None));
		Invalid->Id = FGuid::NewGuid(); Invalid->ResultType = static_cast<EMaterialProgramValueType>(255);
		Graph.Expressions.emplace_back(Invalid.Get());
		ExpectFailure(Graph, "Numeric expression signature does not match its inputs.");
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		TStrongObjectPtr<DMaterialExpressionNegate> A(NewObject<DMaterialExpressionNegate>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionNegate> B(NewObject<DMaterialExpressionNegate>(nullptr, NAME_None));
		A->Id = FGuid::NewGuid(); B->Id = FGuid::NewGuid(); A->Input = {B.Get()}; B->Input = {A.Get()};
		Graph.Expressions.emplace_back(A.Get()); Graph.Expressions.emplace_back(B.Get());
		ExpectFailure(Graph, "Expression inputs contain a cycle or exceed the traversal depth bound.");
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		DMaterialExpression* Previous = nullptr;
		for (uint32 Index = 0; Index <= MaterialProgramMaxDepth; ++Index)
		{
			TStrongObjectPtr<DMaterialExpressionNegate> Node(NewObject<DMaterialExpressionNegate>(nullptr, NAME_None));
			Node->Id = {0xde770001, 0, 0, Index + 1}; Node->Input = {Previous};
			Node->Input.SetConstant({0}); Previous = Node.Get();
			Graph.Expressions.emplace_back(Node.Get());
		}
		ExpectFailure(Graph, "Expression Build exceeds the IR depth bound.");
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		while (Graph.Expressions.size() <= MaterialProgramMaxNodeCount)
		{
			TStrongObjectPtr<DMaterialExpressionScalarConstant> Node(NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None));
			Node->Id = FGuid::NewGuid(); Graph.Expressions.emplace_back(Node.Get());
		}
		ExpectFailure(Graph, "Expression collection exceeds the authored node bound.");
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		const auto Found = std::ranges::find_if(Graph.Expressions, [](const auto& Node) { return Cast<DMaterialExpressionParameter>(Node.Get()) != nullptr; });
		ASSERT_NE(Found, Graph.Expressions.end());
		auto* Parameter = Cast<DMaterialExpressionParameter>(Found->Get());
		Parameter->Metadata.Id = {};
		ExpectFailure(Graph, "Parameter expressions require valid parameter GUIDs and a material owner.");
		Parameter->Metadata.Id = FGuid::NewGuid();
		Parameter->Metadata.DisplayName.assign(MaterialProgramMaxDisplayNameBytes + 1, 'x');
		const auto Forward = ExpectFailure(Graph, "Parameter expression metadata or default is invalid.");
		std::ranges::reverse(Graph.Expressions);
		const auto Reversed = ExpectFailure(Graph, "Parameter expression metadata or default is invalid.");
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
	ASSERT_EQ(Owned.size(), Graph.Expressions.size());
	for (size_t Index = 0; Index < Graph.Expressions.size(); ++Index)
	{
		EXPECT_EQ(Owned[Index]->Id, Graph.Expressions[Index]->Id);
		EXPECT_EQ(Owned[Index]->GetOuter(), Base.Get());
		EXPECT_NE(Owned[Index].Get(), Graph.Expressions[Index].Get());
	}
	EXPECT_EQ(Instance->GetParent(), Base.Get());
	EXPECT_TRUE(GDObjectArray.GetObjectsWithOuter(Instance.Get(), EObjectQueryScope::LiveOnly).empty());
	const auto BeforeChildren = Owned;
	const auto BeforeOutputs = Base->CaptureRootInputs();
	Graph.Outputs.Get(EMaterialProperty::MP_BaseColor).Constant[0] = std::numeric_limits<float>::quiet_NaN();
	EXPECT_FALSE(Graph.Apply(*Base));
	EXPECT_EQ(Base->GetExpressionCollection().Expressions, BeforeChildren);
	EXPECT_EQ(Base->CaptureRootInputs(), BeforeOutputs);
}

TEST(FMaterialProgramNormalizationTests,
	SnapshotIsDetachedAndExcludesDynamicParameterValues)
{
	InitializeDObjectSystem();
	auto* Material = MakeExpandedMaterial("CompilerSnapshotMaterial");
	Durin::FMaterialCompilerEnvironment Environment =
		MakeSyntheticMaterialCompilerInput().Environment;
	auto BeforeCapture = Durin::SnapshotMaterialCompilerInput(*Material, Environment);
	ASSERT_TRUE(BeforeCapture);
	const auto& Before = BeforeCapture.Snapshot->Input;
	ASSERT_TRUE(Material->SetScalarParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::MetallicName(), 0.87f));
	auto AfterCapture = Durin::SnapshotMaterialCompilerInput(*Material, Environment);
	ASSERT_TRUE(AfterCapture);
	const auto& After = AfterCapture.Snapshot->Input;
	EXPECT_EQ(Before.IR, After.IR);
	EXPECT_EQ(Before.Parameters, After.Parameters);
	EXPECT_EQ(Before.StaticProperties, After.StaticProperties);
	EXPECT_EQ(Before.Environment, After.Environment);
	for (const auto& Node : Before.IR.Nodes) EXPECT_TRUE(Node.HasValidPayload());
	EXPECT_LT(Before.Parameters.size(), Material->GetParameterDefinitions().size());
	for (const auto& Parameter : Before.Parameters)
		EXPECT_NE(Material->FindParameterDefinition(Parameter.Id), nullptr);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
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
	EXPECT_FALSE(Normalized.IR.bAggregateAttributes);
	EXPECT_EQ(Normalized.IR.GetAttribute(EMaterialProperty::MP_BaseColor).Literal,
		(Durin::FMaterialProgramLiteral{0.f, 0.f, 0.f, 0.f}));
	EXPECT_EQ(Normalized.IR.GetAttribute(EMaterialProperty::MP_Normal).Literal,
		(Durin::FMaterialProgramLiteral{0.0f, 0.0f, 1.0f, 0.0f}));
	std::string Source;

	const auto SourceGeneration = Durin::GenerateMaterialProgramSlang(Normalized.IR);
	ASSERT_TRUE(SourceGeneration);
	Source = SourceGeneration.Source;
	EXPECT_EQ(Source.find("BaseColorTexture.Sample"), std::string::npos);
	EXPECT_EQ(Source.find("NormalTexture.Sample"), std::string::npos);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	const Durin::FMaterialCompilerResult Compiled =
		Durin::MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty()
		? std::string("no diagnostic") : Compiled.Diagnostics.front().Message);
	size_t ActiveBindings = 0;
	for (const Durin::FShaderCode& Shader : Compiled.GetShaderMap().CompiledShaders)
		ActiveBindings += Shader.Reflection.ResourceBindings.size();
	std::cout << "[MaterialOutputDefaultBaseline] input_ir_nodes="
		<< Input.IR.Nodes.size()
		<< " ir_nodes=" << Normalized.IR.Nodes.size()
		<< " texture_samples=0 generated_bytes=" << Source.size()
		<< " source_hash=" << Durin::FXxHash128::HashBuffer(Source).ToString()
		<< " identity=" << Compiled.GetShaderMap().ShaderMapId.Digest.ToString()
		<< " compiled_stages=" << Compiled.GetShaderMap().CompiledShaders.size()
		<< " active_bindings=" << ActiveBindings << '\n';
}

TEST(FMaterialProgramSchemaTests, AggregateInputRequiresMaterialAttributesType)
{
	InitializeDObjectSystem();
	auto Graph = Durin::Testing::MakePBRMaterialExpressionsForTest();
	Graph.Outputs.Attributes = Graph.Outputs.Get(EMaterialProperty::MP_BaseColor).Connection; Graph.Outputs.bUseMaterialAttributes = true;
	std::vector<Durin::DMaterialExpression*> Expressions;
	for (const auto& Expression : Graph.Expressions) Expressions.push_back(Expression.Get());
	auto Validation = Durin::MIR::FGraphBuilder::ValidateSurface(Expressions, Graph.Outputs);
	EXPECT_FALSE(Validation);
	ASSERT_FALSE(Validation.Diagnostics.empty());
	EXPECT_EQ(Validation.Diagnostics.front().Message, "Aggregate material output requires a Surface expression.");
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
	Input.IR.GetAttribute(EMaterialProperty::MP_Metallic).bExpression = true;
	Input.IR.GetAttribute(EMaterialProperty::MP_Metallic).ExpressionIndex = static_cast<uint32>(Input.IR.Nodes.size() - 1);
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
	Input.IR.GetAttribute(EMaterialProperty::MP_Metallic).bExpression = true;
	Input.IR.GetAttribute(EMaterialProperty::MP_Metallic).ExpressionIndex = static_cast<uint32>(Input.IR.Nodes.size());
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
	Durin::MIR::FCompilerInput BuilderChange = BaselineInput;
	BuilderChange.Environment.BuilderVersion ^= uint64{1} << 48;
	ExpectDifferent(std::move(BuilderChange));
	Durin::MIR::FCompilerInput AdapterChange = BaselineInput;
	AdapterChange.Environment.AdapterVersion++;
	ExpectDifferent(std::move(AdapterChange));
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
}

TEST(FMaterialProgramCompilerTests,
	CanonicalIRGeneratesStableBoundedSourceAndCompleteStages)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::MIR::FCompilerInput Input = MakeSyntheticMaterialCompilerInput();

	const auto CompilerEnvironmentResult = Durin::BuildDefaultMaterialCompilerEnvironment(
		Input.Environment);
	ASSERT_TRUE(CompilerEnvironmentResult) << CompilerEnvironmentResult.error();
	ASSERT_GE(Input.Environment.Dependencies.size(), 1u);
	EXPECT_EQ(Input.Environment.Dependencies.front().VirtualPath,
		"/Engine/MaterialCompilerEnvironment");
	EXPECT_FALSE(Input.Environment.Dependencies.front().ContentHash.IsZero());
	const auto Normalized = Durin::MIR::Normalize(Input);
	ASSERT_TRUE(Normalized);
	std::string FirstSource;
	std::string SecondSource;
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
		: Compiled.Diagnostics.front().Message);
	EXPECT_EQ(Compiled.GetShaderMap().ShaderMapId, Normalized.Identity);
	ASSERT_EQ(Compiled.GetShaderMap().CompiledShaders.size(), Durin::GetDefaultMaterialShaderEntries().size());
	const auto& ForwardBindings = FindCompiledMaterialStage(Compiled.GetShaderMap(), "FragmentMain").Reflection.ResourceBindings;
	EXPECT_EQ(ForwardBindings.size(), 22u);
	const auto LocalLights = std::ranges::find_if(ForwardBindings, [](const auto& Binding) {
		return Binding.Name == "LocalLights";
	});
	ASSERT_NE(LocalLights, ForwardBindings.end());
	EXPECT_EQ(LocalLights->Type, Durin::ERHIBindingType::StorageBuffer);
	EXPECT_EQ(FindCompiledMaterialStage(Compiled.GetShaderMap(), "GeometryFragmentMain").Reflection.ResourceBindings.size(), 14u);
	EXPECT_TRUE(FindCompiledMaterialStage(Compiled.GetShaderMap(), "ShadowFragmentMain").Reflection.ResourceBindings.empty());

	Durin::MIR::FModule InvalidIR = Normalized.IR;
	InvalidIR.Version++;
	const auto InvalidSource = Durin::GenerateMaterialProgramSlang(InvalidIR);
	EXPECT_FALSE(InvalidSource);
	EXPECT_TRUE(InvalidSource.Source.empty());
	EXPECT_FALSE(InvalidSource.Diagnostics.empty());
	Durin::MIR::FCompilerInput InvalidInput = Input;
	InvalidInput.Environment.Target.clear();
	const auto Failed = Durin::MIR::Compile(InvalidInput);
	EXPECT_FALSE(Failed);
	EXPECT_TRUE(Failed.GetShaderMap().CompiledShaders.empty());
	EXPECT_FALSE(Failed.Diagnostics.empty());
}

TEST(FMaterialProgramCompilerTests, NormalizedIRRoundTripPreservesSourceLayoutAndIdentity)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto Input = MakeSyntheticMaterialCompilerInput();
	const auto CompilerEnvironmentResult = BuildDefaultMaterialCompilerEnvironment(Input.Environment);
	ASSERT_TRUE(CompilerEnvironmentResult) << CompilerEnvironmentResult.error();
	const auto Baseline = MIR::Normalize(Input);
	ASSERT_TRUE(Baseline);
	MIR::FCompilerInput Direct{.IR = Baseline.IR, .Parameters = Input.Parameters,
		.StaticProperties = Input.StaticProperties, .Environment = Input.Environment};
	const auto Repeated = MIR::Normalize(Direct);
	ASSERT_TRUE(Repeated);
	EXPECT_EQ(Repeated.Identity, Baseline.Identity);
	EXPECT_EQ(Repeated.CanonicalBytes, Baseline.CanonicalBytes);
	EXPECT_EQ(Repeated.Layout, Baseline.Layout);
	const auto Source = GenerateMaterialProgramSlang(Baseline.IR, Baseline.Layout);
	const auto RepeatedSource = GenerateMaterialProgramSlang(Repeated.IR, Repeated.Layout);
	ASSERT_TRUE(Source);
	ASSERT_TRUE(RepeatedSource);
	EXPECT_EQ(RepeatedSource.Source, Source.Source);
}

TEST(FMaterialProgramCompilerTests, DirectIRNormalizationPrunesDeadCodeAndRemapsSourcesDeterministically)
{
	using namespace Durin;
	MIR::FCompilerInput Input;
	Input.Environment = MakeSyntheticMaterialCompilerInput().Environment;
	InitializeSurfaceRootSemantics(Input.IR);
	Input.IR.Nodes = {
		{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.25f}},
		{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.5f}},
		{.Opcode = EMaterialProgramOpcode::Add, .ResultType = EMaterialProgramValueType::Float, .Inputs = {0, 1}},
		{.Opcode = EMaterialProgramOpcode::Splat3, .ResultType = EMaterialProgramValueType::Float3, .Inputs = {2}},
		{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{9.f}}};
	Input.IR.GetAttribute(EMaterialProperty::MP_BaseColor).bExpression = true;
	Input.IR.GetAttribute(EMaterialProperty::MP_BaseColor).ExpressionIndex = 3;
	Input.Sources = {{.ExpressionIndex = 0, .NodeId = {1, 0, 0, 1}}, {.ExpressionIndex = 1, .NodeId = {1, 0, 0, 2}},
		{.ExpressionIndex = 4, .NodeId = {1, 0, 0, 3}}};
	for (uint32 Index = 0; Index < 40; ++Index) Input.Parameters.push_back({{2, 0, 0, Index + 1}, EMaterialParameterType::Texture});
	const auto Baseline = MIR::Normalize(Input);
	ASSERT_TRUE(Baseline);
	EXPECT_EQ(Baseline.IR.Nodes.size(), 4u);
	EXPECT_EQ(Baseline.Sources.size(), 2u);
	EXPECT_TRUE(Baseline.ActiveParameters.empty());
	std::swap(Input.IR.Nodes[0], Input.IR.Nodes[1]);
	Input.IR.Nodes[2].Inputs = {1, 0};
	Input.Sources[0].ExpressionIndex = 1; Input.Sources[1].ExpressionIndex = 0;
	Input.IR.GetAttribute(EMaterialProperty::MP_BaseColor).Literal = {42, 43, 44};
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
	InitializeSurfaceRootSemantics(IR);
	IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
		.ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.25f}});
	IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Saturate,
		.ResultType = EMaterialProgramValueType::Float, .Inputs = {0}});
	IR.GetAttribute(EMaterialProperty::MP_Roughness).bExpression = true;
	IR.GetAttribute(EMaterialProperty::MP_Roughness).ExpressionIndex = 1;
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
	IR.GetAttribute(EMaterialProperty::MP_Metallic).Literal.X = std::numeric_limits<float>::quiet_NaN(); Reject();
	IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
		.ResultType = EMaterialProgramValueType::Float3, .Payload = FMaterialProgramLiteral{}});
	for (uint32 Index = 0; Index < MaterialFunctionMaxExpandedLinks / 8; ++Index)
		IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::MakeMaterialAttributes,
			.ResultType = EMaterialProgramValueType::MaterialAttributes, .Inputs = {2, 2, 0, 0, 0, 2, 0, 0}});
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
	EXPECT_EQ(Built.Layout.Identity.Version, CompiledMaterialRenderLayoutVersion);
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

TEST(FMaterialProgramCompilerTests, StaleProducerRequestsRejectBeforeGeneration)
{
	const auto Baseline = MakeSyntheticMaterialCompilerInput();
	ASSERT_NE(Durin::GetMaterialBuildVersion(), uint64{0});
	for (int Mutation = 0; Mutation < 2; ++Mutation)
	{
		auto Input = Baseline;
		if (Mutation == 0) Input.Environment.BuilderVersion ^= uint64{1} << 48;
		else Input.Environment.AdapterVersion++;
		const auto Result = Durin::MIR::Compile(Input);
		EXPECT_FALSE(Result);
		EXPECT_TRUE(Result.GetDebugData().GeneratedSource.empty());
		EXPECT_TRUE(Result.GetShaderMap().CompiledShaders.empty());
		ASSERT_FALSE(Result.Diagnostics.empty());
		EXPECT_EQ(Result.Diagnostics.front().Message, "Material compiler environment identity, target, pass contract, or dependency bounds are invalid.");
	}
}

TEST(FMaterialProgramCompilerTests, CustomNumericTextureAndResourceFreeProgramsCompileWithTheirLayouts)
{
	using namespace Durin;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input;
	Input.IR = MakeDefaultMaterialCompilerIR();
	std::expected<void, std::string> Error;
	const auto CompilerEnvironmentResult = BuildDefaultMaterialCompilerEnvironment(Input.Environment, VulkanShaderTarget);
	ASSERT_TRUE(CompilerEnvironmentResult) << CompilerEnvironmentResult.error();
	Input.StaticProperties.BlendMode = EMaterialBlendMode::Masked;
	const FGuid Tint{0, 0, 1, 1}, UV{0, 0, 1, 2}, Texture{0, 0, 1, 3}, Amount{0, 0, 1, 4};
	Input.Parameters = {{Tint, EMaterialParameterType::Vector4}, {UV, EMaterialParameterType::Vector4},
		{Texture, EMaterialParameterType::Texture}, {Amount, EMaterialParameterType::Scalar}};
	auto Add = [&](EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		FGuid Parameter, std::vector<uint32> Links = {}) {
		MIR::FNode Node{.Opcode = Opcode, .ResultType = Type, .Inputs = std::move(Links)};
		if (Parameter.IsValid()) Node.Payload = Parameter;
		ResolveNodeSemantics(Input.IR, Node);
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
	Input.IR.GetAttribute(EMaterialProperty::MP_BaseColor).bExpression = true;
	Input.IR.GetAttribute(EMaterialProperty::MP_BaseColor).ExpressionIndex = Add(EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float3, {}, {Product});
	Input.IR.Nodes.back().Payload = MIR::FSwizzle{3, {0, 1, 2}};
	Input.IR.GetAttribute(EMaterialProperty::MP_OpacityMask).bExpression = true;
	Input.IR.GetAttribute(EMaterialProperty::MP_OpacityMask).ExpressionIndex = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float, Amount);
	const auto Compiled = MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic" : Compiled.Diagnostics.front().Message);
	EXPECT_EQ(Compiled.GetShaderMap().Layout.Identity.Version, CompiledMaterialRenderLayoutVersion);
	EXPECT_EQ(Compiled.GetShaderMap().Layout.Fields.size(), 4u);
	const auto& Entries = Compiled.GetShaderMap().Entries;
	const auto Shadow = std::ranges::find(Entries, "FSurfaceMaskedShadowFragmentShader", [](const auto& E) { return E.Shader.ShaderType; });
	ASSERT_NE(Shadow, Entries.end());
	EXPECT_EQ(Shadow->Requirements.AttributeIDs, (std::vector{FMaterialAttributeDefinitionMap::GetID(EMaterialProperty::MP_OpacityMask)}));
	EXPECT_EQ(Shadow->Requirements.ParameterIDs, (std::vector{Amount}));
	EXPECT_TRUE(Shadow->Requirements.ResourceIndices.empty());
	EXPECT_TRUE(Shadow->Requirements.bUsesMask);
	for (const auto Name : {"ShadowFragmentMain", "HitProxyFragmentMain"})
		EXPECT_FALSE(std::ranges::any_of(FindCompiledMaterialStage(Compiled.GetShaderMap(), Name).Reflection.ResourceBindings,
			[](const auto& B) { return B.Type == ERHIBindingType::Texture || B.Type == ERHIBindingType::Sampler; }));
	for (uint32 Mutation = 0; Mutation < 9; ++Mutation)
	{
		auto Broken = Compiled; auto Map = std::make_shared<FMaterialShaderMap>(Compiled.GetShaderMap()); Broken.Resource.ShaderMap = Map;
		if (Mutation == 0) Map->Entries.front().Shader.PassContractVersion--;
		if (Mutation == 1) Map->Entries.front().Shader.ShaderType = "UnknownMaterialEntry";
		if (Mutation == 2) Map->Entries.push_back(Map->Entries.back());
		if (Mutation == 3) Map->Entries.front().Shader.Target = "unsupported-target";
		if (Mutation == 4) {
			const auto Mesh = std::ranges::find_if(Map->Entries, [](const auto& E) { return E.Mesh.has_value(); });
			ASSERT_NE(Mesh, Map->Entries.end()); Mesh->Mesh->VertexFactoryType = "UnknownFactory";
		}
		if (Mutation == 5) Map->Entries.front().Requirements.ParameterIDs.push_back(FGuid::NewGuid());
		if (Mutation == 6) Map->Entries.front().Requirements.SceneTextures.push_back({});
		if (Mutation == 7) Map->Entries.front().Requirements.bUsesMask = !Map->Entries.front().Requirements.bUsesMask;
		if (Mutation == 8) Map->Entries.front().Requirements.Interpolators.push_back({.Type = EMaterialProgramValueType(255)});
		EXPECT_FALSE(ValidateMaterialCompilerResult(Broken)) << Mutation;
		FByteBuffer Cooked;
		EXPECT_FALSE(Testing::EncodeMaterialCookedProgramFamilyForTest(Broken.Resource, Input.StaticProperties,
			ECookTargetPlatform::Win64, ECookTargetProfile::Game, Cooked)) << Mutation;
	}
	// A coverage-only build removes the unrelated base-color texture from its
	// whole-module layout as well as both pixel entry closures.
	auto Coverage = Input; Coverage.StaticProperties.PassUsage = 4 | 8;
	const auto CoverageProgram = MIR::Compile(Coverage);
	ASSERT_TRUE(CoverageProgram) << FormatMaterialProgramDiagnostics(CoverageProgram.Diagnostics);
	EXPECT_EQ(CoverageProgram.GetShaderMap().ActiveParameters, (std::vector<FMaterialCompilerParameterDeclaration>{{Amount, EMaterialParameterType::Scalar}}));
	EXPECT_EQ(CoverageProgram.GetShaderMap().Layout.ResourceFieldCount, 0u);

	EXPECT_EQ(Compiled.GetShaderMap().Layout.ResourceFieldCount, 1u);
	EXPECT_TRUE(ValidateMaterialCompiledStages(Compiled.GetShaderMap().CompiledShaders, Compiled.GetShaderMap().Layout));
	EXPECT_TRUE(ValidateMaterialCompilerResult(Compiled));
	FByteBuffer CookedBytes;
	ASSERT_TRUE((Error = Testing::EncodeMaterialCookedProgramFamilyForTest(Compiled.Resource, Input.StaticProperties,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game, CookedBytes))) << Error.error();
	FMaterialStaticProperties CookedProperties;
	std::shared_ptr<const FMaterialProgram> Cooked;
	ASSERT_TRUE((Error = Testing::DecodeMaterialCookedProgramFamilyForTest(CookedBytes,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game, Compiled.Resource,
		CookedProperties, Cooked))) << Error.error();
	ASSERT_NE(Cooked, nullptr);
	EXPECT_EQ(Cooked->GetShaderMap().Layout, Compiled.GetShaderMap().Layout);
	EXPECT_EQ(Cooked->GetShaderMap().ActiveParameters, Compiled.GetShaderMap().ActiveParameters);
	EXPECT_TRUE(Cooked->GetDebugData().IR.Nodes.empty());
	EXPECT_TRUE(Cooked->GetDebugData().GeneratedSource.empty());
	FByteBuffer Reencoded;
	ASSERT_TRUE((Error = Testing::EncodeMaterialCookedProgramFamilyForTest(*Cooked, CookedProperties,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game, Reencoded)));
	EXPECT_EQ(Reencoded, CookedBytes);
	for (const uint32 Word : {1u, 2u})
	{
		auto Incompatible = CookedBytes;
		ASSERT_TRUE(Testing::SetMaterialCookedProgramArtifactWordForTest(Incompatible, Word,
			Word == 2 ? 11u : 0xffffffffu));
		ASSERT_FALSE((Error = Testing::DecodeMaterialCookedProgramFamilyForTest(Incompatible,
			ECookTargetPlatform::Win64, ECookTargetProfile::Game, Compiled.Resource,
			CookedProperties, Cooked)));
		if (Word == 1)
			EXPECT_EQ(Error.error(), "Material cooked program format is incompatible; recook from authored assets.");
		else
			EXPECT_TRUE(Error.error().starts_with("Material cooked program versions are incompatible."));
	}
	for (uint32 Mutation = 0; Mutation < 5; ++Mutation)
	{
		auto Invalid = Compiled;
		auto InvalidArtifact = std::make_shared<FMaterialShaderMap>(Compiled.GetShaderMap());
		Invalid.Resource.ShaderMap = InvalidArtifact;
		if (Mutation == 0) InvalidArtifact->Layout.Fields.front().Offset += 4;
		if (Mutation == 1) InvalidArtifact->Layout.Identity.Id = FGuid::NewGuid();
		if (Mutation == 2) InvalidArtifact->Layout.UniformPayloadSize += 16;
		if (Mutation == 3) InvalidArtifact->ActiveParameters.pop_back();
		if (Mutation == 4) InvalidArtifact->CompiledShaders.front().BinaryEntryPoint.clear();
		EXPECT_FALSE((Error = Testing::EncodeMaterialCookedProgramFamilyForTest(Invalid.Resource, Input.StaticProperties,
			ECookTargetPlatform::Win64, ECookTargetProfile::Game, Reencoded)));
	}
	const auto AcceptedCooked = Cooked;
	for (size_t Position : {size_t{4}, CookedBytes.size() / 2, CookedBytes.size() - 1})
	{
		auto Broken = CookedBytes; Broken[Position] ^= std::byte{1};
		EXPECT_FALSE((Error = Testing::DecodeMaterialCookedProgramFamilyForTest(Broken,
			ECookTargetPlatform::Win64, ECookTargetProfile::Game, Compiled.Resource,
			CookedProperties, Cooked)));
		EXPECT_EQ(Cooked, AcceptedCooked);
	}
	auto LegacyCooked = CookedBytes;
	ASSERT_TRUE(Testing::SetMaterialCookedProgramFamilyVersionForTest(
		LegacyCooked, 3));
	ASSERT_FALSE((Error = Testing::DecodeMaterialCookedProgramFamilyForTest(LegacyCooked,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game, Compiled.Resource,
		CookedProperties, Cooked)));
	EXPECT_EQ(Error.error(), "Material cooked program format is incompatible; recook from authored assets.");

	auto InvalidResult = Compiled;
	auto InvalidArtifact = std::make_shared<FMaterialShaderMap>(Compiled.GetShaderMap());
	InvalidResult.Resource.ShaderMap = InvalidArtifact;
	InvalidArtifact->ActiveParameters.pop_back();
	EXPECT_FALSE(ValidateMaterialCompilerResult(InvalidResult));
	InvalidResult = Compiled;
	InvalidArtifact = std::make_shared<FMaterialShaderMap>(Compiled.GetShaderMap());
	InvalidResult.Resource.ShaderMap = InvalidArtifact;
	InvalidArtifact->CompiledShaders.pop_back();
	EXPECT_FALSE(ValidateMaterialCompilerResult(InvalidResult));
	InvalidResult = Compiled;
	InvalidResult.Status = EMaterialProgramStatus::Error;
	EXPECT_FALSE(ValidateMaterialCompilerResult(InvalidResult));
	EXPECT_NE(Compiled.GetDebugData().GeneratedSource.find("MaterialTexture0.Sample(MaterialSampler0"), std::string::npos);
	ReorderIndependentMaterialIRNodes(Input);
	std::ranges::reverse(Input.Parameters);
	const auto Reordered = MIR::Normalize(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Reordered.Identity, Compiled.GetShaderMap().ShaderMapId);
	EXPECT_EQ(Reordered.Layout, Compiled.GetShaderMap().Layout);
	auto Corrupted = Compiled.GetShaderMap().CompiledShaders;
	ASSERT_FALSE(Corrupted.front().Reflection.ResourceBindings.empty());
	Corrupted.front().Reflection.ResourceBindings.front().Type = ERHIBindingType::StorageBuffer;
	EXPECT_FALSE(ValidateMaterialCompiledStages(Corrupted, Compiled.GetShaderMap().Layout));
	Corrupted = Compiled.GetShaderMap().CompiledShaders;
	Corrupted.front().Reflection.ResourceBindings.push_back(Corrupted.front().Reflection.ResourceBindings.front());
	EXPECT_FALSE(ValidateMaterialCompiledStages(Corrupted, Compiled.GetShaderMap().Layout));
	Corrupted.pop_back();
	EXPECT_FALSE(ValidateMaterialCompiledStages(Corrupted, Compiled.GetShaderMap().Layout));
	auto InvalidIR = Compiled.GetDebugData().IR;
	InvalidIR.Nodes.front().Inputs = {0xffffffffu};
	EXPECT_FALSE(GenerateMaterialProgramSlang(InvalidIR, Compiled.GetShaderMap().Layout));

	Input.IR = MakeDefaultMaterialCompilerIR();
	Input.Parameters.clear();
	Input.StaticProperties = {.ShadingModel = EMaterialShadingModel::Unlit};
	const auto ResourceFree = MIR::Compile(Input);
	ASSERT_TRUE(ResourceFree) << (ResourceFree.Diagnostics.empty() ? "missing diagnostic" : ResourceFree.Diagnostics.front().Message);
	EXPECT_TRUE(ResourceFree.GetShaderMap().Layout.Fields.empty());
	for (const auto& Stage : ResourceFree.GetShaderMap().CompiledShaders)
	{
		if (Stage.SourceEntryPoint == "HitProxyFragmentMain")
		{
			ASSERT_EQ(Stage.Reflection.ResourceBindings.size(), 1u);
			EXPECT_EQ(Stage.Reflection.ResourceBindings.front().Name, "HitProxy");
		}
		else if (Stage.Frequency == EShaderFrequency::Fragment) EXPECT_TRUE(Stage.Reflection.ResourceBindings.empty()) << Stage.SourceEntryPoint;
	}
}

TEST(FMaterialProgramCompilerTests, ExplicitUVAndSurfaceCompositionUseOnlyAuthoredInputs)
{
	using namespace Durin;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input;
	Input.IR = MakeDefaultMaterialCompilerIR();

	const auto CompilerEnvironmentResult = BuildDefaultMaterialCompilerEnvironment(Input.Environment);
	ASSERT_TRUE(CompilerEnvironmentResult) << CompilerEnvironmentResult.error();
	const FGuid Channel = FGuid::NewGuid(), Angle = FGuid::NewGuid();
	Input.Parameters = {{Channel, EMaterialParameterType::Scalar}, {Angle, EMaterialParameterType::Scalar}};
	auto Add = [&](EMaterialProgramOpcode Op, EMaterialProgramValueType Type,
		std::vector<uint32> Links = {}, FGuid Parameter = {}, FMaterialProgramLiteral Literal = {}) {
		MIR::FNode Node{.Opcode = Op, .ResultType = Type, .Inputs = std::move(Links)};
		if (Op == EMaterialProgramOpcode::Constant) Node.Payload = Literal;
		else if (Parameter.IsValid()) Node.Payload = Parameter;
		ResolveNodeSemantics(Input.IR, Node);
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
	Input.IR.Nodes[Normal].SpatialKind = EMaterialSpatialKind::Normal;
	Input.IR.Nodes[Normal].CoordinateSpace = EMaterialCoordinateSpace::Tangent;
	const auto Zero = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float);
	const auto One = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float, {}, {}, {1, 0, 0, 0});
	const auto WPO = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float3);
	const auto Specular = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float, {}, {}, {.5f});
	const auto CoatRoughness = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float, {}, {}, {.1f});
	Input.IR.bAggregateAttributes = true;
	Input.IR.AggregateAttributesIndex = Add(EMaterialProgramOpcode::MakeMaterialAttributes, EMaterialProgramValueType::MaterialAttributes,
		{Color, Normal, Zero, One, One, Color, One, One, WPO, Specular, One, CoatRoughness});
	const auto Unlowered = GenerateMaterialProgramSlang(Input.IR);
	EXPECT_FALSE(Unlowered);
	EXPECT_TRUE(Unlowered.Source.empty());
	ASSERT_FALSE(Unlowered.Diagnostics.empty());
	EXPECT_NE(Unlowered.Diagnostics.front().Message.find("normalize aggregate attributes first"), std::string::npos);
	const auto Normalized = MIR::Normalize(Input);
	ASSERT_TRUE(Normalized);
	EXPECT_EQ(Normalized.Layout.Fields.size(), 2u);
	EXPECT_EQ(Normalized.Layout.ResourceFieldCount, 0u);
	const auto Source = GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout);
	ASSERT_TRUE(Source);
	EXPECT_NE(Source.Source.find("SelectAuthoredUV(input,"), std::string::npos);
	EXPECT_EQ(Source.Source.find("MakeAuthoredSurface"), std::string::npos);
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
	TStrongObjectPtr<DMaterialExpressionSwizzle> PositionColor(NewObject<DMaterialExpressionSwizzle>(nullptr, NAME_None));
	Position->Id = FGuid::NewGuid(); Time->Id = FGuid::NewGuid(); PositionColor->Id = FGuid::NewGuid();
	PositionColor->Input = {Position.Get()}; PositionColor->Components = {0, 1, 0};
	const std::array<DMaterialExpression*, 3> Expressions{Position.Get(), Time.Get(), PositionColor.Get()};
	FMaterialRootInputs Outputs;
	Outputs.Get(EMaterialProperty::MP_BaseColor) = {PositionColor.Get()}; Outputs.Get(EMaterialProperty::MP_Roughness) = {Time.Get()};
	Outputs.Get(EMaterialProperty::MP_OpacityMask) = {Time.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
	FMaterialCompilerEnvironment Environment;

	const auto CompilerEnvironmentResult = BuildDefaultMaterialCompilerEnvironment(Environment);
	ASSERT_TRUE(CompilerEnvironmentResult) << CompilerEnvironmentResult.error();
	auto Properties = Material->GetStaticProperties();
	Properties.BlendMode = EMaterialBlendMode::Masked;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));
	auto InputCapture = SnapshotMaterialCompilerInput(*Material, Environment);
	ASSERT_TRUE(InputCapture);
	auto& Input = InputCapture.Snapshot->Input;
	const auto Normalized = MIR::Normalize(Input);
	ASSERT_TRUE(Normalized);
	const auto Source = GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout);
	ASSERT_TRUE(Source);
	EXPECT_NE(Source.Source.find("input.worldPosition"), std::string::npos);
	EXPECT_NE(Source.Source.find("MaterialView.Parameters.x"), std::string::npos);
	EXPECT_EQ(Source.Source.find("Material.SurfaceParams"), std::string::npos);
	EXPECT_EQ(Source.Source.find("materialTime :"), std::string::npos);
	const auto Compiled = MIR::Compile(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic" : Compiled.Diagnostics.front().Message);
	EXPECT_TRUE(ValidateMaterialCompilerResult(Compiled));
	for (const auto& Stage : Compiled.GetShaderMap().CompiledShaders | std::views::take(4))
	{
		const auto Uniform = std::ranges::find(Stage.Reflection.ResourceBindings, "MaterialView",
			&FShaderResourceBinding::Name);
		ASSERT_NE(Uniform, Stage.Reflection.ResourceBindings.end()) << Stage.SourceEntryPoint;
		EXPECT_EQ(Uniform->Type, ERHIBindingType::UniformBuffer);
	}

	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::WorldPosition, EMaterialProgramValueType::Float4));
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::Time, EMaterialProgramValueType::Float3));
}

TEST(FMaterialProgramNormalizationTests, PackedAndIndividualInputsSharePropertyActivation)
{
	using namespace Durin;
	auto Individual = MakeSyntheticMaterialCompilerInput();
	const std::array ExtendedValues{
		std::pair{EMaterialProperty::MP_Specular, FMaterialProgramLiteral{.25f}},
		std::pair{EMaterialProperty::MP_ClearCoat, FMaterialProgramLiteral{.75f}},
		std::pair{EMaterialProperty::MP_ClearCoatRoughness, FMaterialProgramLiteral{.33f}},
		std::pair{EMaterialProperty::MP_WorldPositionOffset, FMaterialProgramLiteral{.1f, .2f, .3f}}};
	for (const auto& [Property, Value] : ExtendedValues)
	{
		auto& Root = Individual.IR.GetAttribute(Property);
		Root.bExpression = false;
		Root.Literal = Value;
	}
	auto Packed = Individual;
	std::vector<uint32> Attributes;
	for (const auto& Root : Packed.IR.AttributeRoots)
	{
		if (Root.bExpression) Attributes.push_back(Root.ExpressionIndex);
		else
		{
			Attributes.push_back(static_cast<uint32>(Packed.IR.Nodes.size()));
			Packed.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
				.ResultType = Root.Type, .LegalStages = EMaterialEvaluationStage::Both,
				.SpatialKind = Root.SpatialKind, .CoordinateSpace = Root.CoordinateSpace,
				.Payload = Root.Literal});
		}
	}
	Individual.IR.Nodes = Packed.IR.Nodes;
	for (size_t Index = 0; Index < Individual.IR.AttributeRoots.size(); ++Index)
	{
		Individual.IR.GetAttribute(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property).bExpression = true;
		Individual.IR.GetAttribute(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property).ExpressionIndex = Attributes[Index];
	}
	Packed.IR.bAggregateAttributes = true;
	Packed.IR.AggregateAttributesIndex = static_cast<uint32>(Packed.IR.Nodes.size());
	Packed.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::MakeMaterialAttributes,
		.ResultType = EMaterialProgramValueType::MaterialAttributes,
		.LegalStages = EMaterialEvaluationStage::Both, .Inputs = Attributes});
	for (const auto Shading : {EMaterialShadingModel::DefaultLit, EMaterialShadingModel::Unlit, EMaterialShadingModel::ClearCoat})
		for (const auto Blend : {EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent})
		{
			Individual.StaticProperties.ShadingModel = Packed.StaticProperties.ShadingModel = Shading;
			Individual.StaticProperties.BlendMode = Packed.StaticProperties.BlendMode = Blend;
			const auto A = MIR::Normalize(Individual), B = MIR::Normalize(Packed);
			if (Shading == EMaterialShadingModel::ClearCoat && Blend == EMaterialBlendMode::Translucent)
			{
				EXPECT_FALSE(A); EXPECT_FALSE(B);
				continue;
			}
			ASSERT_TRUE(A); ASSERT_TRUE(B);
			EXPECT_FALSE(A.IR.bAggregateAttributes); EXPECT_FALSE(B.IR.bAggregateAttributes);
			for (size_t Index = 0; Index < FMaterialAttributeDefinitionMap::GetDefinitions().size(); ++Index)
			{
				const bool bActive = IsMaterialSurfaceOutputActive(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property, Individual.StaticProperties);
				if (!bActive)
				{
					EXPECT_FALSE(A.IR.GetAttribute(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property).bExpression);
					EXPECT_FALSE(B.IR.GetAttribute(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property).bExpression);
				}
			}
			// Every inactive property's exclusive source is absent in both normalized graphs.
			EXPECT_EQ(A.IR.Nodes.size(), B.IR.Nodes.size());
			FByteBuffer BytesA, BytesB;
			std::expected<void, std::string> Error;
			ASSERT_TRUE((Error = MIR::EncodeCanonical(A.IR, BytesA)));
			ASSERT_TRUE((Error = MIR::EncodeCanonical(B.IR, BytesB)));
			EXPECT_EQ(BytesA, BytesB);
			const auto SourceA = GenerateMaterialProgramSlang(A.IR, A.Layout);
			const auto SourceB = GenerateMaterialProgramSlang(B.IR, B.Layout);
			ASSERT_TRUE(SourceA); ASSERT_TRUE(SourceB);
			EXPECT_EQ(SourceA.Source, SourceB.Source);
			for (const auto& [Property, Value] : ExtendedValues)
				if (IsMaterialSurfaceOutputActive(Property, Individual.StaticProperties))
				{
					const auto& Root = B.IR.GetAttribute(Property);
					EXPECT_EQ(Root.bExpression ? B.IR.Nodes[Root.ExpressionIndex].GetLiteral() : Root.Literal, Value);
				}
		}
}

TEST(FMaterialProgramCompilerTests, NormalizedValidationRejectsAuthoredOnlyOperationsIncludingDeadSelectors)
{
	using namespace Durin;
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	MIR::FCompilerInput Base;
	Base.Environment = MakeSyntheticMaterialCompilerInput().Environment;
	InitializeSurfaceRootSemantics(Base.IR);
	Base.IR.Nodes.push_back({.Opcode = Op::Constant, .ResultType = Type::Float,
		.Payload = FMaterialProgramLiteral{.5f}});
	Base.IR.GetAttribute(EMaterialProperty::MP_Roughness).bExpression = true;
	Base.IR.GetAttribute(EMaterialProperty::MP_Roughness).ExpressionIndex = 0;
	ASSERT_TRUE(MIR::Validate(Base.IR, Base.Parameters));
	for (const auto Opcode : {Op::FunctionInput, Op::FunctionOutput, Op::FunctionCall,
		Op::TextureSampleParameter2D, Op::TextureCoordinates, Op::GetMaterialAttributes,
		Op::SetMaterialAttributes, Op::AppendVector, Op::StaticBool, Op::StaticSwitch,
		Op::QualitySwitch, Op::FeatureLevelSwitch})
	{
		SCOPED_TRACE(static_cast<uint32>(Opcode));
		for (const bool Reachable : {false, true})
		{
			SCOPED_TRACE(Reachable);
			auto Input = Base;
			const auto ResultType = Opcode == Op::StaticBool ? Type::StaticBool : Type::Float;
			MIR::FNode Node{.Opcode = Opcode, .ResultType = ResultType};
			if (const auto Signature = GetMaterialProgramNodeSignature(Opcode, ResultType))
				Node.Inputs.assign(Signature->InputCount, 0);
			Input.IR.Nodes.push_back(std::move(Node));
			if (Reachable)
				Input.IR.GetAttribute(EMaterialProperty::MP_Roughness).ExpressionIndex = 1;
			const auto Validated = MIR::Validate(Input.IR, Input.Parameters);
			EXPECT_FALSE(Validated);
			EXPECT_FALSE(Validated.Diagnostics.empty());
			EXPECT_FALSE(GenerateMaterialProgramSlang(Input.IR));
			const auto Normalized = MIR::Normalize(Input);
			EXPECT_FALSE(Normalized);
			EXPECT_TRUE(Normalized.IR.Nodes.empty());
			EXPECT_TRUE(Normalized.CanonicalBytes.empty());
		}
	}
}

TEST(FMaterialProgramOwnershipTests, ShaderDiagnosticsAreCollectedDirectlyByMaterialAssembly)
{
	using namespace Durin;
	FMaterialTranslatedProgram Translation; Translation.Status = EMaterialProgramStatus::Ok;
	FShaderCompilerOutput Output; Output.Errors.emplace_back("first compiler error");
	FShaderCompilerError Error{"second compiler error"}; Error.ErrorVirtualFilePath = "/Generated/Test.slang"; Error.ErrorLineString = "7";
	Output.Errors.push_back(std::move(Error));
	const auto Result = MaterialBuildPrivate::FinalizeMaterialShaderMap(std::move(Translation), std::move(Output));
	ASSERT_FALSE(Result); ASSERT_EQ(Result.Diagnostics.size(), 2u);
	EXPECT_FLOAT_EQ(Result.GetShaderMap().CompileTime, 0.f);
	EXPECT_EQ(Result.Diagnostics[0].Message, "first compiler error");
	EXPECT_EQ(Result.Diagnostics[1].Message, "/Generated/Test.slang(7): second compiler error");
}


TEST(FMaterialTextureReadTests, ModesValidateOperandDimensionsAndStageIntersection)
{
	using namespace Durin;
	using Type = EMaterialProgramValueType;
	using Stage = EMaterialEvaluationStage;
	using Mode = EMaterialTextureReadMode;
	for (const auto ReadMode : {Mode::Implicit, Mode::Level, Mode::Bias, Mode::Grad})
	{
		std::vector<FMaterialValueSemantics> Inputs{{Type::Texture2D}, {Type::Float2}};
		if (ReadMode == Mode::Level || ReadMode == Mode::Bias) Inputs.push_back({Type::Float});
		if (ReadMode == Mode::Grad) { Inputs.push_back({Type::Float2}); Inputs.push_back({Type::Float2}); }
		const auto Resolve = [&] { return ResolveMaterialProgramNodeSemantics(
			EMaterialProgramOpcode::TextureSample2D, Type::Float4, Inputs, nullptr, ReadMode); };
		const auto Result = Resolve();
		ASSERT_TRUE(Result);
		EXPECT_EQ(Result->Stages, ReadMode == Mode::Level ? Stage::Both : Stage::Pixel);
		Inputs[1].Type = Type::Float3;
		EXPECT_FALSE(Resolve());
		Inputs[1].Type = Type::Float;
		EXPECT_FALSE(Resolve());
		Inputs[1] = {Type::Float2, Stage::Vertex};
		EXPECT_EQ(bool(Resolve()), ReadMode == Mode::Level);
		Inputs[1] = {Type::Float2, Stage::Pixel};
		ASSERT_TRUE(Resolve());
		EXPECT_EQ(Resolve()->Stages, Stage::Pixel);
		Inputs.push_back({Type::Float});
		EXPECT_FALSE(Resolve());
	}
	EXPECT_FALSE(GetMaterialProgramNodeSignature(EMaterialProgramOpcode::TextureSample2D,
		Type::Float4, static_cast<Mode>(255)));
}

TEST(FMaterialTextureReadTests, ModesCompileReflectAndRoundTripDetachedTranslation)
{
	using namespace Durin;
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	using Mode = EMaterialTextureReadMode;
	InitializeDObjectSystem();
	std::set<FXxHash128> Identities;
	for (const auto Dimension : {EMaterialTextureDimension::Texture2D, EMaterialTextureDimension::TextureCube})
	for (const auto ReadMode : {Mode::Implicit, Mode::Level, Mode::Bias, Mode::Grad})
	{
		SCOPED_TRACE(static_cast<uint32>(ReadMode));
		MIR::FCompilerInput Input;
		Input.IR = MakeDefaultMaterialCompilerIR();
		ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment, VulkanShaderTarget));
		const FGuid TextureId{1, 2, 3, 4};
		Input.Parameters = {{TextureId, EMaterialParameterType::Texture, Dimension}};
		const bool Cube = Dimension == EMaterialTextureDimension::TextureCube;
		const auto Add = [&](MIR::FNode Node) {
			ResolveNodeSemantics(Input.IR, Node);
			const auto Index = uint32(Input.IR.Nodes.size());
			Input.IR.Nodes.push_back(std::move(Node));
			return Index;
		};
		const auto Texture = Add({.Opcode = Op::TextureParameter, .ResultType = Cube ? Type::TextureCube : Type::Texture2D, .Payload = TextureId});
		const auto UV = Add({.Opcode = Op::Constant, .ResultType = Cube ? Type::Float3 : Type::Float2, .Payload = FMaterialProgramLiteral{1.f,.25f,.75f}});
		std::vector<uint32> Operands{Texture, UV};
		if (ReadMode == Mode::Level || ReadMode == Mode::Bias)
			Operands.push_back(Add({.Opcode = Op::Constant, .ResultType = Type::Float, .Payload = FMaterialProgramLiteral{1}}));
		if (ReadMode == Mode::Grad) { Operands.push_back(UV); Operands.push_back(UV); }
		const auto Sample = Add({.Opcode = Cube ? Op::TextureSampleCube : Op::TextureSample2D, .ResultType = Type::Float4,
			.Inputs = Operands, .Payload = MIR::FTextureRead{ReadMode}});
		const auto RGB = Add({.Opcode = Op::Swizzle, .ResultType = Type::Float3, .Inputs = {Sample},
			.Payload = MIR::FSwizzle{3, {0,1,2}}});
		auto& Emissive = Input.IR.GetAttribute(EMaterialProperty::MP_EmissiveColor);
		Emissive.bExpression = true; Emissive.ExpressionIndex = RGB;
		const auto Normalized = MIR::Normalize(Input);
		ASSERT_TRUE(Normalized);
		EXPECT_TRUE(Identities.insert(Normalized.Identity.Digest).second);
		const auto Encoded = MaterialBuildPrivate::EncodeTranslationInput(Input);
		ASSERT_TRUE(Encoded);
		const auto Decoded = MaterialBuildPrivate::DecodeTranslationInput(Encoded->GetBytes());
		ASSERT_TRUE(Decoded);
		EXPECT_EQ(Decoded->IR, Input.IR);
		const auto Compiled = MIR::Compile(*Decoded);
		ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "" : Compiled.Diagnostics.front().Message);
		const auto CheckCooked = [&](const auto& Program) {
			FByteBuffer Bytes;
			auto Result = Testing::EncodeMaterialCookedProgramFamilyForTest(Program.Resource, Input.StaticProperties,
				ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes);
			ASSERT_TRUE(Result) << Result.error();
			FMaterialStaticProperties Properties;
			std::shared_ptr<const FMaterialProgram> Cooked;
			Result = Testing::DecodeMaterialCookedProgramFamilyForTest(Bytes, ECookTargetPlatform::Win64,
				ECookTargetProfile::Game, Program.Resource, Properties, Cooked);
			ASSERT_TRUE(Result) << Result.error();
			ASSERT_TRUE(Cooked);
			EXPECT_TRUE(Cooked->GetDebugData().IR.Nodes.empty());
			EXPECT_TRUE(Cooked->GetDebugData().GeneratedSource.empty());
			EXPECT_EQ(Cooked->GetShaderMap().Layout, Program.GetShaderMap().Layout);
			EXPECT_EQ(Cooked->GetShaderMap().ShaderMapId, Program.GetShaderMap().ShaderMapId);
		};
		const auto DumpSource = [&](const auto& Program, bool Vertex) {
			if (!Cube) return;
			if (const auto* Directory = std::getenv("DURIN_MATERIAL_CUBE_SOURCE_DIR"))
			{
				const auto Path = std::filesystem::path(Directory) / std::format("Cube-{}-{}.slang", uint32(ReadMode), Vertex ? "wpo" : "pixel");
				const auto& Source = Program.GetDebugData().GeneratedSource;
				ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Source)), Path));
			}
		};
		DumpSource(Compiled, false);
		CheckCooked(Compiled);
		for (const auto& Shader : Compiled.GetShaderMap().CompiledShaders)
		{
			const bool PixelSurface = Shader.SourceEntryPoint == "FragmentMain" || Shader.SourceEntryPoint == "GeometryFragmentMain";
			EXPECT_EQ(std::ranges::contains(Shader.Reflection.ResourceBindings, "MaterialTexture0", &FShaderResourceBinding::Name), PixelSurface);
		}
		Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).bExpression = true;
		Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).ExpressionIndex = RGB;
		const auto Vertex = MIR::Normalize(Input);
		EXPECT_EQ(bool(Vertex), ReadMode == Mode::Level);
		if (ReadMode == Mode::Level)
		{
			const auto VertexCompiled = MIR::Compile(Input);
			ASSERT_TRUE(VertexCompiled) << (VertexCompiled.Diagnostics.empty() ? "" : VertexCompiled.Diagnostics.front().Message);
			DumpSource(VertexCompiled, true);
			CheckCooked(VertexCompiled);
			for (const auto& Shader : VertexCompiled.GetShaderMap().CompiledShaders)
				if (Shader.Frequency == EShaderFrequency::Vertex)
					EXPECT_TRUE(std::ranges::contains(Shader.Reflection.ResourceBindings, "MaterialTexture0", &FShaderResourceBinding::Name));
		}
		Input.IR.Nodes[Sample].Payload = MIR::FTextureRead{static_cast<Mode>(255)};
		EXPECT_FALSE(MIR::Normalize(Input));
	}
}

TEST(FMaterialSceneTextureTests, PostProcessReadsCompileReflectAndSurviveSourceFreeCook)
{
	using namespace Durin;
	using Op = EMaterialProgramOpcode; using Type = EMaterialProgramValueType;
	InitializeDObjectSystem();
	for (const bool Depth : {false, true})
	{
		SCOPED_TRACE(Depth);
		MIR::FCompilerInput Input{.IR = MakeDefaultMaterialCompilerIR()};
		Input.IR.Domain = EMaterialDomain::PostProcess;
		Input.StaticProperties.ShadingModel = EMaterialShadingModel::Unlit;
		const auto EnvironmentResult = BuildDefaultMaterialCompilerEnvironment(Input.Environment, VulkanShaderTarget);
		ASSERT_TRUE(EnvironmentResult) << EnvironmentResult.error();
		const auto Add = [&](MIR::FNode Node) { ResolveNodeSemantics(Input.IR, Node); const auto I = uint32(Input.IR.Nodes.size()); Input.IR.Nodes.push_back(Node); return I; };
		const auto UV = Add({.Opcode = Op::ScreenPosition, .ResultType = Type::Float2});
		const auto Read = Add({.Opcode = Depth ? Op::SceneDepth : Op::SceneTexture, .ResultType = Depth ? Type::Float3 : Type::Float4,
			.Inputs = {UV}, .Payload = MIR::FSceneTextureRead{Depth ? EMaterialSceneTextureId::SceneDepth : EMaterialSceneTextureId::PostProcessInput0,
				Depth ? EMaterialSceneTextureFilter::Point : EMaterialSceneTextureFilter::Linear}});
		const auto Color = Depth ? Read : Add({.Opcode = Op::Swizzle, .ResultType = Type::Float3, .Inputs = {Read}, .Payload = MIR::FSwizzle{3, {0,1,2}}});
		auto& Root = Input.IR.GetAttribute(EMaterialProperty::MP_EmissiveColor); Root.bExpression = true; Root.ExpressionIndex = Color;
		const auto Translation = TranslatePreparedMaterialProgram({.Normalized = MIR::Normalize(Input), .StaticProperties = Input.StaticProperties, .Environment = Input.Environment});
		ASSERT_TRUE(Translation) << FormatMaterialProgramDiagnostics(Translation.Diagnostics);
		ASSERT_EQ(Translation.Entries.size(), 1u);
        const auto Grouped = MakeMaterialShaderCompileRequest(Translation,false);
        EXPECT_EQ(Grouped.EntryPoints,(std::vector<std::string>{"PostProcessMaterialMain"}));
        if (const auto* Directory = std::getenv("DURIN_MATERIAL_SCENE_SOURCE_DIR"))
        {
            const auto File = std::filesystem::path(Directory) / (Depth ? "SceneDepth.slang" : "Input0.slang");
            ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Translation.GeneratedSource)),File));
        }
		EXPECT_FALSE(Translation.Entries[0].Mesh);
		EXPECT_EQ(Translation.Entries[0].Shader.ShaderType, "FPostProcessMaterialShader");
		EXPECT_EQ(Translation.Entries[0].Requirements.SceneTextures.size(), Depth ? 2u : 1u);
        const auto Jobs = MakeMaterialShaderStageRequests(Translation, false); ASSERT_EQ(Jobs.size(), 1u);
        const auto ShaderOutput = FShaderCompilingManager::Get().CompileGeneratedSync(Jobs[0]);
        ASSERT_TRUE(ShaderOutput) << FormatShaderCompilerErrors(ShaderOutput.Errors);
        std::string Reflected;
        for (const auto& B : ShaderOutput.CompiledShaders[0].Reflection.ResourceBindings)
            Reflected += std::format("{} set {} binding {} type {}\n", B.Name, B.SetIndex, B.BindingIndex, uint32(B.Type));
        ASSERT_TRUE(ValidateMaterialCompiledStages(ShaderOutput.CompiledShaders, Translation.Layout, Translation.ResourceLimits,
            Translation.ActiveCollections, Translation.RequiredEntries)) << Reflected;
        auto AcceptedOutput = static_cast<FMaterialCompilationOutput>(Translation);
        const auto Schema = BuildMaterialParameterSchema(Translation.Entries[0].Shader.ShaderType, Translation.Layout, Translation.CollectionLayouts);
        ASSERT_TRUE(Schema);
        const auto AcceptedBindings = DescribeMaterialShaderBindings(ShaderOutput.CompiledShaders[0].Reflection, *Schema);
        ASSERT_TRUE(AcceptedBindings); AcceptedOutput.Entries[0].Bindings = *AcceptedBindings;
        AcceptedOutput.Entries[0].ParameterMapIdentity = GetMaterialParameterMapIdentity(AcceptedOutput.Entries[0], ShaderOutput.CompiledShaders[0]);
        ASSERT_TRUE(ValidateMaterialCompilationOutput(AcceptedOutput, Translation.Identity, Translation.Target, ShaderOutput.CompiledShaders)) << Reflected;
		const auto Encoded = MaterialBuildPrivate::EncodeTranslationInput(Input); ASSERT_TRUE(Encoded);
		const auto Decoded = MaterialBuildPrivate::DecodeTranslationInput(Encoded->GetBytes()); ASSERT_TRUE(Decoded);
		EXPECT_EQ(Decoded->IR, Input.IR);
		const auto Compiled = MIR::Compile(*Decoded); ASSERT_TRUE(Compiled) << FormatMaterialProgramDiagnostics(Compiled.Diagnostics);
		ASSERT_EQ(Compiled.GetShaderMap().CompiledShaders.size(), 1u);
		const auto& Bindings = Compiled.GetShaderMap().CompiledShaders[0].Reflection.ResourceBindings;
		EXPECT_TRUE(std::ranges::contains(Bindings, "PostProcessInput0", &FShaderResourceBinding::Name));
		EXPECT_EQ(std::ranges::contains(Bindings, "MaterialSceneDepth", &FShaderResourceBinding::Name), Depth);
		EXPECT_FALSE(std::ranges::contains(Bindings, "MaterialPrimitive", &FShaderResourceBinding::Name));
		FByteBuffer Bytes;
		auto Saved = Testing::EncodeMaterialCookedProgramFamilyForTest(Compiled.Resource, Input.StaticProperties, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes);
		ASSERT_TRUE(Saved) << Saved.error();
		FMaterialStaticProperties Properties; std::shared_ptr<const FMaterialProgram> Cooked;
		Saved = Testing::DecodeMaterialCookedProgramFamilyForTest(Bytes, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Compiled.Resource, Properties, Cooked);
		ASSERT_TRUE(Saved) << Saved.error(); ASSERT_TRUE(Cooked);
		EXPECT_TRUE(Cooked->GetDebugData().IR.Nodes.empty());
		EXPECT_EQ(Cooked->GetShaderMap().Entries, Compiled.GetShaderMap().Entries);
		Input.IR.Domain = EMaterialDomain::Surface; EXPECT_FALSE(MIR::Normalize(Input));
        Input.IR.GetAttribute(EMaterialProperty::MP_EmissiveColor).bExpression = false;
        auto& Inactive = Input.IR.GetAttribute(EMaterialProperty::MP_BaseColor); Inactive.bExpression = true; Inactive.ExpressionIndex = Color;
        EXPECT_TRUE(MIR::Normalize(Input)); // Unlit Surface does not execute BaseColor.
        Inactive.bExpression = false; Input.IR.GetAttribute(EMaterialProperty::MP_EmissiveColor).bExpression = true;
		Input.IR.Domain = EMaterialDomain::PostProcess;
		Input.IR.Nodes[Read].Payload = MIR::FSceneTextureRead{EMaterialSceneTextureId::SceneDepth, EMaterialSceneTextureFilter::Linear};
		EXPECT_FALSE(MIR::Normalize(Input));
	}
}

TEST(FMaterialSpecularTests, DefaultsReachabilityEntryIdentityAndSourceFreeCookRemainCanonical)
{
    using namespace Durin;
    InitializeDObjectSystem();
    MIR::FCompilerInput Input{.IR = MakeDefaultMaterialCompilerIR()};
    ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment, VulkanShaderTarget));
    const auto Baseline = MIR::Compile(Input); ASSERT_TRUE(Baseline) << FormatMaterialProgramDiagnostics(Baseline.Diagnostics);
    EXPECT_EQ(Baseline.GetDebugData().GeneratedSource.find("#define DURIN_MATERIAL_AUTHORED_SPECULAR 1"),std::string::npos);
    const auto FindGBuffer = [](const auto& Program) -> const FMaterialShaderEntryRequest& {
        return *std::ranges::find(Program.GetShaderMap().Entries, "FGBufferFragmentShader", [](const auto& E) { return E.Shader.ShaderType; });
    };
    Input.IR.GetAttribute(EMaterialProperty::MP_Specular).Literal = {0.f};
    const auto NonDefault = MIR::Compile(Input); ASSERT_TRUE(NonDefault) << FormatMaterialProgramDiagnostics(NonDefault.Diagnostics);
    EXPECT_NE(NonDefault.GetDebugData().GeneratedSource.find("#define DURIN_MATERIAL_AUTHORED_SPECULAR 1"),std::string::npos);
    EXPECT_EQ(FindGBuffer(Baseline).Shader.OutputLayout, FindGBuffer(NonDefault).Shader.OutputLayout);
    EXPECT_NE(Baseline.GetShaderMap().ShaderMapId,NonDefault.GetShaderMap().ShaderMapId);
    Input.IR.GetAttribute(EMaterialProperty::MP_Specular).Literal = {.5f};
    const FGuid Id{1,2,3,45};
    Input.Parameters = {{Id, EMaterialParameterType::Scalar}};
    Input.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Parameter, .ResultType = EMaterialProgramValueType::Float, .Payload = Id});
    auto& Specular = Input.IR.GetAttribute(EMaterialProperty::MP_Specular);
    Specular.bExpression = true; Specular.ExpressionIndex = uint32(Input.IR.Nodes.size()-1);
    const auto Dynamic = MIR::Compile(Input); ASSERT_TRUE(Dynamic) << FormatMaterialProgramDiagnostics(Dynamic.Diagnostics);
    EXPECT_NE(Dynamic.GetDebugData().GeneratedSource.find("#define DURIN_MATERIAL_AUTHORED_SPECULAR 1"),std::string::npos);
    EXPECT_EQ(Dynamic.GetShaderMap().Layout.Fields.size(), Baseline.GetShaderMap().Layout.Fields.size()+1);
    for (const auto& Entry : Dynamic.GetShaderMap().Entries)
        EXPECT_EQ(std::ranges::contains(Entry.Requirements.ParameterIDs,Id), Entry.Shader.ShaderType == "FSurfaceFragmentShader" || Entry.Shader.ShaderType == "FGBufferFragmentShader");
    FByteBuffer Bytes;
    const auto Encoded = Testing::EncodeMaterialCookedProgramFamilyForTest(Dynamic.Resource, Input.StaticProperties, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes);
    ASSERT_TRUE(Encoded) << Encoded.error();
    FMaterialStaticProperties Properties; std::shared_ptr<const FMaterialProgram> Cooked;
    const auto Decoded = Testing::DecodeMaterialCookedProgramFamilyForTest(Bytes,ECookTargetPlatform::Win64,ECookTargetProfile::Game,Dynamic.Resource,Properties,Cooked);
    ASSERT_TRUE(Decoded) << Decoded.error(); ASSERT_TRUE(Cooked);
    EXPECT_TRUE(Cooked->GetDebugData().IR.Nodes.empty());
    EXPECT_EQ(Cooked->GetShaderMap().Entries,Dynamic.GetShaderMap().Entries);
    auto Corrupt = static_cast<const FMaterialCompilationOutput&>(Cooked->GetShaderMap()); Corrupt.Entries.front().Shader.OutputLayout.Version--;
    EXPECT_FALSE(ValidateMaterialCompilationOutput(Corrupt,Cooked->GetShaderMap().ShaderMapId,Cooked->GetShaderMap().Target,Cooked->GetShaderMap().CompiledShaders));
    Input.StaticProperties.ShadingModel = EMaterialShadingModel::Unlit;
    const auto Inactive = MIR::Compile(Input); ASSERT_TRUE(Inactive) << FormatMaterialProgramDiagnostics(Inactive.Diagnostics);
    EXPECT_TRUE(Inactive.GetShaderMap().Layout.Fields.empty());
    Input.StaticProperties.ShadingModel = EMaterialShadingModel::DefaultLit;
    Input.IR.Nodes[Specular.ExpressionIndex] = {.Opcode = EMaterialProgramOpcode::Constant,.ResultType = EMaterialProgramValueType::Float,.Payload = FMaterialProgramLiteral{.5f}};
    const auto ExplicitDefault = MIR::Compile(Input); ASSERT_TRUE(ExplicitDefault) << FormatMaterialProgramDiagnostics(ExplicitDefault.Diagnostics);
    EXPECT_EQ(ExplicitDefault.GetDebugData().GeneratedSource.find("#define DURIN_MATERIAL_AUTHORED_SPECULAR 1"),std::string::npos);
}

TEST(FMaterialClearCoatTests, ModelAttributesReachBothEntriesAndSurviveSourceFreeCook)
{
	using namespace Durin;
	InitializeDObjectSystem();
	MIR::FCompilerInput Input{.IR = MakeDefaultMaterialCompilerIR()};
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment, VulkanShaderTarget));
	const auto Default = MIR::Compile(Input);
	ASSERT_TRUE(Default) << FormatMaterialProgramDiagnostics(Default.Diagnostics);
	Input.StaticProperties.ShadingModel = EMaterialShadingModel::ClearCoat;
	const FGuid WeightId{19, 28, 37, 46}, RoughId{18, 27, 36, 45};
	Input.Parameters = {{WeightId, EMaterialParameterType::Scalar}, {RoughId, EMaterialParameterType::Scalar}};
	for (const auto [Property, Id] : {std::pair{EMaterialProperty::MP_ClearCoat, WeightId}, std::pair{EMaterialProperty::MP_ClearCoatRoughness, RoughId}})
	{
		Input.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Parameter, .ResultType = EMaterialProgramValueType::Float, .Payload = Id});
		auto& Root = Input.IR.GetAttribute(Property);
		Root.bExpression = true;
		Root.ExpressionIndex = uint32(Input.IR.Nodes.size() - 1);
	}
	const auto Coat = MIR::Compile(Input);
	ASSERT_TRUE(Coat) << FormatMaterialProgramDiagnostics(Coat.Diagnostics);
	EXPECT_NE(Coat.GetShaderMap().ShaderMapId, Default.GetShaderMap().ShaderMapId);
	const auto GBuffer = [](const auto& P) -> const FMaterialShaderEntryRequest& { return *std::ranges::find(P.GetShaderMap().Entries, "FGBufferFragmentShader", [](const auto& E) { return E.Shader.ShaderType; }); };
	EXPECT_NE(GBuffer(Coat).Shader.OutputLayout, GBuffer(Default).Shader.OutputLayout);
	for (const auto& Entry : Coat.GetShaderMap().Entries)
	{
		const bool Pixel = Entry.Shader.ShaderType == "FSurfaceFragmentShader" || Entry.Shader.ShaderType == "FGBufferFragmentShader";
		EXPECT_EQ(std::ranges::contains(Entry.Requirements.ParameterIDs, WeightId), Pixel);
		EXPECT_EQ(std::ranges::contains(Entry.Requirements.ParameterIDs, RoughId), Pixel);
	}
	FByteBuffer Bytes;
	const auto Encoded = Testing::EncodeMaterialCookedProgramFamilyForTest(Coat.Resource, Input.StaticProperties, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes);
	ASSERT_TRUE(Encoded) << Encoded.error();
	FMaterialStaticProperties Properties;
	std::shared_ptr<const FMaterialProgram> Cooked;
	const auto Decoded = Testing::DecodeMaterialCookedProgramFamilyForTest(Bytes, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Coat.Resource, Properties, Cooked);
	ASSERT_TRUE(Decoded) << Decoded.error();
	ASSERT_TRUE(Cooked);
	EXPECT_TRUE(Cooked->GetDebugData().IR.Nodes.empty());
	EXPECT_EQ(Properties.ShadingModel, EMaterialShadingModel::ClearCoat);
	EXPECT_EQ(Cooked->GetShaderMap().Entries, Coat.GetShaderMap().Entries);
	Input.StaticProperties.BlendMode = EMaterialBlendMode::Masked;
	const auto Masked = MIR::Compile(Input);
	ASSERT_TRUE(Masked) << FormatMaterialProgramDiagnostics(Masked.Diagnostics);
	Input.StaticProperties.BlendMode = EMaterialBlendMode::Translucent;
	EXPECT_FALSE(MIR::Compile(Input));
	Input.StaticProperties.BlendMode = EMaterialBlendMode::Opaque;
	Input.StaticProperties.ShadingModel = EMaterialShadingModel::DefaultLit;
	const auto Inactive = MIR::Compile(Input);
	ASSERT_TRUE(Inactive) << FormatMaterialProgramDiagnostics(Inactive.Diagnostics);
	EXPECT_TRUE(Inactive.GetShaderMap().Layout.Fields.empty());
}

TEST(FMaterialBindingContractTests, CompilerLocationsSurviveAdmissionAndSourceFreeCookReplay)
{
    using namespace Durin;
    auto Input = MakeSyntheticMaterialCompilerInput();
    const auto Environment = BuildDefaultMaterialCompilerEnvironment(Input.Environment, VulkanShaderTarget);
    ASSERT_TRUE(Environment) << Environment.error();
    const auto Translation = MaterialBuildPrivate::TranslateMaterialInput(Input, false);
    ASSERT_TRUE(Translation);
    auto Requests = MakeMaterialShaderStageRequests(Translation, false);
    ASSERT_FALSE(Requests.empty());
    std::vector<FShaderCode> Stages;
    const auto Original = GenerateShaderParameterDeclaration("ConstantBuffer<MaterialUniform>", "Material");
    const std::string Relocated = "[[vk::binding(11, 2)]] ConstantBuffer<MaterialUniform> Material;\n";
    bool FoundMaterial = false;
    for (auto& Request : Requests)
    {
        const auto Position = Request.Source.find(Original);
        ASSERT_NE(Position, std::string::npos);
        Request.Source.replace(Position, Original.size(), Relocated);
        Request.VirtualPath = "/Generated/AllocationIndependence/" + FXxHash128::HashBuffer(Request.Source).ToString();
        const auto Output = FShaderCompilingManager::Get().CompileGeneratedSync(Request);
        ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
        ASSERT_EQ(Output.CompiledShaders.size(), 1u);
        for (const auto& B : Output.CompiledShaders[0].Reflection.ResourceBindings)
            if (B.Name == "Material") { FoundMaterial = true; EXPECT_EQ(B.SetIndex, 2u); EXPECT_EQ(B.BindingIndex, 11u); }
        Stages.push_back(Output.CompiledShaders[0]);
    }
    ASSERT_TRUE(FoundMaterial);
    auto Accepted = static_cast<FMaterialCompilationOutput>(Translation);
    for (size_t I = 0; I < Stages.size(); ++I)
    {
        const auto* D = FindMaterialShaderEntry(Accepted.Entries[I].Shader.ShaderType);
        ASSERT_NE(D, nullptr);
        auto Bindings = ValidateMaterialShaderBindings(Stages[I].Reflection, D->ShaderType, D->Frequency,
            Accepted.Layout, Accepted.CollectionLayouts);
        ASSERT_TRUE(Bindings);
        Accepted.Entries[I].Bindings = std::move(*Bindings);
        Accepted.Entries[I].ParameterMapIdentity = GetMaterialParameterMapIdentity(Accepted.Entries[I], Stages[I]);
    }
    ASSERT_TRUE(ValidateMaterialCompilationOutput(Accepted, Translation.Identity, Translation.Target, Stages));
    auto Program = MIR::Compile(Input);
    ASSERT_TRUE(Program) << FormatMaterialProgramDiagnostics(Program.Diagnostics);
    auto Map = std::make_shared<FMaterialShaderMap>(Program.GetShaderMap());
    Map->CompiledShaders = Stages;
    Map->Entries = Accepted.Entries;
    Program.Resource.ShaderMap = Map;
    ASSERT_TRUE(ValidateMaterialCompilerResult(Program));
    FByteBuffer Bytes;
    ASSERT_TRUE(Testing::EncodeMaterialCookedProgramFamilyForTest(Program.Resource, Input.StaticProperties,
        ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes));
    FMaterialStaticProperties Properties;
    std::shared_ptr<const FMaterialProgram> Replayed;
    ASSERT_TRUE(Testing::DecodeMaterialCookedProgramFamilyForTest(Bytes, ECookTargetPlatform::Win64,
        ECookTargetProfile::Game, Program.Resource, Properties, Replayed));
    ASSERT_TRUE(Replayed);
    EXPECT_EQ(Replayed->GetShaderMap().Entries, Accepted.Entries);
    for (size_t I = 0; I < Stages.size(); ++I)
    {
        auto Bad = Accepted;
        if (!Bad.Entries[I].Bindings.empty())
        {
            ++Bad.Entries[I].Bindings[0].BindingIndex;
            EXPECT_FALSE(ValidateMaterialCompilationOutput(Bad, Translation.Identity, Translation.Target, Stages));
        }
    }
}

TEST(FMaterialBindingContractTests, PublishedBindingsRejectMissingExtraWrongKindAndSchemaBeforeCook)
{
    using namespace Durin;
    auto Input = MakeSyntheticMaterialCompilerInput();
    ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment, VulkanShaderTarget));
    const auto Compiled = MIR::Compile(Input);
    ASSERT_TRUE(Compiled) << FormatMaterialProgramDiagnostics(Compiled.Diagnostics);
    FByteBuffer ValidBytes;
    const auto Encoded = Testing::EncodeMaterialCookedProgramFamilyForTest(Compiled.Resource, Input.StaticProperties,
        ECookTargetPlatform::Win64, ECookTargetProfile::Game, ValidBytes);
    ASSERT_TRUE(Encoded) << Encoded.error();
    const auto& Map = Compiled.GetShaderMap();
    const auto Entry = std::ranges::find_if(Map.Entries, [](const auto& E) { return !E.Bindings.empty(); });
    ASSERT_NE(Entry, Map.Entries.end());
    const auto Slot = Entry - Map.Entries.begin();
    for (size_t I = 0; I < Map.Entries.size(); ++I)
    {
        const auto Schema = BuildMaterialParameterSchema(Map.Entries[I].Shader.ShaderType, Map.Layout, Map.CollectionLayouts);
        ASSERT_TRUE(Schema);
        const auto Actual = DescribeMaterialShaderBindings(Map.CompiledShaders[I].Reflection, *Schema);
        ASSERT_TRUE(Actual); EXPECT_EQ(*Actual, Map.Entries[I].Bindings);
    }
    const auto& Types = FShaderType::GetTypeList();
    const auto Type = std::ranges::find_if(Types, [&](const auto* T) { return T->GetName() == Entry->Shader.ShaderType; });
    ASSERT_NE(Type, Types.end());
    const FShaderType* RuntimeType = *Type;
    const auto* Descriptor = FindMaterialShaderEntry(Entry->Shader.ShaderType);
    ASSERT_NE(Descriptor, nullptr);
    auto RuntimeEntries = Map.Entries;
    FMeshMaterialShaderMapCompileInput Admission{
        .Identity = Entry->Shader.Material, .Target = Map.Target, .LocalPermutationId = Entry->Shader.PermutationId,
        .VertexFactoryType = Descriptor->VertexFactory ? Descriptor->VertexFactory() : nullptr,
        .ShaderTypes = std::span(&RuntimeType, 1), .GeneratedStages = Map.CompiledShaders,
        .CompiledProgramIdentity = Map.ShaderMapId, .CompiledTarget = Map.Target,
        .GeneratedEntries = RuntimeEntries, .CompiledParameters = Compiled.Resource.ShaderMap, .bCreateRHIShaders = false};
    Admission.CompileOptions.Target = VulkanShaderTarget;
    FMeshMaterialShaderMap RuntimeMap;
    const auto Admitted = FMeshMaterialShaderMap::TryCompileSync(Admission, RuntimeMap);
    ASSERT_TRUE(Admitted) << Admitted.Error;
    RuntimeEntries[Slot].Bindings.clear();
    EXPECT_FALSE(FMeshMaterialShaderMap::TryCompileSync(Admission, RuntimeMap));
    EXPECT_FALSE(RuntimeMap);
    for (uint32 Mutation = 0; Mutation < 16; ++Mutation)
    {
        SCOPED_TRACE(Mutation);
        auto Bad = Compiled;
        auto Artifact = std::make_shared<FMaterialShaderMap>(Map);
        Bad.Resource.ShaderMap = Artifact;
        auto& Reflected = Artifact->CompiledShaders[Slot].Reflection.ResourceBindings;
        if (Mutation == 0) Artifact->Entries[Slot].Bindings.clear();
        if (Mutation == 1) Reflected.erase(Reflected.begin());
        if (Mutation == 2) Reflected.push_back(Reflected.front());
        if (Mutation == 3) Reflected.front().Type = Reflected.front().Type == ERHIBindingType::Sampler ? ERHIBindingType::Texture : ERHIBindingType::Sampler;
        if (Mutation == 4) Reflected.front().Name += "Undeclared";
        if (Mutation == 5) Reflected.front().ArraySize = 2;
        if (Mutation == 6) Reflected.front().BindingIndex = 65535;
        if (Mutation == 7) Artifact->Entries[Slot].Bindings.front().ResourceIndex++;
        if (Mutation == 8) Artifact->BindingSchemaVersion++;
        if (Mutation == 9) Artifact->Entries[Slot].ParameterSchemaIdentity.HashLow ^= 1;
        if (Mutation == 10) { auto Extra = Reflected.front(); Extra.BindingIndex = 60000; Extra.Name = "UndeclaredMaterialResource"; Reflected.push_back(Extra); }
        if (Mutation == 11) Artifact->Entries[Slot].Bindings.push_back(Artifact->Entries[Slot].Bindings.front());
        if (Mutation == 12) Artifact->Entries[Slot].ParameterMapIdentity.HashLow ^= 1;
        if (Mutation == 13) Reflected.front().ResourceShape = EShaderResourceShape::Texture3D;
        if (Mutation == 14) ++Reflected.front().ConstantBufferSize;
        if (Mutation == 15) {
            auto Uniform = std::ranges::find_if(Reflected, [](const auto& B) { return !B.ConstantMembers.empty(); });
            ASSERT_NE(Uniform, Reflected.end());
            ++Uniform->ConstantMembers.front().Offset;
        }
        EXPECT_FALSE(ValidateMaterialCompilerResult(Bad));
        FByteBuffer Bytes;
        EXPECT_FALSE(Testing::EncodeMaterialCookedProgramFamilyForTest(Bad.Resource, Input.StaticProperties,
            ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes));
        EXPECT_TRUE(Bytes.empty());
    }
    auto Reordered = Compiled;
    auto ReorderedMap = std::make_shared<FMaterialShaderMap>(Map);
    Reordered.Resource.ShaderMap = ReorderedMap;
    const auto Multi = std::ranges::find_if(ReorderedMap->Entries, [](const auto& E) { return E.Bindings.size() > 1; });
    ASSERT_NE(Multi, ReorderedMap->Entries.end());
    std::ranges::reverse(Multi->Bindings);
    EXPECT_FALSE(ValidateMaterialCompilerResult(Reordered));
    auto Translation = MaterialBuildPrivate::TranslateMaterialInput(Input, false);
    ASSERT_TRUE(Translation);
    const auto EncodedInput = MaterialBuildPrivate::EncodeTranslationInput(Input);
    ASSERT_TRUE(EncodedInput);
    Translation.BindingSchemaVersion++;
    EXPECT_FALSE(MaterialBuildPrivate::EncodeTranslationProduct(Translation, FXxHash128::HashBuffer(EncodedInput->GetBytes())));
    FShaderReflectionData Empty;
    EXPECT_FALSE(ValidateMaterialShaderBindings(Empty, "FHitProxyFragmentShader", EShaderFrequency::Fragment, {}, {}));
    EXPECT_FALSE(ValidateMaterialShaderBindings(Empty, "FPostProcessMaterialShader", EShaderFrequency::Fragment, {}, {}));
}
