#include "ShaderBuilder.h"
#include "MetalLibraryBuild.h"
#include "MetalShaderFormat.h"
#include "ShaderBuild/ShaderPaths.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "NativeQualificationSupport.h"
#include "Threading/Task.h"
#include <gtest/gtest.h>

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

	#if defined(__APPLE__)
	TEST_F(FShaderBuildQualificationTests, MetalLibraryBuildIsDeterministicAndCached)
	{
		FPaths::SetDerivedDataCacheDirForTests((Root / "DDC").generic_string());
		const auto Producer = GetMetalLibraryProducerIdentity();
		ASSERT_TRUE(Producer) << Producer.error();
		FShaderCode Shader;
		Shader.Target = MetalShaderTarget;
		Shader.CodeFormat = EShaderCodeFormat::Msl20Source;
		Shader.Frequency = EShaderFrequency::Vertex;
		Shader.BinaryEntryPoint = "VertexMain";
		constexpr std::string_view Source = "#include <metal_stdlib>\nusing namespace metal;\nvertex float4 VertexMain(uint id [[vertex_id]]) { return float4(float(id), 0, 0, 1); }\n";
		Shader.Code = FSharedByteBuffer::Copy(std::as_bytes(std::span(Source)));
		Shader.CodeHash = FXxHash128::HashBuffer(Shader.Code);
		const auto Map = ShaderFormat::BuildMetalBindingMap(Shader.Frequency, {}, false);
		ASSERT_TRUE(Map);
		const auto Metadata = ShaderFormat::EncodeMetalMetadata(Shader.Frequency, *Map);
		ASSERT_TRUE(Metadata);
		Shader.PlatformData = *Metadata;
		FShaderBuildService Service;
		FSharedByteBuffer Previous;
		for (uint32 Round = 0; Round < 3; ++Round)
		{
			auto Request = MakeMetalLibraryBuildRequest(Shader, *Producer);
			ASSERT_TRUE(Request) << Request.error();
			DerivedData::FBuildRequestOptions Options;
			Options.Policy.ForceBuild = Round < 2;
			Options.Policy.BuildLocal = Round < 2;
			const auto Begin = std::chrono::steady_clock::now();
			const auto Completion = Service.Execute(std::move(*Request), Options);
			ASSERT_EQ(Completion.GetStatus(), DerivedData::EStatus::Ok) << Completion.GetDiagnostic();
			ASSERT_TRUE(Completion.GetOutput());
			const auto* Library = Completion.GetOutput()->FindValue(DerivedData::FValueId::FromName("Library"));
			ASSERT_TRUE(Library);
			const auto Data = Library->GetData();
			ASSERT_TRUE(ShaderFormat::ValidateMetalLibrary(Data.GetBytes()));
			if (Round) EXPECT_TRUE(std::ranges::equal(Previous.GetBytes(), Data.GetBytes()));
			Previous = Data;
			if (Round == 2) EXPECT_TRUE(DerivedData::HasBuildExecutionFlag(Completion.GetExecutionFlags(), DerivedData::EBuildExecutionFlags::CacheQueryHit));
			std::cout << "metal_library round=" << Round << " bytes=" << Data.size()
				<< " elapsed_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Begin).count()
				<< " producer=" << *Producer << std::endl;
		}
		std::vector<std::filesystem::path> NativeRecords;
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root / "DDC" / "records" / "v4" / "Shader"))
			if (Entry.is_regular_file() && Entry.path().extension() == ".bin") NativeRecords.push_back(Entry.path());
		ASSERT_EQ(NativeRecords.size(), 1u);
		{ std::ofstream Corrupt(NativeRecords.front(), std::ios::binary | std::ios::trunc); Corrupt << "corrupt"; ASSERT_TRUE(Corrupt.good()); }
		auto RepairRequest = MakeMetalLibraryBuildRequest(Shader, *Producer);
		ASSERT_TRUE(RepairRequest);
		const auto Repaired = Service.Execute(std::move(*RepairRequest), {});
		ASSERT_EQ(Repaired.GetStatus(), DerivedData::EStatus::Ok) << Repaired.GetDiagnostic();
		ASSERT_TRUE(Repaired.GetOutput());
		const auto* RepairedValue = Repaired.GetOutput()->FindValue(DerivedData::FValueId::FromName("Library"));
		ASSERT_TRUE(RepairedValue);
		EXPECT_TRUE(std::ranges::equal(Previous.GetBytes(), RepairedValue->GetData().GetBytes()));

		auto InvalidSource = Shader;
		const std::string InvalidText = std::string(Source) + "\n#error native_failure_fixture\n";
		InvalidSource.Code = FSharedByteBuffer::Copy(std::as_bytes(std::span(InvalidText)));
		InvalidSource.CodeHash = FXxHash128::HashBuffer(InvalidSource.Code);
		auto InvalidRequest = MakeMetalLibraryBuildRequest(InvalidSource, *Producer);
		ASSERT_TRUE(InvalidRequest);
		const auto Invalid = Service.Execute(std::move(*InvalidRequest), {});
		EXPECT_EQ(Invalid.GetStatus(), DerivedData::EStatus::Error);
		EXPECT_EQ(Invalid.GetOutput(), nullptr);
		std::filesystem::create_directories(Root / "Source");
		FShaderPaths::RegisterMountPoint("/MetalLibraryQualification/", (Root / "Source").generic_string(), (Root / "Manifest").generic_string());
		FShaderBuilder Builder;
		FGeneratedShaderCompileRequest Request;
		Request.Target = MetalLibraryShaderTarget;
		Request.VirtualPath = "/Generated/MetalLibraryQualification";
		Request.Source = "[shader(\"vertex\")] float4 VertexMain(uint id : SV_VertexID) : SV_Position { return float4(float(id), 0, 0, 1); }";
		Request.EntryPoints = {"VertexMain"};
		Request.Frequencies = {EShaderFrequency::Vertex};
		Request.MaximumOutputBytes = 8ull * 1024 * 1024;
		const auto Built = Builder.GetOrCompileGenerated(Request);
		ASSERT_TRUE(Built) << FormatShaderCompilerErrors(Built.Errors);
		ASSERT_EQ(Built.CompiledShaders.size(), 1);
		EXPECT_EQ(Built.CompiledShaders[0].Target, MetalLibraryShaderTarget);
		EXPECT_TRUE(ShaderFormat::ValidateShaderCode(Built.CompiledShaders[0]));
		{
			const char* CurrentDeveloper = std::getenv("DEVELOPER_DIR");
			struct FRestoreDeveloper
			{
				std::optional<std::string> Previous;
				~FRestoreDeveloper() { if (Previous) setenv("DEVELOPER_DIR", Previous->c_str(), 1); else unsetenv("DEVELOPER_DIR"); }
			} Restore{CurrentDeveloper ? std::optional<std::string>(CurrentDeveloper) : std::nullopt};
			setenv("DEVELOPER_DIR", (Root / "UnavailableDeveloper").c_str(), 1);
			EXPECT_FALSE(GetMetalLibraryProducerIdentity());
			const auto Unavailable = Builder.GetOrCompileGenerated(Request);
			EXPECT_FALSE(Unavailable); EXPECT_TRUE(Unavailable.CompiledShaders.empty());
			FShaderBuildRequestOptions CacheOptions{.bInlineExecution = true, .NativeProducerIdentity = *Producer, .bNativeCacheOnly = true};
			std::optional<FShaderCompilerOutput> Replay;
			auto Replayed = Builder.CompileGenerated(Request, CacheOptions, [&](auto Result) { Replay = std::move(Result); });

			EXPECT_EQ(Replayed.Wait(), EShaderBuildWaitResult::Completed);
			ASSERT_TRUE(Replay); EXPECT_TRUE(*Replay) << FormatShaderCompilerErrors(Replay->Errors);
			CacheOptions.NativeProducerIdentity += ";not-installed";
			std::optional<FShaderCompilerOutput> Miss;
			auto Missing = Builder.CompileGenerated(Request, CacheOptions, [&](auto Result) { Miss = std::move(Result); });

			EXPECT_EQ(Missing.Wait(), EShaderBuildWaitResult::Completed);
			ASSERT_TRUE(Miss); EXPECT_FALSE(*Miss); EXPECT_TRUE(Miss->CompiledShaders.empty());
		}
		auto SmallRequest = Request;
		SmallRequest.MaximumOutputBytes = 2048;
		const auto TooSmall = Builder.GetOrCompileGenerated(SmallRequest);
		EXPECT_FALSE(TooSmall);
		EXPECT_TRUE(TooSmall.CompiledShaders.empty());
		const auto Reused = Builder.GetOrCompileGenerated(Request);
		EXPECT_TRUE(Reused) << FormatShaderCompilerErrors(Reused.Errors);
		ASSERT_FALSE(IsTaskSchedulerRunning());
		ASSERT_TRUE(InitializeTaskScheduler(1));
		struct FStop { ~FStop() { ShutdownTaskScheduler(); } } Stop;
		{
			FShaderBuilder AsyncBuilder;
			std::atomic<uint32> Canceled = 0, Completed = 0;
			auto First = AsyncBuilder.CompileGenerated(Request, {}, [&](auto Result) {
				EXPECT_EQ(Result.Status, EShaderBuildStatus::Canceled); ++Canceled;
			});

			First.RequestCancel();
			auto Second = AsyncBuilder.CompileGenerated(Request, {}, [&](auto Result) {
				EXPECT_TRUE(Result) << FormatShaderCompilerErrors(Result.Errors);
				if (Result) EXPECT_EQ(Result.CompiledShaders.front().Target, MetalLibraryShaderTarget);
				++Completed;
			});

			EXPECT_EQ(First.Wait(), EShaderBuildWaitResult::Completed);
			EXPECT_EQ(Second.Wait(), EShaderBuildWaitResult::Completed);
			EXPECT_EQ(Canceled, 1u); EXPECT_EQ(Completed, 1u);
		}
	}
	#endif

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
					ASSERT_TRUE(Outputs[Index]) << FormatShaderCompilerErrors(Outputs[Index].Errors);
					ASSERT_EQ(Outputs[Index].CompiledShaders.size(), 1u);
					const auto& Shader = Outputs[Index].CompiledShaders[0];
					ASSERT_TRUE(!Shader.Code.IsEmpty());
					Bytes += Shader.Code.size();
					if (Mode) EXPECT_EQ(Shader.CodeHash, Cold[Index].CompiledShaders[0].CodeHash);
				}
				const auto After = Builder->GetStats();
				EXPECT_EQ(After.Compilations - Before.Compilations, Mode == 0 ? BatchSize : 0u);
				EXPECT_EQ(After.DdcHits - Before.DdcHits, Mode == 1 ? BatchSize : 0u);
				EXPECT_EQ(After.MemoryHits - Before.MemoryHits, Mode == 2 ? BatchSize : 0u);
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
