#include "../../RDGTestAccess.h"
#include "RDG.h"
#include "RDGParameterTestSupport.h"
#include <gtest/gtest.h>
#include <chrono>

namespace Durin
{
	TEST(FRDGQualificationTests, ParameterTraversalStaysWithinFoundationBudget)
	{
		FRDGBuilder Builder;
		Builder.SetBudget({.MaxCompileMicroseconds = 1'000'000});
		const auto Started = std::chrono::steady_clock::now();
		auto Parameters = Builder.AllocParameters<FLargeTokenGraphParameters>();
		for (uint32 Index = 0; Index < Parameters->Tokens.size(); ++Index)
			Parameters->Tokens[Index] = {
				Builder.CreateToken("Token." + std::to_string(Index))};
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "LargeParameters",
			ERDGPassType::Graphics, std::move(Parameters));
		ASSERT_TRUE(Pass.IsValid());
		const auto DeclarationMicroseconds = std::chrono::duration_cast<
			std::chrono::microseconds>(std::chrono::steady_clock::now() - Started)
			.count();
		EXPECT_LT(DeclarationMicroseconds, 1'000'000);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.has_value()) << ToString(Result.error());
		EXPECT_EQ(Builder.Capture().Uses.size(), 128u);
		EXPECT_FALSE(Builder.GetStatistics().bCompileBudgetExceeded);
		EXPECT_EQ(Builder.Capture().Uses.back().ParameterPath,
			"FLargeTokenGraphParameters.Tokens[127]");
	}

	TEST(FRDGQualificationTests, DumpIsDeterministicAndSyntheticCompileCostIsBounded)
	{
		auto CompileFixture = [] {
			static const auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
				"Fixture", 512, 4, EBufferUsageFlags::UnorderedAccess
			));
			FRDGBuilder Builder;
			const auto Work = Builder.CreateBuffer({.Buffer = Buffer->GetDesc()}, "Fixture");
			for (uint32 Index = 0; Index < 128; ++Index)
			{
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Pass" + std::to_string(Index),
					ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Work, 0, 512, ERDGUse::Write,
					ERHIAccess::ComputeShaderReadWrite, Index == 0);
			}
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_TRUE(Result.has_value()) << ToString(Result.error());
			return Builder.Capture();
		};
		auto First = CompileFixture();
		auto Second = CompileFixture();
		EXPECT_EQ(First.Dump, Second.Dump);
		EXPECT_LT(First.Statistics.CompileMicroseconds, 250000u);
		EXPECT_LT(Second.Statistics.CompileMicroseconds, 250000u);
		EXPECT_EQ(First.Dependencies.size(), 127u);
	}
}
