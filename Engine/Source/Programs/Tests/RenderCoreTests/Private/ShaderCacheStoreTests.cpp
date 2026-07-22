#include "Shader/ShaderPaths.h"
#include "Shader/ShaderCompilerCore.h"

#include "Hash/XxHash.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Shader/ShaderCacheStore.h"
#include "Shader/ShaderCompileUtilities.h"

#include "gtest/gtest.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view GTestVirtualRoot = "/ShaderCacheTests/";
		constexpr std::string_view GVariantHex = "00112233445566778899aabbccddeeff";

		auto GetTestCacheRoot() -> std::filesystem::path
		{
			return std::filesystem::path(DURIN_TEST_WORK_DIR) / "ShaderCacheStore";
		}

		auto EnsureTestMount() -> void
		{
			static std::once_flag Once;
			std::call_once(Once, [] {
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
				const std::filesystem::path Root = GetTestCacheRoot();
				std::filesystem::create_directories(Root / "Source");
				std::filesystem::create_directories(Root / "Cache");
				FShaderPaths::RegisterMountPoint(
					GTestVirtualRoot,
					(Root / "Source").generic_string(),
					(Root / "Cache").generic_string()
				);
			});
		}

		auto MakeOptions(const char8* EntryPoint = "VertexMain", EShaderFrequency Frequency = EShaderFrequency::Vertex) -> FShaderCompileOptions
		{
			FShaderCompileOptions Options;
			Options.VirtualShaderPath = "/ShaderCacheTests/Test";
			Options.EntryPoints = {EntryPoint};
			Options.Frequencies = {Frequency};
			Options.CompilerEnvironment = "test-compiler";
			return Options;
		}

		auto MakeVariantKey() -> FShaderVariantKey
		{
			FShaderVariantKey Key;
			Key.Value = FXxHash128::FromString(GVariantHex);
			Key.Hex = GVariantHex;
			return Key;
		}

		auto MakeCompiledOutput(const FShaderCompileOptions& Options) -> FShaderCompilerOutput
		{
			const std::array<uint32, 5> SpirvHeader = {0x07230203u, 0x00010500u, 0u, 1u, 0u};
			FCompiledShader Shader;
			Shader.Frequency = Options.Frequencies.front();
			Shader.SourceEntryPoint = Options.EntryPoints.front();
			Shader.BinaryEntryPoint = "main";
			Shader.DebugName = "ShaderCacheStoreTest";
			Shader.Code = std::make_shared<std::vector<std::byte>>(sizeof(SpirvHeader));
			std::memcpy(Shader.Code->data(), SpirvHeader.data(), sizeof(SpirvHeader));
			Shader.Hash = FXxHash128::HashBuffer(*Shader.Code);

			FShaderCompilerOutput Output;
			Output.bSucceeded = true;
			Output.CompiledShaders.push_back(std::move(Shader));
			return Output;
		}

		class FShaderCacheStoreTests : public testing::Test
		{
		protected:
			void SetUp() override
			{
				EnsureTestMount();
				std::error_code ErrorCode;
				std::filesystem::remove_all(GetTestCacheRoot() / "Cache", ErrorCode);
				ASSERT_FALSE(ErrorCode);
				std::filesystem::create_directories(GetTestCacheRoot() / "Cache");
			}
		};
	}

	TEST_F(FShaderCacheStoreTests, ValidArtifactRoundTrips)
	{
		const FShaderCompileOptions Options = MakeOptions();
		const FShaderVariantKey Key = MakeVariantKey();
		const FShaderCompilerOutput Expected = MakeCompiledOutput(Options);
		FShaderCacheStore Store;

		ASSERT_TRUE(Store.Save(Options.VirtualShaderPath, Options, Key, Expected));
		FShaderCompilerOutput Loaded;
		ASSERT_TRUE(Store.TryLoad(Options.VirtualShaderPath, Options, Key, Loaded));
		ASSERT_EQ(Loaded.CompiledShaders.size(), 1u);
		EXPECT_EQ(Loaded.CompiledShaders.front().Hash, Expected.CompiledShaders.front().Hash);
		EXPECT_EQ(*Loaded.CompiledShaders.front().Code, *Expected.CompiledShaders.front().Code);
	}

	TEST_F(FShaderCacheStoreTests, RejectsInvalidSpirvMagic)
	{
		const FShaderCompileOptions Options = MakeOptions();
		const FShaderVariantKey Key = MakeVariantKey();
		FShaderCacheStore Store;
		ASSERT_TRUE(Store.Save(Options.VirtualShaderPath, Options, Key, MakeCompiledOutput(Options)));

		const std::array<uint32, 5> InvalidWords = {0u, 0u, 0u, 0u, 0u};
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(InvalidWords, FShaderPaths::BinaryPath(
			Options.VirtualShaderPath, Options.EntryPoints.front(), Options.Frequencies.front(), Key.Hex)));

		FShaderCompilerOutput Loaded;
		EXPECT_FALSE(Store.TryLoad(Options.VirtualShaderPath, Options, Key, Loaded));
	}

	TEST_F(FShaderCacheStoreTests, RejectsBytecodeHashMismatch)
	{
		const FShaderCompileOptions Options = MakeOptions();
		const FShaderVariantKey Key = MakeVariantKey();
		FShaderCacheStore Store;
		ASSERT_TRUE(Store.Save(Options.VirtualShaderPath, Options, Key, MakeCompiledOutput(Options)));

		const std::array<uint32, 5> DifferentWords = {0x07230203u, 0x00010500u, 1u, 1u, 0u};
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(DifferentWords, FShaderPaths::BinaryPath(
			Options.VirtualShaderPath, Options.EntryPoints.front(), Options.Frequencies.front(), Key.Hex)));

		FShaderCompilerOutput Loaded;
		EXPECT_FALSE(Store.TryLoad(Options.VirtualShaderPath, Options, Key, Loaded));
	}

	TEST_F(FShaderCacheStoreTests, RejectsSidecarRequestIdentityMismatch)
	{
		const FShaderCompileOptions Options = MakeOptions();
		const FShaderVariantKey Key = MakeVariantKey();
		FShaderCacheStore Store;
		ASSERT_TRUE(Store.Save(Options.VirtualShaderPath, Options, Key, MakeCompiledOutput(Options)));

		const std::string SidecarPath = FShaderPaths::ReflectionPath(
			Options.VirtualShaderPath, Options.EntryPoints.front(), Options.Frequencies.front(), Key.Hex);
		FJsonDocument Document;
		ASSERT_TRUE(Document.LoadFromFile(SidecarPath));
		Document.GetMutableRoot().SetChildValue("SourceEntryPoint", "WrongMain");
		ASSERT_TRUE(Document.SaveToFile(SidecarPath));

		FShaderCompilerOutput Loaded;
		EXPECT_FALSE(Store.TryLoad(Options.VirtualShaderPath, Options, Key, Loaded));
	}

	TEST_F(FShaderCacheStoreTests, ArtifactPathsSeparateSanitizedNamesAndFrequencies)
	{
		const std::string First = FShaderPaths::BinaryPath("/ShaderCacheTests/Test", "Main:A", EShaderFrequency::Vertex, GVariantHex);
		const std::string Second = FShaderPaths::BinaryPath("/ShaderCacheTests/Test", "Main?A", EShaderFrequency::Vertex, GVariantHex);
		const std::string Third = FShaderPaths::BinaryPath("/ShaderCacheTests/Test", "Main:A", EShaderFrequency::Fragment, GVariantHex);
		EXPECT_NE(First, Second);
		EXPECT_NE(First, Third);
	}

	TEST(FShaderCompileUtilitiesTests, VariantKeyIncludesCompilerEnvironment)
	{
		FShaderMetaData MetaData;
		MetaData.SourceTreeSignature = FXxHash128::FromString("ffeeddccbbaa99887766554433221100");
		std::vector<FShaderMacroDefinition> Macros;
		FShaderVariantKey First;
		FShaderVariantKey Second;
		ShaderCompileUtilities::BuildVariantKey("/ShaderCacheTests/Test", MetaData, Macros, "slang-build-a", First);
		ShaderCompileUtilities::BuildVariantKey("/ShaderCacheTests/Test", MetaData, Macros, "slang-build-b", Second);
		EXPECT_NE(First.Value, Second.Value);
	}
} // namespace Durin
