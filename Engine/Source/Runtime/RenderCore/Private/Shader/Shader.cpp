#include "Shader/Shader.h"

#include "DynamicRHI.h"
#include "Shader/ShaderCompiler.h"

namespace Durin
{
	namespace
	{
		struct FShaderBindingKey
		{
			uint32 SetIndex = 0;
			uint32 BindingIndex = 0;

			auto operator==(const FShaderBindingKey& Other) const -> bool
			{
				return SetIndex == Other.SetIndex && BindingIndex == Other.BindingIndex;
			}
		};

		struct FShaderBindingKeyHasher
		{
			auto operator()(const FShaderBindingKey& Key) const noexcept -> size_t
			{
				return std::hash<uint64>{}((static_cast<uint64>(Key.SetIndex) << 32) | Key.BindingIndex);
			}
		};

		class FShaderTypeRegistry
		{
		public:
			auto Register(const FShaderType* ShaderType) -> void
			{
				checkf(ShaderType, "Shader type must be valid");
				const FName TypeName = ShaderType->GetFName();
				const auto FoundIt = std::ranges::find_if(ShaderTypes, [TypeName](const FShaderType* ExistingType) {
					return ExistingType && ExistingType->GetFName() == TypeName;
				});
				checkf(FoundIt == ShaderTypes.end(), "Duplicate shader type registration: {}", TypeName.ToString());
				ShaderTypes.push_back(ShaderType);
			}

			auto Unregister(const FShaderType* ShaderType) -> void
			{
				const auto FoundIt = std::ranges::find(ShaderTypes, ShaderType);
				if (FoundIt != ShaderTypes.end())
				{
					ShaderTypes.erase(FoundIt);
				}
			}

			auto GetTypeList() const -> const std::vector<const FShaderType*>&
			{
				return ShaderTypes;
			}

		private:
			std::vector<const FShaderType*> ShaderTypes;
		};

		auto GetShaderTypeRegistry() -> FShaderTypeRegistry&
		{
			static FShaderTypeRegistry Registry;
			return Registry;
		}

		auto MakeDefaultShaderInstance(
			const FShaderType* ShaderType,
			FShaderMapBase* ShaderMap,
			uint32 ShaderIndex,
			const FShaderReflectionData& Reflection
		) -> std::unique_ptr<FShader>
		{
			return std::make_unique<FShader>(ShaderType, ShaderMap, ShaderIndex, Reflection);
		}
	} // namespace

	FShaderType::FShaderType(
		std::string_view InName,
		std::string_view InVirtualShaderPath,
		EShaderFrequency InFrequency,
		std::string_view InEntryPoint,
		std::string_view InDebugName,
		FShaderFactoryFunction InFactory,
		FShouldCompilePermutationFunction InShouldCompilePermutation,
		FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment
	)
		: Name(InName)
		, TypeName(Name)
		, VirtualShaderPath(InVirtualShaderPath)
		, Frequency(InFrequency)
		, EntryPoint(InEntryPoint)
		, DebugName(InDebugName.empty() ? InName : InDebugName)
		, Factory(InFactory ? InFactory : &MakeDefaultShaderInstance)
		, ShouldCompilePermutationFn(InShouldCompilePermutation)
		, ModifyCompilationEnvironmentFn(InModifyCompilationEnvironment)
	{
		GetShaderTypeRegistry().Register(this);
	}

	FShaderType::~FShaderType()
	{
		GetShaderTypeRegistry().Unregister(this);
	}

	auto FShaderType::CreateShaderInstance(FShaderMapBase* ShaderMap, uint32 ShaderIndex, const FShaderReflectionData& Reflection) const -> std::unique_ptr<FShader>
	{
		return Factory(this, ShaderMap, ShaderIndex, Reflection);
	}

	auto FShaderType::ShouldCompilePermutation(const FShaderPermutationParameters& Parameters) const -> bool
	{
		return ShouldCompilePermutationFn ? ShouldCompilePermutationFn(Parameters) : true;
	}

	auto FShaderType::ModifyCompilationEnvironment(const FShaderPermutationParameters& Parameters, FShaderCompileOptions& CompileOptions) const -> void
	{
		if (ModifyCompilationEnvironmentFn)
		{
			ModifyCompilationEnvironmentFn(Parameters, CompileOptions);
		}
	}

