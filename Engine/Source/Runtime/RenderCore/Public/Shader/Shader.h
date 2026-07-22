#pragma once

#include "RenderCoreAPI.h"
#include "RHICommandList.h"
#include "RHIResources.h"

#include "ShaderCompilerCore.h"

namespace Durin
{
	class FShaderType;
	class FShader;
	class FShaderMapBase;

	struct FShaderPermutationParameters
	{
		const FShaderType* ShaderType = nullptr;
		std::string_view VirtualShaderPath;
		std::string_view EntryPoint;
		EShaderFrequency Frequency = EShaderFrequency::Vertex;
	};

	using FShaderFactoryFunction = std::unique_ptr<FShader> (*)(
		const FShaderType* ShaderType,
		FShaderMapBase* ShaderMap,
		const FShaderReflectionData& Reflection
	);

	using FShouldCompilePermutationFunction = bool (*)(const FShaderPermutationParameters& Parameters);
	using FModifyCompilationEnvironmentFunction = void (*)(const FShaderPermutationParameters& Parameters, FShaderCompileOptions& CompileOptions);

	class FShaderType
	{
	public:
		RENDERCORE_API FShaderType(
			std::string_view InName,
			std::string_view InVirtualShaderPath,
			EShaderFrequency InFrequency,
			std::string_view InEntryPoint,
			std::string_view InDebugName = {},
			FShaderFactoryFunction InFactory = nullptr,
			FShouldCompilePermutationFunction InShouldCompilePermutation = nullptr,
			FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment = nullptr,
			const FShaderParametersMetadata* InParametersMetadata = nullptr
		);

		RENDERCORE_API ~FShaderType();

		FShaderType(const FShaderType&) = delete;
		auto operator=(const FShaderType&) -> FShaderType& = delete;

		auto GetName() const -> std::string_view { return Name; }
		auto GetFName() const -> FName { return TypeName; }
		auto GetVirtualShaderPath() const -> std::string_view { return VirtualShaderPath; }
		auto GetFrequency() const -> EShaderFrequency { return Frequency; }
		auto GetEntryPoint() const -> std::string_view { return EntryPoint; }
		auto GetDebugName() const -> std::string_view { return DebugName; }
		auto GetParametersMetadata() const -> const FShaderParametersMetadata* { return ParametersMetadata; }
		auto GetParameterMetadata() const -> std::span<const FShaderParameterMemberMetadata>
		{
			return ParametersMetadata ? ParametersMetadata->Members : std::span<const FShaderParameterMemberMetadata>{};
		}

		RENDERCORE_API auto CreateShaderInstance(FShaderMapBase* ShaderMap, const FShaderReflectionData& Reflection) const -> std::unique_ptr<FShader>;
		RENDERCORE_API auto ShouldCompilePermutation(const FShaderPermutationParameters& Parameters) const -> bool;
		RENDERCORE_API auto ModifyCompilationEnvironment(const FShaderPermutationParameters& Parameters, FShaderCompileOptions& CompileOptions) const -> void;

		RENDERCORE_API static auto GetTypeList() -> const std::vector<const FShaderType*>&;

	private:
		std::string Name;
		FName TypeName;
		std::string VirtualShaderPath;
		EShaderFrequency Frequency = EShaderFrequency::Vertex;
		std::string EntryPoint;
		std::string DebugName;
		const FShaderParametersMetadata* ParametersMetadata = nullptr;
		FShaderFactoryFunction Factory = nullptr;
		FShouldCompilePermutationFunction ShouldCompilePermutationFn = nullptr;
		FModifyCompilationEnvironmentFunction ModifyCompilationEnvironmentFn = nullptr;
	};

	class FShader
	{
	public:
		RENDERCORE_API FShader(const FShaderType* InType, FShaderMapBase* InShaderMap, const FShaderReflectionData& InReflection);
		virtual ~FShader() = default;

		auto GetType() const -> const FShaderType* { return Type; }
		auto GetShaderMap() const -> FShaderMapBase* { return ShaderMap; }
		auto GetReflection() const -> const FShaderReflectionData& { return Reflection; }
		auto GetParameterBindings() const -> std::span<const FShaderParameterBinding> { return ParameterBindings; }

