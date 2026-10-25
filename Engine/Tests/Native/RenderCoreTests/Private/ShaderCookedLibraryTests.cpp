#include "MetalShaderFormat.h"
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
			uint32 Bound) -> FShaderCode
		{
			const std::array<uint32, 5> Words = {
				0x07230203u, 0x00010500u, 0u, Bound, 0u};
			FShaderCode Shader;
			Shader.Frequency = Frequency;
			Shader.SourceEntryPoint = std::move(EntryPoint);
			Shader.BinaryEntryPoint = "main";
			Shader.DebugName = Shader.SourceEntryPoint;
			Shader.Code = FSharedByteBuffer::Copy(std::as_bytes(std::span(Words)));
			Shader.CodeHash = FXxHash128::HashBuffer(Shader.Code);
			return Shader;
		}

		auto MakeOutput() -> FShaderCompilerOutput
		{
			FShaderCompilerOutput Output;
			Output.Status = EShaderBuildStatus::Ok;
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
				Shader.Target = MetalLibraryShaderTarget;
				Shader.CodeFormat = EShaderCodeFormat::MetalLibrary;
				Shader.BinaryEntryPoint = "main0";
				FByteBuffer Library(88);
				Library[0] = std::byte{'M'}; Library[1] = std::byte{'T'};
				Library[2] = std::byte{'L'}; Library[3] = std::byte{'B'};
				Library[16] = std::byte{88};
				Shader.Code = FSharedByteBuffer::Take(std::move(Library));
				Shader.CodeHash = FXxHash128::HashBuffer(Shader.Code);
				auto Map = ShaderFormat::BuildMetalBindingMap(Shader.Frequency, ShaderFormat::BuildLogicalBindings(Shader.Reflection), !Shader.Reflection.PushConstantRanges.empty());
				if (!Map) return FShaderCompilerOutput::Failure("Metal shader binding remap exceeds or conflicts with native slots");
				Shader.PlatformData = ShaderFormat::EncodeMetalMetadata(Shader.Frequency,
					{std::move(Map->Bindings), Map->PushConstantBufferSlot, Map->Identity}).value();
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

	TEST(FShaderRuntimeInventoryTests, RequestIdentityRemainsStableAcrossContainerChanges)
	{
		FXxHash128 Identity;
		const auto Result = BuildShaderRuntimeRequestIdentity(MakeRequest(), Identity);
		ASSERT_TRUE(Result) << std::string(Result.error());
		EXPECT_EQ(Identity.ToString(), "59028fd192e6cbbe8ec1fa7dea3f0817");
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
		ASSERT_TRUE(GameRegistration.IsValid()) << std::string(Error.error());
		FShaderRuntimeRequest Editor = MakeRequest("Tests.EditorOnly");
		Editor.TargetPlatform = EShaderTargetPlatform::Invalid;
		Editor.TargetProfile = EShaderTargetProfile::Invalid;
		FShaderRequestRegistration EditorRegistration;
		Error = RegisterShaderRuntimeRequest(std::move(Editor), EShaderRequestEligibility::EditorOnly, EditorRegistration);
		ASSERT_TRUE(EditorRegistration.IsValid()) << std::string(Error.error());

		std::vector<FShaderRuntimeRequest> Inventory;
		ASSERT_TRUE((Error = FreezeShaderRuntimeInventory(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, Inventory))) << std::string(Error.error());
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
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Record, 1), First))) << std::string(Error.error());
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Record, 1), Second))) << std::string(Error.error());
		EXPECT_EQ(First, Second);

		FShaderCookedLibrary Library;
		ASSERT_TRUE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(First), EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Request, 1), Library))) << std::string(Error.error());
		EXPECT_EQ(Library.GetRecordCount(), 1u);
		EXPECT_FALSE(Library.GetGenerationIdentity().IsZero());
		FShaderCompilerOutput Loaded;
		ASSERT_TRUE((Error = Library.Load(Request, Loaded))) << std::string(Error.error());
		ASSERT_EQ(Loaded.CompiledShaders.size(), 2u);
		EXPECT_EQ(Loaded.CompiledShaders[0].SourceEntryPoint, "FragmentMain");
		EXPECT_EQ(Loaded.CompiledShaders[1].SourceEntryPoint, "VertexMain");
		EXPECT_TRUE(std::ranges::equal(Loaded.CompiledShaders[0].Code.GetBytes(),
			Record.Output.CompiledShaders[0].Code.GetBytes()));
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
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Record, 1), Bytes))) << std::string(Error.error());

		for (const size_t Offset : {size_t{4}, size_t{8}})
		{
			auto Invalid = Bytes;
			Invalid[Offset] ^= std::byte{0x40};
			FShaderCookedLibrary Rejected;
			const auto Result = FShaderCookedLibrary::OpenBytes(
				std::make_shared<const FByteBuffer>(Invalid), EShaderTargetPlatform::Win64,
				EShaderTargetProfile::Game, std::span(&Request, 1), Rejected);
			ASSERT_FALSE(Result);
		}

		FShaderCookedLibrary Library;
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(Bytes), EShaderTargetPlatform::Win64, EShaderTargetProfile::EditorValidation, {}, Library)));
		const FShaderRuntimeRequest Missing = MakeRequest("Tests.Missing");
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(Bytes), EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Missing, 1), Library)));
		Bytes.back() ^= std::byte{1};
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(Bytes), EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, {}, Library)));
	}

	TEST(FShaderCookedLibraryTests, PendingResolutionCannotBePublishedAsCookedCompilerOutput)
	{
		FShaderCookedLibraryRecord Record{
			.Request = MakeRequest(), .ProductionIdentity = {7, 11}, .Output = MakeOutput()};
		FByteBuffer Bytes;
		ASSERT_TRUE(EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64,
			EShaderTargetProfile::Game, std::span(&Record, 1), Bytes));
		Record.Output.Status = EShaderBuildStatus::Pending;
		EXPECT_FALSE(EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64,
			EShaderTargetProfile::Game, std::span(&Record, 1), Bytes));
	}

	TEST(FShaderCookedLibraryTests, MacOSLibraryKeepsBinaryAndRejectsVulkanAdmission)
	{
		auto Request = MakeRequest();
		Request.TargetPlatform = EShaderTargetPlatform::MacOS;
		auto Output = MakeMetalOutput();
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		const FShaderCookedLibraryRecord Record{
			.Request = Request, .ProductionIdentity = {7, 11}, .Output = Output};
		FByteBuffer Bytes;
		FShaderOperationResult Error;
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::MacOS,
			EShaderTargetProfile::Game, std::span(&Record, 1), Bytes)))
			<< std::string(Error.error());
		FShaderCookedLibrary Library;
		ASSERT_TRUE((Error = FShaderCookedLibrary::OpenBytes(
			std::make_shared<const FByteBuffer>(Bytes), EShaderTargetPlatform::MacOS,
			EShaderTargetProfile::Game, std::span(&Request, 1), Library)))
			<< std::string(Error.error());
		FShaderCompilerOutput Loaded;
		ASSERT_TRUE((Error = Library.Load(Request, Loaded))) << std::string(Error.error());
		ASSERT_EQ(Loaded.CompiledShaders.size(), 2u);
		EXPECT_EQ(Loaded.CompiledShaders[0].Target, MetalLibraryShaderTarget);
		EXPECT_EQ(Loaded.CompiledShaders[0].CodeFormat, EShaderCodeFormat::MetalLibrary);
		EXPECT_EQ(Loaded.CompiledShaders[0].BinaryEntryPoint, "main0");
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Loaded.CompiledShaders[0].Frequency, Loaded.CompiledShaders[0].PlatformData.GetBytes())->Identity,
			ShaderFormat::DecodeMetalMetadata(Output.CompiledShaders[0].Frequency, Output.CompiledShaders[0].PlatformData.GetBytes())->Identity);
		FByteBuffer Corrupt = Bytes;
		const uint64 PayloadOffset = ReadU64At(Corrupt, 52);
		ASSERT_LT(PayloadOffset, Corrupt.size());
		const std::string_view Marker = "MTLB";
		const auto Found = std::ranges::search(Corrupt.begin() + PayloadOffset,
			Corrupt.end(), std::as_bytes(std::span(Marker)).begin(),
			std::as_bytes(std::span(Marker)).end());
		ASSERT_NE(Found.begin(), Corrupt.end());
		*Found.begin() = std::byte{0};
		const FXxHash128 PayloadDigest = FXxHash128::HashBuffer(
			std::span(Corrupt).subspan(PayloadOffset));
		WriteU64At(Corrupt, 140, PayloadDigest.HashLow);
		WriteU64At(Corrupt, 148, PayloadDigest.HashHigh);
		constexpr std::array<std::byte, 16> Zeros{};
		FXxHash128Builder FileDigest;
		FileDigest.Update(std::span(Corrupt).first(92));
		FileDigest.Update(Zeros);
		FileDigest.Update(std::span(Corrupt).subspan(108));
		const FXxHash128 NewFileDigest = FileDigest.Finalize();
		WriteU64At(Corrupt, 92, NewFileDigest.HashLow);
		WriteU64At(Corrupt, 100, NewFileDigest.HashHigh);
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(
			std::make_shared<const FByteBuffer>(Corrupt), EShaderTargetPlatform::MacOS,
			EShaderTargetProfile::Game, std::span(&Request, 1), Library)));
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(
			std::make_shared<const FByteBuffer>(Bytes), EShaderTargetPlatform::Win64,
			EShaderTargetProfile::Game, {}, Library)));
		auto WrongOutput = MakeOutput();
		const FShaderCookedLibraryRecord WrongRecord{
			.Request = Request, .ProductionIdentity = {7, 11}, .Output = WrongOutput};
		EXPECT_FALSE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::MacOS,
			EShaderTargetProfile::Game, std::span(&WrongRecord, 1), Bytes)));
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
		ASSERT_TRUE(Registration.IsValid()) << std::string(Error.error());
		EXPECT_FALSE((Error = InitializeShaderData(FShaderDataConfiguration::Authored())));
		const std::filesystem::path MissingRoot =
			std::filesystem::absolute("MissingShaderCookRoot").lexically_normal();
		ASSERT_TRUE((Error = InitializeShaderData(FShaderDataConfiguration::Cooked(MissingRoot)))) << std::string(Error.error());
		EXPECT_EQ(GetShaderDataDomain(), EShaderDataDomain::Cooked);
		FShaderCompilerOutput Output;
		EXPECT_FALSE((Error = LoadCookedShaderRuntimeRequest("Tests.Primary", {}, Output)));
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
			<< std::string(Error.error());
		EXPECT_EQ(GetShaderDataDomain(), EShaderDataDomain::Cooked);
		FShaderCompilerOutput Output;
		EXPECT_FALSE((Error = LoadCookedShaderRuntimeRequest("Absent", {}, Output)));
		ShutdownShaderData();
		ResetShaderRuntimeInventoryForTesting();
	}
}
