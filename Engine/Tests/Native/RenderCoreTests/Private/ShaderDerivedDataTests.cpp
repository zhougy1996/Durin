#include "Shader/ShaderCompilerCore.h"
#include "ShaderBuild/ShaderPaths.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/FileFingerprintCache.h"
#include "NativeTestSupport.h"
#include "Serialization/BinaryFormat.h"
#include "ShaderCompileUtilities.h"
#include "ShaderDependencyManifestStore.h"
#include "ShaderDerivedData.h"

#include "gtest/gtest.h"

#include <fstream>

namespace Durin
{
	namespace
	{
		auto GetRoot() -> std::filesystem::path
		{
			return Testing::GetTestWorkDirectory() / "ShaderDerivedData";
		}

		auto EnsureMount() -> void
		{
			static std::once_flag Once;
			std::call_once(Once, [] {
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
				FShaderPaths::RegisterMountPoint(
					"/ShaderDerivedDataTests/",
					(GetRoot() / "Source").generic_string(),
					(GetRoot() / "Manifests").generic_string());
			});
		}

		auto MakeOptions() -> FShaderCompileOptions
		{
			FShaderCompileOptions Options;
			Options.VirtualShaderPath = "/ShaderDerivedDataTests/Test";
			Options.EntryPoints = {"VertexMain", "FragmentMain"};
			Options.Frequencies = {
				EShaderFrequency::Vertex, EShaderFrequency::Fragment};
			Options.CompilerEnvironment = "slang-test-spirv";
			return Options;
		}

		auto MakeShader(std::string_view EntryPoint,
			EShaderFrequency Frequency, uint32 Bound) -> FCompiledShader
		{
			const std::array<uint32, 5> Words = {
				0x07230203u, 0x00010500u, 0u, Bound, 0u};
			FCompiledShader Shader;
			Shader.Frequency = Frequency;
			Shader.SourceEntryPoint = EntryPoint;
			Shader.BinaryEntryPoint = "main";
			Shader.DebugName = std::string(EntryPoint) + "Debug";
			Shader.Code = std::make_shared<std::vector<std::byte>>(
				sizeof(Words));
			std::memcpy(Shader.Code->data(), Words.data(), sizeof(Words));
			Shader.Hash = FXxHash128::HashBuffer(*Shader.Code);
			Shader.Reflection.ResourceBindings.push_back({
				.Name = "Scene",
				.StageFlags = Frequency == EShaderFrequency::Vertex
					? EShaderStageFlags::Vertex : EShaderStageFlags::Fragment,
				.SetIndex = 0,
				.BindingIndex = Bound,
				.Type = ERHIBindingType::UniformBuffer,
				.ArraySize = 1});
			Shader.Reflection.PushConstantRanges.push_back({
				.StageFlags = Shader.Reflection.ResourceBindings.front().StageFlags,
				.Offset = 0, .Size = 16});
			return Shader;
		}

		auto MakeOutput() -> FShaderCompilerOutput
		{
			FShaderCompilerOutput Output;
			Output.bSucceeded = true;
			Output.CompiledShaders.push_back(MakeShader(
				"VertexMain", EShaderFrequency::Vertex, 1));
			Output.CompiledShaders.push_back(MakeShader(
				"FragmentMain", EShaderFrequency::Fragment, 2));
			return Output;
		}

		auto WriteU32At(std::vector<std::byte>& Bytes,
			size_t Offset, uint32 Value) -> void
		{
			ASSERT_LE(Offset + sizeof(Value), Bytes.size());
			for (size_t Index = 0; Index < sizeof(Value); ++Index)
				Bytes[Offset + Index] = static_cast<std::byte>(
					(Value >> (Index * 8)) & 0xffu);
		}

		auto ToHex(std::span<const std::byte> Bytes) -> std::string
		{
			std::string Result;
			Result.reserve(Bytes.size() * 2);
			for (std::byte Byte : Bytes)
				Result += std::format("{:02x}", std::to_integer<uint8>(Byte));
			return Result;
		}