		RENDERCORE_API auto GetOrCreateRHIShader(bool bRequired = true) -> FRHIShader*;
		RENDERCORE_API auto InitializeParameterBindings(std::string& OutErrorMessage) -> bool;

	protected:
		const FShaderType* Type = nullptr;
		FShaderMapBase* ShaderMap = nullptr;
		FShaderReflectionData Reflection;
		std::vector<FShaderParameterBinding> ParameterBindings;
	};

	template<typename ShaderType>
	auto CreateDefaultShaderInstance(
		const FShaderType* InType,
		FShaderMapBase* InShaderMap,
		const FShaderReflectionData& InReflection
	) -> std::unique_ptr<FShader>
	{
		return std::make_unique<ShaderType>(InType, InShaderMap, InReflection);
	}

	template<typename ShaderType>
	auto MakeShaderType(
		std::string_view InName,
		std::string_view InVirtualShaderPath,
		EShaderFrequency InFrequency,
		std::string_view InEntryPoint,
		std::string_view InDebugName = {},
		const FShaderParametersMetadata* InParametersMetadata = nullptr
	) -> FShaderType
	{
		return FShaderType(
			InName,
			InVirtualShaderPath,
			InFrequency,
			InEntryPoint,
			InDebugName,
			&CreateDefaultShaderInstance<ShaderType>,
			nullptr,
			nullptr,
			InParametersMetadata
		);
	}

	template<typename ParameterStruct, size_t N>
	constexpr auto MakeInlineShaderParametersMetadata(
		std::string_view StructName,
		const std::array<FShaderParameterMemberMetadata, N>& Members
	) -> FShaderParametersMetadata
	{
		return FShaderParametersMetadata{
			.StructName = StructName.data(),
			.StructSize = sizeof(ParameterStruct),
			.StructAlignment = alignof(ParameterStruct),
			.IncludedParameters = nullptr,
			.Members = Members
		};
	}

	template<typename ShaderClass>
	struct TShaderParametersOwnerTag
	{
	};

	template<ERHIBindingType BindingType>
	struct TShaderParameterCppType;

	template<>
	struct TShaderParameterCppType<ERHIBindingType::Texture>
	{
		using Type = FRHITexture*;
	};

	template<>
	struct TShaderParameterCppType<ERHIBindingType::Sampler>
	{
		using Type = FRHISampler*;
	};

	template<>
	struct TShaderParameterCppType<ERHIBindingType::UniformBuffer>
	{
		using Type = FRHIUniformBufferRange;
	};

	template<>
	struct TShaderParameterCppType<ERHIBindingType::UniformBufferDynamic>
	{
		using Type = FRHIUniformBufferRange;
	};

	template<>
	struct TShaderParameterCppType<ERHIBindingType::StorageBuffer>
	{
		using Type = FRHIStorageBufferRange;
	};

	template<>
	struct TShaderParameterCppType<ERHIBindingType::StorageImage>
	{
		using Type = FRHITexture*;
	};

	template<ERHIBindingType BindingType, typename MemberType>
	consteval auto MakeShaderParameterMemberMetadata(const char* Name, uint32 Offset) -> FShaderParameterMemberMetadata
	{
		static_assert(std::same_as<MemberType, typename TShaderParameterCppType<BindingType>::Type>, "Shader parameter member type does not match binding type");
		return FShaderParameterMemberMetadata{
			.Name = Name,
			.Offset = Offset,
			.Size = static_cast<uint32>(sizeof(MemberType)),
			.ArraySize = 1,
			.Type = BindingType,
			.Kind = EShaderParameterMemberKind::Resource
		};
	}

	inline auto GetOwnShaderParametersMetadata(...) -> const FShaderParametersMetadata* { return nullptr; }
	inline auto GetIncludedShaderParametersMetadata(...) -> const FShaderParametersMetadata* { return nullptr; }

	template<typename ShaderClass>
	auto GetOwnShaderParametersMetadataOrNull() -> const FShaderParametersMetadata*
	{
		return GetOwnShaderParametersMetadata(TShaderParametersOwnerTag<ShaderClass>{});
	}

