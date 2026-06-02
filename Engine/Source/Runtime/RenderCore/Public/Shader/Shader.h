#pragma once

#include "RenderCoreAPI.h"
#include "RHIResources.h"

namespace Durin
{
	struct FShaderCompileOptions;
	struct FShaderCompilerOutput;

	using FShaderCode = std::vector<std::byte>;
	using FShaderCodeView = std::span<const std::byte>;

	struct FShaderResourceBinding
	{
		std::string Name;
		EShaderStageFlags StageFlags = EShaderStageFlags::None;
		uint32 SetIndex = 0;
		uint32 BindingIndex = 0;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		uint32 ArraySize = 1;

		auto operator==(const FShaderResourceBinding& Other) const -> bool
		{
			return Name == Other.Name
				&& StageFlags == Other.StageFlags
				&& SetIndex == Other.SetIndex
				&& BindingIndex == Other.BindingIndex
				&& Type == Other.Type
				&& ArraySize == Other.ArraySize;
		}
	};

	struct FShaderReflectionData
	{
		std::vector<FShaderResourceBinding> ResourceBindings;
		std::vector<FPushConstantRange> PushConstantRanges;
	};

	struct FCompiledShader
	{
		EShaderFrequency Frequency = EShaderFrequency::Vertex;
		// Source-level entry point requested by the caller, such as `vertexMain`.
		std::string SourceEntryPoint;
		// Backend-visible entry point exported by the compiled binary, such as Vulkan SPIR-V `main`.
		std::string BinaryEntryPoint = "main";
		std::string DebugName;
		std::shared_ptr<FShaderCode> Code;
		FXxHash128 Hash{};
		FShaderReflectionData Reflection;
	};

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
		uint32 ShaderIndex,
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
			FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment = nullptr
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

		RENDERCORE_API auto CreateShaderInstance(FShaderMapBase* ShaderMap, uint32 ShaderIndex, const FShaderReflectionData& Reflection) const -> std::unique_ptr<FShader>;
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
		FShaderFactoryFunction Factory = nullptr;
		FShouldCompilePermutationFunction ShouldCompilePermutationFn = nullptr;
		FModifyCompilationEnvironmentFunction ModifyCompilationEnvironmentFn = nullptr;
	};

	class FShader
	{
	public:
		RENDERCORE_API FShader(const FShaderType* InType, FShaderMapBase* InShaderMap, uint32 InShaderIndex, const FShaderReflectionData& InReflection);
		virtual ~FShader() = default;

		auto GetType() const -> const FShaderType* { return Type; }
		auto GetShaderMap() const -> FShaderMapBase* { return ShaderMap; }
		auto GetShaderIndex() const -> uint32 { return ShaderIndex; }
		auto GetReflection() const -> const FShaderReflectionData& { return Reflection; }

		RENDERCORE_API auto GetOrCreateRHIShader(bool bRequired = true) -> FRHIShader*;

	protected:
		const FShaderType* Type = nullptr;
		FShaderMapBase* ShaderMap = nullptr;
		uint32 ShaderIndex = 0;
		FShaderReflectionData Reflection;
	};

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

		auto GetCodeView(uint32 ShaderIndex) const -> FShaderCodeView
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
} // namespace Durin
