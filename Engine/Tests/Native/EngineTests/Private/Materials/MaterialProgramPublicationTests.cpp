#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "MaterialProgramTestFixture.h"

TEST(FMaterialProgramPublicationTests,
	BasePublishesCompleteProgramAndInstancesReuseDynamicIdentity)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedMaterial("CompiledProgramBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
		nullptr, "CompiledProgramInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	const auto Initial = Base->GetRenderData();
	ASSERT_TRUE(Initial.CompiledProgram);
	ASSERT_TRUE(Initial.PlanningPassIdentity.ShaderMap.ProgramIdentity.IsValid());
	EXPECT_EQ(Initial.CompiledProgram, Instance->GetAcceptedCompiledProgram());
	EXPECT_EQ(Initial.CompiledProgram,
		Instance->GetRenderData().CompiledProgram);

	ASSERT_TRUE(Base->SetScalarParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::MetallicName(), 0.73f));
	const auto Dynamic = Base->GetRenderData();
	EXPECT_EQ(Dynamic.CompiledProgram, Initial.CompiledProgram);
	EXPECT_EQ(Dynamic.PlanningPassIdentity.ShaderMap.ProgramIdentity,
		Initial.PlanningPassIdentity.ShaderMap.ProgramIdentity);

	Durin::FMaterialStaticProperties PipelineOnly = Base->GetStaticProperties();
	PipelineOnly.bTwoSided = true;
	PipelineOnly.DepthWritePolicy = Durin::EMaterialDepthWritePolicy::Enabled;
	ASSERT_TRUE(Base->SetStaticProperties(PipelineOnly));
	const auto PipelineChanged = Base->GetRenderData();
	EXPECT_EQ(PipelineChanged.CompiledProgram, Initial.CompiledProgram);
	EXPECT_NE(PipelineChanged.PlanningPassIdentity,
		Initial.PlanningPassIdentity);

	Durin::FMaterialStaticProperties ShaderProperties = PipelineOnly;
	ShaderProperties.BlendMode = Durin::EMaterialBlendMode::Masked;
	ASSERT_TRUE(Base->SetStaticProperties(ShaderProperties));
	const auto ShaderChanged = Base->GetRenderData();
	ASSERT_TRUE(ShaderChanged.CompiledProgram);
	EXPECT_NE(ShaderChanged.CompiledProgram, Initial.CompiledProgram);
	EXPECT_NE(ShaderChanged.PlanningPassIdentity.ShaderMap.ProgramIdentity,
		Initial.PlanningPassIdentity.ShaderMap.ProgramIdentity);

	auto Edited = CaptureMaterialExpressions(*Base);
	Edited.Outputs.Roughness.SetConstant({ReadMaterialOutputDefault(Edited.Outputs, Durin::EMaterialOutputPin::Roughness)[0] + 0.01f});
	auto Validation = Edited.Apply(*Base);
	ASSERT_TRUE(Validation);
	EXPECT_EQ(Base->GetRenderData().PlanningPassIdentity.ShaderMap.ProgramIdentity,
		ShaderChanged.PlanningPassIdentity.ShaderMap.ProgramIdentity);
	Edited = CaptureMaterialExpressions(*Base);
	Edited.Outputs.Roughness.Connection = {};
	ASSERT_TRUE(Edited.Apply(*Base));
	const auto ProgramChanged = Base->GetRenderData();
	ASSERT_TRUE(ProgramChanged.CompiledProgram);
	EXPECT_NE(ProgramChanged.PlanningPassIdentity.ShaderMap.ProgramIdentity,
		ShaderChanged.PlanningPassIdentity.ShaderMap.ProgramIdentity);
	EXPECT_EQ(ProgramChanged.CompiledProgram,
		Instance->GetAcceptedCompiledProgram());

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}
