#include "Shader/ShaderCookedLibrary.h"
#include "Shader/ShaderData.h"

#include "Hash/XxHash.h"

#include "gtest/gtest.h"

namespace Durin
{
	namespace
	{
		auto MakeRequest(
			std::string Name = "Tests.Primary",
			EShaderTargetProfile Profile = EShaderTargetProfile::Game)
			-> FShaderRuntimeRequest
		{
			return {
				.TargetPlatform = EShaderTargetPlatform::Win64,
				.TargetProfile = Profile,
				.Category = EShaderRuntimeRequestCategory::GlobalSet,
				.Owner = "RenderCoreTests",
				.Name = std::move(Name),
				.Members = {
					{"FTestFragment", "FragmentMain",
						EShaderFrequency::Fragment},
					{"FTestVertex", "VertexMain", EShaderFrequency::Vertex},
				},
			};
		}

		auto MakeShader(
			std::string EntryPoint,
			EShaderFrequency Frequency,
			uint32 Bound) -> FCompiledShader
		{
			const std::array<uint32, 5> Words = {
				0x07230203u, 0x00010500u, 0u, Bound, 0u};
			FCompiledShader Shader;
			Shader.Frequency = Frequency;
			Shader.SourceEntryPoint = std::move(EntryPoint);
			Shader.BinaryEntryPoint = "main";
			Shader.DebugName = Shader.SourceEntryPoint;
			Shader.Code = std::make_shared<const FSharedByteBuffer>(FSharedByteBuffer::Copy(std::as_bytes(std::span(Words))));
			Shader.Hash = FXxHash128::HashBuffer(*Shader.Code);
			return Shader;
		}

		auto MakeOutput() -> FShaderCompilerOutput
		{
			FShaderCompilerOutput Output;
			Output.Error = {};
			Output.CompiledShaders.push_back(MakeShader(
				"FragmentMain", EShaderFrequency::Fragment, 1));
			Output.CompiledShaders.push_back(MakeShader(
				"VertexMain", EShaderFrequency::Vertex, 2));
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
				const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\n";
				Shader.Code = std::make_shared<const FSharedByteBuffer>(
					FSharedByteBuffer::Copy(std::as_bytes(std::span(Source))));
				Shader.Hash = FXxHash128::HashBuffer(*Shader.Code);
				auto Map = BuildMetalShaderBindingMap(Shader.Frequency, Shader.Reflection);
				if (!Map) return {.Error = std::move(Map.error())};
				Shader.MetalBindings = std::move(Map->Bindings);
				Shader.MetalPushConstantBufferSlot = Map->PushConstantBufferSlot;
				Shader.BindingRemapIdentity = Map->Identity;
			}
			return Output;
		}

		auto ReadU64At(FByteView Bytes, size_t Offset) -> uint64
		{
			uint64 Result = 0;
			for (size_t Index = 0; Index < sizeof(uint64); ++Index)
				Result |= uint64(std::to_integer<uint8>(Bytes[Offset + Index])) << (Index * 8);
			return Result;
		}