	template<typename ShaderClass>
	auto GetIncludedShaderParametersMetadataOrNull() -> const FShaderParametersMetadata*
	{
		return GetIncludedShaderParametersMetadata(TShaderParametersOwnerTag<ShaderClass>{});
	}

	RENDERCORE_API auto BuildCombinedShaderParametersMetadataStorage(
		std::string_view StructName,
		uint32 StructSize,
		uint32 StructAlignment,
		std::span<const FShaderParameterMemberMetadata> OwnMembers,
		const FShaderParametersMetadata* IncludedParameters
	) -> FShaderParametersMetadataStorage;

	template<typename ShaderClass>
	auto GetShaderParametersMetadataOrNull() -> const FShaderParametersMetadata*
	{
		const FShaderParametersMetadata* OwnParameters = GetOwnShaderParametersMetadataOrNull<ShaderClass>();
		const FShaderParametersMetadata* IncludedParameters = GetIncludedShaderParametersMetadataOrNull<ShaderClass>();
		if (OwnParameters == nullptr)
		{
			return IncludedParameters;
		}

		static const FShaderParametersMetadataStorage Storage = BuildCombinedShaderParametersMetadataStorage(
			OwnParameters->StructName ? OwnParameters->StructName : "FParameters",
			OwnParameters->StructSize,
			OwnParameters->StructAlignment,
			OwnParameters->Members,
			IncludedParameters
		);
		return &Storage.Metadata;
	}

	RENDERCORE_API auto BuildShaderParameterBindings(
		const FShaderParametersMetadata* ParametersMetadata,
		const FShaderReflectionData& Reflection,
		std::vector<FShaderParameterBinding>& OutBindings,
		std::string& OutErrorMessage
	) -> bool;

	RENDERCORE_API auto SetShaderParametersImpl(
		FRHICommandListBase& RHICmdList,
		FRHIShader* RHIShader,
		const FShaderParametersMetadata& ParametersMetadata,
		std::span<const FShaderParameterBinding> ParameterBindings,
		const void* ParameterData
	) -> void;

	#define DURIN_PRIVATE_SHADER_PARAMETER(MemberType, MemberName, BindingTypeValue) \
		MemberType MemberName = nullptr; \
		static auto GetShaderParameterMemberMetadata(TShaderParameterTag<__COUNTER__>) -> FShaderParameterMemberMetadata \
		{ \
			return MakeShaderParameterMemberMetadata<BindingTypeValue, decltype(FParameters::MemberName)>( \
				#MemberName, \
				static_cast<uint32>(offsetof(FParameters, MemberName)) \
			); \
		}

	#define DURIN_BEGIN_SHADER_PARAMETERS(ShaderClass) \
	private: \
		using FShaderParametersOwner = ShaderClass; \
		template<int Index> struct TShaderParameterTag {}; \
		static constexpr int ShaderParameterCounterBegin = __COUNTER__; \
	public: \
		struct FParameters {

	#define DURIN_SHADER_PARAMETER_TEXTURE(MemberName) \
		DURIN_PRIVATE_SHADER_PARAMETER(FRHITexture*, MemberName, ERHIBindingType::Texture)

	#define DURIN_SHADER_PARAMETER_SAMPLER(MemberName) \
		DURIN_PRIVATE_SHADER_PARAMETER(FRHISampler*, MemberName, ERHIBindingType::Sampler)

	#define DURIN_SHADER_PARAMETER_STORAGE_IMAGE(MemberName) \
		DURIN_PRIVATE_SHADER_PARAMETER(FRHITexture*, MemberName, ERHIBindingType::StorageImage)

	#define DURIN_SHADER_PARAMETER_UNIFORM_BUFFER(MemberName) \
		FRHIUniformBufferRange MemberName; \
		static auto GetShaderParameterMemberMetadata(TShaderParameterTag<__COUNTER__>) -> FShaderParameterMemberMetadata \
		{ \
			return MakeShaderParameterMemberMetadata<ERHIBindingType::UniformBuffer, decltype(FParameters::MemberName)>( \
				#MemberName, \
				static_cast<uint32>(offsetof(FParameters, MemberName)) \
			); \
		}