	auto FShaderType::GetTypeList() -> const std::vector<const FShaderType*>&
	{
		return GetShaderTypeRegistry().GetTypeList();
	}

	FShader::FShader(const FShaderType* InType, FShaderMapBase* InShaderMap, uint32 InShaderIndex, const FShaderReflectionData& InReflection)
		: Type(InType)
		, ShaderMap(InShaderMap)
		, ShaderIndex(InShaderIndex)
		, Reflection(InReflection)
	{
	}

	auto FShader::GetOrCreateRHIShader(bool bRequired) -> FRHIShader*
	{
		if (!ShaderRHI && ShaderMap && Type)
		{
			ShaderRHI = ShaderMap->GetOrCreateShaderRHI(Type, bRequired);
		}
		return ShaderRHI;
	}

	auto MakeShaderCreateDesc(const FCompiledShader& CompiledShader) -> FRHIShaderCreateDesc
	{
		checkf(CompiledShader.Code, "Compiled shader code must not be null");
		FRHIShaderCreateDesc CreateDesc = FRHIShaderCreateDesc::Create(
			CompiledShader.DebugName.empty() ? nullptr : CompiledShader.DebugName.c_str(),
			CompiledShader.Frequency,
			*CompiledShader.Code,
			CompiledShader.Hash
		);
		CreateDesc.SetEntryPoint(CompiledShader.EntryPoint.empty() ? "main" : CompiledShader.EntryPoint.c_str());
		return CreateDesc;
	}

	auto BuildPipelineLayoutFromReflection(
		std::span<const FShaderReflectionData> ReflectionData,
		FPipelineLayoutDesc& OutPipelineLayout,
		std::string& OutErrorMessage
	) -> bool
	{
		OutPipelineLayout = {};
		OutErrorMessage.clear();

		std::unordered_map<FShaderBindingKey, FShaderResourceBinding, FShaderBindingKeyHasher> MergedBindings;
		std::vector<FPushConstantRange> MergedPushConstants;
		uint32 MaxSetIndex = 0;

		for (const FShaderReflectionData& StageReflection : ReflectionData)
		{
			for (const FShaderResourceBinding& Binding : StageReflection.ResourceBindings)
			{
				FShaderBindingKey BindingKey{Binding.SetIndex, Binding.BindingIndex};
				MaxSetIndex = std::max(MaxSetIndex, Binding.SetIndex);

				auto FoundIt = MergedBindings.find(BindingKey);
				if (FoundIt == MergedBindings.end())
				{
					MergedBindings.emplace(BindingKey, Binding);
					continue;
				}

				FShaderResourceBinding& ExistingBinding = FoundIt->second;
				if (ExistingBinding.Type != Binding.Type)
				{
					OutErrorMessage = std::format(
						"Conflicting shader binding types at set {}, binding {}",
						Binding.SetIndex,
						Binding.BindingIndex
					);
					return false;
				}
				if (ExistingBinding.ArraySize != Binding.ArraySize)
				{
					OutErrorMessage = std::format(
						"Conflicting shader binding array sizes at set {}, binding {}",
						Binding.SetIndex,
						Binding.BindingIndex
					);
					return false;
				}

				ExistingBinding.StageFlags |= Binding.StageFlags;
				if (ExistingBinding.Name.empty())
				{
					ExistingBinding.Name = Binding.Name;
				}
			}

			for (const FPushConstantRange& PushConstantRange : StageReflection.PushConstantRanges)
			{
				bool bMergedRange = false;
				for (FPushConstantRange& ExistingRange : MergedPushConstants)
				{
					const uint32 ExistingBegin = ExistingRange.Offset;
					const uint32 ExistingEnd = ExistingRange.Offset + ExistingRange.Size;
					const uint32 NewBegin = PushConstantRange.Offset;
					const uint32 NewEnd = PushConstantRange.Offset + PushConstantRange.Size;

					if (ExistingRange.Offset == PushConstantRange.Offset && ExistingRange.Size == PushConstantRange.Size)
					{
						ExistingRange.StageFlags |= PushConstantRange.StageFlags;
						bMergedRange = true;
						break;
					}

					if (NewBegin < ExistingEnd && ExistingBegin < NewEnd)
					{
						OutErrorMessage = std::format(
							"Conflicting push constant ranges: existing [{}..{}), new [{}..{})",
							ExistingBegin,
							ExistingEnd,
							NewBegin,
							NewEnd
						);
						return false;
					}
				}

				if (!bMergedRange)
				{
					MergedPushConstants.push_back(PushConstantRange);
				}
			}
		}

		OutPipelineLayout.BindingLayouts.clear();
		OutPipelineLayout.BindingLayouts.resize(MergedBindings.empty() ? 0 : MaxSetIndex + 1);
		for (const auto& [Key, Binding] : MergedBindings)
		{
			OutPipelineLayout.BindingLayouts[Key.SetIndex].BindingLayouts.emplace_back(
				Binding.StageFlags,
				Binding.BindingIndex,
				Binding.Type,
				Binding.ArraySize
			);
		}

		for (FBindingLayout& BindingLayout : OutPipelineLayout.BindingLayouts)
		{
			std::ranges::sort(BindingLayout.BindingLayouts, [](const FBindingLayoutItem& A, const FBindingLayoutItem& B) {
				return A.Slot < B.Slot;
			});
		}

		std::ranges::sort(MergedPushConstants, [](const FPushConstantRange& A, const FPushConstantRange& B) {
			if (A.Offset != B.Offset)
			{
				return A.Offset < B.Offset;
			}
			return A.Size < B.Size;
		});
		OutPipelineLayout.PushConstantRanges = std::move(MergedPushConstants);
		return true;
	}