		class FShaderDerivedDataTests : public testing::Test
		{
		protected:
			void SetUp() override
			{
				EnsureMount();
				std::error_code Error;
				Testing::RemoveTestWorkDirectory(GetRoot(), Error);
				ASSERT_FALSE(Error);
				std::filesystem::create_directories(GetRoot() / "Source");
				std::filesystem::create_directories(GetRoot() / "Manifests");
			}
		};
	}

	TEST_F(FShaderDerivedDataTests, CompleteMultiStagePayloadRoundTrips)
	{
		const FShaderCompileOptions Options = MakeOptions();
		const FShaderCompilerOutput Expected = MakeOutput();
		std::vector<std::byte> First;
		std::vector<std::byte> Second;
		std::string Error;
		ASSERT_TRUE(ShaderDerivedData::Encode(
			Options, Expected, First, Error)) << Error;
		ASSERT_TRUE(ShaderDerivedData::Encode(
			Options, Expected, Second, Error)) << Error;
		EXPECT_EQ(First, Second);
		EXPECT_EQ(ToHex(First),
			"4453484401000000010000000403020100000000020000000a000000000000005665727465784d61696e04000000000000006d61696e00000000000000000f000000000000005665727465784d61696e4465627567cf9a2d3c094317863728ab64520b8eeb140000000000000003022307000501000000000001000000000000000100000005000000000000005363656e65010000000000000001000000000000000100000001000000010000000000000010000000000000000c00000000000000467261676d656e744d61696e04000000000000006d61696e01000000000000001100000000000000467261676d656e744d61696e446562756776835ac38ce6e7a67c3015d75a3a6f26140000000000000003022307000501000000000002000000000000000100000005000000000000005363656e6502000000000000000200000000000000010000000100000002000000000000001000000000000000");
		ASSERT_GE(First.size(), 24u);
		uint32 Magic = 0;
		uint32 Schema = 0;
		uint32 Builder = 0;
		EXPECT_TRUE(ReadLittleEndianAt<uint32>(First, 0, Magic));
		EXPECT_TRUE(ReadLittleEndianAt<uint32>(First, 4, Schema));
		EXPECT_TRUE(ReadLittleEndianAt<uint32>(First, 8, Builder));
		EXPECT_EQ(Magic, ShaderDerivedData::PayloadMagic);
		EXPECT_EQ(Schema, ShaderDerivedData::PayloadSchemaVersion);
		EXPECT_EQ(Builder, ShaderDerivedData::BuilderVersion);

		FShaderCompilerOutput Loaded;
		ASSERT_TRUE(ShaderDerivedData::Decode(
			First, Options, Loaded, Error)) << Error;
		ASSERT_EQ(Loaded.CompiledShaders.size(), 2u);
		for (size_t Index = 0; Index < Loaded.CompiledShaders.size(); ++Index)
		{
			const FCompiledShader& Actual = Loaded.CompiledShaders[Index];
			const FCompiledShader& Wanted = Expected.CompiledShaders[Index];
			EXPECT_EQ(Actual.SourceEntryPoint, Wanted.SourceEntryPoint);
			EXPECT_EQ(Actual.Frequency, Wanted.Frequency);
			EXPECT_EQ(Actual.Hash, Wanted.Hash);
			EXPECT_EQ(*Actual.Code, *Wanted.Code);
			EXPECT_EQ(Actual.Reflection.ResourceBindings,
				Wanted.Reflection.ResourceBindings);
			EXPECT_EQ(Actual.Reflection.PushConstantRanges,
				Wanted.Reflection.PushConstantRanges);
		}
	}

