#include "ShaderBuild/ShaderPaths.h"
#include "Shader/ShaderCompilerCore.h"
#include "Shader/IShaderBuildModule.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Paths.h"
#include "ShaderBuilder.h"
#include "ShaderSharedOutput.h"
#include "Hash/CanonicalHash.h"
#include "ShaderCompileUtilities.h"
#include "SlangShaderDependencyResolver.h"
#include "NativeTestSupport.h"

#include "gtest/gtest.h"

#include <fstream>
#include <iostream>
#include <latch>

namespace Durin
{
	namespace
	{
		auto GetBuilderTestRoot() -> std::filesystem::path
		{
			return Durin::Testing::GetTestWorkDirectory() / "ShaderBuilder";
		}

		auto WriteTestShader(const std::filesystem::path& FilePath) -> void
		{
			std::filesystem::create_directories(FilePath.parent_path());
			std::ofstream Stream(FilePath, std::ios::binary | std::ios::trunc);
			Stream << R"([shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    return float4(positions[vertexID], 0.0, 1.0);
}
)";
			ASSERT_TRUE(Stream.good());
		}

		auto WriteTextFile(const std::filesystem::path& FilePath, std::string_view Text) -> void
		{
			std::filesystem::create_directories(FilePath.parent_path());
			std::ofstream Stream(FilePath, std::ios::binary | std::ios::trunc);
			Stream << Text;
			ASSERT_TRUE(Stream.good());
		}

		auto MakeCompileOptions() -> FShaderCompileOptions
		{
			FShaderCompileOptions Options;
			Options.EntryPoints = {"VertexMain"};
			Options.Frequencies = {EShaderFrequency::Vertex};
			return Options;
		}

		class FShaderBuilderTests : public testing::Test
		{
		protected:
			void SetUp() override
			{
				Builder.reset();
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
				const std::filesystem::path Root = GetBuilderTestRoot();
				PreviousDdcRoot = FPaths::DerivedDataCacheDir();
				std::error_code ErrorCode;
				Durin::Testing::RemoveTestWorkDirectory(Root, ErrorCode);
				ASSERT_FALSE(ErrorCode);
				std::filesystem::create_directories(Root / "Source");
				std::filesystem::create_directories(Root / "Cache");
				FPaths::SetDerivedDataCacheDirForTests(
					(Root / "DDC").generic_string());
				FShaderPaths::RegisterMountPoint(
					"/ShaderBuilderTests/",
					(Root / "Source").generic_string(),
					(Root / "Cache").generic_string()
				);
				WriteTestShader(Root / "Source" / "Simple.slang");
			}

			void TearDown() override
			{
				Builder.reset();
				FPaths::SetDerivedDataCacheDirForTests(PreviousDdcRoot);
			}

			std::unique_ptr<FShaderBuilder> Builder;
			std::string PreviousDdcRoot;
		};
	}

	TEST_F(FShaderBuilderTests, WarmRestartUsesManifestWithoutResolvingOrHashingSources)
	{
		const FShaderCompileOptions Options = MakeCompileOptions();
		Builder = std::make_unique<FShaderBuilder>();
		ASSERT_TRUE(Builder->GetOrCompile("/ShaderBuilderTests/Simple", Options));
		const FShaderBuildStats ColdStats = Builder->GetStats();
		EXPECT_EQ(ColdStats.DependencyResolutions, 1u);
		EXPECT_EQ(ColdStats.Compilations, 1u);
		EXPECT_GT(ColdStats.ContentReads, 0u);

		Builder.reset();
		Builder = std::make_unique<FShaderBuilder>();
		ASSERT_TRUE(Builder->GetOrCompile("/ShaderBuilderTests/Simple", Options));
		const FShaderBuildStats WarmStats = Builder->GetStats();
		EXPECT_EQ(WarmStats.DependencyResolutions, 0u);
		EXPECT_EQ(WarmStats.Compilations, 0u);
		EXPECT_EQ(WarmStats.ContentReads, 0u);
		EXPECT_EQ(WarmStats.ManifestHits, 1u);
		EXPECT_EQ(WarmStats.DdcHits, 1u);
	}

	TEST_F(FShaderBuilderTests, CorruptDdcValueIsRecompiledAndRepaired)
	{
		const FShaderCompileOptions Options = MakeCompileOptions();
		Builder = std::make_unique<FShaderBuilder>();
		ASSERT_TRUE(Builder->GetOrCompile(
			"/ShaderBuilderTests/Simple", Options));
		Builder.reset();

		const std::filesystem::path Bucket = GetBuilderTestRoot()
			/ "DDC" / "Shaders" / "CompiledOutput";
		std::vector<std::filesystem::path> Entries;
		std::error_code Error;
		for (std::filesystem::recursive_directory_iterator It(Bucket, Error), End;
			!Error && It != End; It.increment(Error))
		{
			if (It->is_regular_file() && It->path().extension() == ".bin")
				Entries.push_back(It->path());
		}
		ASSERT_FALSE(Error) << Error.message();
		ASSERT_EQ(Entries.size(), 1u);
		{
			std::ofstream Stream(Entries.front(),
				std::ios::binary | std::ios::trunc);
			Stream << "corrupt";
			ASSERT_TRUE(Stream.good());
		}

		Builder = std::make_unique<FShaderBuilder>();
		const FShaderCompilerOutput Repaired = Builder->GetOrCompile(
			"/ShaderBuilderTests/Simple", Options);
		ASSERT_TRUE(Repaired) << FormatShaderError(Repaired.Error);
		const FShaderBuildStats Stats =
			Builder->GetStats();
		EXPECT_EQ(Stats.DdcHits, 0u);
		EXPECT_EQ(Stats.DdcCorruptMisses, 1u);
		EXPECT_EQ(Stats.Compilations, 1u);
		EXPECT_EQ(Stats.ManifestHits, 1u);
	}

	TEST_F(FShaderBuilderTests, DdcStoreFailureDoesNotDiscardCompiledOutput)
	{
		const std::filesystem::path BlockedRoot =
			GetBuilderTestRoot() / "BlockedDdc";
		{
			std::ofstream Stream(BlockedRoot, std::ios::binary);
			Stream << "not a directory";
			ASSERT_TRUE(Stream.good());
		}
		FPaths::SetDerivedDataCacheDirForTests(BlockedRoot.generic_string());
		Builder = std::make_unique<FShaderBuilder>();
		const FShaderCompilerOutput Output = Builder->GetOrCompile(
			"/ShaderBuilderTests/Simple", MakeCompileOptions());
		ASSERT_TRUE(Output) << FormatShaderError(Output.Error);
		const FShaderBuildStats Stats =
			Builder->GetStats();
		EXPECT_EQ(Stats.Compilations, 1u);
		EXPECT_EQ(Stats.DdcStoreFailures, 1u);
		EXPECT_EQ(Stats.OutputEntries, 1u);
	}

	TEST_F(FShaderBuilderTests,
		SourceTreeFingerprintReusesManifestAndInvalidatesOnChange)
	{
		const FShaderCompileOptions Options = MakeCompileOptions();
		Builder = std::make_unique<FShaderBuilder>();
		FShaderSourceDependencyFingerprint ColdFingerprint;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = Builder->BuildSourceTreeFingerprint("/ShaderBuilderTests/Simple", Options, ColdFingerprint))) << FormatShaderError(Error.error());
		EXPECT_EQ(ColdFingerprint.VirtualPath,
			"/ShaderBuilderTests/Simple");
		EXPECT_FALSE(ColdFingerprint.ContentHash.IsZero());
		EXPECT_EQ(Builder->GetStats().DependencyResolutions, 1u);
		FShaderSourceDependencyFingerprint SameGenerationFingerprint;
		ASSERT_TRUE((Error = Builder->BuildSourceTreeFingerprint("/ShaderBuilderTests/Simple", Options, SameGenerationFingerprint))) << FormatShaderError(Error.error());
		EXPECT_EQ(SameGenerationFingerprint, ColdFingerprint);
		const auto SameGenerationStats = Builder->GetStats();
		EXPECT_EQ(SameGenerationStats.DependencyResolutions, 1u);
		EXPECT_EQ(SameGenerationStats.ManifestHits, 0u);
		EXPECT_EQ(SameGenerationStats.SourceTreeFingerprintHits, 1u);

		Builder.reset();
		Builder = std::make_unique<FShaderBuilder>();
		FShaderSourceDependencyFingerprint WarmFingerprint;
		ASSERT_TRUE((Error = Builder->BuildSourceTreeFingerprint("/ShaderBuilderTests/Simple", Options, WarmFingerprint))) << FormatShaderError(Error.error());
		EXPECT_EQ(WarmFingerprint, ColdFingerprint);
		const auto WarmStats = Builder->GetStats();
		EXPECT_EQ(WarmStats.DependencyResolutions, 0u);
		EXPECT_EQ(WarmStats.ManifestHits, 1u);
		EXPECT_EQ(WarmStats.ContentReads, 0u);
		EXPECT_EQ(WarmStats.SourceTreeFingerprintHits, 0u);

		WriteTextFile(GetBuilderTestRoot() / "Source/Simple.slang",
			R"([shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    return float4(float(vertexID), 1.0, 2.0, 1.0);
}
)");
		FShaderSourceDependencyFingerprint BeforeReloadFingerprint;
		ASSERT_TRUE((Error = Builder->BuildSourceTreeFingerprint("/ShaderBuilderTests/Simple", Options, BeforeReloadFingerprint))) << FormatShaderError(Error.error());
		EXPECT_EQ(BeforeReloadFingerprint, WarmFingerprint);
		EXPECT_EQ(Builder->GetStats().DependencyResolutions, 0u);
		EXPECT_EQ(
			Builder->GetStats().SourceTreeFingerprintHits, 1u);
		const uint64 PreviousReloadGeneration = GetShaderReloadGeneration();
		EXPECT_GT(AdvanceShaderReloadGeneration(), PreviousReloadGeneration);
		FShaderSourceDependencyFingerprint ChangedFingerprint;
		ASSERT_TRUE((Error = Builder->BuildSourceTreeFingerprint("/ShaderBuilderTests/Simple", Options, ChangedFingerprint))) << FormatShaderError(Error.error());
		EXPECT_NE(ChangedFingerprint.ContentHash,
			ColdFingerprint.ContentHash);
		EXPECT_EQ(Builder->GetStats().DependencyResolutions, 1u);
	}

	TEST_F(FShaderBuilderTests,
		GeneratedSourceCompilesFromMemoryUsesCacheAndEnforcesImports)
	{
		WriteTextFile(GetBuilderTestRoot() / "Source/Imported.slang",
			"module Imported; public float GeneratedValue() { return 0.25; }\n");
		Builder = std::make_unique<FShaderBuilder>();
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath =
			"/Generated/Materials/00112233445566778899aabbccddeeff";
		Request.Source = R"(module GeneratedMaterialTest;
import Imported;
[shader("fragment")]
float4 FragmentMain() : SV_Target0
{
    return float4(GeneratedValue(), 0.5, 0.75, 1.0);
}
)";
		Request.EntryPoints = {"FragmentMain"};
		Request.Frequencies = {EShaderFrequency::Fragment};
		Request.AllowedImportVirtualPrefixes = {
			"/ShaderBuilderTests/"};
		const FShaderCompilerOutput First =
			Builder->GetOrCompileGenerated(Request);
		ASSERT_TRUE(First) << FormatShaderError(First.Error);
		ASSERT_EQ(First.CompiledShaders.size(), 1u);
		const auto ColdStats = Builder->GetStats();
		EXPECT_EQ(ColdStats.Compilations, 1u);
		const FShaderCompilerOutput Warm =
			Builder->GetOrCompileGenerated(Request);
		ASSERT_TRUE(Warm) << FormatShaderError(Warm.Error);
		const auto WarmStats = Builder->GetStats();
		EXPECT_EQ(WarmStats.Compilations, ColdStats.Compilations);
		EXPECT_EQ(WarmStats.MemoryHits, ColdStats.MemoryHits + 1u);

		Builder.reset();
		Builder = std::make_unique<FShaderBuilder>();
		const FShaderCompilerOutput RestartWarm =
			Builder->GetOrCompileGenerated(Request);
		ASSERT_TRUE(RestartWarm) << FormatShaderError(RestartWarm.Error);
		const auto RestartWarmStats = Builder->GetStats();
		EXPECT_EQ(RestartWarmStats.DependencyResolutions, 0u);
		EXPECT_EQ(RestartWarmStats.ManifestHits, 1u);
		EXPECT_EQ(RestartWarmStats.DdcHits, 1u);
		EXPECT_EQ(RestartWarmStats.Compilations, 0u);
		EXPECT_EQ(RestartWarmStats.ContentReads, 0u);

		Request.AllowedImportVirtualPrefixes.clear();
		const FShaderCompilerOutput Rejected =
			Builder->GetOrCompileGenerated(Request);
		EXPECT_FALSE(Rejected);
		EXPECT_EQ(Rejected.Error.Code, EShaderError::ImportNotAllowed);
	}

	TEST_F(FShaderBuilderTests,
		GeneratedManifestInvalidatesWhenImportedModuleChanges)
	{
		const std::filesystem::path ImportedPath =
			GetBuilderTestRoot() / "Source/Imported.slang";
		WriteTextFile(ImportedPath,
			"module Imported; public float GeneratedValue() { return 0.25; }\n");
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath =
			"/Generated/Materials/ffeeddccbbaa99887766554433221100";
		Request.Source = R"(module GeneratedMaterialInvalidationTest;
import Imported;
[shader("fragment")]
float4 FragmentMain() : SV_Target0
{
    return float4(GeneratedValue(), 0.5, 0.75, 1.0);
}
)";
		Request.EntryPoints = {"FragmentMain"};
		Request.Frequencies = {EShaderFrequency::Fragment};
		Request.AllowedImportVirtualPrefixes = {
			"/ShaderBuilderTests/"};

		Builder = std::make_unique<FShaderBuilder>();
		const FShaderCompilerOutput First =
			Builder->GetOrCompileGenerated(Request);
		ASSERT_TRUE(First) << FormatShaderError(First.Error);
		ASSERT_EQ(First.CompiledShaders.size(), 1u);
		const FXxHash128 FirstHash = First.CompiledShaders.front().Hash;
		Builder.reset();

		WriteTextFile(ImportedPath,
			"module Imported; public float GeneratedValue() { return 0.875; }\n");
		std::filesystem::last_write_time(
			ImportedPath,
			std::filesystem::last_write_time(ImportedPath)
				+ std::chrono::seconds(2));
		Builder = std::make_unique<FShaderBuilder>();
		const FShaderCompilerOutput Changed =
			Builder->GetOrCompileGenerated(Request);
		ASSERT_TRUE(Changed) << FormatShaderError(Changed.Error);
		ASSERT_EQ(Changed.CompiledShaders.size(), 1u);
		EXPECT_NE(Changed.CompiledShaders.front().Hash, FirstHash);
		const auto Stats = Builder->GetStats();
		EXPECT_EQ(Stats.DependencyResolutions, 1u);
		EXPECT_EQ(Stats.ManifestHits, 0u);
		EXPECT_EQ(Stats.DdcHits, 0u);
		EXPECT_EQ(Stats.Compilations, 1u);
	}

	TEST_F(FShaderBuilderTests,
		GeneratedShaderWithoutImportsReusesEmptyManifestAfterRestart)
	{
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath =
			"/Generated/Materials/1234567890abcdef1234567890abcdef";
		Request.Source = R"(module GeneratedMaterialWithoutImports;
[shader("fragment")]
float4 FragmentMain() : SV_Target0
{
    return float4(0.25, 0.5, 0.75, 1.0);
}
)";
		Request.EntryPoints = {"FragmentMain"};
		Request.Frequencies = {EShaderFrequency::Fragment};

		Builder = std::make_unique<FShaderBuilder>();
		const FShaderCompilerOutput First =
			Builder->GetOrCompileGenerated(Request);
		ASSERT_TRUE(First) << FormatShaderError(First.Error);
		Builder.reset();

		Builder = std::make_unique<FShaderBuilder>();
		const FShaderCompilerOutput Warm =
			Builder->GetOrCompileGenerated(Request);
		ASSERT_TRUE(Warm) << FormatShaderError(Warm.Error);
		const auto Stats = Builder->GetStats();
		EXPECT_EQ(Stats.DependencyResolutions, 0u);
		EXPECT_EQ(Stats.ManifestHits, 1u);
		EXPECT_EQ(Stats.DdcHits, 1u);
		EXPECT_EQ(Stats.Compilations, 0u);
	}

	// The gate is inside CompileSource, after acquiring its global session.
	// Deadlines only prevent a regression from hanging the test process; no
	// throughput/timing threshold is used as a performance acceptance gate.
	TEST_F(FShaderBuilderTests, WarmGeneratedCachesBypassBlockedCompiler)
	{
		for (const bool Restart : {false, true})
		{
			Builder.reset();
			FGeneratedShaderCompileRequest Warm;
			Warm.VirtualPath = "/Generated/Materials/Warm";
			Warm.Source = "[shader(\"fragment\")] float4 FragmentMain() : SV_Target0 { return 1.0; }";
			Warm.EntryPoints = {"FragmentMain"};
			Warm.Frequencies = {EShaderFrequency::Fragment};
			auto Cold = Warm;
			Cold.VirtualPath = "/Generated/Materials/Blocked";
			Cold.bForceRecompile = true;
			std::promise<void> Entered, Release;
			auto EnteredFuture = Entered.get_future();
			auto ReleaseFuture = Release.get_future().share();
			auto Hook = [&](std::string_view Path) {
				if (Path != Cold.VirtualPath) return;
				Entered.set_value();
				if (ReleaseFuture.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
					throw std::runtime_error("Compile gate was not released");
			};
			Builder = std::make_unique<FShaderBuilder>(Hook);
			ASSERT_TRUE(Builder->GetOrCompileGenerated(Warm));
			if (Restart)
			{
				Builder.reset();
				Builder = std::make_unique<FShaderBuilder>(Hook);
			}
			const auto Before = Builder->GetStats();
			auto Slow = std::async(std::launch::async, [&] { return Builder->GetOrCompileGenerated(Cold); });
			const auto EnteredStatus = EnteredFuture.wait_for(std::chrono::seconds(10));
			auto Hit = std::async(std::launch::async, [&] { return Builder->GetOrCompileGenerated(Warm); });
			const auto HitStatus = Hit.wait_for(std::chrono::seconds(10));
			Release.set_value();
			EXPECT_EQ(EnteredStatus, std::future_status::ready);
			EXPECT_EQ(HitStatus, std::future_status::ready);
			EXPECT_TRUE(Hit.get());
			EXPECT_TRUE(Slow.get());
			const auto After = Builder->GetStats();
			EXPECT_EQ(Restart ? After.DdcHits - Before.DdcHits : After.MemoryHits - Before.MemoryHits, 1u);
			Builder.reset();
		}
	}

	TEST_F(FShaderBuilderTests, DifferentGeneratedRequestsUseIndependentSlangSessions)
	{
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath = "/Generated/Materials/BlockedCold";
		Request.Source = "[shader(\"fragment\")] float4 FragmentMain() : SV_Target0 { return 1.0; }";
		Request.EntryPoints = {"FragmentMain"};
		Request.Frequencies = {EShaderFrequency::Fragment};
		std::promise<void> Entered, Release;
		auto EnteredFuture = Entered.get_future();
		auto ReleaseFuture = Release.get_future().share();
		Builder = std::make_unique<FShaderBuilder>([&](std::string_view Path) {
			if (Path != Request.VirtualPath) return;
			Entered.set_value();
			if (ReleaseFuture.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
				throw std::runtime_error("Compile gate was not released");
		});
		auto First = std::async(std::launch::async, [&] { return Builder->GetOrCompileGenerated(Request); });
		const auto EnteredStatus = EnteredFuture.wait_for(std::chrono::seconds(10));
		auto SecondRequest = Request;
		SecondRequest.VirtualPath = "/Generated/Materials/IndependentCold";
		SecondRequest.Source += "\n// distinct dependency identity";
		auto Second = std::async(std::launch::async, [&] { return Builder->GetOrCompileGenerated(SecondRequest); });
		const auto SecondStatus = Second.wait_for(std::chrono::seconds(10));
		Release.set_value();
		EXPECT_EQ(EnteredStatus, std::future_status::ready);
		EXPECT_EQ(SecondStatus, std::future_status::ready);
		EXPECT_TRUE(First.get());
		EXPECT_TRUE(Second.get());
		EXPECT_EQ(Builder->GetStats().Compilations, 2u);
	}

	TEST_F(FShaderBuilderTests, ConcurrentGeneratedRequestsCompileOnceAndExceptionsAllowRetry)
	{
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath = "/Generated/Materials/SingleFlight";
		Request.Source = "[shader(\"fragment\")] float4 FragmentMain() : SV_Target0 { return 1.0; }";
		Request.EntryPoints = {"FragmentMain"};
		Request.Frequencies = {EShaderFrequency::Fragment};
		std::atomic_bool Throw = true;
		Builder = std::make_unique<FShaderBuilder>([&](std::string_view) {
			if (Throw.load()) throw std::runtime_error("Injected compiler failure");
		});
		std::latch Start(1);
		std::vector<std::future<FShaderCompilerOutput>> Requests;
		for (int Index = 0; Index < 8; ++Index)
			Requests.push_back(std::async(std::launch::async, [&] {
				Start.wait();
				return Builder->GetOrCompileGenerated(Request);
			}));
		Start.count_down();
		for (auto& Future : Requests)
		{
			const auto Failed = Future.get();
			EXPECT_FALSE(Failed);
			EXPECT_EQ(Failed.Error.Code, EShaderError::InvalidCompileRequest);
			EXPECT_TRUE(Failed.CompiledShaders.empty());
		}
		Throw = false;
		const auto Before = Builder->GetStats();
		Requests.clear();
		for (int Index = 0; Index < 8; ++Index)
			Requests.push_back(std::async(std::launch::async, [&] { return Builder->GetOrCompileGenerated(Request); }));
		for (auto& Future : Requests) EXPECT_TRUE(Future.get());
		EXPECT_EQ(Builder->GetStats().Compilations - Before.Compilations, 1u);
	}

	TEST_F(FShaderBuilderTests, ConcurrentCapturedRequestsKeepContentAndImportPolicySeparate)
	{
		auto Artifacts = [](std::string_view Text) {
			const auto View = std::as_bytes(std::span(Text.data(), Text.size()));
			return std::make_shared<FShaderSourceArtifacts>(
				std::map<std::string, FByteBuffer>{{"/Captured/Included.slang", FByteBuffer(View.begin(), View.end())}},
				std::vector<std::string>{"/Captured/"});
		};
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath = "/Generated/Materials/CapturedConcurrent";
		Request.Source = "import Included; [shader(\"fragment\")] float4 FragmentMain() : SV_Target0 { return Value(); }";
		Request.EntryPoints = {"FragmentMain"};
		Request.Frequencies = {EShaderFrequency::Fragment};
		Request.AllowedImportVirtualPrefixes = {"/Captured/"};
		Request.SourceArtifacts = Artifacts("public float4 Value() { return float4(1, 0, 0, 1); }");
		std::promise<void> Entered, Release;
		auto EnteredFuture = Entered.get_future();
		auto ReleaseFuture = Release.get_future().share();
		std::atomic_uint32_t Calls = 0;
		Builder = std::make_unique<FShaderBuilder>([&](std::string_view) {
			if (Calls.fetch_add(1) != 0) return;
			Entered.set_value();
			if (ReleaseFuture.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
				throw std::runtime_error("Compile gate was not released");
		});
		auto First = std::async(std::launch::async, [&] { return Builder->GetOrCompileGenerated(Request); });
		const auto EnteredStatus = EnteredFuture.wait_for(std::chrono::seconds(10));
		auto Changed = Request;
		Changed.SourceArtifacts = Artifacts("public float4 Value() { return float4(0, 1, 0, 1); }");
		auto Second = std::async(std::launch::async, [&] { return Builder->GetOrCompileGenerated(Changed); });
		auto Denied = Request;
		Denied.AllowedImportVirtualPrefixes = {"/Other/"};
		auto Invalid = std::async(std::launch::async, [&] { return Builder->GetOrCompileGenerated(Denied); });
		const auto SecondStatus = Second.wait_for(std::chrono::seconds(10));
		const auto InvalidStatus = Invalid.wait_for(std::chrono::seconds(10));
		Release.set_value();
		EXPECT_EQ(EnteredStatus, std::future_status::ready);
		EXPECT_EQ(SecondStatus, std::future_status::ready);
		EXPECT_EQ(InvalidStatus, std::future_status::ready);
		const auto FirstOutput = First.get();
		const auto SecondOutput = Second.get();
		EXPECT_FALSE(Invalid.get());
		ASSERT_TRUE(FirstOutput) << FormatShaderError(FirstOutput.Error);
		ASSERT_TRUE(SecondOutput) << FormatShaderError(SecondOutput.Error);
		ASSERT_EQ(FirstOutput.CompiledShaders.size(), 1u);
		ASSERT_EQ(SecondOutput.CompiledShaders.size(), 1u);
		EXPECT_NE(FirstOutput.CompiledShaders[0].Hash, SecondOutput.CompiledShaders[0].Hash);
		EXPECT_EQ(Calls.load(), 2u);
	}

	TEST_F(FShaderBuilderTests, ConcurrentIdenticalRequestsCompileOnce)
	{
		const FShaderCompileOptions Options = MakeCompileOptions();
		Builder = std::make_unique<FShaderBuilder>();
		std::vector<std::future<FShaderCompilerOutput>> Requests;
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			Requests.push_back(std::async(std::launch::async, [this, Options] {
				return Builder->GetOrCompile("/ShaderBuilderTests/Simple", Options);
			}));
		}

		std::vector<FShaderCompilerOutput> Outputs;
		for (auto& Request : Requests)
		{
			Outputs.push_back(Request.get()); ASSERT_TRUE(Outputs.back());
		}
		auto Hit = Builder->GetOrCompile("/ShaderBuilderTests/Simple", Options);
		ASSERT_TRUE(Hit);
		for (const auto& Output : Outputs)
			EXPECT_EQ(Output.CompiledShaders[0].Code->data(), Hit.CompiledShaders[0].Code->data());
		const FShaderBuildStats Stats = Builder->GetStats();
		EXPECT_EQ(Stats.DependencyResolutions, 1u);
		EXPECT_EQ(Stats.Compilations, 1u);
		Builder.reset();
		for (const auto& Output : Outputs)
			EXPECT_EQ(FXxHash128::HashBuffer(*Output.CompiledShaders[0].Code), Hit.CompiledShaders[0].Hash);
	}

	TEST_F(FShaderBuilderTests,
		ConcurrentColdFileAndGeneratedRequestsCompleteSafely)
	{
		constexpr uint32 FileRequestCount = 6;
		const std::filesystem::path SourceRoot =
			GetBuilderTestRoot() / "Source";
		for (uint32 Index = 0; Index < FileRequestCount; ++Index)
		{
			WriteTestShader(SourceRoot /
				std::format("Concurrent{}.slang", Index));
		}
		WriteTextFile(SourceRoot / "ConcurrentImported.slang",
			"module ConcurrentImported; "
			"public float ConcurrentValue() { return 0.25; }\n");

		FGeneratedShaderCompileRequest GeneratedRequest;
		GeneratedRequest.VirtualPath =
			"/Generated/Materials/abcdefabcdefabcdefabcdefabcdefab";
		GeneratedRequest.Source = R"(module ConcurrentGeneratedMaterial;
import ConcurrentImported;
[shader("fragment")]
float4 FragmentMain() : SV_Target0
{
    return float4(ConcurrentValue(), 0.5, 0.75, 1.0);
}
)";
		GeneratedRequest.EntryPoints = {"FragmentMain"};
		GeneratedRequest.Frequencies = {EShaderFrequency::Fragment};
		GeneratedRequest.AllowedImportVirtualPrefixes = {
			"/ShaderBuilderTests/"};

		Builder = std::make_unique<FShaderBuilder>();
		std::latch StartGate(1);
		std::vector<std::future<FShaderCompilerOutput>> Requests;
		Requests.reserve(FileRequestCount + 1);
		for (uint32 Index = 0; Index < FileRequestCount; ++Index)
		{
			Requests.push_back(std::async(std::launch::async,
				[this, &StartGate, Index] {
					StartGate.wait();
					return Builder->GetOrCompile(std::format(
						"/ShaderBuilderTests/Concurrent{}", Index),
						MakeCompileOptions());
				}));
		}
		Requests.push_back(std::async(std::launch::async,
			[this, &StartGate, GeneratedRequest] {
				StartGate.wait();
				return Builder->GetOrCompileGenerated(GeneratedRequest);
			}));
		StartGate.count_down();

		for (auto& Request : Requests)
		{
			const FShaderCompilerOutput Output = Request.get();
			EXPECT_TRUE(Output) << FormatShaderError(Output.Error);
		}
		const FShaderBuildStats Stats =
			Builder->GetStats();
		EXPECT_EQ(Stats.DependencyResolutions, FileRequestCount + 1u);
		EXPECT_EQ(Stats.Compilations, FileRequestCount + 1u);
	}

	TEST_F(FShaderBuilderTests,
		CorrectedShaderRecompilesAfterAnUncachedFailure)
	{
		const std::filesystem::path ShaderPath =
			GetBuilderTestRoot() / "Source" / "Simple.slang";
		WriteTextFile(
			ShaderPath,
			"[shader(\"vertex\")] broken shader source\n");
		Builder = std::make_unique<FShaderBuilder>();

		const FShaderCompileOptions Options = MakeCompileOptions();
		const FShaderCompilerOutput Broken =
			Builder->GetOrCompile("/ShaderBuilderTests/Simple", Options);
		EXPECT_FALSE(Broken);
		EXPECT_EQ(Broken.Error.Code, EShaderError::SlangFailure);

		WriteTestShader(ShaderPath);
		std::filesystem::last_write_time(
			ShaderPath,
			std::filesystem::last_write_time(ShaderPath)
				+ std::chrono::seconds(2));
		const FShaderCompilerOutput Corrected =
			Builder->GetOrCompile("/ShaderBuilderTests/Simple", Options);
		EXPECT_TRUE(Corrected);
		const FShaderBuildStats Stats =
			Builder->GetStats();
		EXPECT_EQ(Stats.DependencyResolutions, 2u);
		EXPECT_EQ(Stats.Compilations, 1u);
	}

	TEST_F(FShaderBuilderTests,
		ForceRecompileBypassesSuccessfulMemoryAndDiskOutputReuse)
	{
		Builder = std::make_unique<FShaderBuilder>();
		const FShaderCompileOptions Options = MakeCompileOptions();
		ASSERT_TRUE(
			Builder->GetOrCompile("/ShaderBuilderTests/Simple", Options));
		ASSERT_TRUE(
			Builder->GetOrCompile("/ShaderBuilderTests/Simple", Options));
		const FShaderBuildStats WarmStats =
			Builder->GetStats();
		ASSERT_EQ(WarmStats.Compilations, 1u);
		ASSERT_EQ(WarmStats.MemoryHits, 1u);

		FShaderCompileOptions ForcedOptions = Options;
		ForcedOptions.bForceRecompile = true;
		ASSERT_TRUE(Builder->GetOrCompile(
			"/ShaderBuilderTests/Simple", ForcedOptions));
		const FShaderBuildStats ForcedStats =
			Builder->GetStats();
		EXPECT_EQ(ForcedStats.Compilations, 2u);
		EXPECT_EQ(ForcedStats.MemoryHits, 1u);
		EXPECT_EQ(ForcedStats.DdcHits, 0u);
	}

	TEST_F(FShaderBuilderTests, AlternatingMacroDependencyGraphsRemainWarmHits)
	{
		const std::filesystem::path SourceRoot = GetBuilderTestRoot() / "Source";
		WriteTextFile(SourceRoot / "VariantA.slang", "static const float SelectedValue = 1.0;\n");
		WriteTextFile(SourceRoot / "VariantB.slang", "static const float SelectedValue = 2.0;\n");
		WriteTextFile(SourceRoot / "Conditional.slang", R"(#ifdef USE_A
#include "VariantA.slang"
#else
#include "VariantB.slang"
#endif
[shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    return float4(SelectedValue + float(vertexID), 0.0, 0.0, 1.0);
}
)"
		);

		FShaderCompileOptions FirstOptions = MakeCompileOptions();
		FirstOptions.Macros.emplace_back("USE_A");
		FShaderCompileOptions SecondOptions = MakeCompileOptions();
		SecondOptions.Macros.emplace_back("USE_B");

		Builder = std::make_unique<FShaderBuilder>();
		ASSERT_TRUE(Builder->GetOrCompile("/ShaderBuilderTests/Conditional", FirstOptions));
		ASSERT_TRUE(Builder->GetOrCompile("/ShaderBuilderTests/Conditional", SecondOptions));
		Builder.reset();

		Builder = std::make_unique<FShaderBuilder>();
		ASSERT_TRUE(Builder->GetOrCompile("/ShaderBuilderTests/Conditional", FirstOptions));
		ASSERT_TRUE(Builder->GetOrCompile("/ShaderBuilderTests/Conditional", SecondOptions));
		const FShaderBuildStats WarmStats = Builder->GetStats();
		EXPECT_EQ(WarmStats.DependencyResolutions, 0u);
		EXPECT_EQ(WarmStats.Compilations, 0u);
		EXPECT_EQ(WarmStats.ContentReads, 0u);
		EXPECT_EQ(WarmStats.ManifestHits, 2u);
		EXPECT_EQ(WarmStats.DdcHits, 2u);
	}

	TEST_F(FShaderBuilderTests,
		ImportedModuleChangeRecompilesEveryDependentShader)
	{
		const std::filesystem::path SourceRoot =
			GetBuilderTestRoot() / "Source";
		const std::filesystem::path ModulePath =
			SourceRoot / "VertexFactory" / "Shared.slang";
		constexpr std::string_view FirstModule = R"(module Shared;
public float GetPositionOffset()
{
    return 1.0;
}
)";
		constexpr std::string_view SecondModule = R"(module Shared;