	#define DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(MemberName) \
		FRHIUniformBufferRange MemberName; \
		static auto GetShaderParameterMemberMetadata(TShaderParameterTag<__COUNTER__>) -> FShaderParameterMemberMetadata \
		{ \
			return MakeShaderParameterMemberMetadata<ERHIBindingType::UniformBufferDynamic, decltype(FParameters::MemberName)>( \
				#MemberName, \
				static_cast<uint32>(offsetof(FParameters, MemberName)) \
			); \
		}

	#define DURIN_SHADER_PARAMETER_STORAGE_BUFFER(MemberName) \
		FRHIStorageBufferRange MemberName; \
		static auto GetShaderParameterMemberMetadata(TShaderParameterTag<__COUNTER__>) -> FShaderParameterMemberMetadata \
		{ \
			return MakeShaderParameterMemberMetadata<ERHIBindingType::StorageBuffer, decltype(FParameters::MemberName)>( \
				#MemberName, \
				static_cast<uint32>(offsetof(FParameters, MemberName)) \
			); \
		}

	#define DURIN_END_SHADER_PARAMETERS() \
		}; \
	private: \
		static constexpr int ShaderParameterCounterEnd = __COUNTER__; \
		template<int... Indices> \
		static auto BuildShaderParameterMemberMetadata(std::integer_sequence<int, Indices...>) \
			-> std::array<FShaderParameterMemberMetadata, sizeof...(Indices)> \
		{ \
			return {FParameters::GetShaderParameterMemberMetadata(TShaderParameterTag<ShaderParameterCounterBegin + 1 + Indices>{})...}; \
		} \
	public: \
		static auto GetOwnParametersMetadata() -> const FShaderParametersMetadata* \
		{ \
			static_assert(std::is_standard_layout_v<FParameters>, "Shader parameter structs must use standard layout"); \
			static const auto Members = BuildShaderParameterMemberMetadata(std::make_integer_sequence<int, ShaderParameterCounterEnd - ShaderParameterCounterBegin - 1>{}); \
			static const FShaderParametersMetadata ParametersMetadata = MakeInlineShaderParametersMetadata<FParameters>("FParameters", Members); \
			return &ParametersMetadata; \
		} \
 \
		friend auto GetOwnShaderParametersMetadata(TShaderParametersOwnerTag<FShaderParametersOwner>) -> const FShaderParametersMetadata* \
		{ \
			return GetOwnParametersMetadata(); \
		}

	#define DURIN_INCLUDE_SHADER_PARAMETERS(ParameterOwnerClass) \
		friend auto GetIncludedShaderParametersMetadata(TShaderParametersOwnerTag<FShaderParametersOwner>) -> const FShaderParametersMetadata* \
		{ \
			return GetShaderParametersMetadataOrNull<ParameterOwnerClass>(); \
		}

	#define DURIN_INCLUDE_SHADER_PARAMETERS_FOR(ShaderClass, ParameterOwnerClass) \
		friend auto GetIncludedShaderParametersMetadata(TShaderParametersOwnerTag<ShaderClass>) -> const FShaderParametersMetadata* \
		{ \
			return GetShaderParametersMetadataOrNull<ParameterOwnerClass>(); \
		}

	#define DURIN_DECLARE_SHADER_NAMED(ShaderClass, SuperClass, TypeNameLiteral, VirtualPathLiteral, FrequencyValue, EntryPointLiteral) \
		ShaderClass(const FShaderType* InType, FShaderMapBase* InShaderMap, const FShaderReflectionData& InReflection) \
			: SuperClass(InType, InShaderMap, InReflection) \
		{ \
		} \
 \
		static auto GetParametersMetadata() -> const FShaderParametersMetadata* \
		{ \
			return GetShaderParametersMetadataOrNull<ShaderClass>(); \
		} \
 \
		static auto StaticType() -> FShaderType& \
		{ \
			static FShaderType ShaderType = MakeShaderType<ShaderClass>( \
				TypeNameLiteral, \
				VirtualPathLiteral, \
				FrequencyValue, \
				EntryPointLiteral, \
				{}, \
				GetShaderParametersMetadataOrNull<ShaderClass>() \
			); \
			return ShaderType; \
		}

