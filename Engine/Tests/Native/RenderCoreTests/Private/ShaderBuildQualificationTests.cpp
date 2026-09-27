#include "ShaderBuilder.h"
#include "ShaderBuild/ShaderPaths.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "NativeQualificationSupport.h"
#include <gtest/gtest.h>
#include <fstream>
#include <iostream>

namespace Durin
{
	namespace
	{
		class FShaderBuildQualificationTests : public testing::Test
		{
		protected:
			std::string PreviousRoot;
			std::filesystem::path Root;
			std::unique_ptr<Testing::FQualificationLogSession> Log;
			auto SetUp() -> void override
			{
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
				PreviousRoot = FPaths::DerivedDataCacheDir();
				Root = Testing::CreateTestFixtureDirectory("ShaderBuildBaseline");
				Log = std::make_unique<Testing::FQualificationLogSession>((Root / "Logs").generic_string());
				ASSERT_TRUE(Log->IsStarted());
			}
			auto TearDown() -> void override
			{
				FPaths::SetDerivedDataCacheDirForTests(PreviousRoot);
				Log.reset();
				Testing::RemoveTestWorkDirectory(Root);
			}
		};
	}

	TEST_F(FShaderBuildQualificationTests, ColdDdcAndMemoryBatches)
	{
		constexpr uint32 BatchSize = 8;
		std::array<std::vector<double>, 3> Samples;
		std::array<uint64, 3> PeakIncrease{}, PeakAllocated{};
		bool bAllocationSampleAvailable = false;
		uint64 OutputBytes = 0;
		uint64 ColdContentReads = 0;
		for (uint32 Round = 0; Round < 4; ++Round)
		{
			const auto RoundRoot = Root / std::to_string(Round);
			std::filesystem::create_directories(RoundRoot / "Source");
			FPaths::SetDerivedDataCacheDirForTests((RoundRoot / "DDC").generic_string());
			const auto Prefix = std::format("/ShaderBuildBaseline{}/", Round);
			FShaderPaths::RegisterMountPoint(Prefix, (RoundRoot / "Source").generic_string(),
				(RoundRoot / "Manifest").generic_string());
			for (uint32 Index = 0; Index < BatchSize; ++Index)
			{
				std::ofstream File(RoundRoot / "Source" / std::format("Shader{}.slang", Index));
				File << "[shader(\"vertex\")] float4 VertexMain(uint id : SV_VertexID) : SV_Position { return float4(float(id), "
					<< Index << ".0, 0.0, 1.0); }\n";
				ASSERT_TRUE(File.good());
			}
			FShaderCompileOptions Options;
			Options.EntryPoints = {"VertexMain"};
			Options.Frequencies = {EShaderFrequency::Vertex};
			std::array<FShaderCompilerOutput, BatchSize> Cold;
			auto Builder = std::make_unique<FShaderBuilder>();
			for (uint32 Mode = 0; Mode < 3; ++Mode)
			{
				// Destroy compiler/LRU state before measuring DDC. Keep that new
				// builder for the third pass so memory hits are measured separately.
				if (Mode == 1) Builder = std::make_unique<FShaderBuilder>();
				const auto Before = Builder->GetStats();
				std::array<FShaderCompilerOutput, BatchSize> Outputs;
				Testing::FQualificationAllocationSampler Allocations;
				const auto Start = std::chrono::steady_clock::now();
				for (uint32 Index = 0; Index < BatchSize; ++Index)
					Outputs[Index] = Builder->GetOrCompile(std::format("{}Shader{}", Prefix, Index), Options);
				const auto End = std::chrono::steady_clock::now();
				const auto Allocation = Allocations.Finish();
				uint64 Bytes = 0;
				for (uint32 Index = 0; Index < BatchSize; ++Index)
				{
					ASSERT_TRUE(Outputs[Index]) << FormatShaderError(Outputs[Index].Error);
					ASSERT_EQ(Outputs[Index].CompiledShaders.size(), 1u);
					const auto& Shader = Outputs[Index].CompiledShaders[0];
					ASSERT_TRUE(Shader.Code);
					Bytes += Shader.Code->size();
					if (Mode) EXPECT_EQ(Shader.Hash, Cold[Index].CompiledShaders[0].Hash);
				}
				const auto After = Builder->GetStats();
				EXPECT_EQ(After.Compilations - Before.Compilations, Mode == 0 ? BatchSize : 0u);
				EXPECT_EQ(After.DdcHits - Before.DdcHits, Mode == 1 ? BatchSize : 0u);
				EXPECT_EQ(After.MemoryHits - Before.MemoryHits, Mode == 2 ? BatchSize : 0u);
				EXPECT_EQ(After.DdcStoreFailures, 0u);
				if (Mode) EXPECT_EQ(After.ContentReads - Before.ContentReads, 0u);
				else
				{
					Cold = std::move(Outputs);
					ColdContentReads = After.ContentReads - Before.ContentReads;
					OutputBytes = Bytes;
				}
				if (Round)
				{
					Samples[Mode].push_back(std::chrono::duration<double, std::milli>(End - Start).count());
					PeakIncrease[Mode] = std::max(PeakIncrease[Mode], Allocation.GetPeakIncrease());
					PeakAllocated[Mode] = std::max(PeakAllocated[Mode], Allocation.PeakBytes);
					bAllocationSampleAvailable = Allocation.bAvailable;
				}
			}
		}
		for (auto& Values : Samples) std::ranges::sort(Values);
		std::cout << "derived_data_shader batch_size=" << BatchSize
			<< " cold_median_ms=" << Samples[0][1] << " ddc_median_ms=" << Samples[1][1]
			<< " lru_median_ms=" << Samples[2][1] << " output_bytes=" << OutputBytes
			<< " cold_content_reads=" << ColdContentReads << " ddc_content_reads=0 lru_content_reads=0"
			<< " allocation_sample_available=" << bAllocationSampleAvailable
			<< " cold_sampled_zone_peak_bytes=" << PeakAllocated[0] << " ddc_sampled_zone_peak_bytes=" << PeakAllocated[1]
			<< " lru_sampled_zone_peak_bytes=" << PeakAllocated[2]
			<< " cold_sampled_zone_increase_bytes=" << PeakIncrease[0] << " ddc_sampled_zone_increase_bytes=" << PeakIncrease[1]
			<< " lru_sampled_zone_increase_bytes=" << PeakIncrease[2]
			<< " warmup_batches=1 measured_batches=3" << std::endl;
	}
}
