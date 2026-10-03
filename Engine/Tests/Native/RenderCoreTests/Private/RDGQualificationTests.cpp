#include "../../RDGTestAccess.h"
#include "RDG/RDG.h"
#include "RDGParameterTestSupport.h"
#include <gtest/gtest.h>

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
		constexpr size_t WarmupCompiles = 30;
		constexpr size_t MeasuredCompiles = 120;
		constexpr size_t Runs = 3;
		for (size_t Index = 0; Index < WarmupCompiles; ++Index)
			CompileFixture();

		std::array<uint64, Runs> Medians{};
		std::array<uint64, Runs> P95s{};
		FRDGCapture First;
		for (size_t Run = 0; Run < Runs; ++Run)
		{
			std::array<uint64, MeasuredCompiles> Samples{};
			for (size_t Index = 0; Index < MeasuredCompiles; ++Index)
			{
				auto Capture = CompileFixture();
				if (Run == 0 && Index == 0)
					First = Capture;
				Samples[Index] = Capture.Statistics.CompileMicroseconds;
			}
			std::ranges::sort(Samples);
			Medians[Run] = (Samples[59] + Samples[60]) / 2;
			P95s[Run] = Samples[113];
			RecordProperty(std::format("baseline_run_{}_median_us", Run + 1), Medians[Run]);
			RecordProperty(std::format("baseline_run_{}_p95_us", Run + 1), P95s[Run]);
			std::cout << std::format(
				"RHI_RDG_BASELINE workload=synthetic_compile_128_passes,run={},median_us={},p95_us={},authority=diagnostic\n",
				Run + 1, Medians[Run], P95s[Run]);
		}
		RecordProperty("baseline_warmup_compiles", WarmupCompiles);
		RecordProperty("baseline_measured_compiles", MeasuredCompiles);
		RecordProperty("baseline_runs", Runs);
		RecordProperty("baseline_authority", "diagnostic");

		auto Second = CompileFixture();
		EXPECT_EQ(First.Dump, Second.Dump);
		EXPECT_LT(Second.Statistics.CompileMicroseconds, 250000u);
		EXPECT_EQ(First.Dependencies.size(), 127u);
	}
}
