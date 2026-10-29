#include "BuildCacheRecordPrivate.h"
#include "CacheRecordFixtureCodec.h"
#include "MetalShaderFormat.h"
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
			Options.Target = VulkanShaderTarget;
			Options.VirtualShaderPath = "/ShaderDerivedDataTests/Test";
			Options.EntryPoints = {"VertexMain", "FragmentMain"};
			Options.Frequencies = {
				EShaderFrequency::Vertex, EShaderFrequency::Fragment};
			Options.CompilerEnvironment = "slang-test-spirv";
			return Options;
		}

		auto MakeShader(std::string_view EntryPoint,
			EShaderFrequency Frequency, uint32 Bound) -> FShaderCode
		{
			const std::array<uint32, 5> Words = {
				0x07230203u, 0x00010500u, 0u, Bound, 0u};
			FShaderCode Shader;
			Shader.Frequency = Frequency;
			Shader.SourceEntryPoint = EntryPoint;
			Shader.BinaryEntryPoint = "main";
			Shader.DebugName = std::string(EntryPoint) + "Debug";
			Shader.Code = FSharedByteBuffer::Copy(std::as_bytes(std::span(Words)));
			Shader.CodeHash = FXxHash128::HashBuffer(Shader.Code);
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
			Output.Status = EShaderBuildStatus::Ok;
			Output.CompiledShaders.push_back(MakeShader(
				"VertexMain", EShaderFrequency::Vertex, 1));
			Output.CompiledShaders.push_back(MakeShader(
				"FragmentMain", EShaderFrequency::Fragment, 2));
			return Output;
		}

		auto MakeMetalOutput() -> FShaderCompilerOutput
		{
			auto Output = MakeOutput();
			for (auto& Shader : Output.CompiledShaders)
			{
				Shader.Target = MetalShaderTarget;
				Shader.CodeFormat = EShaderCodeFormat::Msl20Source;
				Shader.BinaryEntryPoint = "main0";
				const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\nvoid main0() {}\n";
				Shader.Code = FSharedByteBuffer::Copy(std::as_bytes(std::span(Source)));
				Shader.CodeHash = FXxHash128::HashBuffer(Shader.Code);
				auto Map = ShaderFormat::BuildMetalBindingMap(Shader.Frequency, ShaderFormat::BuildLogicalBindings(Shader.Reflection), !Shader.Reflection.PushConstantRanges.empty());
				if (!Map) return FShaderCompilerOutput::Failure("Metal shader binding remap exceeds or conflicts with native slots");
				Shader.PlatformData = ShaderFormat::EncodeMetalMetadata(Shader.Frequency,
					{std::move(Map->Bindings), Map->PushConstantBufferSlot, Map->Identity}).value();
			}
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

		struct FMutableShaderOutput
		{
			std::string Schema;
			uint32 SchemaVersion = 0;
			FSharedByteBuffer Metadata;
			std::vector<std::pair<DerivedData::FValueId, FSharedByteBuffer>> Values;
			auto Build() && -> std::expected<DerivedData::FBuildOutput, std::string>
			{
				DerivedData::FBuildOutputBuilder Builder(std::move(Schema), SchemaVersion);
				auto Object = DerivedData::MakeBuildMetadata(Metadata);
				if (!Object || !Builder.AddMeta(DerivedData::FValueId::FromName("Metadata"), std::move(*Object)))
					return std::unexpected("Invalid test metadata.");
				for (auto& [Id, Data] : Values) if (!Builder.AddValue(Id, std::move(Data)))
					return std::unexpected("Invalid test value.");
				return std::move(Builder).Build();
			}
		};
		auto CopyShaderOutput(const DerivedData::FBuildOutput& Output) -> FMutableShaderOutput
		{
			FMutableShaderOutput Copy{.Schema = std::string(Output.GetSchema()), .SchemaVersion = Output.GetSchemaVersion(),
				.Metadata = DerivedData::GetBuildMetadataPayload(Output)};
			for (const auto& Value : Output.GetValues()) Copy.Values.emplace_back(Value.Id, Value.Value.GetData());
			return Copy;
		}
		auto ShaderValueId(uint32 Entry, bool Reflection) -> DerivedData::FValueId
		{ return DerivedData::FValueId::FromName("Durin.Shader.EntryValue").MakeIndexed(Entry * 2 + uint32(Reflection)); }
	}

	TEST_F(FShaderDerivedDataTests, CompleteMultiStagePayloadRoundTrips)
	{
		const FShaderCompileOptions Options = MakeOptions();
		const FShaderCompilerOutput Expected = MakeOutput();
		Durin::FByteBuffer First;
		Durin::FByteBuffer Second;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = ShaderCompiledOutput::Encode(Options, Expected, First))) << std::string(Error.error());
		ASSERT_TRUE((Error = ShaderCompiledOutput::Encode(Options, Expected, Second))) << std::string(Error.error());
		EXPECT_EQ(First, Second);
		EXPECT_EQ(ToHex(First),
			"445348440600000004030201000000000100000001000000010000000100000000000000000000000000000000000000020000000a000000000000005665727465784d61696e04000000000000006d61696e00000000010000000000000000000000000000000f000000000000005665727465784d61696e4465627567cf9a2d3c094317863728ab64520b8eeb140000000000000003022307000501000000000001000000000000000100000005000000000000005363656e6501000000000000000100000000000000010000000100000001000000000000001000000000000000010000000000000000000000301e0caed88d833d17a8187ca4344e5d0c00000000000000467261676d656e744d61696e04000000000000006d61696e01000000010000000000000000000000000000001100000000000000467261676d656e744d61696e446562756776835ac38ce6e7a67c3015d75a3a6f26140000000000000003022307000501000000000002000000000000000100000005000000000000005363656e650200000000000000020000000000000001000000010000000200000000000000100000000000000001000000000000000000000025e865d6966669a47adfd8028841b997");
		ASSERT_GE(First.size(), 24u);
		uint32 Magic = 0;
		uint32 Schema = 0;
		uint32 Marker = 0;
		EXPECT_TRUE(ReadLittleEndianAt<uint32>(First, 0, Magic));
		EXPECT_TRUE(ReadLittleEndianAt<uint32>(First, 4, Schema));
		EXPECT_TRUE(ReadLittleEndianAt<uint32>(First, 8, Marker));
		EXPECT_EQ(Magic, ShaderCompiledOutput::PayloadMagic);
		EXPECT_EQ(Schema, ShaderCompiledOutput::PayloadSchemaVersion);
		EXPECT_EQ(Marker, BinaryFormatMarker);

		FShaderCompilerOutput Loaded;
		ASSERT_TRUE((Error = ShaderCompiledOutput::Decode(First, Options, Loaded))) << std::string(Error.error());
		ASSERT_EQ(Loaded.CompiledShaders.size(), 2u);
		for (size_t Index = 0; Index < Loaded.CompiledShaders.size(); ++Index)
		{
			const FShaderCode& Actual = Loaded.CompiledShaders[Index];
			const FShaderCode& Wanted = Expected.CompiledShaders[Index];
			EXPECT_EQ(Actual.Target, Wanted.Target);
			EXPECT_EQ(Actual.CodeFormat, Wanted.CodeFormat);
			EXPECT_EQ(Actual.SourceEntryPoint, Wanted.SourceEntryPoint);
			EXPECT_EQ(Actual.Frequency, Wanted.Frequency);
			EXPECT_EQ(Actual.ComputeThreadGroupSize, Wanted.ComputeThreadGroupSize);
			EXPECT_EQ(Actual.CodeHash, Wanted.CodeHash);
			EXPECT_TRUE(std::ranges::equal(Actual.Code.GetBytes(), Wanted.Code.GetBytes()));
			EXPECT_EQ(Actual.Reflection.ResourceBindings,
				Wanted.Reflection.ResourceBindings);
			EXPECT_EQ(Actual.Reflection.PushConstantRanges,
				Wanted.Reflection.PushConstantRanges);
		}
	}


	TEST_F(FShaderDerivedDataTests, SessionRejectsMismatchedIdentityBeforeOptionsDecoding)
	{
		FShaderBuildService Service;
		const uint64 Version = Service.GetBuildVersion();
		EXPECT_NE(Version, 0u);
		EXPECT_EQ(Version, Service.GetBuildVersion());
		auto Options = MakeOptions();
		Options.CompilerEnvironment = Service.GetCompilerEnvironmentIdentity();
		const FShaderVariantKey Variant{.Value = {17, 29}};
		auto Request = MakeShaderSessionRequest(Options, Variant, Version ^ (uint64{1} << 63), {}, std::nullopt,
			[]() -> std::expected<std::shared_ptr<const FShaderSourceArtifacts>, std::string> {
				return std::make_shared<const FShaderSourceArtifacts>(std::map<std::string, FSharedByteBuffer>{});
			});
		ASSERT_TRUE(Request);
		DerivedData::FBuildDefinitionBuilder Definition("Durin.Shader.Compile");
		Definition.AddConstant("BuilderVersion", Version ^ (uint64{1} << 63))
			.AddConstant("Options", std::string("invalid"))
			.AddInput("Closure", "CapturedShaderClosure");
		auto InvalidOptions = std::move(Definition).Build();
		ASSERT_TRUE(InvalidOptions);
		Request->Definition = std::move(*InvalidOptions);
		const auto Result = Service.Execute(std::move(*Request), {.Policy = DerivedData::EBuildPolicy::Build});
		EXPECT_EQ(Result.GetStatus(), DerivedData::EStatus::Error);
		EXPECT_EQ(Result.GetOutput(), nullptr);
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
		ASSERT_TRUE((Error = ShaderCompiledOutput::Encode(Options, Output, Bytes))) << std::string(Error.error());
		EXPECT_EQ(ToHex(Bytes),
			"445348440600000004030201000000000100000001000000010000000100000000000000000000000000000000000000010000000a000000000000005665727465784d61696e04000000000000006d61696e00000000010000000000000000000000000000000f000000000000005665727465784d61696e4465627567cf9a2d3c094317863728ab64520b8eeb140000000000000003022307000501000000000001000000000000000100000005000000000000005363656e6501000000000000000100000000000000010000000100000001000000000000001000000000000000010000000000000000000000301e0caed88d833d17a8187ca4344e5d");
	}

	TEST_F(FShaderDerivedDataTests, MetalComputeThreadGroupSurvivesBothPayloads)
	{
		FShaderCompileOptions Options = MakeOptions();
		Options.Target = MetalShaderTarget;
		Options.EntryPoints = {"ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Compute};
		FShaderCode Shader;
		Shader.Target = MetalShaderTarget;
		Shader.CodeFormat = EShaderCodeFormat::Msl20Source;
		Shader.Frequency = EShaderFrequency::Compute;
		Shader.ComputeThreadGroupSize = {8, 4, 2};
		Shader.SourceEntryPoint = "ComputeMain";
		Shader.BinaryEntryPoint = "main0";
		const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\n"
			"kernel void main0(uint3 tid [[thread_position_in_grid]]) {}\n";
		Shader.Code = FSharedByteBuffer::Copy(std::as_bytes(std::span(Source)));
		Shader.CodeHash = FXxHash128::HashBuffer(Shader.Code);
		auto Map = ShaderFormat::BuildMetalBindingMap(Shader.Frequency, ShaderFormat::BuildLogicalBindings(Shader.Reflection), !Shader.Reflection.PushConstantRanges.empty());
		ASSERT_TRUE(Map);
		Shader.PlatformData = ShaderFormat::EncodeMetalMetadata(Shader.Frequency,
			{std::move(Map->Bindings), Map->PushConstantBufferSlot, Map->Identity}).value();
		FShaderCompilerOutput Product;
		Product.Status = EShaderBuildStatus::Ok;
		Product.CompiledShaders.push_back(std::move(Shader));
		Durin::FByteBuffer Bytes;
		ASSERT_TRUE(ShaderCompiledOutput::Encode(Options, Product, Bytes));
		FShaderCompilerOutput Decoded;
		ASSERT_TRUE(ShaderCompiledOutput::Decode(Bytes, Options, Decoded));
		ASSERT_EQ(Decoded.CompiledShaders.size(), 1u);
		EXPECT_EQ(Decoded.CompiledShaders[0].ComputeThreadGroupSize,
			(std::array<uint32, 3>{8, 4, 2}));
		auto Shared = ShaderSharedOutput::Make(Options, Product);
		ASSERT_TRUE(Shared) << Shared.Error;
		auto Restored = ShaderSharedOutput::Assemble(Options, *Shared);
		ASSERT_TRUE(Restored) << FormatShaderCompilerErrors(Restored.Errors);
		ASSERT_EQ(Restored.CompiledShaders.size(), 1u);
		EXPECT_EQ(Restored.CompiledShaders[0].ComputeThreadGroupSize,
			(std::array<uint32, 3>{8, 4, 2}));
	}

	TEST_F(FShaderDerivedDataTests, SuccessfulDiagnosticsSurvivePersistentCacheRoundTrip)
	{
		using namespace DerivedData;
		auto Product = MakeOutput(); Product.AddDiagnostics("/Test/Main.slang(12): warning: first\nwarning: second");
		ASSERT_TRUE(Product); ASSERT_EQ(Product.Errors.size(), 2u);
		auto Built = ShaderSharedOutput::Make(MakeOptions(), Product); ASSERT_TRUE(Built) << Built.Error;
		const auto Key = FCacheKey::FromHash(FCacheBucket::FromString("ShaderDiagnostics"), FXxHash128::HashBuffer("diagnostics"));
		auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key, *Built);
		ASSERT_TRUE(Record);
		auto Encoded = FCacheRecordFixtureCodec::Encode(*Record); ASSERT_TRUE(Encoded);
		auto Loaded = FCacheRecordFixtureCodec::Decode(Key, *Encoded); ASSERT_TRUE(Loaded);
		auto Cached = Durin::DerivedData::Private::ReadBuildOutput(Key, *Loaded);
		ASSERT_TRUE(Cached);
		auto Restored = ShaderSharedOutput::Assemble(MakeOptions(), *Cached); ASSERT_TRUE(Restored);
		ASSERT_EQ(Restored.Errors.size(), 2u); EXPECT_EQ(Restored.Errors[0].ErrorVirtualFilePath, "/Test/Main.slang");
		EXPECT_EQ(Restored.Errors[0].ErrorLineString, "12"); EXPECT_EQ(Restored.Errors[1].StrippedErrorMessage, "warning: second");
		EXPECT_EQ(Restored.CompiledShaders.size(), Product.CompiledShaders.size());
	}

	TEST_F(FShaderDerivedDataTests, RejectsMalformedValuesWithoutPartialOutput)
	{
		const FShaderCompileOptions Options = MakeOptions();
		Durin::FByteBuffer Bytes;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = ShaderCompiledOutput::Encode(Options, MakeOutput(), Bytes))) << std::string(Error.error());
		auto ExpectRejected = [&](Durin::FByteBuffer Candidate) {
			FShaderCompilerOutput Loaded;
			Loaded.Status = EShaderBuildStatus::Ok;
			Loaded.CompiledShaders.push_back(MakeShader(
				"Old", EShaderFrequency::Vertex, 1));
			ASSERT_FALSE((Error = ShaderCompiledOutput::Decode(Candidate, Options, Loaded)));
			EXPECT_FALSE(static_cast<bool>(Loaded));
			EXPECT_TRUE(Loaded.CompiledShaders.empty());
		};

		Durin::FByteBuffer BadMagic = Bytes;
		BadMagic[0] = std::byte{0};
		ExpectRejected(std::move(BadMagic));
		Durin::FByteBuffer BadMarker = Bytes;
		BadMarker[8] ^= std::byte{0x40};
		ExpectRejected(std::move(BadMarker));
		Durin::FByteBuffer BadVersion = Bytes;
		WriteU32At(BadVersion, 4,
			ShaderCompiledOutput::PayloadSchemaVersion + 1);
		ExpectRejected(std::move(BadVersion));
		Durin::FByteBuffer LegacyVersion = Bytes;
		WriteU32At(LegacyVersion, 4,
			ShaderCompiledOutput::PayloadSchemaVersion - 1);
		ExpectRejected(std::move(LegacyVersion));
		Durin::FByteBuffer Reserved = Bytes;
		Reserved[12] = std::byte{1};
		ExpectRejected(std::move(Reserved));
		Durin::FByteBuffer WrongTarget = Bytes;
		WriteU32At(WrongTarget, 16, uint32(EShaderTargetPlatform::MacOS));
		ExpectRejected(std::move(WrongTarget));
		Durin::FByteBuffer WrongOutputFormat = Bytes;
		WriteU32At(WrongOutputFormat, 28, uint32(EShaderCodeFormat::Msl20Source));
		ExpectRejected(std::move(WrongOutputFormat));
		Durin::FByteBuffer Truncated = Bytes;
		Truncated.pop_back();
		ExpectRejected(std::move(Truncated));
		Durin::FByteBuffer Trailing = Bytes;
		Trailing.push_back(std::byte{0});
		ExpectRejected(std::move(Trailing));
		Durin::FByteBuffer CorruptCode = Bytes;
		const auto It = std::ranges::search(CorruptCode,
			std::array{std::byte{0x03}, std::byte{0x02},
				std::byte{0x23}, std::byte{0x07}});
		ASSERT_NE(It.begin(), CorruptCode.end());
		*It.begin() = std::byte{0};
		ExpectRejected(std::move(CorruptCode));
		Durin::FByteBuffer BadFrequency = Bytes;
		WriteU32At(BadFrequency, 82,
			static_cast<uint32>(EShaderFrequency::RayMiss) + 1);
		ExpectRejected(std::move(BadFrequency));
		Durin::FByteBuffer BadFormat = Bytes;
		WriteU32At(BadFormat, 86, uint32(EShaderCodeFormat::Msl20Source));
		ExpectRejected(std::move(BadFormat));
		Durin::FByteBuffer BadHash = Bytes;
		BadHash[125] ^= std::byte{1};
		ExpectRejected(std::move(BadHash));
		Durin::FByteBuffer BadBindingCount = Bytes;
		WriteU32At(BadBindingCount, 169, 65537);
		ExpectRejected(std::move(BadBindingCount));

		FShaderCompileOptions WrongRequest = Options;
		WrongRequest.EntryPoints[0] = "WrongMain";
		FShaderCompilerOutput Loaded;
		EXPECT_FALSE((Error = ShaderCompiledOutput::Decode(Bytes, WrongRequest, Loaded)));
		EXPECT_TRUE(Loaded.CompiledShaders.empty());

		auto WrongStage = MakeOutput();
		WrongStage.CompiledShaders[0].Target = MetalShaderTarget;
		EXPECT_FALSE((Error = ShaderCompiledOutput::Encode(Options, WrongStage, Bytes)));
		WrongStage = MakeOutput();
		WrongStage.CompiledShaders[0].CodeFormat = EShaderCodeFormat::Msl20Source;
		EXPECT_FALSE((Error = ShaderCompiledOutput::Encode(Options, WrongStage, Bytes)));
	}

	TEST_F(FShaderDerivedDataTests, OutputKeyIncludesExactRequestButNotForcePolicy)
	{
		const FShaderVariantKey Variant{
			.Value = FXxHash128::FromString(
				"00112233445566778899aabbccddeeff"),
			.Hex = "00112233445566778899aabbccddeeff"};
		FShaderCompileOptions Options = MakeOptions();
		auto Key = [&](const FShaderCompileOptions& RequestOptions, uint64 BuilderVersion = 0x123456789abcdef0ull,
			uint32 FunctionVersion = ShaderCompileBuildFunctionVersion) -> DerivedData::FCacheKey {
			auto Request = MakeShaderSessionRequest(RequestOptions, Variant, BuilderVersion, {}, std::nullopt,
				[]() -> std::expected<std::shared_ptr<const FShaderSourceArtifacts>, std::string> { return std::make_shared<const FShaderSourceArtifacts>(std::map<std::string, FSharedByteBuffer>{}); });
			if (!Request) return {};
			const auto Identities = Request->Resolver->Describe(Request->Definition.GetSources(), {});
			if (!Identities) return {};
			auto Descriptor = GetShaderBuildDescriptor();
			Descriptor.Version = FunctionVersion;
			DerivedData::FBuildActionBuilder Builder(Request->Definition, Descriptor);
			for (const auto& Identity : *Identities) Builder.AddInput(Identity);
			auto Action = std::move(Builder).Build();
			return Action ? Action->GetKey() : DerivedData::FCacheKey{};
		};
		const auto First = Key(Options);
		ASSERT_TRUE(First.IsValid());
		EXPECT_NE(Key(Options, 0x123456789abcdef0ull ^ (uint64{1} << 63)), First);
		EXPECT_NE(Key(Options, 0x123456789abcdef0ull, ShaderCompileBuildFunctionVersion + 1), First);
		EXPECT_FALSE(Key(Options, 0).IsValid());
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
			"/Engine/Test", First, Macros, "compiler", VulkanShaderTarget, FirstKey);
		ShaderCompileUtilities::BuildVariantKey(
			"/Engine/Test", Second, Macros, "compiler", VulkanShaderTarget, SecondKey);
		EXPECT_EQ(FirstKey.Value, SecondKey.Value);
		FShaderVariantKey MetalKey;
		ShaderCompileUtilities::BuildVariantKey(
			"/Engine/Test", First, Macros, "compiler", MetalShaderTarget, MetalKey);
		EXPECT_NE(FirstKey.Value, MetalKey.Value);
		FShaderDependencyKey VulkanDependency;
		FShaderDependencyKey MetalDependency;
		ShaderCompileUtilities::BuildDependencyKey(
			"/Engine/Test", Macros, "compiler", VulkanShaderTarget, VulkanDependency);
		ShaderCompileUtilities::BuildDependencyKey(
			"/Engine/Test", Macros, "compiler", MetalShaderTarget, MetalDependency);
		EXPECT_NE(VulkanDependency.Value, MetalDependency.Value);
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
		ASSERT_TRUE((Error = ShaderCompileUtilities::BuildShaderMetaData({Dependency.generic_string()}, Cold, MetaData))) << std::string(Error.error());
		ASSERT_EQ(MetaData.PortableDependencies.size(), 1u);
		EXPECT_EQ(MetaData.PortableDependencies.front().VirtualPath,
			"/ShaderDerivedDataTests/Common");

		FShaderDependencyKey Key;
		ShaderCompileUtilities::BuildDependencyKey(
			"/ShaderDerivedDataTests/Test", {}, "compiler", VulkanShaderTarget, Key);
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
		EXPECT_TRUE(ReuseResult.Error.empty());
		EXPECT_EQ(Warm.GetContentReadCount(), 0u);

		Loaded.PortableDependencies.front().ContentHash = {};
		const ShaderCompileUtilities::FMetaDataReuseResult StaleResult =
			ShaderCompileUtilities::TryReuseMetaData(Loaded, Warm);
		EXPECT_EQ(StaleResult.Status,
			ShaderCompileUtilities::EMetaDataReuseStatus::Stale);
		EXPECT_TRUE(StaleResult.Error.empty());
	}
	TEST_F(FShaderDerivedDataTests, SharedOutputRetainsColdCodeAcrossProducerAndConsumerCopies)
	{
		static_assert(std::is_same_v<decltype(FShaderCode::Code), FSharedByteBuffer>);
		const auto Options = MakeOptions();
		auto Product = MakeOutput();
		const auto* Bytes = Product.CompiledShaders[0].Code.data();
		auto Output = ShaderSharedOutput::Make(Options, Product); ASSERT_TRUE(Output);
		EXPECT_EQ(Output->FindValue(ShaderValueId(0, false))->GetData().data(), Bytes);
		Product = {};
		ASSERT_TRUE(ShaderSharedOutput::Validate(Options, *Output));
		auto Assembled = ShaderSharedOutput::Assemble(Options, *Output); ASSERT_TRUE(Assembled);
		auto Waiter = Assembled;
		Output = TShaderValueResult<DerivedData::FBuildOutput>{}; Assembled = FShaderCompilerOutput{};
		EXPECT_EQ(Waiter.CompiledShaders[0].Code.data(), Bytes);
		EXPECT_EQ(FXxHash128::HashBuffer(Waiter.CompiledShaders[0].Code), Waiter.CompiledShaders[0].CodeHash);
		EXPECT_EQ(Waiter.CompiledShaders[0].DebugName, Options.VirtualShaderPath + "::VertexMain");
		EXPECT_EQ(Waiter.CompiledShaders[0].Reflection.ResourceBindings, MakeOutput().CompiledShaders[0].Reflection.ResourceBindings);
	}

	TEST_F(FShaderDerivedDataTests, MetalSharedOutputRoundTripsAndRejectsStaleRemaps)
	{
		auto Options = MakeOptions();
		Options.Target = MetalShaderTarget;
		auto Product = MakeMetalOutput();
		ASSERT_TRUE(Product) << FormatShaderCompilerErrors(Product.Errors);
		auto Output = ShaderSharedOutput::Make(Options, Product);
		ASSERT_TRUE(Output) << Output.Error;
		EXPECT_EQ(Output->GetSchemaVersion(), ShaderCompileOutputSchemaVersion);
		auto Legacy = CopyShaderOutput(*Output);
		Legacy.SchemaVersion = 5;
		auto OldPayload = std::move(Legacy).Build();
		ASSERT_TRUE(OldPayload);
		EXPECT_FALSE(ShaderSharedOutput::Assemble(Options, *OldPayload));
		auto Assembled = ShaderSharedOutput::Assemble(Options, *Output);
		ASSERT_TRUE(Assembled) << FormatShaderCompilerErrors(Assembled.Errors);
		ASSERT_EQ(Assembled.CompiledShaders.size(), Product.CompiledShaders.size());
		for (size_t Index = 0; Index < Product.CompiledShaders.size(); ++Index)
		{
			EXPECT_EQ(Assembled.CompiledShaders[Index].CodeFormat, EShaderCodeFormat::Msl20Source);
			EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Assembled.CompiledShaders[Index].Frequency, Assembled.CompiledShaders[Index].PlatformData.GetBytes())->Bindings,
				ShaderFormat::DecodeMetalMetadata(Product.CompiledShaders[Index].Frequency, Product.CompiledShaders[Index].PlatformData.GetBytes())->Bindings);
			EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Assembled.CompiledShaders[Index].Frequency, Assembled.CompiledShaders[Index].PlatformData.GetBytes())->Identity,
				ShaderFormat::DecodeMetalMetadata(Product.CompiledShaders[Index].Frequency, Product.CompiledShaders[Index].PlatformData.GetBytes())->Identity);
		}
		auto CorruptCode = CopyShaderOutput(*Output);
		for (auto& [Id, Value] : CorruptCode.Values)
			if (Id == ShaderValueId(0, false))
			{
				FByteBuffer Bytes(Value.begin(), Value.end());
				Bytes[0] = std::byte{0};
				Value = FSharedByteBuffer::Take(std::move(Bytes));
			}
		auto BadCode = std::move(CorruptCode).Build();
		ASSERT_TRUE(BadCode);
		auto Rejected = ShaderSharedOutput::Assemble(Options, *BadCode);
		ASSERT_FALSE(Rejected);
		EXPECT_EQ(Rejected.Status, EShaderBuildStatus::Error); EXPECT_FALSE(Rejected.Errors.empty());
		auto CorruptHash = CopyShaderOutput(*Output);
		for (auto& [Id, Value] : CorruptHash.Values)
			if (Id == ShaderValueId(0, false))
			{
				FByteBuffer Bytes(Value.begin(), Value.end());
				Bytes.back() ^= std::byte{1};
				Value = FSharedByteBuffer::Take(std::move(Bytes));
			}
		auto BadHash = std::move(CorruptHash).Build();
		ASSERT_TRUE(BadHash);
		Rejected = ShaderSharedOutput::Assemble(Options, *BadHash);
		ASSERT_FALSE(Rejected);
		EXPECT_EQ(Rejected.Status, EShaderBuildStatus::Error); EXPECT_FALSE(Rejected.Errors.empty());
		auto CorruptMap = CopyShaderOutput(*Output);
		for (auto& [Id, Value] : CorruptMap.Values)
			if (Id == ShaderValueId(0, true))
			{
				FByteBuffer Bytes(Value.begin(), Value.end());
				Bytes.back() ^= std::byte{1};
				Value = FSharedByteBuffer::Take(std::move(Bytes));
			}
		auto BadMap = std::move(CorruptMap).Build();
		ASSERT_TRUE(BadMap);
		Rejected = ShaderSharedOutput::Assemble(Options, *BadMap);
		ASSERT_FALSE(Rejected);
		EXPECT_EQ(Rejected.Status, EShaderBuildStatus::Error); EXPECT_FALSE(Rejected.Errors.empty());
	}

	TEST_F(FShaderDerivedDataTests, MetalCookedPayloadRoundTripsAndRejectsWrongTargetOrRemap)
	{
		auto Options = MakeOptions();
		Options.Target = MetalShaderTarget;
		const auto Product = MakeMetalOutput();
		ASSERT_TRUE(Product) << FormatShaderCompilerErrors(Product.Errors);
		FByteBuffer Bytes;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = ShaderCompiledOutput::Encode(Options, Product, Bytes)))
			<< std::string(Error.error());
		FShaderCompilerOutput Loaded;
		ASSERT_TRUE((Error = ShaderCompiledOutput::Decode(Bytes, Options, Loaded)))
			<< std::string(Error.error());
		ASSERT_EQ(Loaded.CompiledShaders.size(), Product.CompiledShaders.size());
		for (size_t Index = 0; Index < Product.CompiledShaders.size(); ++Index)
		{
			const auto& Actual = Loaded.CompiledShaders[Index];
			const auto& Expected = Product.CompiledShaders[Index];
			EXPECT_EQ(Actual.Target, MetalShaderTarget);
			EXPECT_EQ(Actual.CodeFormat, EShaderCodeFormat::Msl20Source);
			EXPECT_EQ(Actual.BinaryEntryPoint, "main0");
			EXPECT_EQ(Actual.CodeHash, Expected.CodeHash);
			EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Actual.Frequency, Actual.PlatformData.GetBytes())->Bindings, ShaderFormat::DecodeMetalMetadata(Expected.Frequency, Expected.PlatformData.GetBytes())->Bindings);
			EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Actual.Frequency, Actual.PlatformData.GetBytes())->PushConstantBufferSlot, ShaderFormat::DecodeMetalMetadata(Expected.Frequency, Expected.PlatformData.GetBytes())->PushConstantBufferSlot);
			EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Actual.Frequency, Actual.PlatformData.GetBytes())->Identity, ShaderFormat::DecodeMetalMetadata(Expected.Frequency, Expected.PlatformData.GetBytes())->Identity);
		}
		auto VulkanOptions = MakeOptions();
		EXPECT_FALSE((Error = ShaderCompiledOutput::Decode(Bytes, VulkanOptions, Loaded)));
		EXPECT_TRUE(Loaded.CompiledShaders.empty());
		FByteBuffer VulkanBytes;
		ASSERT_TRUE(ShaderCompiledOutput::Encode(VulkanOptions, MakeOutput(), VulkanBytes));
		EXPECT_FALSE((Error = ShaderCompiledOutput::Decode(VulkanBytes, Options, Loaded)));
		auto StaleMap = Product;
		FByteBuffer StaleMetadata(StaleMap.CompiledShaders[0].PlatformData.begin(), StaleMap.CompiledShaders[0].PlatformData.end());
		StaleMetadata.back() ^= std::byte{1};
		StaleMap.CompiledShaders[0].PlatformData = FSharedByteBuffer::Take(std::move(StaleMetadata));
		EXPECT_FALSE((Error = ShaderCompiledOutput::Encode(Options, StaleMap, VulkanBytes)));
		auto CorruptMap = Bytes;
		CorruptMap.back() ^= std::byte{1};
		EXPECT_FALSE((Error = ShaderCompiledOutput::Decode(CorruptMap, Options, Loaded)));
		EXPECT_TRUE(Loaded.CompiledShaders.empty());
		auto CorruptMsl = Bytes;
		const auto Header = std::as_bytes(std::span(std::string_view("#include <metal_stdlib>")));
		const auto Match = std::ranges::search(CorruptMsl, Header);
		ASSERT_NE(Match.begin(), CorruptMsl.end());
		*Match.begin() = std::byte{0};
		EXPECT_FALSE((Error = ShaderCompiledOutput::Decode(CorruptMsl, Options, Loaded)));
	}

	TEST_F(FShaderDerivedDataTests, IndependentlyBuiltOutputIsValidatedAgainstRequest)
	{
		using namespace DerivedData;
		auto Options = MakeOptions();
		auto Output = ShaderSharedOutput::Make(Options, MakeOutput()); ASSERT_TRUE(Output);
		auto Clone = std::move(CopyShaderOutput(*Output)).Build(); ASSERT_TRUE(Clone);
		EXPECT_FALSE(Clone->SharesStateWith(*Output));
		EXPECT_TRUE(ShaderSharedOutput::Assemble(Options, *Clone));
		auto OtherOptions = Options;
		OtherOptions.VirtualShaderPath += ".other";
		EXPECT_FALSE(ShaderSharedOutput::Assemble(OtherOptions, *Output));
		OtherOptions = Options;
		OtherOptions.Target = MetalShaderTarget;
		EXPECT_FALSE(ShaderSharedOutput::Assemble(OtherOptions, *Output));
		auto RejectedRequest = ShaderSharedOutput::Make(OtherOptions, MakeOutput());
		ASSERT_FALSE(RejectedRequest);
		EXPECT_EQ(RejectedRequest.Status, EShaderBuildStatus::Error); EXPECT_FALSE(RejectedRequest.Error.empty());

		auto WrongFormat = MakeOutput();
		WrongFormat.CompiledShaders[0].CodeFormat = EShaderCodeFormat::Msl20Source;
		auto RejectedFormat = ShaderSharedOutput::Make(Options, WrongFormat);
		ASSERT_FALSE(RejectedFormat);
		EXPECT_EQ(RejectedFormat.Status, EShaderBuildStatus::Error); EXPECT_FALSE(RejectedFormat.Error.empty());
		auto WrongTarget = MakeOutput();
		WrongTarget.CompiledShaders[0].Target = MetalShaderTarget;
		auto RejectedTarget = ShaderSharedOutput::Make(Options, WrongTarget);
		ASSERT_FALSE(RejectedTarget);
		EXPECT_EQ(RejectedTarget.Status, EShaderBuildStatus::Error); EXPECT_FALSE(RejectedTarget.Error.empty());

		auto Tampered = CopyShaderOutput(*Output);
		FByteBuffer Metadata(Tampered.Metadata.begin(), Tampered.Metadata.end());
		WriteU32At(Metadata, 12 + Options.VirtualShaderPath.size(),
			uint32(EShaderTargetPlatform::MacOS));
		Tampered.Metadata = FSharedByteBuffer::Take(std::move(Metadata));
		auto WrongMetadata = std::move(Tampered).Build(); ASSERT_TRUE(WrongMetadata);
		auto RejectedMetadata = ShaderSharedOutput::Assemble(Options, *WrongMetadata);
		ASSERT_FALSE(RejectedMetadata);
		EXPECT_EQ(RejectedMetadata.Status, EShaderBuildStatus::Error); EXPECT_FALSE(RejectedMetadata.Errors.empty());
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
			auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key, *Output);
			ASSERT_TRUE(Record);
			auto Encoded = FCacheRecordFixtureCodec::Encode(*Record); ASSERT_TRUE(Encoded);
			if (Compressed) Encoded = FCacheRecordFixtureCodec::CompressEncoded(*Encoded);
			ASSERT_TRUE(Encoded);
			auto Loaded = FCacheRecordFixtureCodec::Decode(Key, *Encoded); ASSERT_TRUE(Loaded);
			auto Warm = Durin::DerivedData::Private::ReadBuildOutput(Key, *Loaded);
			ASSERT_TRUE(Warm);
			const auto* Code = Warm->FindValue(ShaderValueId(0, false))->GetData().data();
			auto Assembled = ShaderSharedOutput::Assemble(Options, *Warm); ASSERT_TRUE(Assembled);
			EXPECT_EQ(Assembled.CompiledShaders[0].Code.data(), Code);
			Warm = FBuildOutput{}; Loaded = FCacheRecord{}; Encoded = FSharedByteBuffer{};
			FByteBuffer After; ASSERT_TRUE(ShaderCompiledOutput::Encode(Options, Assembled, After));
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
			auto Data = CopyShaderOutput(*Output);
			if (Fault == 0) ++Data.SchemaVersion;
			if (Fault == 1) Data.Metadata = FSharedByteBuffer::Copy(Data.Metadata.GetBytes().first(4));
			if (Fault == 2) Data.Values.pop_back();
			if (Fault == 3) Data.Values.push_back({FValueId::FromName("Unexpected"), FSharedByteBuffer::Take(FByteBuffer(1))});
			if (Fault >= 4) for (auto& [Id, Value] : Data.Values)
			{
				if ((Fault < 6 && Id == ShaderValueId(0, false)) || (Fault >= 6 && Id == ShaderValueId(0, true)))
				{
					FByteBuffer Bytes(Value.begin(), Value.end());
					if (Fault == 4) Bytes[0] ^= std::byte{1};
					if (Fault == 5) Bytes[12] ^= std::byte{1};
					if (Fault == 6) WriteU32At(Bytes, 0, 65537);
					if (Fault == 7) Bytes.pop_back();
					if (Fault == 8) Bytes.push_back(std::byte{});
					Value = FSharedByteBuffer::Take(std::move(Bytes));
				}
			}
			auto Bad = std::move(Data).Build(); ASSERT_TRUE(Bad);
			EXPECT_FALSE(ShaderSharedOutput::Validate(Options, *Bad));
			EXPECT_FALSE(ShaderSharedOutput::Assemble(Options, *Bad));
		}
		uint32 Checks = 0;
		auto Cancelled = ShaderSharedOutput::Validate(Options, *Output, [&] { return ++Checks == 3; });
		ASSERT_FALSE(Cancelled); EXPECT_EQ(Cancelled.Status, EShaderBuildStatus::Canceled); EXPECT_TRUE(Cancelled.Error.empty());
		const auto CanceledAssembly = ShaderSharedOutput::Assemble(Options, *Output, [] { return true; });
		EXPECT_EQ(CanceledAssembly.Status, EShaderBuildStatus::Canceled); EXPECT_TRUE(CanceledAssembly.CompiledShaders.empty()); EXPECT_TRUE(CanceledAssembly.Errors.empty());
		const auto CanceledOutput = ShaderSharedOutput::Make(Options, MakeOutput(), [] { return true; });
		EXPECT_EQ(CanceledOutput.Status, EShaderBuildStatus::Canceled); EXPECT_FALSE(CanceledOutput.Value); EXPECT_TRUE(CanceledOutput.Error.empty());
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
