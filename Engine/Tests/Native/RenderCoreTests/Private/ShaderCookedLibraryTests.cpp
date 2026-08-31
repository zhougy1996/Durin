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
			Shader.Code = std::make_shared<Durin::FByteArray>(sizeof(Words));
			std::memcpy(Shader.Code->data(), Words.data(), sizeof(Words));
			Shader.Hash = FXxHash128::HashBuffer(*Shader.Code);
			return Shader;
		}

		auto MakeOutput() -> FShaderCompilerOutput
		{
			FShaderCompilerOutput Output;
			Output.bSucceeded = true;
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
		std::string Error;
		FShaderRuntimeRequest Game = MakeRequest();
		Game.TargetPlatform = EShaderTargetPlatform::Invalid;
		Game.TargetProfile = EShaderTargetProfile::Invalid;
		FShaderRequestRegistration GameRegistration = RegisterShaderRuntimeRequest(
			std::move(Game), EShaderRequestEligibility::GameAndEditor, {}, &Error);
		ASSERT_TRUE(GameRegistration.IsValid()) << Error;
		FShaderRuntimeRequest Editor = MakeRequest("Tests.EditorOnly");
		Editor.TargetPlatform = EShaderTargetPlatform::Invalid;
		Editor.TargetProfile = EShaderTargetProfile::Invalid;
		FShaderRequestRegistration EditorRegistration = RegisterShaderRuntimeRequest(
			std::move(Editor), EShaderRequestEligibility::EditorOnly, {}, &Error);
		ASSERT_TRUE(EditorRegistration.IsValid()) << Error;

		std::vector<FShaderRuntimeRequest> Inventory;
		ASSERT_TRUE(FreezeShaderRuntimeInventory(
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			Inventory, Error)) << Error;
		ASSERT_EQ(Inventory.size(), 1u);
		EXPECT_EQ(Inventory.front().Name, "Tests.Primary");
		EXPECT_EQ(Inventory.front().Members.front().TypeName, "FTestFragment");
		EXPECT_FALSE(FreezeShaderRuntimeInventory(
			EShaderTargetPlatform::Win64,
			EShaderTargetProfile::EditorValidation, Inventory, Error));
		EXPECT_FALSE(GameRegistration.Reset(&Error));
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
		Durin::FByteArray First;
		Durin::FByteArray Second;
		std::string Error;
		ASSERT_TRUE(EncodeShaderCookedLibrary(
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			std::span(&Record, 1), First, Error)) << Error;
		ASSERT_TRUE(EncodeShaderCookedLibrary(
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			std::span(&Record, 1), Second, Error)) << Error;
		EXPECT_EQ(First, Second);

		FShaderCookedLibrary Library;
		ASSERT_TRUE(FShaderCookedLibrary::OpenBytes(
			std::make_shared<const Durin::FByteArray>(First),
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			std::span(&Request, 1), Library, Error)) << Error;
		EXPECT_EQ(Library.GetRecordCount(), 1u);
		EXPECT_FALSE(Library.GetGenerationIdentity().IsZero());
		FShaderCompilerOutput Loaded;
		ASSERT_TRUE(Library.Load(Request, Loaded, Error)) << Error;
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
		Durin::FByteArray Bytes;
		std::string Error;
		ASSERT_TRUE(EncodeShaderCookedLibrary(
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			std::span(&Record, 1), Bytes, Error)) << Error;

		FShaderCookedLibrary Library;
		EXPECT_FALSE(FShaderCookedLibrary::OpenBytes(
			std::make_shared<const Durin::FByteArray>(Bytes),
			EShaderTargetPlatform::Win64,
			EShaderTargetProfile::EditorValidation, {}, Library, Error));
		const FShaderRuntimeRequest Missing = MakeRequest("Tests.Missing");
		EXPECT_FALSE(FShaderCookedLibrary::OpenBytes(
			std::make_shared<const Durin::FByteArray>(Bytes),
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			std::span(&Missing, 1), Library, Error));
		Bytes.back() ^= std::byte{1};
		EXPECT_FALSE(FShaderCookedLibrary::OpenBytes(
			std::make_shared<const Durin::FByteArray>(Bytes),
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			{}, Library, Error));
	}

	TEST(FShaderDataDomainTests,
		CookedDomainFailsOnMissingLibraryWithoutAuthoringFallback)
	{
		ShutdownShaderData();
		ResetShaderRuntimeInventoryForTesting();
		std::string Error;
		FShaderRuntimeRequest Request = MakeRequest();
		Request.TargetPlatform = EShaderTargetPlatform::Invalid;
		Request.TargetProfile = EShaderTargetProfile::Invalid;
		FShaderRequestRegistration Registration = RegisterShaderRuntimeRequest(
			std::move(Request), EShaderRequestEligibility::GameAndEditor, {}, &Error);
		ASSERT_TRUE(Registration.IsValid()) << Error;
		EXPECT_FALSE(InitializeShaderData(
			FShaderDataConfiguration::Authored(), Error));
		EXPECT_NE(Error.find("requires a ShaderBuild provider"), std::string::npos);
		const std::filesystem::path MissingRoot =
			std::filesystem::absolute("MissingShaderCookRoot").lexically_normal();
		ASSERT_TRUE(InitializeShaderData(
			FShaderDataConfiguration::Cooked(MissingRoot), Error)) << Error;
		EXPECT_EQ(GetShaderDataDomain(), EShaderDataDomain::Cooked);
		FShaderCompilerOutput Output;
		EXPECT_FALSE(LoadCookedShaderRuntimeRequest(
			"Tests.Primary", {}, Output, Error));
		EXPECT_NE(Error.find("could not be read"), std::string::npos);
		ShutdownShaderData();
		ResetShaderRuntimeInventoryForTesting();
	}
}