	auto BuildPipelineLayoutFromShaders(
		std::span<const FCompiledShader> CompiledShaders,
		FPipelineLayoutDesc& OutPipelineLayout,
		std::string& OutErrorMessage
	) -> bool
	{
		std::vector<FShaderReflectionData> ReflectionData;
		ReflectionData.reserve(CompiledShaders.size());
		for (const FCompiledShader& CompiledShader : CompiledShaders)
		{
			ReflectionData.push_back(CompiledShader.Reflection);
		}

		return BuildPipelineLayoutFromReflection(ReflectionData, OutPipelineLayout, OutErrorMessage);
	}

	auto FShaderMapResourceCode::AddCompiledShader(const FCompiledShader& CompiledShader) -> uint32
	{
		CompiledShaders.push_back(CompiledShader);
		return static_cast<uint32>(CompiledShaders.size() - 1);
	}

	FShaderMapResource::FShaderMapResource(std::shared_ptr<FShaderMapResourceCode> InCode)
		: Code(std::move(InCode))
	{
		checkf(Code, "Shader map resource code must not be null");
	}

	auto FShaderMapResource::AddShaderCompilerOutput(const FShaderCompilerOutput& Output) -> void
	{
		for (const FCompiledShader& CompiledShader : Output.CompiledShaders)
		{
			Code->AddCompiledShader(CompiledShader);
		}

		Shaders.resize(Code->GetNumShaders());
	}

	auto FShaderMapResource::GetShader(uint32 ShaderIndex, bool bRequired) const -> FRHIShader*
	{
		check(ShaderIndex < Code->GetNumShaders());
		if (ShaderIndex >= Shaders.size())
		{
			Shaders.resize(Code->GetNumShaders());
		}

		if (!Shaders[ShaderIndex])
		{
			return CreateRHIShader(ShaderIndex, bRequired);
		}

		return Shaders[ShaderIndex];
	}

	auto FShaderMapResource::CreateRHIShader(uint32 ShaderIndex, bool bRequired) const -> FRHIShader*
	{
		check(ShaderIndex < Code->GetNumShaders());
		checkf(GDynamicRHI, "Cannot create RHI shader without an active DynamicRHI");

		const FCompiledShader& CompiledShader = Code->GetCompiledShader(ShaderIndex);
		const FRHIShaderCreateDesc ShaderCreateDesc = MakeShaderCreateDesc(CompiledShader);
		FShaderRHIRef RHIShader = GDynamicRHI->RHICreateShader(ShaderCreateDesc);

		if (bRequired)
		{
			checkf(RHIShader, "Failed to create required shader '{}'", CompiledShader.DebugName);
		}

		if (ShaderIndex >= Shaders.size())
		{
			Shaders.resize(Code->GetNumShaders());
		}
		Shaders[ShaderIndex] = std::move(RHIShader);
		return Shaders[ShaderIndex];
	}

	auto FShaderMapResource::ReleaseRHIShader(uint32 ShaderIndex) -> FRHIShader*
	{
		check(ShaderIndex < Shaders.size());
		FRHIShader* Shader = Shaders[ShaderIndex];
		Shaders[ShaderIndex] = nullptr;
		return Shader;
	}

