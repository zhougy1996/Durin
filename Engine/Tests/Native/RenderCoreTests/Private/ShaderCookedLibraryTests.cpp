#include "Shader/ShaderCookedLibrary.h"
#include "Shader/ShaderData.h"

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
			Shader.Code = std::make_shared<Durin::FByteBuffer>(sizeof(Words));
			std::memcpy(Shader.Code->data(), Words.data(), sizeof(Words));
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
		ASSERT_TRUE(GameRegistration.IsValid()) << FormatShaderError(Error.Error);
		FShaderRuntimeRequest Editor = MakeRequest("Tests.EditorOnly");
		Editor.TargetPlatform = EShaderTargetPlatform::Invalid;
		Editor.TargetProfile = EShaderTargetProfile::Invalid;
		FShaderRequestRegistration EditorRegistration;
		Error = RegisterShaderRuntimeRequest(std::move(Editor), EShaderRequestEligibility::EditorOnly, EditorRegistration);
		ASSERT_TRUE(EditorRegistration.IsValid()) << FormatShaderError(Error.Error);

		std::vector<FShaderRuntimeRequest> Inventory;
		ASSERT_TRUE((Error = FreezeShaderRuntimeInventory(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, Inventory))) << FormatShaderError(Error.Error);
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
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Record, 1), First))) << FormatShaderError(Error.Error);
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Record, 1), Second))) << FormatShaderError(Error.Error);
		EXPECT_EQ(First, Second);

		FShaderCookedLibrary Library;
		ASSERT_TRUE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(First), EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Request, 1), Library))) << FormatShaderError(Error.Error);
		EXPECT_EQ(Library.GetRecordCount(), 1u);
		EXPECT_FALSE(Library.GetGenerationIdentity().IsZero());
		FShaderCompilerOutput Loaded;
		ASSERT_TRUE((Error = Library.Load(Request, Loaded))) << FormatShaderError(Error.Error);
		ASSERT_EQ(Loaded.CompiledShaders.size(), 2u);
		EXPECT_EQ(Loaded.CompiledShaders[0].SourceEntryPoint, "FragmentMain");
		EXPECT_EQ(Loaded.CompiledShaders[1].SourceEntryPoint, "VertexMain");
		EXPECT_EQ(*Loaded.CompiledShaders[0].Code,
			*Record.Output.CompiledShaders[0].Code);
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
		ASSERT_TRUE((Error = EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Record, 1), Bytes))) << FormatShaderError(Error.Error);

		FShaderCookedLibrary Library;
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(Bytes), EShaderTargetPlatform::Win64, EShaderTargetProfile::EditorValidation, {}, Library)));
		const FShaderRuntimeRequest Missing = MakeRequest("Tests.Missing");
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(Bytes), EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Missing, 1), Library)));
		Bytes.back() ^= std::byte{1};
		EXPECT_FALSE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(Bytes), EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, {}, Library)));
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
		ASSERT_TRUE(Registration.IsValid()) << FormatShaderError(Error.Error);
		EXPECT_FALSE((Error = InitializeShaderData(FShaderDataConfiguration::Authored())));
		EXPECT_EQ(Error.Error.Code, EShaderError::ProviderRequired);
		const std::filesystem::path MissingRoot =
			std::filesystem::absolute("MissingShaderCookRoot").lexically_normal();
		ASSERT_TRUE((Error = InitializeShaderData(FShaderDataConfiguration::Cooked(MissingRoot)))) << FormatShaderError(Error.Error);
		EXPECT_EQ(GetShaderDataDomain(), EShaderDataDomain::Cooked);
		FShaderCompilerOutput Output;
		EXPECT_FALSE((Error = LoadCookedShaderRuntimeRequest("Tests.Primary", {}, Output)));
		EXPECT_EQ(Error.Error.Code, EShaderError::LibraryReadFailed);
		ShutdownShaderData();
		ResetShaderRuntimeInventoryForTesting();
	}
}
