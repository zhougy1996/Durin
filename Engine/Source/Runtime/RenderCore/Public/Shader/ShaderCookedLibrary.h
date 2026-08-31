#pragma once

#include "RenderCoreAPI.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	class FShaderType;
	inline constexpr std::string_view ShaderCookedLibraryRelativePath =
		"Shaders/ShaderLibrary.dslb";

	// Identifies the platform encoded by one Shader request and library.
	enum class EShaderTargetPlatform : uint32
	{
		Invalid = 0,
		Win64 = 1,
	};

	// Identifies the runtime profile encoded by one Shader request and library.
	enum class EShaderTargetProfile : uint32
	{
		Invalid = 0,
		Game = 1,
		EditorValidation = 2,
	};

	// Separates exact Global Shader sets from finite feature-owned programs.
	enum class EShaderRuntimeRequestCategory : uint32
	{
		GlobalSet = 1,
		FeatureProgram = 2,
	};

	// Declares which target profiles may select a registered request.
	enum class EShaderRequestEligibility : uint8
	{
		GameAndEditor,
		EditorOnly,
	};

	// Carries one ordered, source-independent member of a runtime request.
	struct FShaderRuntimeRequestMember
	{
		std::string TypeName;
		std::string EntryPoint;
		EShaderFrequency Frequency = EShaderFrequency::Vertex;

		auto operator==(const FShaderRuntimeRequestMember&) const -> bool = default;
	};

	// Describes one exact non-Material Shader output without authoring identity.
	struct FShaderRuntimeRequest
	{
		EShaderTargetPlatform TargetPlatform = EShaderTargetPlatform::Invalid;
		EShaderTargetProfile TargetProfile = EShaderTargetProfile::Invalid;
		EShaderRuntimeRequestCategory Category =
			EShaderRuntimeRequestCategory::GlobalSet;
		std::string Owner;
		std::string Name;
		std::vector<FShaderRuntimeRequestMember> Members;

		auto operator==(const FShaderRuntimeRequest&) const -> bool = default;
	};

	class FShaderRequestRegistration;
	RENDERCORE_API auto RegisterShaderRuntimeRequest(
		FShaderRuntimeRequest Request,
		EShaderRequestEligibility Eligibility,
		std::span<const FShaderType* const> BuildTypes = {},
		std::string* OutError = nullptr) -> FShaderRequestRegistration;

	// Owns registration of one target-filtered request contribution.
	class FShaderRequestRegistration final
	{
	public:
		FShaderRequestRegistration() = default;
		RENDERCORE_API ~FShaderRequestRegistration();
		FShaderRequestRegistration(const FShaderRequestRegistration&) = delete;
		auto operator=(const FShaderRequestRegistration&)
			-> FShaderRequestRegistration& = delete;
		RENDERCORE_API FShaderRequestRegistration(
			FShaderRequestRegistration&& Other) noexcept;
		RENDERCORE_API auto operator=(FShaderRequestRegistration&& Other) noexcept
			-> FShaderRequestRegistration&;
		[[nodiscard]] auto IsValid() const -> bool { return Handle != 0; }
		RENDERCORE_API auto Reset(std::string* OutError = nullptr) -> bool;

	private:
		explicit FShaderRequestRegistration(uint64 InHandle) : Handle(InHandle) {}
		uint64 Handle = 0;
		friend RENDERCORE_API auto RegisterShaderRuntimeRequest(
			FShaderRuntimeRequest, EShaderRequestEligibility,
			std::span<const FShaderType* const>, std::string*)
			-> FShaderRequestRegistration;
	};

	RENDERCORE_API auto FreezeShaderRuntimeInventory(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		std::vector<FShaderRuntimeRequest>& OutRequests,
		std::string& OutError) -> bool;
	RENDERCORE_API auto ResetShaderRuntimeInventoryForTesting() -> void;
	RENDERCORE_API auto BuildShaderRuntimeRequestIdentity(
		const FShaderRuntimeRequest& Request,
		FXxHash128& OutIdentity,
		std::string& OutError) -> bool;
	RENDERCORE_API auto GetShaderRuntimeRequestBuildTypes(
		const FShaderRuntimeRequest& Request,
		std::vector<const FShaderType*>& OutTypes,
		std::string& OutError) -> bool;

	// Registers one finite feature-owned Shader program and its build types.
	class FShaderProgramRegistration final
	{
	public:
		RENDERCORE_API FShaderProgramRegistration(
			std::string_view Owner,
			std::string_view Name,
			EShaderRequestEligibility Eligibility,
			std::initializer_list<const FShaderType*> Types);
		[[nodiscard]] auto IsValid() const -> bool
		{
			return Registration.IsValid();
		}

	private:
		FShaderRequestRegistration Registration;
	};

	// Supplies one complete value and opaque production provenance to encoding.
	struct FShaderCookedLibraryRecord
	{
		FShaderRuntimeRequest Request;
		FXxHash128 ProductionIdentity;
		FShaderCompilerOutput Output;
	};

	RENDERCORE_API auto EncodeShaderCookedLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		std::span<const FShaderCookedLibraryRecord> Records,
		FByteArray& OutBytes,
		std::string& OutError) -> bool;

	// Owns preflight-qualified library bytes and returns value-owned decoded data.
	class FShaderCookedLibrary final
	{
	public:
		FShaderCookedLibrary() = default;
		RENDERCORE_API static auto Open(
			const std::filesystem::path& Path,
			EShaderTargetPlatform TargetPlatform,
			EShaderTargetProfile TargetProfile,
			std::span<const FShaderRuntimeRequest> RequiredRequests,
			FShaderCookedLibrary& OutLibrary,
			std::string& OutError) -> bool;
		RENDERCORE_API static auto OpenBytes(
			std::shared_ptr<const FByteArray> Bytes,
			EShaderTargetPlatform TargetPlatform,
			EShaderTargetProfile TargetProfile,
			std::span<const FShaderRuntimeRequest> RequiredRequests,
			FShaderCookedLibrary& OutLibrary,
			std::string& OutError) -> bool;
		RENDERCORE_API auto Load(
			const FShaderRuntimeRequest& Request,
			FShaderCompilerOutput& OutOutput,
			std::string& OutError) const -> bool;
		[[nodiscard]] auto IsOpen() const -> bool { return State != nullptr; }
		RENDERCORE_API auto GetRecordCount() const -> uint32;
		RENDERCORE_API auto GetGenerationIdentity() const -> FXxHash128;

	private:
		struct FState;
		std::shared_ptr<const FState> State;
	};
}
