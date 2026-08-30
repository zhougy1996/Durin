#include "ShaderBuild/ShaderPaths.h"
#include "Shader/ShaderCompilerCore.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Paths.h"
#include "ShaderCompileService.h"
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
		auto GetServiceTestRoot() -> std::filesystem::path
		{
			return Durin::Testing::GetTestWorkDirectory() / "ShaderCompileService";
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

		auto MakeServiceOptions() -> FShaderCompileOptions
		{
			FShaderCompileOptions Options;
			Options.EntryPoints = {"VertexMain"};
			Options.Frequencies = {EShaderFrequency::Vertex};
			return Options;
		}

		class FShaderCompileServiceTests : public testing::Test
		{
		protected:
			void SetUp() override
			{
				ShutdownShaderCompileService();
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
				const std::filesystem::path Root = GetServiceTestRoot();
				PreviousDdcRoot = FPaths::DerivedDataCacheDir();
				std::error_code ErrorCode;
				Durin::Testing::RemoveTestWorkDirectory(Root, ErrorCode);
				ASSERT_FALSE(ErrorCode);
				std::filesystem::create_directories(Root / "Source");
				std::filesystem::create_directories(Root / "Cache");
				FPaths::SetDerivedDataCacheDirForTests(
					(Root / "DDC").generic_string());
				FShaderPaths::RegisterMountPoint(
					"/ShaderCompileServiceTests/",
					(Root / "Source").generic_string(),
					(Root / "Cache").generic_string()
				);
				WriteTestShader(Root / "Source" / "Simple.slang");
			}

			void TearDown() override
			{
				ShutdownShaderCompileService();
				FPaths::SetDerivedDataCacheDirForTests(PreviousDdcRoot);
			}

			std::string PreviousDdcRoot;
		};
	}

	TEST_F(FShaderCompileServiceTests, WarmRestartUsesManifestWithoutResolvingOrHashingSources)
	{
		const FShaderCompileOptions Options = MakeServiceOptions();
		InitShaderCompileService();
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options));
		const FShaderCompileServiceStats ColdStats = GetShaderCompileServiceStats();
		EXPECT_EQ(ColdStats.DependencyResolutions, 1u);
		EXPECT_EQ(ColdStats.Compilations, 1u);
		EXPECT_GT(ColdStats.ContentReads, 0u);

		ShutdownShaderCompileService();
		InitShaderCompileService();
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options));
		const FShaderCompileServiceStats WarmStats = GetShaderCompileServiceStats();
		EXPECT_EQ(WarmStats.DependencyResolutions, 0u);
		EXPECT_EQ(WarmStats.Compilations, 0u);
		EXPECT_EQ(WarmStats.ContentReads, 0u);
		EXPECT_EQ(WarmStats.ManifestHits, 1u);
		EXPECT_EQ(WarmStats.DdcHits, 1u);
	}

	TEST_F(FShaderCompileServiceTests, CorruptDdcValueIsRecompiledAndRepaired)
	{
		const FShaderCompileOptions Options = MakeServiceOptions();
		InitShaderCompileService();
		ASSERT_TRUE(GetOrCompileShader(
			"/ShaderCompileServiceTests/Simple", Options));
		ShutdownShaderCompileService();

		const std::filesystem::path Bucket = GetServiceTestRoot()
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

		InitShaderCompileService();
		const FShaderCompilerOutput Repaired = GetOrCompileShader(
			"/ShaderCompileServiceTests/Simple", Options);
		ASSERT_TRUE(Repaired) << Repaired.ErrorMessage;
		const FShaderCompileServiceStats Stats =
			GetShaderCompileServiceStats();
		EXPECT_EQ(Stats.DdcHits, 0u);
		EXPECT_EQ(Stats.DdcCorruptMisses, 1u);
		EXPECT_EQ(Stats.Compilations, 1u);
		EXPECT_EQ(Stats.ManifestHits, 1u);
	}

	TEST_F(FShaderCompileServiceTests, DdcStoreFailureDoesNotDiscardCompiledOutput)
	{
		const std::filesystem::path BlockedRoot =
			GetServiceTestRoot() / "BlockedDdc";
		{
			std::ofstream Stream(BlockedRoot, std::ios::binary);
			Stream << "not a directory";
			ASSERT_TRUE(Stream.good());
		}
		FPaths::SetDerivedDataCacheDirForTests(BlockedRoot.generic_string());
		InitShaderCompileService();
		const FShaderCompilerOutput Output = GetOrCompileShader(
			"/ShaderCompileServiceTests/Simple", MakeServiceOptions());
		ASSERT_TRUE(Output) << Output.ErrorMessage;
		const FShaderCompileServiceStats Stats =
			GetShaderCompileServiceStats();
		EXPECT_EQ(Stats.Compilations, 1u);
		EXPECT_EQ(Stats.DdcStoreFailures, 1u);
		EXPECT_EQ(Stats.OutputEntries, 1u);
	}

	TEST_F(FShaderCompileServiceTests,
		SourceTreeFingerprintReusesManifestAndInvalidatesOnChange)
	{
		const FShaderCompileOptions Options = MakeServiceOptions();
		InitShaderCompileService();
		FShaderSourceDependencyFingerprint ColdFingerprint;
		std::string Error;
		ASSERT_TRUE(BuildShaderSourceTreeFingerprintFromService(
			"/ShaderCompileServiceTests/Simple", Options,
			ColdFingerprint, Error)) << Error;
		EXPECT_EQ(ColdFingerprint.VirtualPath,
			"/ShaderCompileServiceTests/Simple");
		EXPECT_FALSE(ColdFingerprint.ContentHash.IsZero());
		EXPECT_EQ(GetShaderCompileServiceStats().DependencyResolutions, 1u);
		FShaderSourceDependencyFingerprint SameGenerationFingerprint;
		ASSERT_TRUE(BuildShaderSourceTreeFingerprintFromService(
			"/ShaderCompileServiceTests/Simple", Options,
			SameGenerationFingerprint, Error)) << Error;
		EXPECT_EQ(SameGenerationFingerprint, ColdFingerprint);
		const auto SameGenerationStats = GetShaderCompileServiceStats();
		EXPECT_EQ(SameGenerationStats.DependencyResolutions, 1u);
		EXPECT_EQ(SameGenerationStats.ManifestHits, 0u);
		EXPECT_EQ(SameGenerationStats.SourceTreeFingerprintHits, 1u);

		ShutdownShaderCompileService();
		InitShaderCompileService();
		FShaderSourceDependencyFingerprint WarmFingerprint;
		ASSERT_TRUE(BuildShaderSourceTreeFingerprintFromService(
			"/ShaderCompileServiceTests/Simple", Options,
			WarmFingerprint, Error)) << Error;
		EXPECT_EQ(WarmFingerprint, ColdFingerprint);
		const auto WarmStats = GetShaderCompileServiceStats();
		EXPECT_EQ(WarmStats.DependencyResolutions, 0u);
		EXPECT_EQ(WarmStats.ManifestHits, 1u);
		EXPECT_EQ(WarmStats.ContentReads, 0u);
		EXPECT_EQ(WarmStats.SourceTreeFingerprintHits, 0u);

		WriteTextFile(GetServiceTestRoot() / "Source/Simple.slang",
			R"([shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    return float4(float(vertexID), 1.0, 2.0, 1.0);
}
)");
		FShaderSourceDependencyFingerprint BeforeReloadFingerprint;
		ASSERT_TRUE(BuildShaderSourceTreeFingerprintFromService(
			"/ShaderCompileServiceTests/Simple", Options,
			BeforeReloadFingerprint, Error)) << Error;
		EXPECT_EQ(BeforeReloadFingerprint, WarmFingerprint);
		EXPECT_EQ(GetShaderCompileServiceStats().DependencyResolutions, 0u);
		EXPECT_EQ(
			GetShaderCompileServiceStats().SourceTreeFingerprintHits, 1u);
		const uint64 PreviousReloadGeneration = GetShaderReloadGeneration();
		EXPECT_GT(AdvanceShaderReloadGeneration(), PreviousReloadGeneration);
		FShaderSourceDependencyFingerprint ChangedFingerprint;
		ASSERT_TRUE(BuildShaderSourceTreeFingerprintFromService(
			"/ShaderCompileServiceTests/Simple", Options,
			ChangedFingerprint, Error)) << Error;
		EXPECT_NE(ChangedFingerprint.ContentHash,
			ColdFingerprint.ContentHash);
		EXPECT_EQ(GetShaderCompileServiceStats().DependencyResolutions, 1u);
	}

	TEST_F(FShaderCompileServiceTests,
		GeneratedSourceCompilesFromMemoryUsesCacheAndEnforcesImports)
	{
		WriteTextFile(GetServiceTestRoot() / "Source/Imported.slang",
			"module Imported; public float GeneratedValue() { return 0.25; }\n");
		InitShaderCompileService();
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
			"/ShaderCompileServiceTests/"};
		const FShaderCompilerOutput First =
			GetOrCompileGeneratedShader(Request);
		ASSERT_TRUE(First) << First.ErrorMessage;
		ASSERT_EQ(First.CompiledShaders.size(), 1u);
		const auto ColdStats = GetShaderCompileServiceStats();
		EXPECT_EQ(ColdStats.Compilations, 1u);
		const FShaderCompilerOutput Warm =
			GetOrCompileGeneratedShader(Request);
		ASSERT_TRUE(Warm) << Warm.ErrorMessage;
		const auto WarmStats = GetShaderCompileServiceStats();
		EXPECT_EQ(WarmStats.Compilations, ColdStats.Compilations);
		EXPECT_EQ(WarmStats.MemoryHits, ColdStats.MemoryHits + 1u);

		ShutdownShaderCompileService();
		InitShaderCompileService();
		const FShaderCompilerOutput RestartWarm =
			GetOrCompileGeneratedShader(Request);
		ASSERT_TRUE(RestartWarm) << RestartWarm.ErrorMessage;
		const auto RestartWarmStats = GetShaderCompileServiceStats();
		EXPECT_EQ(RestartWarmStats.DependencyResolutions, 0u);
		EXPECT_EQ(RestartWarmStats.ManifestHits, 1u);
		EXPECT_EQ(RestartWarmStats.DdcHits, 1u);
		EXPECT_EQ(RestartWarmStats.Compilations, 0u);
		EXPECT_EQ(RestartWarmStats.ContentReads, 0u);

		Request.AllowedImportVirtualPrefixes.clear();
		const FShaderCompilerOutput Rejected =
			GetOrCompileGeneratedShader(Request);
		EXPECT_FALSE(Rejected);
		EXPECT_NE(Rejected.ErrorMessage.find("allowlisted"), std::string::npos);
	}

	TEST_F(FShaderCompileServiceTests,
		GeneratedManifestInvalidatesWhenImportedModuleChanges)
	{
		const std::filesystem::path ImportedPath =
			GetServiceTestRoot() / "Source/Imported.slang";
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
			"/ShaderCompileServiceTests/"};

		InitShaderCompileService();
		const FShaderCompilerOutput First =
			GetOrCompileGeneratedShader(Request);
		ASSERT_TRUE(First) << First.ErrorMessage;
		ASSERT_EQ(First.CompiledShaders.size(), 1u);
		const FXxHash128 FirstHash = First.CompiledShaders.front().Hash;
		ShutdownShaderCompileService();

		WriteTextFile(ImportedPath,
			"module Imported; public float GeneratedValue() { return 0.875; }\n");
		std::filesystem::last_write_time(
			ImportedPath,
			std::filesystem::last_write_time(ImportedPath)
				+ std::chrono::seconds(2));
		InitShaderCompileService();
		const FShaderCompilerOutput Changed =
			GetOrCompileGeneratedShader(Request);
		ASSERT_TRUE(Changed) << Changed.ErrorMessage;
		ASSERT_EQ(Changed.CompiledShaders.size(), 1u);
		EXPECT_NE(Changed.CompiledShaders.front().Hash, FirstHash);
		const auto Stats = GetShaderCompileServiceStats();
		EXPECT_EQ(Stats.DependencyResolutions, 1u);
		EXPECT_EQ(Stats.ManifestHits, 0u);
		EXPECT_EQ(Stats.DdcHits, 0u);
		EXPECT_EQ(Stats.Compilations, 1u);
	}

	TEST_F(FShaderCompileServiceTests,
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

		InitShaderCompileService();
		const FShaderCompilerOutput First =
			GetOrCompileGeneratedShader(Request);
		ASSERT_TRUE(First) << First.ErrorMessage;
		ShutdownShaderCompileService();

		InitShaderCompileService();
		const FShaderCompilerOutput Warm =
			GetOrCompileGeneratedShader(Request);
		ASSERT_TRUE(Warm) << Warm.ErrorMessage;
		const auto Stats = GetShaderCompileServiceStats();
		EXPECT_EQ(Stats.DependencyResolutions, 0u);
		EXPECT_EQ(Stats.ManifestHits, 1u);
		EXPECT_EQ(Stats.DdcHits, 1u);
		EXPECT_EQ(Stats.Compilations, 0u);
	}

	TEST_F(FShaderCompileServiceTests, ConcurrentIdenticalRequestsCompileOnce)
	{
		const FShaderCompileOptions Options = MakeServiceOptions();
		InitShaderCompileService();
		std::vector<std::future<FShaderCompilerOutput>> Requests;
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			Requests.push_back(std::async(std::launch::async, [Options] {
				return GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options);
			}));
		}

		for (auto& Request : Requests)
		{
			EXPECT_TRUE(Request.get());
		}
		const FShaderCompileServiceStats Stats = GetShaderCompileServiceStats();
		EXPECT_EQ(Stats.DependencyResolutions, 1u);
		EXPECT_EQ(Stats.Compilations, 1u);
	}

	TEST_F(FShaderCompileServiceTests,
		ConcurrentColdFileAndGeneratedRequestsCompleteSafely)
	{
		constexpr uint32 FileRequestCount = 6;
		const std::filesystem::path SourceRoot =
			GetServiceTestRoot() / "Source";
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
			"/ShaderCompileServiceTests/"};

		InitShaderCompileService();
		std::latch StartGate(1);
		std::vector<std::future<FShaderCompilerOutput>> Requests;
		Requests.reserve(FileRequestCount + 1);
		for (uint32 Index = 0; Index < FileRequestCount; ++Index)
		{
			Requests.push_back(std::async(std::launch::async,
				[&StartGate, Index] {
					StartGate.wait();
					return GetOrCompileShader(std::format(
						"/ShaderCompileServiceTests/Concurrent{}", Index),
						MakeServiceOptions());
				}));
		}
		Requests.push_back(std::async(std::launch::async,
			[&StartGate, GeneratedRequest] {
				StartGate.wait();
				return GetOrCompileGeneratedShader(GeneratedRequest);
			}));
		StartGate.count_down();

		for (auto& Request : Requests)
		{
			const FShaderCompilerOutput Output = Request.get();
			EXPECT_TRUE(Output) << Output.ErrorMessage;
		}
		const FShaderCompileServiceStats Stats =
			GetShaderCompileServiceStats();
		EXPECT_EQ(Stats.DependencyResolutions, FileRequestCount + 1u);
		EXPECT_EQ(Stats.Compilations, FileRequestCount + 1u);
	}

	TEST_F(FShaderCompileServiceTests,
		CorrectedShaderRecompilesAfterAnUncachedFailure)
	{
		const std::filesystem::path ShaderPath =
			GetServiceTestRoot() / "Source" / "Simple.slang";
		WriteTextFile(
			ShaderPath,
			"[shader(\"vertex\")] broken shader source\n");
		InitShaderCompileService();

		const FShaderCompileOptions Options = MakeServiceOptions();
		const FShaderCompilerOutput Broken =
			GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options);
		EXPECT_FALSE(Broken);
		EXPECT_FALSE(Broken.ErrorMessage.empty());

		WriteTestShader(ShaderPath);
		std::filesystem::last_write_time(
			ShaderPath,
			std::filesystem::last_write_time(ShaderPath)
				+ std::chrono::seconds(2));
		const FShaderCompilerOutput Corrected =
			GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options);
		EXPECT_TRUE(Corrected);
		const FShaderCompileServiceStats Stats =
			GetShaderCompileServiceStats();
		EXPECT_EQ(Stats.DependencyResolutions, 2u);
		EXPECT_EQ(Stats.Compilations, 1u);
	}

	TEST_F(FShaderCompileServiceTests,
		ForceRecompileBypassesSuccessfulMemoryAndDiskOutputReuse)
	{
		InitShaderCompileService();
		const FShaderCompileOptions Options = MakeServiceOptions();
		ASSERT_TRUE(
			GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options));
		ASSERT_TRUE(
			GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options));
		const FShaderCompileServiceStats WarmStats =
			GetShaderCompileServiceStats();
		ASSERT_EQ(WarmStats.Compilations, 1u);
		ASSERT_EQ(WarmStats.MemoryHits, 1u);

		FShaderCompileOptions ForcedOptions = Options;
		ForcedOptions.bForceRecompile = true;
		ASSERT_TRUE(GetOrCompileShader(
			"/ShaderCompileServiceTests/Simple", ForcedOptions));
		const FShaderCompileServiceStats ForcedStats =
			GetShaderCompileServiceStats();
		EXPECT_EQ(ForcedStats.Compilations, 2u);
		EXPECT_EQ(ForcedStats.MemoryHits, 1u);
		EXPECT_EQ(ForcedStats.DdcHits, 0u);
	}

	TEST_F(FShaderCompileServiceTests, AlternatingMacroDependencyGraphsRemainWarmHits)
	{
		const std::filesystem::path SourceRoot = GetServiceTestRoot() / "Source";
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

		FShaderCompileOptions FirstOptions = MakeServiceOptions();
		FirstOptions.Macros.emplace_back("USE_A");
		FShaderCompileOptions SecondOptions = MakeServiceOptions();
		SecondOptions.Macros.emplace_back("USE_B");

		InitShaderCompileService();
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Conditional", FirstOptions));
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Conditional", SecondOptions));
		ShutdownShaderCompileService();

		InitShaderCompileService();
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Conditional", FirstOptions));
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Conditional", SecondOptions));
		const FShaderCompileServiceStats WarmStats = GetShaderCompileServiceStats();
		EXPECT_EQ(WarmStats.DependencyResolutions, 0u);
		EXPECT_EQ(WarmStats.Compilations, 0u);
		EXPECT_EQ(WarmStats.ContentReads, 0u);
		EXPECT_EQ(WarmStats.ManifestHits, 2u);
		EXPECT_EQ(WarmStats.DdcHits, 2u);
	}

	TEST_F(FShaderCompileServiceTests,
		ImportedModuleChangeRecompilesEveryDependentShader)
	{
		const std::filesystem::path SourceRoot =
			GetServiceTestRoot() / "Source";
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

		InitShaderCompileService();
		const FShaderCompileOptions Options = MakeServiceOptions();
		const FShaderCompilerOutput FirstA = GetOrCompileShader(
			"/ShaderCompileServiceTests/DependentA", Options);
		const FShaderCompilerOutput FirstB = GetOrCompileShader(
			"/ShaderCompileServiceTests/DependentB", Options);
		ASSERT_TRUE(FirstA) << FirstA.ErrorMessage;
		ASSERT_TRUE(FirstB) << FirstB.ErrorMessage;
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
		const FShaderCompilerOutput SecondA = GetOrCompileShader(
			"/ShaderCompileServiceTests/DependentA", Options);
		const FShaderCompilerOutput SecondB = GetOrCompileShader(
			"/ShaderCompileServiceTests/DependentB", Options);
		ASSERT_TRUE(SecondA) << SecondA.ErrorMessage;
		ASSERT_TRUE(SecondB) << SecondB.ErrorMessage;
		ASSERT_EQ(SecondA.CompiledShaders.size(), 1u);
		ASSERT_EQ(SecondB.CompiledShaders.size(), 1u);
		EXPECT_NE(SecondA.CompiledShaders[0].Hash, FirstAHash);
		EXPECT_NE(SecondB.CompiledShaders[0].Hash, FirstBHash);

		const FShaderCompileServiceStats Stats =
			GetShaderCompileServiceStats();
		EXPECT_EQ(Stats.DependencyResolutions, 4u);
		EXPECT_EQ(Stats.Compilations, 4u);
		EXPECT_EQ(Stats.MemoryHits, 0u);
		EXPECT_EQ(Stats.DdcHits, 0u);
	}

	TEST_F(FShaderCompileServiceTests,
		M5FixedMaterialPathRecordsCompleteColdAndWarmBaseline)
	{
		const std::filesystem::path Root = GetServiceTestRoot();
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
		InitShaderCompileService();
		std::vector<FShaderSourceDependencyFingerprint> FirstManifest;
		std::vector<FShaderSourceDependencyFingerprint> SecondManifest;
		std::string ManifestError;
		ASSERT_TRUE(BuildShaderSourceDependencyManifestFromService(
			VirtualPath, Options, FirstManifest, ManifestError))
			<< ManifestError;
		ASSERT_TRUE(BuildShaderSourceDependencyManifestFromService(
			VirtualPath, Options, SecondManifest, ManifestError))
			<< ManifestError;
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
			GetShaderCompilerEnvironmentIdentityFromService();
		EXPECT_FALSE(CompilerIdentity.empty());
		EXPECT_NE(CompilerIdentity.find("spirv"), std::string::npos);

		std::vector<std::string> Dependencies;
		std::string DependencyDiagnostic;
		FSlangShaderDependencyResolver Resolver;
		ASSERT_TRUE(Resolver.Resolve(
			SourcePath.generic_string(), Options, Dependencies,
			DependencyDiagnostic)) << DependencyDiagnostic;
		ASSERT_FALSE(Dependencies.empty());

		FShaderCompileOptions ColdOptions = Options;
		ColdOptions.bForceRecompile = true;
		const auto ColdBegin = std::chrono::steady_clock::now();
		const FShaderCompilerOutput Cold = GetOrCompileShader(
			VirtualPath, ColdOptions);
		const auto ColdEnd = std::chrono::steady_clock::now();
		ASSERT_TRUE(Cold) << Cold.ErrorMessage;
		ASSERT_EQ(Cold.CompiledShaders.size(), 4u);
		const FShaderCompileServiceStats ColdStats =
			GetShaderCompileServiceStats();
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
			EXPECT_FALSE(Shader.Code->empty());
			SpirvBytes += Shader.Code->size();
		}
		EXPECT_EQ(
			Cold.CompiledShaders[0].Reflection.ResourceBindings.size(), 24u);
		EXPECT_EQ(
			Cold.CompiledShaders[1].Reflection.ResourceBindings.size(), 17u);
		EXPECT_TRUE(
			Cold.CompiledShaders[2].Reflection.ResourceBindings.empty());
		EXPECT_EQ(
			Cold.CompiledShaders[3].Reflection.ResourceBindings.size(), 3u);

		const auto WarmBegin = std::chrono::steady_clock::now();
		const FShaderCompilerOutput Warm = GetOrCompileShader(
			VirtualPath, Options);
		const auto WarmEnd = std::chrono::steady_clock::now();
		ASSERT_TRUE(Warm) << Warm.ErrorMessage;
		const FShaderCompileServiceStats WarmStats =
			GetShaderCompileServiceStats();
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
			<< "[M5FixedMaterialBaseline] cold_us=" << ColdMicroseconds
			<< " warm_us=" << WarmMicroseconds
			<< " dependencies=" << Dependencies.size()
			<< " source_bytes=" << SourceBytes
			<< " spirv_bytes=" << SpirvBytes
			<< " ddc_bytes=" << DdcBytes << '\n';
	}
} // namespace Durin