	#define DURIN_DECLARE_SHADER(ShaderClass, SuperClass, VirtualPathLiteral, FrequencyValue, EntryPointLiteral) \
		DURIN_DECLARE_SHADER_NAMED(ShaderClass, SuperClass, #ShaderClass, VirtualPathLiteral, FrequencyValue, EntryPointLiteral)

	#define DURIN_DECLARE_SHADER_TYPE(ShaderClass, TypeNameLiteral, VirtualPathLiteral, FrequencyValue, EntryPointLiteral) \
		DURIN_DECLARE_SHADER_NAMED(ShaderClass, FShader, TypeNameLiteral, VirtualPathLiteral, FrequencyValue, EntryPointLiteral)

	#define DURIN_DECLARE_SHADER_TYPE_WITH_PARAMETERS(ShaderClass, TypeNameLiteral, VirtualPathLiteral, FrequencyValue, EntryPointLiteral) \
		DURIN_DECLARE_SHADER_NAMED(ShaderClass, FShader, TypeNameLiteral, VirtualPathLiteral, FrequencyValue, EntryPointLiteral)

	RENDERCORE_API auto MakeShaderCreateDesc(const FCompiledShader& CompiledShader) -> FRHIShaderCreateDesc;
	RENDERCORE_API auto BuildPipelineLayoutFromReflection(
		std::span<const FShaderReflectionData> ReflectionData,
		FPipelineLayoutDesc& OutPipelineLayout,
		std::string& OutErrorMessage
	) -> bool;
	RENDERCORE_API auto BuildPipelineLayoutFromShaders(
		std::span<const FCompiledShader> CompiledShaders,
		FPipelineLayoutDesc& OutPipelineLayout,
		std::string& OutErrorMessage
	) -> bool;
	struct FShaderMapResourceCacheStats
	{
		uint64 EntryCount = 0;
		uint64 LiveEntryCount = 0;
	};
	RENDERCORE_API auto GetShaderMapResourceCacheStats() -> FShaderMapResourceCacheStats;
	RENDERCORE_API auto ClearShaderMapResourceCache() -> void;

	class FShaderMapResourceCode
	{
	public:
		auto GetNumShaders() const -> uint32 { return static_cast<uint32>(CompiledShaders.size()); }

		auto GetCompiledShader(uint32 ShaderIndex) const -> const FCompiledShader&
		{
			check(ShaderIndex < CompiledShaders.size());
			return CompiledShaders[ShaderIndex];
		}

		auto GetCodeView(uint32 ShaderIndex) const -> std::span<const std::byte>
		{
			const FCompiledShader& CompiledShader = GetCompiledShader(ShaderIndex);
			checkf(CompiledShader.Code, "Compiled shader code must not be null");
			return *CompiledShader.Code;
		}

		auto AddCompiledShader(const FCompiledShader& CompiledShader) -> uint32;

	private:
		std::vector<FCompiledShader> CompiledShaders;
	};

	class FShaderMapResource
	{
	public:
		RENDERCORE_API explicit FShaderMapResource(std::shared_ptr<FShaderMapResourceCode> InCode);
		~FShaderMapResource() = default;

		RENDERCORE_API auto AddShaderCompilerOutput(const FShaderCompilerOutput& Output) -> void;
		RENDERCORE_API auto GetShader(uint32 ShaderIndex, bool bRequired = true) const -> FRHIShader*;
		RENDERCORE_API auto CreateRHIShader(uint32 ShaderIndex, bool bRequired = true) const -> FRHIShader*;

		auto GetCode() const -> const FShaderMapResourceCode& { return *Code; }

	protected:
		auto ReleaseRHIShader(uint32 ShaderIndex) -> FRHIShader*;

		std::shared_ptr<FShaderMapResourceCode> Code;
		mutable std::vector<FShaderRHIRef> Shaders;
		mutable std::mutex Mutex;
	};

	class FShaderMapBase
	{
	public:
		RENDERCORE_API FShaderMapBase();
		virtual ~FShaderMapBase() = default;

		auto GetResource() const -> FShaderMapResource* { return Resource.get(); }
		auto GetCode() const -> FShaderMapResourceCode* { return Code.get(); }
		auto GetMergedPipelineLayout() const -> const FPipelineLayoutDesc& { return MergedPipelineLayout; }
		auto GetCacheKey() const -> FXxHash128 { return CacheKey; }

		RENDERCORE_API auto Initialize(std::span<const FShaderType* const> ShaderTypes, const FShaderCompilerOutput& Output, std::string& OutErrorMessage) -> bool;
		RENDERCORE_API auto Initialize(
			std::span<const FShaderType* const> ShaderTypes,
			const FShaderCompilerOutput& Output,
			const FShaderCompileOptions& CompileOptions,
			std::string& OutErrorMessage
		) -> bool;
		RENDERCORE_API auto InitializeFromShaderTypes(
			std::span<const FShaderType* const> ShaderTypes,
			const FShaderCompileOptions& CompileOptions,
			std::string& OutErrorMessage
		) -> bool;
		RENDERCORE_API auto FindShaderIndex(const FShaderType* ShaderType) const -> const uint32*;
		RENDERCORE_API auto GetShader(const FShaderType* ShaderType) const -> FShader*;
		RENDERCORE_API auto GetOrCreateShaderRHI(const FShaderType* ShaderType, bool bRequired = true) -> FRHIShader*;

	protected:
		RENDERCORE_API auto Reset() -> void;

		std::shared_ptr<FShaderMapResource> Resource;
		std::shared_ptr<FShaderMapResourceCode> Code;
		std::unordered_map<const FShaderType*, uint32> ShaderTypeToIndex;
		std::unordered_map<const FShaderType*, std::unique_ptr<FShader>> ShaderInstances;
		FPipelineLayoutDesc MergedPipelineLayout;
		FXxHash128 CacheKey{};
	};

	template<typename ShaderType>
	class TShaderRef
	{
	public:
		TShaderRef() = default;

		TShaderRef(ShaderType* InShader, FShaderMapBase* InShaderMap)
			: ShaderContent(InShader)
			, ShaderMap(InShaderMap)
		{
		}

		auto GetResource() const -> FShaderMapResource*
		{
			check(ShaderMap);
			return ShaderMap->GetResource();
		}

		auto GetShader() const -> ShaderType* { return ShaderContent; }
		auto GetShaderMap() const -> FShaderMapBase* { return ShaderMap; }

		auto GetRHIShader() const -> FRHIShader*
		{
			return ShaderContent ? ShaderContent->GetOrCreateRHIShader() : nullptr;
		}

		explicit operator bool() const
		{
			return ShaderContent != nullptr;
		}

	private:
		ShaderType* ShaderContent = nullptr;
		FShaderMapBase* ShaderMap = nullptr;
	};

	template<typename ShaderType>
	auto SetShaderParameters(FRHICommandListBase& RHICmdList, const TShaderRef<ShaderType>& Shader, const typename ShaderType::FParameters& Parameters) -> void
	{
		const ShaderType* ShaderContent = Shader.GetShader();
		check(ShaderContent);
		static_assert(std::is_standard_layout_v<typename ShaderType::FParameters>, "Shader parameter structs must use standard layout");

		const FShaderParametersMetadata* ParametersMetadata = ShaderContent->GetType() ? ShaderContent->GetType()->GetParametersMetadata() : nullptr;
		checkf(ParametersMetadata, "Shader '{}' must provide parameter metadata", ShaderContent->GetType() ? ShaderContent->GetType()->GetName() : "<unknown>");
		checkf(
			ParametersMetadata->StructSize == sizeof(typename ShaderType::FParameters),
			"Shader '{}' parameter struct size mismatch",
			ShaderContent->GetType() ? ShaderContent->GetType()->GetName() : "<unknown>"
		);
		SetShaderParametersImpl(RHICmdList, Shader.GetRHIShader(), *ParametersMetadata, ShaderContent->GetParameterBindings(), &Parameters);
	}
} // namespace Durin