	TEST_F(FShaderDerivedDataTests, SingleStagePayloadMatchesGoldenBytes)
	{
		FShaderCompileOptions Options = MakeOptions();
		Options.EntryPoints.resize(1);
		Options.Frequencies.resize(1);
		FShaderCompilerOutput Output = MakeOutput();
		Output.CompiledShaders.resize(1);
		std::vector<std::byte> Bytes;
		std::string Error;
		ASSERT_TRUE(ShaderDerivedData::Encode(
			Options, Output, Bytes, Error)) << Error;
		EXPECT_EQ(ToHex(Bytes),
			"4453484401000000010000000403020100000000010000000a000000000000005665727465784d61696e04000000000000006d61696e00000000000000000f000000000000005665727465784d61696e4465627567cf9a2d3c094317863728ab64520b8eeb140000000000000003022307000501000000000001000000000000000100000005000000000000005363656e6501000000000000000100000000000000010000000100000001000000000000001000000000000000");
	}

	TEST_F(FShaderDerivedDataTests, RejectsMalformedValuesWithoutPartialOutput)
	{
		const FShaderCompileOptions Options = MakeOptions();
		std::vector<std::byte> Bytes;
		std::string Error;
		ASSERT_TRUE(ShaderDerivedData::Encode(
			Options, MakeOutput(), Bytes, Error)) << Error;
		auto ExpectRejected = [&](std::vector<std::byte> Candidate) {
			FShaderCompilerOutput Loaded;
			Loaded.bSucceeded = true;
			Loaded.CompiledShaders.push_back(MakeShader(
				"Old", EShaderFrequency::Vertex, 1));
			EXPECT_FALSE(ShaderDerivedData::Decode(
				Candidate, Options, Loaded, Error));
			EXPECT_FALSE(Loaded.bSucceeded);
			EXPECT_TRUE(Loaded.CompiledShaders.empty());
		};

		std::vector<std::byte> BadMagic = Bytes;
		BadMagic[0] = std::byte{0};
		ExpectRejected(std::move(BadMagic));
		std::vector<std::byte> BadVersion = Bytes;
		WriteU32At(BadVersion, 4,
			ShaderDerivedData::PayloadSchemaVersion + 1);
		ExpectRejected(std::move(BadVersion));
		std::vector<std::byte> Reserved = Bytes;
		Reserved[16] = std::byte{1};
		ExpectRejected(std::move(Reserved));
		std::vector<std::byte> Truncated = Bytes;
		Truncated.pop_back();
		ExpectRejected(std::move(Truncated));
		std::vector<std::byte> Trailing = Bytes;
		Trailing.push_back(std::byte{0});
		ExpectRejected(std::move(Trailing));
		std::vector<std::byte> CorruptCode = Bytes;
		const auto It = std::ranges::search(CorruptCode,
			std::array{std::byte{0x03}, std::byte{0x02},
				std::byte{0x23}, std::byte{0x07}});
		ASSERT_NE(It.begin(), CorruptCode.end());
		*It.begin() = std::byte{0};
		ExpectRejected(std::move(CorruptCode));
		std::vector<std::byte> BadFrequency = Bytes;
		WriteU32At(BadFrequency, 54,
			static_cast<uint32>(EShaderFrequency::RayMiss) + 1);
		ExpectRejected(std::move(BadFrequency));
		std::vector<std::byte> BadHash = Bytes;
		BadHash[85] ^= std::byte{1};
		ExpectRejected(std::move(BadHash));
		std::vector<std::byte> BadBindingCount = Bytes;
		WriteU32At(BadBindingCount, 129, 65537);
		ExpectRejected(std::move(BadBindingCount));

		FShaderCompileOptions WrongRequest = Options;
		WrongRequest.EntryPoints[0] = "WrongMain";
		FShaderCompilerOutput Loaded;
		EXPECT_FALSE(ShaderDerivedData::Decode(
			Bytes, WrongRequest, Loaded, Error));
		EXPECT_TRUE(Loaded.CompiledShaders.empty());
	}

