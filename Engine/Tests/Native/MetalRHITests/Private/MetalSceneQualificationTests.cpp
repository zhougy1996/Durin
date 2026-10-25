#include "SceneRenderQualification.h"
#include "MetalShaderStatistics.h"

namespace Durin
{
	namespace
	{
		struct FSceneReplay
		{
			const char* Backend;
			const char* Mode;
		};
		class FMetalSceneQualificationTests : public ::testing::TestWithParam<FSceneReplay>
		{
		protected:
			auto SetUp() -> void override
			{
				if (const char* Value = std::getenv("DURIN_RHI_BACKEND")) Backend = Value;
				if (const char* Value = std::getenv("DURIN_RHI_EXECUTION")) Execution = Value;
				setenv("DURIN_RHI_BACKEND", GetParam().Backend, 1);
				setenv("DURIN_RHI_EXECUTION", GetParam().Mode, 1);
			}
			auto TearDown() -> void override
			{
				if (Backend) setenv("DURIN_RHI_BACKEND", Backend->c_str(), 1);
				else unsetenv("DURIN_RHI_BACKEND");
				if (Execution) setenv("DURIN_RHI_EXECUTION", Execution->c_str(), 1);
				else unsetenv("DURIN_RHI_EXECUTION");
			}
		private:
			std::optional<std::string> Backend, Execution;
		};
	}

	TEST_P(FMetalSceneQualificationTests, GraphStagesPreserveOutputAndFailure)
	{
		// Both backends execute production RenderView without AppKit or a drawable.
		QualifySceneRenderRoutes(false);
	}

	TEST_P(FMetalSceneQualificationTests, GPUCullingPreservesAttachmentsCountsAndFallback)
	{
		QualifySceneRenderRoutes(false, true);
	}

	TEST_P(FMetalSceneQualificationTests, AliasingPolicyPreservesSceneAndUnsupportedCapabilityFallback)
	{
		QualifySceneRenderRoutes(false, false, true);
	}

	TEST_P(FMetalSceneQualificationTests, PreparedSceneFirstDrawReusesNativeShadersAndPipelines)
	{
		FMetalShaderCreationStatistics Before;
		QualifySceneRenderRoutes(false, false, false, [&](bool Starting) {
			if (std::string_view(GetParam().Backend) != "metal") return;
			if (Starting) { Before = GetMetalShaderCreationStatistics(); return; }
			const auto After = GetMetalShaderCreationStatistics();
			EXPECT_EQ(After.FunctionsCreated, Before.FunctionsCreated);
			EXPECT_EQ(After.SourceCompileAttempts, Before.SourceCompileAttempts);
			EXPECT_EQ(After.GraphicsPipelinesCreated, Before.GraphicsPipelinesCreated);
			EXPECT_EQ(After.ComputePipelinesCreated, Before.ComputePipelinesCreated);
		});
	}

	INSTANTIATE_TEST_SUITE_P(ReplayModes, FMetalSceneQualificationTests,
		::testing::Values(FSceneReplay{"metal", "inline"}, FSceneReplay{"metal", "threaded"},
			FSceneReplay{"vulkan", "inline"}, FSceneReplay{"vulkan", "threaded"}),
		[](const ::testing::TestParamInfo<FSceneReplay>& Info) {
			return std::string(Info.param.Backend) + "_" + Info.param.Mode;
		});
}