		auto WriteU64At(FByteBuffer& Bytes, size_t Offset, uint64 Value) -> void
		{
			for (size_t Index = 0; Index < sizeof(uint64); ++Index)
				Bytes[Offset + Index] = std::byte((Value >> (Index * 8)) & 0xffu);
		}
	}

	TEST(FShaderRuntimeInventoryTests,
		CanonicalFreezeFiltersEditorAndRejectsTargetReplacement)
	{
		ResetShaderRuntimeInventoryForTesting();
		FShaderOperationResult Error;
		FShaderRuntimeRequest Game = MakeRequest();
		Game.TargetPlatform = EShaderTargetPlatform::Invalid;
		Game.TargetProfile = EShaderTargetProfile::Invalid;
		FShaderRequestRegistration GameRegistration;
		Error = RegisterShaderRuntimeRequest(std::move(Game), EShaderRequestEligibility::GameAndEditor, GameRegistration);
		ASSERT_TRUE(GameRegistration.IsValid()) << FormatShaderError(Error.error());
		FShaderRuntimeRequest Editor = MakeRequest("Tests.EditorOnly");
		Editor.TargetPlatform = EShaderTargetPlatform::Invalid;
		Editor.TargetProfile = EShaderTargetProfile::Invalid;
		FShaderRequestRegistration EditorRegistration;
		Error = RegisterShaderRuntimeRequest(std::move(Editor), EShaderRequestEligibility::EditorOnly, EditorRegistration);
		ASSERT_TRUE(EditorRegistration.IsValid()) << FormatShaderError(Error.error());

		std::vector<FShaderRuntimeRequest> Inventory;
		ASSERT_TRUE((Error = FreezeShaderRuntimeInventory(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, Inventory))) << FormatShaderError(Error.error());
		ASSERT_EQ(Inventory.size(), 1u);
		EXPECT_EQ(Inventory.front().Name, "Tests.Primary");
		EXPECT_EQ(Inventory.front().Members.front().TypeName, "FTestFragment");
		EXPECT_FALSE((Error = FreezeShaderRuntimeInventory(EShaderTargetPlatform::Win64, EShaderTargetProfile::EditorValidation, Inventory)));
		EXPECT_FALSE((Error = GameRegistration.Reset()));
		ResetShaderRuntimeInventoryForTesting();
	}

	TEST(FShaderCookedLibraryTests,
		CanonicalBytesRoundTripCompleteOutputAndRequiredClosure)
	{
		const FShaderRuntimeRequest Request = MakeRequest();
		const FShaderCookedLibraryRecord Record{
			.Request = Request,
			.ProductionIdentity = {17, 29},
			.Output = MakeOutput(),
		};
		Durin::FByteBuffer First;
		Durin::FByteBuffer Second;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Record, 1), First))) << FormatShaderError(Error.error());
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Record, 1), Second))) << FormatShaderError(Error.error());
		EXPECT_EQ(First, Second);

		FShaderCookedLibrary Library;
		ASSERT_TRUE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(First), EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Request, 1), Library))) << FormatShaderError(Error.error());
		EXPECT_EQ(Library.GetRecordCount(), 1u);
		EXPECT_FALSE(Library.GetGenerationIdentity().IsZero());
		FShaderCompilerOutput Loaded;
		ASSERT_TRUE((Error = Library.Load(Request, Loaded))) << FormatShaderError(Error.error());
		ASSERT_EQ(Loaded.CompiledShaders.size(), 2u);
		EXPECT_EQ(Loaded.CompiledShaders[0].SourceEntryPoint, "FragmentMain");
		EXPECT_EQ(Loaded.CompiledShaders[1].SourceEntryPoint, "VertexMain");
		EXPECT_TRUE(std::ranges::equal(Loaded.CompiledShaders[0].Code->GetBytes(),
			Record.Output.CompiledShaders[0].Code->GetBytes()));
	}

	TEST(FShaderCookedLibraryTests,
		RejectsWrongTargetMissingRequestAndCorruptBytes)
	{
		const FShaderRuntimeRequest Request = MakeRequest();
		const FShaderCookedLibraryRecord Record{
			.Request = Request,
			.ProductionIdentity = {3, 5},
			.Output = MakeOutput(),
		};
		Durin::FByteBuffer Bytes;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Record, 1), Bytes))) << FormatShaderError(Error.error());

		FShaderCookedLibrary Library;
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(Bytes), EShaderTargetPlatform::Win64, EShaderTargetProfile::EditorValidation, {}, Library)));
		const FShaderRuntimeRequest Missing = MakeRequest("Tests.Missing");
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(Bytes), EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Missing, 1), Library)));
		Bytes.back() ^= std::byte{1};
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(Bytes), EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, {}, Library)));
	}

	TEST(FShaderCookedLibraryTests, MacOSLibraryKeepsMslAndRejectsVulkanAdmission)
	{
		auto Request = MakeRequest();
		Request.TargetPlatform = EShaderTargetPlatform::MacOS;
		auto Output = MakeMetalOutput();
		ASSERT_TRUE(Output) << FormatShaderError(Output.Error);
		const FShaderCookedLibraryRecord Record{
			.Request = Request, .ProductionIdentity = {7, 11}, .Output = Output};
		FByteBuffer Bytes;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::MacOS,
			EShaderTargetProfile::Game, std::span(&Record, 1), Bytes)))
			<< FormatShaderError(Error.error());
		FShaderCookedLibrary Library;
		ASSERT_TRUE((Error = FShaderCookedLibrary::OpenBytes(
			std::make_shared<const FByteBuffer>(Bytes), EShaderTargetPlatform::MacOS,
			EShaderTargetProfile::Game, std::span(&Request, 1), Library)))
			<< FormatShaderError(Error.error());
		FShaderCompilerOutput Loaded;
		ASSERT_TRUE((Error = Library.Load(Request, Loaded))) << FormatShaderError(Error.error());
		ASSERT_EQ(Loaded.CompiledShaders.size(), 2u);
		EXPECT_EQ(Loaded.CompiledShaders[0].Target, MetalShaderTarget);
		EXPECT_EQ(Loaded.CompiledShaders[0].CodeFormat, EShaderCodeFormat::Msl20Source);
		EXPECT_EQ(Loaded.CompiledShaders[0].BinaryEntryPoint, "main0");
		EXPECT_EQ(Loaded.CompiledShaders[0].BindingRemapIdentity,
			Output.CompiledShaders[0].BindingRemapIdentity);
		FByteBuffer Corrupt = Bytes;
		const uint64 PayloadOffset = ReadU64At(Corrupt, 56);
		ASSERT_LT(PayloadOffset, Corrupt.size());
		const std::string_view Marker = "#include <metal_stdlib>";
		const auto Found = std::ranges::search(Corrupt.begin() + PayloadOffset,
			Corrupt.end(), std::as_bytes(std::span(Marker)).begin(),
			std::as_bytes(std::span(Marker)).end());
		ASSERT_NE(Found.begin(), Corrupt.end());
		*Found.begin() = std::byte{'X'};
		const FXxHash128 PayloadDigest = FXxHash128::HashBuffer(
			std::span(Corrupt).subspan(PayloadOffset));
		WriteU64At(Corrupt, 144, PayloadDigest.HashLow);
		WriteU64At(Corrupt, 152, PayloadDigest.HashHigh);
		constexpr std::array<std::byte, 16> Zeros{};
		FXxHash128Builder FileDigest;
		FileDigest.Update(std::span(Corrupt).first(96));
		FileDigest.Update(Zeros);
		FileDigest.Update(std::span(Corrupt).subspan(112));
		const FXxHash128 NewFileDigest = FileDigest.Finalize();
		WriteU64At(Corrupt, 96, NewFileDigest.HashLow);
		WriteU64At(Corrupt, 104, NewFileDigest.HashHigh);
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(
			std::make_shared<const FByteBuffer>(Corrupt), EShaderTargetPlatform::MacOS,
			EShaderTargetProfile::Game, std::span(&Request, 1), Library)));
		EXPECT_EQ(Error.error().Code, EShaderError::PayloadMslInvalid);
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(
			std::make_shared<const FByteBuffer>(Bytes), EShaderTargetPlatform::Win64,
			EShaderTargetProfile::Game, {}, Library)));
		EXPECT_EQ(Error.error().Code, EShaderError::LibraryHeaderInvalid);
		auto WrongOutput = MakeOutput();
		const FShaderCookedLibraryRecord WrongRecord{
			.Request = Request, .ProductionIdentity = {7, 11}, .Output = WrongOutput};
		EXPECT_FALSE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::MacOS,
			EShaderTargetProfile::Game, std::span(&WrongRecord, 1), Bytes)));
		EXPECT_EQ(Error.error().Code, EShaderError::PayloadOutputInvalid);
	}

	TEST(FShaderDataDomainTests,
		CookedDomainFailsOnMissingLibraryWithoutAuthoringFallback)
	{
		ShutdownShaderData();
		ResetShaderRuntimeInventoryForTesting();
		FShaderOperationResult Error;
		FShaderRuntimeRequest Request = MakeRequest();
		Request.TargetPlatform = EShaderTargetPlatform::Invalid;
		Request.TargetProfile = EShaderTargetProfile::Invalid;
		FShaderRequestRegistration Registration;
		Error = RegisterShaderRuntimeRequest(std::move(Request), EShaderRequestEligibility::GameAndEditor, Registration);
		ASSERT_TRUE(Registration.IsValid()) << FormatShaderError(Error.error());
		EXPECT_FALSE((Error = InitializeShaderData(FShaderDataConfiguration::Authored())));
		EXPECT_EQ(Error.error().Code, EShaderError::BuildModuleRequired);
		const std::filesystem::path MissingRoot =
			std::filesystem::absolute("MissingShaderCookRoot").lexically_normal();
		ASSERT_TRUE((Error = InitializeShaderData(FShaderDataConfiguration::Cooked(MissingRoot)))) << FormatShaderError(Error.error());
		EXPECT_EQ(GetShaderDataDomain(), EShaderDataDomain::Cooked);
		FShaderCompilerOutput Output;
		EXPECT_FALSE((Error = LoadCookedShaderRuntimeRequest("Tests.Primary", {}, Output)));
		EXPECT_EQ(Error.error().Code, EShaderError::LibraryReadFailed);
		ShutdownShaderData();
		ResetShaderRuntimeInventoryForTesting();
	}

	TEST(FShaderDataDomainTests, MacOSCookedDomainKeepsTargetAndReportsMissingLibrary)
	{
		ShutdownShaderData();
		ResetShaderRuntimeInventoryForTesting();
		const std::filesystem::path MissingRoot =
			std::filesystem::absolute("MissingMetalShaderCookRoot").lexically_normal();
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = InitializeShaderData(FShaderDataConfiguration::Cooked(
			MissingRoot, EShaderTargetPlatform::MacOS))))
			<< FormatShaderError(Error.error());
		EXPECT_EQ(GetShaderDataDomain(), EShaderDataDomain::Cooked);
		FShaderCompilerOutput Output;
		EXPECT_FALSE((Error = LoadCookedShaderRuntimeRequest("Absent", {}, Output)));
		EXPECT_EQ(Error.error().Code, EShaderError::LibraryReadFailed);
		ShutdownShaderData();
		ResetShaderRuntimeInventoryForTesting();
	}
}