	TEST_F(FShaderDerivedDataTests, OutputKeyIncludesExactRequestButNotForcePolicy)
	{
		const FShaderVariantKey Variant{
			.Value = FXxHash128::FromString(
				"00112233445566778899aabbccddeeff"),
			.Hex = "00112233445566778899aabbccddeeff"};
		FShaderCompileOptions Options = MakeOptions();
		const auto First = ShaderDerivedData::BuildKey(Variant, Options);
		Options.bForceRecompile = true;
		EXPECT_EQ(ShaderDerivedData::BuildKey(Variant, Options), First);
		Options.EntryPoints[1] = "OtherMain";
		EXPECT_NE(ShaderDerivedData::BuildKey(Variant, Options), First);
		Options.EntryPoints[1] = Options.EntryPoints[0];
		Options.Frequencies[1] = Options.Frequencies[0];
		EXPECT_FALSE(ShaderDerivedData::BuildKey(Variant, Options).IsValid());
	}

	TEST_F(FShaderDerivedDataTests, PortableIdentityIgnoresPhysicalFingerprintFacts)
	{
		FShaderMetaData First;
		First.SourceTreeSignature = FXxHash128::FromString(
			"ffeeddccbbaa99887766554433221100");
		First.Dependencies.push_back({
			.NormalizedPath = "C:/checkout-a/Shaders/Common.slang",
			.LastWriteTime = std::filesystem::file_time_type(
				std::filesystem::file_time_type::duration(10)),
			.FileSize = 10,
			.ContentHash = FXxHash64::FromString("abcdefabcdefabcd")});
		First.PortableDependencies.push_back({
			.VirtualPath = "/Engine/Common.slang",
			.ContentHash = First.Dependencies.front().ContentHash});
		FShaderMetaData Second = First;
		Second.Dependencies.front().NormalizedPath =
			"D:/checkout-b/Shaders/Common.slang";
		Second.Dependencies.front().LastWriteTime =
			std::filesystem::file_time_type(
				std::filesystem::file_time_type::duration(999));
		std::vector<FShaderMacroDefinition> Macros;
		FShaderVariantKey FirstKey;
		FShaderVariantKey SecondKey;
		ShaderCompileUtilities::BuildVariantKey(
			"/Engine/Test", First, Macros, "compiler", FirstKey);
		ShaderCompileUtilities::BuildVariantKey(
			"/Engine/Test", Second, Macros, "compiler", SecondKey);
		EXPECT_EQ(FirstKey.Value, SecondKey.Value);
	}

	TEST_F(FShaderDerivedDataTests, LocalManifestRoundTripsAndWarmValidationReadsNoContent)
	{
		const std::filesystem::path Dependency =
			GetRoot() / "Source" / "Common.slang";
		{
			std::ofstream Stream(Dependency, std::ios::binary);
			Stream << "static const float Value = 1.0;\n";
		}
		FFileFingerprintCache Cold;
		FShaderMetaData MetaData;
		std::string Error;
		ASSERT_TRUE(ShaderCompileUtilities::BuildShaderMetaData(
			{Dependency.generic_string()}, Cold, MetaData, Error)) << Error;
		ASSERT_EQ(MetaData.PortableDependencies.size(), 1u);
		EXPECT_EQ(MetaData.PortableDependencies.front().VirtualPath,
			"/ShaderDerivedDataTests/Common");

		FShaderDependencyKey Key;
		ShaderCompileUtilities::BuildDependencyKey(
			"/ShaderDerivedDataTests/Test", {}, "compiler", Key);
		FShaderDependencyManifestStore Store;
		ASSERT_TRUE(Store.Save(
			"/ShaderDerivedDataTests/Test", Key, MetaData));
		FShaderMetaData Loaded;
		ASSERT_TRUE(Store.Load(
			"/ShaderDerivedDataTests/Test", Key, Loaded));
		FFileFingerprintCache Warm;
		bool bCurrent = false;
		ASSERT_TRUE(ShaderCompileUtilities::TryReuseMetaData(
			Loaded, Warm, bCurrent, Error)) << Error;
		EXPECT_TRUE(bCurrent);
		EXPECT_EQ(Warm.GetContentReadCount(), 0u);
	}
}