	FShaderMapBase::FShaderMapBase()
	{
		Reset();
	}

	auto FShaderMapBase::Initialize(std::span<const FShaderType* const> ShaderTypes, const FShaderCompilerOutput& Output, std::string& OutErrorMessage) -> bool
	{
		Reset();
		OutErrorMessage.clear();

		if (ShaderTypes.size() != Output.CompiledShaders.size())
		{
			OutErrorMessage = std::format(
				"Shader type count ({}) does not match compiled shader count ({})",
				ShaderTypes.size(),
				Output.CompiledShaders.size()
			);
			return false;
		}

		Resource->AddShaderCompilerOutput(Output);
		std::vector<FCompiledShader> CompiledShaders;
		CompiledShaders.reserve(Output.CompiledShaders.size());

		for (uint32 ShaderIndex = 0; ShaderIndex < ShaderTypes.size(); ++ShaderIndex)
		{
			const FShaderType* ShaderType = ShaderTypes[ShaderIndex];
			checkf(ShaderType, "Shader type must not be null");

			const FCompiledShader& CompiledShader = Code->GetCompiledShader(ShaderIndex);
			if (CompiledShader.Frequency != ShaderType->GetFrequency())
			{
				OutErrorMessage = std::format(
					"Compiled shader frequency does not match shader type '{}' at index {}",
					ShaderType->GetName(),
					ShaderIndex
				);
				Reset();
				return false;
			}

			if (!ShaderType->GetEntryPoint().empty() && CompiledShader.EntryPoint != ShaderType->GetEntryPoint())
			{
				OutErrorMessage = std::format(
					"Compiled shader entry point '{}' does not match shader type '{}' entry point '{}'",
					CompiledShader.EntryPoint,
					ShaderType->GetName(),
					ShaderType->GetEntryPoint()
				);
				Reset();
				return false;
			}

			ShaderTypeToIndex.emplace(ShaderType, ShaderIndex);
			std::unique_ptr<FShader> ShaderInstance = ShaderType->CreateShaderInstance(this, ShaderIndex, CompiledShader.Reflection);
			if (!ShaderInstance)
			{
				OutErrorMessage = std::format("Shader type '{}' failed to create a shader instance", ShaderType->GetName());
				Reset();
				return false;
			}
			ShaderInstances.emplace(ShaderType, std::move(ShaderInstance));
			CompiledShaders.push_back(CompiledShader);
		}

		if (!BuildPipelineLayoutFromShaders(CompiledShaders, MergedPipelineLayout, OutErrorMessage))
		{
			Reset();
			return false;
		}

		return true;
	}

	auto FShaderMapBase::FindShaderIndex(const FShaderType* ShaderType) const -> const uint32*
	{
		const auto FoundIt = ShaderTypeToIndex.find(ShaderType);
		return FoundIt != ShaderTypeToIndex.end() ? &FoundIt->second : nullptr;
	}

	auto FShaderMapBase::GetShader(const FShaderType* ShaderType) const -> FShader*
	{
		const auto FoundIt = ShaderInstances.find(ShaderType);
		return FoundIt != ShaderInstances.end() ? FoundIt->second.get() : nullptr;
	}

	auto FShaderMapBase::GetOrCreateShaderRHI(const FShaderType* ShaderType, bool bRequired) -> FRHIShader*
	{
		const uint32* ShaderIndex = FindShaderIndex(ShaderType);
		checkf(ShaderIndex, "Shader type '{}' is not part of this shader map", ShaderType ? ShaderType->GetName() : std::string_view("<null>"));
		FRHIShader* RHIShader = Resource ? Resource->GetShader(*ShaderIndex, bRequired) : nullptr;

		if (RHIShader)
		{
			if (FShader* ShaderInstance = GetShader(ShaderType))
			{
				ShaderInstance->SetShaderRHI(FShaderRHIRef(RHIShader));
			}
		}

		return RHIShader;
	}

	auto FShaderMapBase::Reset() -> void
	{
		ShaderTypeToIndex.clear();
		ShaderInstances.clear();
		MergedPipelineLayout = {};
		Code = std::make_shared<FShaderMapResourceCode>();
		Resource = std::make_shared<FShaderMapResource>(Code);
	}
} // namespace Durin