public float GetPositionOffset()
{
    return 2.0;
}
)";
		constexpr std::string_view DependentTemplate = R"(module DEPENDENT_MODULE;
import VertexFactory.Shared;

[shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    return float4(GetPositionOffset() + float(vertexID), 0.0, 0.0, 1.0);
}
)";
		WriteTextFile(ModulePath, FirstModule);
		for (const std::string_view Name : {"DependentA", "DependentB"})
		{
			std::string Source(DependentTemplate);
			Source.replace(
				Source.find("DEPENDENT_MODULE"),
				std::string_view("DEPENDENT_MODULE").size(),
				Name);
			WriteTextFile(
				SourceRoot / (std::string(Name) + ".slang"),
				Source);
		}

		Builder = std::make_unique<FShaderBuilder>();
		const FShaderCompileOptions Options = MakeCompileOptions();
		const FShaderCompilerOutput FirstA = Builder->GetOrCompile(
			"/ShaderBuilderTests/DependentA", Options);
		const FShaderCompilerOutput FirstB = Builder->GetOrCompile(
			"/ShaderBuilderTests/DependentB", Options);
		ASSERT_TRUE(FirstA) << FormatShaderError(FirstA.Error);
		ASSERT_TRUE(FirstB) << FormatShaderError(FirstB.Error);
		ASSERT_EQ(FirstA.CompiledShaders.size(), 1u);
		ASSERT_EQ(FirstB.CompiledShaders.size(), 1u);
		const FXxHash128 FirstAHash =
			FirstA.CompiledShaders[0].Hash;
		const FXxHash128 FirstBHash =
			FirstB.CompiledShaders[0].Hash;

		WriteTextFile(ModulePath, SecondModule);
		std::filesystem::last_write_time(
			ModulePath,
			std::filesystem::last_write_time(ModulePath)
				+ std::chrono::seconds(2));
		const FShaderCompilerOutput SecondA = Builder->GetOrCompile(
			"/ShaderBuilderTests/DependentA", Options);
		const FShaderCompilerOutput SecondB = Builder->GetOrCompile(
			"/ShaderBuilderTests/DependentB", Options);
		ASSERT_TRUE(SecondA) << FormatShaderError(SecondA.Error);
		ASSERT_TRUE(SecondB) << FormatShaderError(SecondB.Error);
		ASSERT_EQ(SecondA.CompiledShaders.size(), 1u);
		ASSERT_EQ(SecondB.CompiledShaders.size(), 1u);
		EXPECT_NE(SecondA.CompiledShaders[0].Hash, FirstAHash);
		EXPECT_NE(SecondB.CompiledShaders[0].Hash, FirstBHash);

		const FShaderBuildStats Stats =
			Builder->GetStats();
		EXPECT_EQ(Stats.DependencyResolutions, 4u);
		EXPECT_EQ(Stats.Compilations, 4u);
		EXPECT_EQ(Stats.MemoryHits, 0u);
		EXPECT_EQ(Stats.DdcHits, 0u);
	}

	TEST_F(FShaderBuilderTests,
		ErrorTerminalRecordsCompleteColdAndWarmBaseline)
	{
		const std::filesystem::path Root = GetBuilderTestRoot();
		const std::filesystem::path SourceRoot =
			std::filesystem::path(FPaths::EngineDir()) / "Shaders/Slang";
		const std::filesystem::path CacheRoot = Root / "MaterialCache";
		FShaderPaths::RegisterMountPoint(
			"/M5FixedMaterial/",
			SourceRoot.generic_string(),
			CacheRoot.generic_string());

		constexpr std::string_view VirtualPath =
			"/M5FixedMaterial/StaticMeshBasePass";
		const std::filesystem::path SourcePath =
			SourceRoot / "StaticMeshBasePass.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {
			"FragmentMain",
			"GeometryFragmentMain",
			"OpaqueShadowFragmentMain",
			"ShadowFragmentMain"};
		Options.Frequencies.assign(
			Options.EntryPoints.size(), EShaderFrequency::Fragment);
		Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", "1");
		Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", "1");
		Options.Macros.emplace_back(
			"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", "1056964608");
		Builder = std::make_unique<FShaderBuilder>();
		std::vector<FShaderSourceDependencyFingerprint> FirstManifest;
		std::vector<FShaderSourceDependencyFingerprint> SecondManifest;
		FShaderOperationResult ManifestError;
		ASSERT_TRUE((ManifestError = Builder->BuildSourceDependencyManifest(VirtualPath, Options, FirstManifest)))
			<< FormatShaderError(ManifestError.error());
		ASSERT_TRUE((ManifestError = Builder->BuildSourceDependencyManifest(VirtualPath, Options, SecondManifest)))
			<< FormatShaderError(ManifestError.error());
		EXPECT_EQ(FirstManifest, SecondManifest);
		ASSERT_EQ(FirstManifest.size(), 8u);
		EXPECT_TRUE(std::ranges::is_sorted(FirstManifest, {},
			&FShaderSourceDependencyFingerprint::VirtualPath));
		for (const auto& Dependency : FirstManifest)
		{
			EXPECT_TRUE(Dependency.VirtualPath.starts_with('/'));
			EXPECT_EQ(Dependency.VirtualPath.find(':'), std::string::npos);
			EXPECT_EQ(Dependency.VirtualPath.find('\\'), std::string::npos);
			EXPECT_FALSE(Dependency.ContentHash.IsZero());
		}
		const std::string CompilerIdentity =
			Builder->GetCompilerEnvironmentIdentity();
		EXPECT_FALSE(CompilerIdentity.empty());
		EXPECT_NE(CompilerIdentity.find("spirv"), std::string::npos);

		std::vector<std::string> Dependencies;
		FShaderOperationResult DependencyDiagnostic;
		FSlangShaderDependencyResolver Resolver;
		ASSERT_TRUE((DependencyDiagnostic = Resolver.Resolve(
			SourcePath.generic_string(), Options, Dependencies))) << FormatShaderError(DependencyDiagnostic.error());
		ASSERT_FALSE(Dependencies.empty());

		FShaderCompileOptions ColdOptions = Options;
		ColdOptions.bForceRecompile = true;
		const auto ColdBegin = std::chrono::steady_clock::now();
		const FShaderCompilerOutput Cold = Builder->GetOrCompile(
			VirtualPath, ColdOptions);
		const auto ColdEnd = std::chrono::steady_clock::now();
		ASSERT_TRUE(Cold) << FormatShaderError(Cold.Error);
		ASSERT_EQ(Cold.CompiledShaders.size(), 4u);
		const FShaderBuildStats ColdStats =
			Builder->GetStats();
		EXPECT_EQ(ColdStats.DependencyResolutions, 1u);
		EXPECT_EQ(ColdStats.Compilations, 1u);

		const std::array<std::string_view, 4> ExpectedEntries{
			"FragmentMain", "GeometryFragmentMain",
			"OpaqueShadowFragmentMain", "ShadowFragmentMain"};
		uint64 SpirvBytes = 0;
		for (size_t Index = 0; Index < Cold.CompiledShaders.size(); ++Index)
		{
			const FCompiledShader& Shader = Cold.CompiledShaders[Index];
			EXPECT_EQ(Shader.SourceEntryPoint, ExpectedEntries[Index]);
			EXPECT_EQ(Shader.Frequency, EShaderFrequency::Fragment);
			ASSERT_TRUE(Shader.Code);
			EXPECT_FALSE(Shader.Code->IsEmpty());
			SpirvBytes += Shader.Code->size();
		}
		EXPECT_EQ(
			Cold.CompiledShaders[0].Reflection.ResourceBindings.size(), 0u);
		EXPECT_EQ(
			Cold.CompiledShaders[1].Reflection.ResourceBindings.size(), 0u);
		EXPECT_TRUE(
			Cold.CompiledShaders[2].Reflection.ResourceBindings.empty());
		EXPECT_EQ(
			Cold.CompiledShaders[3].Reflection.ResourceBindings.size(), 0u);

		const auto WarmBegin = std::chrono::steady_clock::now();
		const FShaderCompilerOutput Warm = Builder->GetOrCompile(
			VirtualPath, Options);
		const auto WarmEnd = std::chrono::steady_clock::now();
		ASSERT_TRUE(Warm) << FormatShaderError(Warm.Error);
		const FShaderBuildStats WarmStats =
			Builder->GetStats();
		EXPECT_EQ(WarmStats.Compilations, ColdStats.Compilations);
		EXPECT_EQ(WarmStats.MemoryHits, ColdStats.MemoryHits + 1u);

		uint64 DdcBytes = 0;
		uint32 DdcFiles = 0;
		std::error_code ErrorCode;
		for (std::filesystem::recursive_directory_iterator It(
			Root / "DDC" / "Shaders" / "CompiledOutput", ErrorCode),
			End; !ErrorCode && It != End; It.increment(ErrorCode))
		{
			if (!It->is_regular_file()) continue;
			if (It->path().extension() == ".bin")
			{
				DdcBytes += It->file_size();
				++DdcFiles;
			}
		}
		ASSERT_FALSE(ErrorCode) << ErrorCode.message();
		EXPECT_EQ(DdcFiles, 1u);
		bool bFoundLegacyArtifact = false;
		for (std::filesystem::recursive_directory_iterator It(CacheRoot, ErrorCode),
			End; !ErrorCode && It != End; It.increment(ErrorCode))
		{
			if (It->is_regular_file()
				&& (It->path().extension() == ".spv"
					|| It->path().filename().generic_string().ends_with(
						".reflect.json"))) bFoundLegacyArtifact = true;
		}
		ASSERT_FALSE(ErrorCode) << ErrorCode.message();
		EXPECT_FALSE(bFoundLegacyArtifact);

		const uint64 SourceBytes =
			std::filesystem::file_size(SourcePath);
		const auto ColdMicroseconds =
			std::chrono::duration_cast<std::chrono::microseconds>(
				ColdEnd - ColdBegin).count();
		const auto WarmMicroseconds =
			std::chrono::duration_cast<std::chrono::microseconds>(
				WarmEnd - WarmBegin).count();
		RecordProperty("ColdCompileMicroseconds", ColdMicroseconds);
		RecordProperty("WarmCompileMicroseconds", WarmMicroseconds);
		RecordProperty("DependencyCount", Dependencies.size());
		RecordProperty("SourceBytes", SourceBytes);
		RecordProperty("SpirvBytes", SpirvBytes);
		RecordProperty("DdcBytes", DdcBytes);
		std::cout
			<< "[ErrorTerminalBaseline] cold_us=" << ColdMicroseconds
			<< " warm_us=" << WarmMicroseconds
			<< " dependencies=" << Dependencies.size()
			<< " source_bytes=" << SourceBytes
			<< " spirv_bytes=" << SpirvBytes
			<< " ddc_bytes=" << DdcBytes << '\n';
	}
	TEST_F(FShaderBuilderTests, CapturedIncludesNeverReopenLiveSources)
	{
		Builder = std::make_unique<FShaderBuilder>();
		const auto Root = GetBuilderTestRoot() / "Source";
		const auto Main = (Root / "Owned.slang").generic_string();
		const auto Include = (Root / "OwnedInclude.slang").generic_string();
		const std::string Source = R"(#include "OwnedInclude.slang"
[shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    return CapturedPosition();
}
)";
		const std::string Included = "float4 CapturedPosition() { return float4(0, 0, 0, 1); }";
		std::map<std::string, FByteBuffer> Files;
		auto Bytes = [](std::string_view Text) {
			const auto View = std::as_bytes(std::span(Text.data(), Text.size()));
			return FByteBuffer(View.begin(), View.end());
		};
		Files.emplace(Main, Bytes(Source));
		Files.emplace(Include, Bytes(Included));
		FShaderCompileOptions Options = MakeCompileOptions();
		const auto Artifacts = std::make_shared<FShaderSourceArtifacts>(Files);
		Options.SourceArtifacts = Artifacts;
		Files.clear();
		std::vector<FShaderSourceDependencyFingerprint> Before, After;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = Builder->BuildSourceDependencyManifest(Main, Options, Before))) << FormatShaderError(Error.error());
		ASSERT_EQ(Before.size(), 2u);
		WriteTextFile(Root / "Owned.slang", "invalid live root");
		WriteTextFile(Root / "OwnedInclude.slang", "invalid live include");
		const auto Output = Builder->GetOrCompile(Main, Options);
		ASSERT_TRUE(Output) << FormatShaderError(Output.Error);
		ASSERT_TRUE((Error = Builder->BuildSourceDependencyManifest(Main, Options, After))) << FormatShaderError(Error.error());
		EXPECT_EQ(Before, After);
		WriteTextFile(Root / "OwnedInclude.slang", Included);
		auto Incomplete = Artifacts->GetFiles();
		Incomplete.erase(Include);
		Options.SourceArtifacts = std::make_shared<FShaderSourceArtifacts>(Incomplete);
		EXPECT_FALSE(Builder->GetOrCompile(Main, Options));
		EXPECT_FALSE((Error = Builder->BuildSourceDependencyManifest(Main, Options, After)));
	}
	TEST_F(FShaderBuilderTests, GeneratedRootUsesCapturedIncludeAndDeclarationLimits)
	{
		Builder = std::make_unique<FShaderBuilder>();
		const std::string Include = "float4 CapturedPosition() { return float4(0, 0, 0, 1); }";
		const auto View = std::as_bytes(std::span(Include.data(), Include.size()));
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath = "/Generated/Materials/Captured.slang";
		Request.Source = R"(import Included;
[shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position { return CapturedPosition(); }
)";
		Request.EntryPoints = {"VertexMain"};
		Request.Frequencies = {EShaderFrequency::Vertex};
		Request.AllowedImportVirtualPrefixes = {"/Captured/"};
		Request.SourceArtifacts = std::make_shared<FShaderSourceArtifacts>(
			std::map<std::string, FByteBuffer>{{"/Captured/Included.slang", FByteBuffer(View.begin(), View.end())}},
			std::vector<std::string>{"/Captured/"});
		const auto Output = Builder->GetOrCompileGenerated(Request);
		ASSERT_TRUE(Output) << FormatShaderError(Output.Error);
		Request.AllowedImportVirtualPrefixes = {"/Different/"};
		EXPECT_FALSE(Builder->GetOrCompileGenerated(Request));
	}
	TEST_F(FShaderBuilderTests, ResolverRejectsChangedBytesUnderCapturedIdentity)
	{
		const auto File = GetBuilderTestRoot() / "Source/Simple.slang";
		FFileFingerprintCache Fingerprints;
		FShaderMetaData MetaData;
		ASSERT_TRUE(ShaderCompileUtilities::BuildShaderMetaData({File.generic_string()}, Fingerprints, MetaData));
		auto Captured = ShaderCompileUtilities::CaptureSourceArtifacts(MetaData);
		ASSERT_TRUE(Captured);
		EXPECT_TRUE((*Captured)->GetFiles().contains("/ShaderBuilderTests/Simple.slang"));
		WriteTextFile(File, "changed after definition capture");
		auto Rejected = ShaderCompileUtilities::CaptureSourceArtifacts(MetaData);
		ASSERT_FALSE(Rejected);
		EXPECT_EQ(Rejected.error().Code, EShaderError::DependencyContentConflict);
		EXPECT_EQ(Rejected.error().ActualIdentity, "/ShaderBuilderTests/Simple");
		// The already captured generation remains independently compilable.
		auto Options = MakeCompileOptions();
		Options.SourceArtifacts = *Captured;
		Builder = std::make_unique<FShaderBuilder>();
		ASSERT_TRUE(Builder->GetOrCompile("/ShaderBuilderTests/Simple", Options));
	}

	TEST_F(FShaderBuilderTests, GeneratedCompilerUsesResolvedGenerationAfterLiveIncludeChanges)
	{
		const auto Include = GetBuilderTestRoot() / "Source/Frozen.slang";
		WriteTextFile(Include, "public float4 Position() { return float4(0, 0, 0, 1); }");
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath = "/Generated/Materials/FrozenGeneration";
		Request.Source = R"(import Frozen;
[shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position { return Position(); }
)";
		Request.EntryPoints = {"VertexMain"};
		Request.Frequencies = {EShaderFrequency::Vertex};
		Request.AllowedImportVirtualPrefixes = {"/ShaderBuilderTests/"};
		bool bHookRan = false;
		Builder = std::make_unique<FShaderBuilder>([&](std::string_view) {
			bHookRan = true;
			WriteTextFile(Include, "invalid live include after source resolution");
		});
		const auto Output = Builder->GetOrCompileGenerated(Request);
		ASSERT_TRUE(Output) << FormatShaderError(Output.Error);
		EXPECT_TRUE(bHookRan);
		Builder = std::make_unique<FShaderBuilder>();
		EXPECT_FALSE(Builder->GetOrCompileGenerated(Request));
	}	namespace
	{
		struct FShaderSessionHarness
		{
			using Phase = DerivedData::EBuildSessionPhase;
			std::shared_ptr<FShaderBuildService> Service;
			FShaderCompileOptions CompileOptions;
			std::string Source = "[shader(\"vertex\")] float4 VertexMain(uint id : SV_VertexID) : SV_Position { return float4(float(id), 0, 0, 1); }";
			DerivedData::FBuildRequestOptions Execution;
			FSharedByteBuffer Stored;
			DerivedData::FCacheKey Key;
			uint32 Captures = 0, Compiles = 0, Hits = 0, Issues = 0, Puts = 0;
			bool Cancel = false;
			std::vector<Phase> Phases;
			explicit FShaderSessionHarness(std::function<void(std::string_view)> Hook = {})
				: Service(std::make_shared<FShaderBuildService>(std::move(Hook)))
			{
				using namespace DerivedData;
				CompileOptions = MakeCompileOptions();
				CompileOptions.VirtualShaderPath = "/Generated/Materials/SessionFixture";
				CompileOptions.CompilerEnvironment = Service->GetCompilerEnvironmentIdentity();
				CompileOptions.SourceArtifacts = std::make_shared<const FShaderSourceArtifacts>(std::map<std::string, FByteBuffer>{});
				Execution.Policy.Compress = true;
				Execution.Cache.Get = [this](const FCacheGetRequest& Request) -> FCacheGetResult {
					if (Stored.IsEmpty() || Request.Key != Key) return std::unexpected(FCacheError{ECacheError::Miss, "miss"});
					return Stored;
				};
				Execution.Cache.Put = [this](const FCachePutRequest& Request) -> FCachePutResult {
					++Puts; Key = Request.Key; Stored = FSharedByteBuffer::Copy(Request.Value); return {};
				};
				Execution.Cancellation = FBuildCancellation([this] { return Cancel; });
				Execution.Observer.OnMetric = [this](auto Name, uint64 Count) { if (Name == "Shader.Compilations") Compiles += Count; };
				Execution.Observer.OnCacheIssue = [this](const auto&, auto, const auto&) { ++Issues; };
				Execution.Observer.OnCacheHit = [this] { ++Hits; };
				Execution.Observer.OnPhase = [this](auto Value) { Phases.push_back(Value); };
			}
			auto Request(std::optional<std::string> CapturedSource = {}) -> FShaderSessionRequest
			{
				FXxHash128Builder EmptyTree; UpdateCanonicalHashString(EmptyTree, "DurinShaderPortableSourceTree_v1"); UpdateCanonicalHash(EmptyTree, uint64(0));
				FXxHash128Builder Tree; Tree.Update("DurinGeneratedShaderSourceTree_v1");
				UpdateCanonicalHash(Tree, FXxHash128::HashBuffer(Source)); UpdateCanonicalHash(Tree, EmptyTree.Finalize());
				FShaderMetaData Meta{.SourceTreeSignature = Tree.Finalize()}; FShaderVariantKey Variant;
				ShaderCompileUtilities::BuildVariantKey(CompileOptions.VirtualShaderPath, Meta, {}, CompileOptions.CompilerEnvironment, Variant);
				return MakeShaderSessionRequest(CompileOptions, Variant, {}, CapturedSource.value_or(Source), [this] {
					++Captures; return std::expected<std::shared_ptr<const FShaderSourceArtifacts>, FShaderError>(CompileOptions.SourceArtifacts);
				}).value();
			}
			auto Run() -> DerivedData::FBuildResult { return Service->Execute(Request(), Execution); }
		};
	}

	TEST_F(FShaderBuilderTests, SharedSessionRetainsBytecodeAndRepairsMalformedRecordOnce)
	{
		using namespace DerivedData;
		FShaderSessionHarness H;
		auto Cold = H.Run(); ASSERT_TRUE(Cold);
		auto Warm = H.Run(); ASSERT_TRUE(Warm);
		EXPECT_EQ(H.Captures, 1u); EXPECT_EQ(H.Compiles, 1u); EXPECT_EQ(H.Hits, 1u); EXPECT_EQ(H.Puts, 1u);
		auto Product = ShaderSharedOutput::Assemble(H.CompileOptions, *Warm); ASSERT_TRUE(Product);
		EXPECT_EQ(Product->CompiledShaders[0].Code->data(), Warm->FindValue("Entry/0/Code")->Data.data());
		FBuildOutputData Bad{.Schema = "Shader.Output", .SchemaVersion = 1, .Metadata = Warm->GetMetadata(),
			.Values = {Warm->GetValues().begin(), Warm->GetValues().end()}};
		for (auto& Value : Bad.Values) if (Value.Id == "Entry/0/Reflection") Value.Data = FSharedByteBuffer{};
		H.Stored = FCacheRecord::FromOutput(H.Key, FBuildOutput::TryCreate(std::move(Bad)).value())->Encode().value();
		auto Rebuilt = H.Run(); ASSERT_TRUE(Rebuilt);
		EXPECT_EQ(H.Issues, 1u); EXPECT_EQ(H.Captures, 2u); EXPECT_EQ(H.Compiles, 2u); EXPECT_EQ(H.Puts, 2u);
		Cold = {}; Warm = {}; Rebuilt = {}; H.Stored = {}; H.Service.reset();
		EXPECT_EQ(FXxHash128::HashBuffer(*Product->CompiledShaders[0].Code), Product->CompiledShaders[0].Hash);
	}

	TEST_F(FShaderBuilderTests, SharedSessionSeparatesOptionalPersistenceAndCancellation)
	{
		using namespace DerivedData;
		FShaderSessionHarness H;
		H.Execution.Cache.MakeRecord = [](const auto&, const auto&, auto) -> std::expected<FCacheRecord, FCacheError> {
			return std::unexpected(FCacheError{ECacheError::Corrupt, "injected record failure"});
		};
		EXPECT_TRUE(H.Run()); EXPECT_EQ(H.Issues, 1u); EXPECT_EQ(H.Puts, 0u);
		H.Execution.Policy.WriteCache = false; H.Phases.clear();
		EXPECT_TRUE(H.Run());
		for (auto Phase : {EBuildSessionPhase::Record, EBuildSessionPhase::Encode, EBuildSessionPhase::Compress, EBuildSessionPhase::Store})
			EXPECT_EQ(std::ranges::count(H.Phases, Phase), 0);
		H.Execution.Cache.MakeRecord = {}; H.Execution.Policy.WriteCache = true;
		ASSERT_TRUE(H.Run());
		const auto Captures = H.Captures; const auto Compiles = H.Compiles;
		H.Execution.Observer.OnPhase = [&](auto Phase) { if (Phase == EBuildSessionPhase::Validate) H.Cancel = true; };
		auto Cancelled = H.Run(); EXPECT_TRUE(IsBuildCancelled(Cancelled));
		EXPECT_EQ(H.Captures, Captures); EXPECT_EQ(H.Compiles, Compiles); EXPECT_EQ(H.Issues, 1u);
	}

	TEST_F(FShaderBuilderTests, SharedSessionVerifiesSourceIdentityBeforeCompilation)
	{
		using namespace DerivedData;
		FShaderSessionHarness H;
		auto Forged = H.Service->Execute(H.Request(H.Source + "\n// changed bytes"), H.Execution);
		ASSERT_FALSE(Forged);
		EXPECT_EQ(ShaderSessionError(Forged.error()).Code, EShaderError::DependencyContentConflict);
		EXPECT_EQ(H.Compiles, 0u); EXPECT_EQ(H.Puts, 0u);
		H.Source = "invalid shader source";
		auto Failed = H.Run(); ASSERT_FALSE(Failed);
		const auto Converted = ShaderSessionError(Failed.error());
		EXPECT_EQ(Converted.Code, EShaderError::SlangFailure);
		EXPECT_EQ(FormatShaderError(Converted), Failed.error().Description);
		ASSERT_TRUE(Failed.error().DiagnosticIdentity);
		EXPECT_EQ(Converted.GetSemanticFingerprint(), Failed.error().DiagnosticIdentity->HashLow);
	}

	TEST_F(FShaderBuilderTests, SharedSessionShutdownCancelsAndDrainsCompilerBeforeRelease)
	{
		using namespace DerivedData;
		std::promise<void> Acquired, Release;
		auto Released = Release.get_future().share();
		FShaderSessionHarness H([&](std::string_view) { Acquired.set_value(); Released.wait(); });
		auto Request = H.Request();
		auto Work = std::async(std::launch::async, [&] { return H.Service->Execute(std::move(Request), H.Execution); });
		const auto Ready = Acquired.get_future().wait_for(std::chrono::seconds(5));
		if (Ready != std::future_status::ready) { Release.set_value(); Work.wait(); FAIL() << "Compiler did not reach shutdown checkpoint"; }
		auto Shutdown = std::async(std::launch::async, [&] { H.Service->Close(); });
		EXPECT_EQ(Shutdown.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
		Release.set_value(); Shutdown.get();
		auto Result = Work.get(); EXPECT_TRUE(IsBuildCancelled(Result));
		EXPECT_EQ(H.Puts, 0u);
		auto Rejected = H.Service->Execute(H.Request(), H.Execution); ASSERT_FALSE(Rejected);
		EXPECT_EQ(ShaderSessionError(Rejected.error()).Code, EShaderError::Cancelled);
	}
	TEST_F(FShaderBuilderTests, SharedSessionCaptureRetainsSourceBytesAndCanonicalOptions)
	{
		using namespace DerivedData;
		FShaderSessionHarness H;
		const std::string Text = "public float Value() { return 1.0; }";
		FByteBuffer Bytes(std::as_bytes(std::span(Text.data(), Text.size())).begin(), std::as_bytes(std::span(Text.data(), Text.size())).end());
		const auto* Address = Bytes.data();
		std::map<std::string, FByteBuffer> Files; Files.emplace("/Shared/Helper.slang", std::move(Bytes));
		auto Artifacts = std::make_shared<const FShaderSourceArtifacts>(std::move(Files), std::vector<std::string>{"/Shared/"});
		EXPECT_EQ(Artifacts->GetFiles().at("/Shared/Helper.slang").data(), Address);
		H.CompileOptions.SourceArtifacts = Artifacts;
		FShaderVariantKey Variant{.Value = FXxHash128::HashBuffer("resolver fixture")};
		auto Request = MakeShaderSessionRequest(H.CompileOptions, Variant,
			{{"/Shared/Helper", FXxHash64::HashBuffer(Text)}}, H.Source,
			[Artifacts]() -> FShaderArtifactResolver::result_type { return Artifacts; }); ASSERT_TRUE(Request);
		auto Identities = Request->Resolver->Describe(Request->Definition.GetSources(), {}); ASSERT_TRUE(Identities);
		auto Inputs = Request->Resolver->Resolve(*Identities, {}); ASSERT_TRUE(Inputs);
		const auto Block = (*Inputs)[0].Values[0].Data;
		EXPECT_EQ(Block.data(), Address);
		Request = std::unexpected(FShaderError{}); H.CompileOptions.SourceArtifacts.reset(); Artifacts.reset(); Inputs = std::vector<FBuildInput>{};
		EXPECT_EQ(FXxHash64::HashBuffer(Block), FXxHash64::HashBuffer(Text));

		H.CompileOptions.SourceArtifacts = std::make_shared<const FShaderSourceArtifacts>(std::map<std::string, FByteBuffer>{});
		H.CompileOptions.Macros = {FShaderMacroDefinition("Z", "2"), FShaderMacroDefinition("A")};
		auto First = H.Request();
		std::ranges::reverse(H.CompileOptions.Macros); H.CompileOptions.bForceRecompile = true;
		auto Same = H.Request();
		EXPECT_EQ(First.Definition.GetConstants()[0].Value, Same.Definition.GetConstants()[0].Value);
		H.CompileOptions.Macros[0].Value = "";
		auto Different = H.Request();
		EXPECT_NE(First.Definition.GetConstants()[0].Value, Different.Definition.GetConstants()[0].Value);

		std::vector<FBuildConstant> Malformed{Same.Definition.GetConstants().begin(), Same.Definition.GetConstants().end()};
		std::get<std::string>(Malformed[0].Value).pop_back();
		auto Definition = FBuildDefinition::TryCreate(std::string(Same.Definition.GetFunctionName()), std::move(Malformed),
			{Same.Definition.GetSources().begin(), Same.Definition.GetSources().end()}); ASSERT_TRUE(Definition);
		Same.Definition = std::move(*Definition);
		auto Failed = H.Service->Execute(std::move(Same), H.Execution);
		EXPECT_FALSE(Failed); EXPECT_EQ(H.Compiles, 0u);
	}
	TEST_F(FShaderBuilderTests, SharedSessionAcceptsFullFileCountAndLargeDescriptorTable)
	{
		using namespace DerivedData;
		FShaderSessionHarness H;
		constexpr uint32 Count = 65536;
		const auto Contents = FSharedByteBuffer::Copy(std::as_bytes(std::span("// source", 9)));
		std::map<std::string, FSharedByteBuffer> Files;
		FShaderMetaData Meta;
		const std::string Prefix = "/" + std::string(80, 'a') + "/File";
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			const auto Name = Prefix + std::format("{:05}", Index);
			Files.emplace(Name + ".slang", Contents);
			Meta.PortableDependencies.push_back({Name, FXxHash64::HashBuffer(Contents)});
		}
		H.CompileOptions.SourceArtifacts = std::make_shared<const FShaderSourceArtifacts>(std::move(Files));
		FXxHash128Builder Portable; UpdateCanonicalHashString(Portable, "DurinShaderPortableSourceTree_v1"); UpdateCanonicalHash(Portable, uint64(Count));
		for (const auto& Dependency : Meta.PortableDependencies)
		{ UpdateCanonicalHashString(Portable, Dependency.VirtualPath); UpdateCanonicalHash(Portable, Dependency.ContentHash); }
		FXxHash128Builder Tree; Tree.Update("DurinGeneratedShaderSourceTree_v1");
		UpdateCanonicalHash(Tree, FXxHash128::HashBuffer(H.Source)); UpdateCanonicalHash(Tree, Portable.Finalize());
		Meta.SourceTreeSignature = Tree.Finalize();
		FShaderVariantKey Variant;
		ShaderCompileUtilities::BuildVariantKey(H.CompileOptions.VirtualShaderPath, Meta, {}, H.CompileOptions.CompilerEnvironment, Variant);
		auto Request = [&] {
			return MakeShaderSessionRequest(H.CompileOptions, Variant, Meta.PortableDependencies, H.Source,
				[&]() -> FShaderArtifactResolver::result_type { return H.CompileOptions.SourceArtifacts; }).value();
		};
		{
			auto Captured = Request(); auto Ids = Captured.Resolver->Describe(Captured.Definition.GetSources(), {}); ASSERT_TRUE(Ids);
			auto Inputs = Captured.Resolver->Resolve(*Ids, {}); ASSERT_TRUE(Inputs);
			const auto Table = std::ranges::find((*Inputs)[0].Values, "FileTable", &FBuildValue::Id);
			ASSERT_NE(Table, (*Inputs)[0].Values.end()); EXPECT_GT(Table->Data.size(), 4u * 1024 * 1024);
			EXPECT_TRUE((*Inputs)[0].Metadata.IsEmpty()); EXPECT_EQ((*Inputs)[0].Values.size(), Count + 2u);
			EXPECT_EQ((*Inputs)[0].Values[0].Data.data(), Contents.data());
		}
		H.Execution.Policy.WriteCache = false;
		auto Rejected = H.Service->Execute(Request(), H.Execution); ASSERT_FALSE(Rejected);
		EXPECT_EQ(Rejected.error().Phase, EBuildSessionPhase::Resolve); EXPECT_EQ(H.Compiles, 0u);
		H.Execution.Policy.InputLimits.MaximumValues = Count + 2;
		H.Execution.Policy.InputLimits.MaximumTotalBytes = 800ull * 1024 * 1024;
		auto Accepted = H.Service->Execute(Request(), H.Execution); ASSERT_TRUE(Accepted);
		EXPECT_EQ(H.Compiles, 1u);
		EXPECT_TRUE(ShaderSharedOutput::Assemble(H.CompileOptions, *Accepted));
	}

} // namespace Durin
