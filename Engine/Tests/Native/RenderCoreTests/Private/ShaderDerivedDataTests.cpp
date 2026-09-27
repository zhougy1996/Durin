#include "Shader/ShaderCompilerCore.h"
#include "ShaderBuild/ShaderPaths.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/FileFingerprintCache.h"
#include "NativeTestSupport.h"
#include "Serialization/BinaryFormat.h"
#include "ShaderCompileUtilities.h"
#include "ShaderDependencyManifestStore.h"
#include "Shader/ShaderCompiledOutput.h"
#include "ShaderBuildSession.h"
#include "ShaderSharedOutput.h"

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
			Shader.Code = std::make_shared<const FSharedByteBuffer>(FSharedByteBuffer::Copy(std::as_bytes(std::span(Words))));
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
			Output.Error = {};
			Output.CompiledShaders.push_back(MakeShader(
				"VertexMain", EShaderFrequency::Vertex, 1));
			Output.CompiledShaders.push_back(MakeShader(
				"FragmentMain", EShaderFrequency::Fragment, 2));
			return Output;
		}

		auto WriteU32At(Durin::FByteBuffer& Bytes,
			size_t Offset, uint32 Value) -> void
		{
			ASSERT_LE(Offset + sizeof(Value), Bytes.size());
			for (size_t Index = 0; Index < sizeof(Value); ++Index)
				Bytes[Offset + Index] = static_cast<std::byte>(
					(Value >> (Index * 8)) & 0xffu);
		}

		auto ToHex(Durin::FByteView Bytes) -> std::string
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
		Durin::FByteBuffer First;
		Durin::FByteBuffer Second;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = ShaderCompiledOutput::Encode(Options, Expected, First))) << FormatShaderError(Error.error());
		ASSERT_TRUE((Error = ShaderCompiledOutput::Encode(Options, Expected, Second))) << FormatShaderError(Error.error());
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
		EXPECT_EQ(Magic, ShaderCompiledOutput::PayloadMagic);
		EXPECT_EQ(Schema, ShaderCompiledOutput::PayloadSchemaVersion);
		EXPECT_EQ(Builder, ShaderCompiledOutput::BuilderVersion);

		FShaderCompilerOutput Loaded;
		ASSERT_TRUE((Error = ShaderCompiledOutput::Decode(First, Options, Loaded))) << FormatShaderError(Error.error());
		ASSERT_EQ(Loaded.CompiledShaders.size(), 2u);
		for (size_t Index = 0; Index < Loaded.CompiledShaders.size(); ++Index)
		{
			const FCompiledShader& Actual = Loaded.CompiledShaders[Index];
			const FCompiledShader& Wanted = Expected.CompiledShaders[Index];
			EXPECT_EQ(Actual.SourceEntryPoint, Wanted.SourceEntryPoint);
			EXPECT_EQ(Actual.Frequency, Wanted.Frequency);
			EXPECT_EQ(Actual.Hash, Wanted.Hash);
			EXPECT_TRUE(std::ranges::equal(Actual.Code->GetBytes(), Wanted.Code->GetBytes()));
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
		Durin::FByteBuffer Bytes;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = ShaderCompiledOutput::Encode(Options, Output, Bytes))) << FormatShaderError(Error.error());
		EXPECT_EQ(ToHex(Bytes),
			"4453484401000000010000000403020100000000010000000a000000000000005665727465784d61696e04000000000000006d61696e00000000000000000f000000000000005665727465784d61696e4465627567cf9a2d3c094317863728ab64520b8eeb140000000000000003022307000501000000000001000000000000000100000005000000000000005363656e6501000000000000000100000000000000010000000100000001000000000000001000000000000000");
	}

	TEST_F(FShaderDerivedDataTests, RejectsMalformedValuesWithoutPartialOutput)
	{
		const FShaderCompileOptions Options = MakeOptions();
		Durin::FByteBuffer Bytes;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = ShaderCompiledOutput::Encode(Options, MakeOutput(), Bytes))) << FormatShaderError(Error.error());
		auto ExpectRejected = [&](Durin::FByteBuffer Candidate, EShaderError Code) {
			FShaderCompilerOutput Loaded;
			Loaded.Error = {};
			Loaded.CompiledShaders.push_back(MakeShader(
				"Old", EShaderFrequency::Vertex, 1));
			EXPECT_FALSE((Error = ShaderCompiledOutput::Decode(Candidate, Options, Loaded)));
			EXPECT_EQ(Error.error().Code, Code);
			EXPECT_FALSE(static_cast<bool>(Loaded));
			EXPECT_TRUE(Loaded.CompiledShaders.empty());
		};

		Durin::FByteBuffer BadMagic = Bytes;
		BadMagic[0] = std::byte{0};
		ExpectRejected(std::move(BadMagic), EShaderError::PayloadHeaderInvalid);
		Durin::FByteBuffer BadVersion = Bytes;
		WriteU32At(BadVersion, 4,
			ShaderCompiledOutput::PayloadSchemaVersion + 1);
		ExpectRejected(std::move(BadVersion), EShaderError::PayloadHeaderInvalid);
		Durin::FByteBuffer Reserved = Bytes;
		Reserved[16] = std::byte{1};
		ExpectRejected(std::move(Reserved), EShaderError::PayloadHeaderInvalid);
		Durin::FByteBuffer Truncated = Bytes;
		Truncated.pop_back();
		ExpectRejected(std::move(Truncated), EShaderError::PayloadPushConstantInvalid);
		Durin::FByteBuffer Trailing = Bytes;
		Trailing.push_back(std::byte{0});
		ExpectRejected(std::move(Trailing), EShaderError::PayloadTrailingBytes);
		Durin::FByteBuffer CorruptCode = Bytes;
		const auto It = std::ranges::search(CorruptCode,
			std::array{std::byte{0x03}, std::byte{0x02},
				std::byte{0x23}, std::byte{0x07}});
		ASSERT_NE(It.begin(), CorruptCode.end());
		*It.begin() = std::byte{0};
		ExpectRejected(std::move(CorruptCode), EShaderError::PayloadSpirvInvalid);
		Durin::FByteBuffer BadFrequency = Bytes;
		WriteU32At(BadFrequency, 54,
			static_cast<uint32>(EShaderFrequency::RayMiss) + 1);
		ExpectRejected(std::move(BadFrequency), EShaderError::PayloadEntryInvalid);
		Durin::FByteBuffer BadHash = Bytes;
		BadHash[85] ^= std::byte{1};
		ExpectRejected(std::move(BadHash), EShaderError::PayloadSpirvHashMismatch);
		Durin::FByteBuffer BadBindingCount = Bytes;
		WriteU32At(BadBindingCount, 129, 65537);
		ExpectRejected(std::move(BadBindingCount), EShaderError::PayloadBindingCountInvalid);

		FShaderCompileOptions WrongRequest = Options;
		WrongRequest.EntryPoints[0] = "WrongMain";
		FShaderCompilerOutput Loaded;
		EXPECT_FALSE((Error = ShaderCompiledOutput::Decode(Bytes, WrongRequest, Loaded)));
		EXPECT_EQ(Error.error().Code, EShaderError::PayloadEntryInvalid);
		EXPECT_EQ(Error.error().Index, 0u);
		EXPECT_TRUE(Loaded.CompiledShaders.empty());
	}

	TEST_F(FShaderDerivedDataTests, OutputKeyIncludesExactRequestButNotForcePolicy)
	{
		const FShaderVariantKey Variant{
			.Value = FXxHash128::FromString(
				"00112233445566778899aabbccddeeff"),
			.Hex = "00112233445566778899aabbccddeeff"};
		FShaderCompileOptions Options = MakeOptions();
		auto Key = [&](const FShaderCompileOptions& RequestOptions) -> DerivedData::FCacheKey {
			auto Request = MakeShaderSessionRequest(RequestOptions, Variant, {}, std::nullopt,
				[]() -> std::expected<std::shared_ptr<const FShaderSourceArtifacts>, FShaderError> { return std::make_shared<const FShaderSourceArtifacts>(std::map<std::string, FSharedByteBuffer>{}); });
			if (!Request) return {};
			auto Inputs = Request->Resolver->Describe(Request->Definition.GetSources(), {});
			if (!Inputs) return {};
			auto Action = DerivedData::FBuildAction::TryCreate(Request->Definition,
				{"Durin.Shader.Compile", 2, 1, "Shader.Output", 1, DerivedData::FCacheBucket::FromString("Shaders/CompiledOutput")}, std::move(*Inputs));
			return Action ? Action->GetKey() : DerivedData::FCacheKey{};
		};
		const auto First = Key(Options);
		ASSERT_TRUE(First.IsValid());
		// New action identity cannot alias the frozen pre-session key.
		EXPECT_NE(First.ToString(), "34e7cc7d296f3588e078eec6cd12b469");
		Options.bForceRecompile = true;
		EXPECT_EQ(Key(Options), First);
		Options.EntryPoints[1] = "OtherMain";
		EXPECT_NE(Key(Options), First);
		Options.EntryPoints[1] = Options.EntryPoints[0];
		Options.Frequencies[1] = Options.Frequencies[0];
		EXPECT_FALSE(Key(Options).IsValid());
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
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = ShaderCompileUtilities::BuildShaderMetaData({Dependency.generic_string()}, Cold, MetaData))) << FormatShaderError(Error.error());
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
		const ShaderCompileUtilities::FMetaDataReuseResult ReuseResult =
			ShaderCompileUtilities::TryReuseMetaData(Loaded, Warm);
		EXPECT_EQ(ReuseResult.Status,
			ShaderCompileUtilities::EMetaDataReuseStatus::Current);
		EXPECT_TRUE(ReuseResult.Error.IsSuccess());
		EXPECT_EQ(Warm.GetContentReadCount(), 0u);

		Loaded.PortableDependencies.front().ContentHash = {};
		const ShaderCompileUtilities::FMetaDataReuseResult StaleResult =
			ShaderCompileUtilities::TryReuseMetaData(Loaded, Warm);
		EXPECT_EQ(StaleResult.Status,
			ShaderCompileUtilities::EMetaDataReuseStatus::Stale);
		EXPECT_TRUE(StaleResult.Error.IsSuccess());
	}
	TEST_F(FShaderDerivedDataTests, SharedOutputRetainsColdCodeAcrossProducerAndConsumerCopies)
	{
		static_assert(std::is_same_v<decltype(FCompiledShader::Code), std::shared_ptr<const FSharedByteBuffer>>);
		const auto Options = MakeOptions();
		auto Product = MakeOutput();
		const auto* Bytes = Product.CompiledShaders[0].Code->data();
		auto Output = ShaderSharedOutput::Make(Options, Product); ASSERT_TRUE(Output);
		EXPECT_EQ(Output->FindValue("Entry/0/Code")->Data.data(), Bytes);
		Product = {};
		ASSERT_TRUE(ShaderSharedOutput::Validate(Options, *Output));
		auto Assembled = ShaderSharedOutput::Assemble(Options, *Output); ASSERT_TRUE(Assembled);
		auto Waiter = *Assembled;
		Output = DerivedData::FBuildOutput{}; Assembled = FShaderCompilerOutput{};
		EXPECT_EQ(Waiter.CompiledShaders[0].Code->data(), Bytes);
		EXPECT_EQ(FXxHash128::HashBuffer(*Waiter.CompiledShaders[0].Code), Waiter.CompiledShaders[0].Hash);
		EXPECT_EQ(Waiter.CompiledShaders[0].DebugName, Options.VirtualShaderPath + "::VertexMain");
		EXPECT_EQ(Waiter.CompiledShaders[0].Reflection.ResourceBindings, MakeOutput().CompiledShaders[0].Reflection.ResourceBindings);
	}

	TEST_F(FShaderDerivedDataTests, SharedOutputRetainsRawAndCompressedRecordCodeWithoutPackageChanges)
	{
		using namespace DerivedData;
		const auto Options = MakeOptions();
		for (bool Compressed : {false, true})
		{
			auto Product = MakeOutput();
			for (auto& Shader : Product.CompiledShaders) Shader.DebugName = Options.VirtualShaderPath + "::" + Shader.SourceEntryPoint;
			FByteBuffer Before; ASSERT_TRUE(ShaderCompiledOutput::Encode(Options, Product, Before));
			auto Output = ShaderSharedOutput::Make(Options, Product); ASSERT_TRUE(Output);
			const auto Key = FCacheKey::FromHash(FCacheBucket::FromString("ShaderOutputFixture"), FXxHash128::HashBuffer("shader"));
			auto Record = FCacheRecord::FromOutput(Key, *Output); ASSERT_TRUE(Record);
			auto Encoded = Record->Encode(); ASSERT_TRUE(Encoded);
			if (Compressed) Encoded = FCacheRecord::CompressEncoded(*Encoded);
			ASSERT_TRUE(Encoded);
			auto Loaded = FCacheRecord::Decode(Key, *Encoded); ASSERT_TRUE(Loaded);
			auto Warm = Loaded->ToOutput(Key); ASSERT_TRUE(Warm);
			const auto* Code = Warm->FindValue("Entry/0/Code")->Data.data();
			auto Assembled = ShaderSharedOutput::Assemble(Options, *Warm); ASSERT_TRUE(Assembled);
			EXPECT_EQ(Assembled->CompiledShaders[0].Code->data(), Code);
			Warm = FBuildOutput{}; Loaded = FCacheRecord{}; Encoded = FSharedByteBuffer{};
			FByteBuffer After; ASSERT_TRUE(ShaderCompiledOutput::Encode(Options, *Assembled, After));
			EXPECT_EQ(Before, After);
		}
	}

	TEST_F(FShaderDerivedDataTests, SharedOutputRejectsMalformedMetadataCodeAndReflection)
	{
		using namespace DerivedData;
		const auto Options = MakeOptions();
		auto Output = ShaderSharedOutput::Make(Options, MakeOutput()); ASSERT_TRUE(Output);
		for (uint32 Fault = 0; Fault < 9; ++Fault)
		{
			SCOPED_TRACE(Fault);
			FBuildOutputData Data{.Schema = std::string(Output->GetSchema()), .SchemaVersion = Output->GetSchemaVersion(),
				.Metadata = Output->GetMetadata(), .Values = {Output->GetValues().begin(), Output->GetValues().end()}};
			if (Fault == 0) ++Data.SchemaVersion;
			if (Fault == 1) Data.Metadata = FSharedByteBuffer::Copy(Data.Metadata.GetBytes().first(4));
			if (Fault == 2) Data.Values.pop_back();
			if (Fault == 3) Data.Values.push_back({"Unexpected", FSharedByteBuffer::Take(FByteBuffer(1))});
			if (Fault >= 4) for (auto& Value : Data.Values)
			{
				if ((Fault < 6 && Value.Id == "Entry/0/Code") || (Fault >= 6 && Value.Id == "Entry/0/Reflection"))
				{
					FByteBuffer Bytes(Value.Data.begin(), Value.Data.end());
					if (Fault == 4) Bytes[0] ^= std::byte{1};
					if (Fault == 5) Bytes[12] ^= std::byte{1};
					if (Fault == 6) WriteU32At(Bytes, 0, 65537);
					if (Fault == 7) Bytes.pop_back();
					if (Fault == 8) Bytes.push_back(std::byte{});
					Value.Data = FSharedByteBuffer::Take(std::move(Bytes));
				}
			}
			auto Bad = FBuildOutput::TryCreate(std::move(Data)); ASSERT_TRUE(Bad);
			EXPECT_FALSE(ShaderSharedOutput::Validate(Options, *Bad));
			EXPECT_FALSE(ShaderSharedOutput::Assemble(Options, *Bad));
		}
		uint32 Checks = 0;
		auto Cancelled = ShaderSharedOutput::Validate(Options, *Output, [&] { return ++Checks == 3; });
		ASSERT_FALSE(Cancelled); EXPECT_EQ(Cancelled.error().Code, EShaderError::Cancelled);
		EXPECT_FALSE(ShaderSharedOutput::Assemble(Options, *Output, [] { return true; }));
		EXPECT_FALSE(ShaderSharedOutput::Make(Options, MakeOutput(), [] { return true; }));
		for (uint32 Fault = 0; Fault < 4; ++Fault)
		{
			auto Product = MakeOutput();
			if (Fault == 0) Product.CompiledShaders[0].Reflection.ResourceBindings[0].SetIndex = 65536;
			if (Fault == 1) Product.CompiledShaders[0].Reflection.ResourceBindings[0].ArraySize = 0;
			if (Fault == 2) Product.CompiledShaders[0].Reflection.PushConstantRanges[0].Size = 65537;
			if (Fault == 3) Product.CompiledShaders[0].Reflection.PushConstantRanges[0].Offset = 65535;
			EXPECT_FALSE(ShaderSharedOutput::Make(Options, Product));
		}
	}

}
