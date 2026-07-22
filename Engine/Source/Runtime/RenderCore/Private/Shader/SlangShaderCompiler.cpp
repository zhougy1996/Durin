#include "SlangShaderCompiler.h"

#include "Hash/XxHash.h"
#include "RHIResources.h"
#include "ShaderCompileUtilities.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view GSlangTargetProfile = "spirv_1_5";

		auto ToStageFlags(EShaderFrequency Frequency) -> EShaderStageFlags
		{
			switch (Frequency)
			{
			case EShaderFrequency::Vertex:
				return EShaderStageFlags::Vertex;
			case EShaderFrequency::Fragment:
				return EShaderStageFlags::Fragment;
			case EShaderFrequency::Compute:
				return EShaderStageFlags::Compute;
			default:
				return EShaderStageFlags::None;
			}
		}

		auto ResolvePushConstantByteSize(slang::TypeLayoutReflection* TypeLayout) -> uint32
		{
			if (!TypeLayout)
			{
				return 0;
			}

			slang::TypeLayoutReflection* SizeLayout = TypeLayout;
			const slang::TypeReflection::Kind TypeKind = TypeLayout->getKind();
			if (TypeKind == slang::TypeReflection::Kind::ConstantBuffer || TypeKind == slang::TypeReflection::Kind::ParameterBlock)
			{
				if (slang::TypeLayoutReflection* ElementTypeLayout = TypeLayout->getElementTypeLayout())
				{
					SizeLayout = ElementTypeLayout;
				}
			}

			const size_t UniformSize = SizeLayout->getSize(slang::ParameterCategory::Uniform);
			if (UniformSize != SLANG_UNKNOWN_SIZE && UniformSize != SLANG_UNBOUNDED_SIZE)
			{
				return static_cast<uint32>(UniformSize);
			}

			const size_t PushConstantSize = SizeLayout->getSize(slang::ParameterCategory::PushConstantBuffer);
			if (PushConstantSize != SLANG_UNKNOWN_SIZE && PushConstantSize != SLANG_UNBOUNDED_SIZE)
			{
				return static_cast<uint32>(PushConstantSize);
			}

			return 0;
		}

		auto CollectPushConstantOffsets(
			slang::VariableLayoutReflection* VariableLayout,
			EShaderStageFlags StageFlags,
			std::unordered_map<std::string, FPushConstantRange>& OutPushConstantRanges
		) -> void
		{
			if (!VariableLayout)
			{
				return;
			}

			slang::TypeLayoutReflection* TypeLayout = VariableLayout->getTypeLayout();
			if (!TypeLayout)
			{
				return;
			}

			if (VariableLayout->getCategory() == slang::ParameterCategory::PushConstantBuffer)
			{
				const size_t Offset = VariableLayout->getOffset(slang::ParameterCategory::PushConstantBuffer);
				const uint32 Size = ResolvePushConstantByteSize(TypeLayout);
				if (Offset != SLANG_UNKNOWN_SIZE
					&& Offset != SLANG_UNBOUNDED_SIZE
					&& Size > 0)
				{
					const std::string Name = VariableLayout->getName() ? VariableLayout->getName() : "";
					OutPushConstantRanges[Name] = FPushConstantRange{
						StageFlags,
						static_cast<uint32>(Offset),
						Size
					};
				}
			}

			for (unsigned FieldIndex = 0; FieldIndex < TypeLayout->getFieldCount(); ++FieldIndex)
			{
				CollectPushConstantOffsets(TypeLayout->getFieldByIndex(FieldIndex), StageFlags, OutPushConstantRanges);
			}
		}

		auto AppendResourceBinding(FShaderReflectionData& OutReflection, FShaderResourceBinding Binding) -> void
		{
			const auto FoundIt = std::ranges::find_if(OutReflection.ResourceBindings, [&Binding](const FShaderResourceBinding& ExistingBinding) {
				return ExistingBinding.SetIndex == Binding.SetIndex
					&& ExistingBinding.BindingIndex == Binding.BindingIndex
					&& ExistingBinding.Type == Binding.Type
					&& ExistingBinding.ArraySize == Binding.ArraySize;
			});
			if (FoundIt == OutReflection.ResourceBindings.end())
			{
				OutReflection.ResourceBindings.push_back(std::move(Binding));
				return;
			}

			FoundIt->StageFlags |= Binding.StageFlags;
			if (FoundIt->Name.empty())
			{
				FoundIt->Name = std::move(Binding.Name);
			}
		}

		auto AppendPushConstantRange(FShaderReflectionData& OutReflection, FPushConstantRange Range) -> void
		{
			const auto FoundIt = std::ranges::find_if(OutReflection.PushConstantRanges, [&Range](const FPushConstantRange& ExistingRange) {
				return ExistingRange.Offset == Range.Offset && ExistingRange.Size == Range.Size;
			});
			if (FoundIt == OutReflection.PushConstantRanges.end())
			{
				OutReflection.PushConstantRanges.push_back(Range);
				return;
			}

			FoundIt->StageFlags |= Range.StageFlags;
		}

		auto CollectUsedPushConstantVariableNamesFromSpirv(std::span<const uint32> SpirvWords, std::unordered_set<std::string>& OutNames) -> bool
		{
			OutNames.clear();
			if (SpirvWords.size() < 5)
			{
				return false;
			}

			constexpr uint32 SpvMagicNumber = 0x07230203u;
			constexpr uint16 OpName = 5u;
			constexpr uint16 OpVariable = 59u;
			constexpr uint32 StorageClassPushConstant = 9u;

			std::unordered_map<uint32, std::string> DebugNames;
			std::unordered_set<uint32> PushConstantVariableIds;

			for (size_t WordIndex = 5; WordIndex < SpirvWords.size();)
			{
				const uint32 InstructionWord = SpirvWords[WordIndex];
				const uint16 WordCount = static_cast<uint16>(InstructionWord >> 16);
				const uint16 Opcode = static_cast<uint16>(InstructionWord & 0xffffu);
				if (WordCount == 0 || WordIndex + WordCount > SpirvWords.size())
				{
					return false;
				}

				switch (Opcode)
				{
				case OpName:
				{
					if (WordCount >= 3)
					{
						const uint32 TargetId = SpirvWords[WordIndex + 1];
						const char* NameChars = reinterpret_cast<const char*>(SpirvWords.data() + WordIndex + 2);
						DebugNames[TargetId] = NameChars ? std::string(NameChars) : std::string();
					}
					break;
				}
				case OpVariable:
				{
					if (WordCount >= 4)
					{
						const uint32 ResultId = SpirvWords[WordIndex + 2];
						const uint32 StorageClass = SpirvWords[WordIndex + 3];
						if (StorageClass == StorageClassPushConstant)
						{
							PushConstantVariableIds.insert(ResultId);
						}
					}
					break;
				}
				default:
					break;
				}

				WordIndex += WordCount;
			}

			if (SpirvWords[0] != SpvMagicNumber)
			{
				return false;
			}

			for (const uint32 VariableId : PushConstantVariableIds)
			{
				if (const auto FoundIt = DebugNames.find(VariableId); FoundIt != DebugNames.end())
				{
					OutNames.insert(FoundIt->second);
				}
			}

			return !PushConstantVariableIds.empty();
		}

		auto AppendUsedPushConstantRanges(
			const std::unordered_map<std::string, FPushConstantRange>& PushConstantRanges,
			std::span<const uint32> SpirvWords,
			FShaderReflectionData& OutReflection
		) -> void
		{
			std::unordered_set<std::string> UsedVariableNames;
			const bool bHasPushConstantVariables = CollectUsedPushConstantVariableNamesFromSpirv(SpirvWords, UsedVariableNames);
			if (!bHasPushConstantVariables)
			{
				return;
			}

			if (UsedVariableNames.empty())
			{
				for (const auto& [Name, Range] : PushConstantRanges)
				{
					AppendPushConstantRange(OutReflection, Range);
				}
				return;
			}

			for (const std::string& UsedName : UsedVariableNames)
			{
				if (const auto FoundIt = PushConstantRanges.find(UsedName); FoundIt != PushConstantRanges.end())
				{
					AppendPushConstantRange(OutReflection, FoundIt->second);
				}
			}
		}

		auto IsBindingRangeUsed(
			slang::TypeLayoutReflection* TypeLayout,
			SlangInt BindingRangeIndex,
			slang::IMetadata* Metadata,
			std::string_view BindingName,
			std::string& OutErrorMessage,
			bool& bOutUsed
		) -> bool
		{
			bOutUsed = true;
			if (!TypeLayout || !Metadata)
			{
				return true;
			}

			const slang::BindingType BindingType = TypeLayout->getBindingRangeType(BindingRangeIndex);
			SlangInt SpaceIndex = 0;
			SlangInt RegisterIndex = 0;
			slang::ParameterCategory Category = slang::ParameterCategory::None;

			switch (BindingType)
			{
			case slang::BindingType::ConstantBuffer:
			case slang::BindingType::Texture:
			case slang::BindingType::MutableTexture:
			case slang::BindingType::TypedBuffer:
			case slang::BindingType::RawBuffer:
			case slang::BindingType::MutableTypedBuffer:
			case slang::BindingType::MutableRawBuffer:
			case slang::BindingType::Sampler:
			{
				const SlangInt DescriptorSetIndex = TypeLayout->getBindingRangeDescriptorSetIndex(BindingRangeIndex);
				const SlangInt DescriptorRangeIndex = TypeLayout->getBindingRangeFirstDescriptorRangeIndex(BindingRangeIndex);
				if (DescriptorSetIndex < 0 || DescriptorRangeIndex < 0)
				{
					OutErrorMessage = std::format("Invalid descriptor set information for shader binding '{}'", BindingName);
					return false;
				}

				SpaceIndex = DescriptorSetIndex;
				RegisterIndex = TypeLayout->getDescriptorSetDescriptorRangeIndexOffset(DescriptorSetIndex, DescriptorRangeIndex);
				Category = TypeLayout->getDescriptorSetDescriptorRangeCategory(DescriptorSetIndex, DescriptorRangeIndex);
				break;
			}
			case slang::BindingType::VaryingInput:
			case slang::BindingType::VaryingOutput:
			case slang::BindingType::PushConstant:
				bOutUsed = false;
				return true;
			default:
				OutErrorMessage = std::format(
					"Unsupported Slang binding type {} for shader binding '{}'",
					static_cast<uint32>(BindingType),
					BindingName
				);
				return false;
			}

			if (RegisterIndex < 0)
			{
				OutErrorMessage = std::format("Invalid register index for shader binding '{}'", BindingName);
				return false;
			}

			bool bUsed = false;
			if (SLANG_FAILED(Metadata->isParameterLocationUsed(
				static_cast<SlangParameterCategory>(Category),
				static_cast<SlangUInt>(SpaceIndex),
				static_cast<SlangUInt>(RegisterIndex),
				bUsed)))
			{
				// Some Slang paths do not expose per-entry-point usage metadata for every
				// binding category. Fall back to keeping the binding instead of failing
				// shader compilation and blocking cache generation.
				bOutUsed = true;
				return true;
			}

			bOutUsed = bUsed;
			return true;
		}

		auto ExtractBindingsFromTypeLayout(
			slang::TypeLayoutReflection* TypeLayout,
			EShaderStageFlags StageFlags,
			const std::unordered_map<std::string, FPushConstantRange>& PushConstantRanges,
			slang::IMetadata* Metadata,
			FShaderReflectionData& OutReflection,
			std::string& OutErrorMessage
		) -> bool
		{
			if (!TypeLayout)
			{
				return true;
			}

			for (SlangInt BindingRangeIndex = 0; BindingRangeIndex < TypeLayout->getBindingRangeCount(); ++BindingRangeIndex)
			{
				const slang::BindingType BindingType = TypeLayout->getBindingRangeType(BindingRangeIndex);
				slang::VariableReflection* LeafVariable = TypeLayout->getBindingRangeLeafVariable(BindingRangeIndex);
				const std::string BindingName = (LeafVariable && LeafVariable->getName()) ? LeafVariable->getName() : "";
				bool bBindingUsed = true;
				if (!IsBindingRangeUsed(TypeLayout, BindingRangeIndex, Metadata, BindingName, OutErrorMessage, bBindingUsed))
				{
					return false;
				}
				if (!bBindingUsed)
				{
					continue;
				}

				switch (BindingType)
				{
				case slang::BindingType::ConstantBuffer:
				case slang::BindingType::Texture:
				case slang::BindingType::MutableTexture:
				case slang::BindingType::TypedBuffer:
				case slang::BindingType::RawBuffer:
				case slang::BindingType::MutableTypedBuffer:
				case slang::BindingType::MutableRawBuffer:
				case slang::BindingType::Sampler:
				{
					const SlangInt DescriptorSetIndex = TypeLayout->getBindingRangeDescriptorSetIndex(BindingRangeIndex);
					const SlangInt DescriptorRangeIndex = TypeLayout->getBindingRangeFirstDescriptorRangeIndex(BindingRangeIndex);
					if (DescriptorSetIndex < 0 || DescriptorRangeIndex < 0)
					{
						OutErrorMessage = std::format("Invalid descriptor set information for shader binding '{}'", BindingName);
						return false;
					}

					FShaderResourceBinding Binding;
					Binding.Name = BindingName;
					Binding.StageFlags = StageFlags;
					Binding.SetIndex = static_cast<uint32>(DescriptorSetIndex);
					Binding.BindingIndex = static_cast<uint32>(TypeLayout->getDescriptorSetDescriptorRangeIndexOffset(DescriptorSetIndex, DescriptorRangeIndex));
					Binding.ArraySize = static_cast<uint32>(TypeLayout->getBindingRangeBindingCount(BindingRangeIndex));

					switch (BindingType)
					{
					case slang::BindingType::ConstantBuffer:
						Binding.Type = ERHIBindingType::UniformBuffer;
						break;
					case slang::BindingType::Texture:
						Binding.Type = ERHIBindingType::Texture;
						break;
					case slang::BindingType::MutableTexture:
						Binding.Type = ERHIBindingType::StorageImage;
						break;
					case slang::BindingType::TypedBuffer:
					case slang::BindingType::RawBuffer:
					case slang::BindingType::MutableTypedBuffer:
					case slang::BindingType::MutableRawBuffer:
						Binding.Type = ERHIBindingType::StorageBuffer;
						break;
					case slang::BindingType::Sampler:
						Binding.Type = ERHIBindingType::Sampler;
						break;
					default:
						break;
					}

					AppendResourceBinding(OutReflection, std::move(Binding));
					break;
				}
				case slang::BindingType::PushConstant:
				{
					slang::TypeLayoutReflection* LeafTypeLayout = TypeLayout->getBindingRangeLeafTypeLayout(BindingRangeIndex);
					const uint32 Size = ResolvePushConstantByteSize(LeafTypeLayout);
					if (Size == 0)
					{
						OutErrorMessage = std::format("Unsupported push constant size for shader binding '{}'", BindingName);
						return false;
					}

					FPushConstantRange Range{
						StageFlags,
						0,
						Size
					};
					if (const auto FoundIt = PushConstantRanges.find(BindingName); FoundIt != PushConstantRanges.end())
					{
						Range.Offset = FoundIt->second.Offset;
						Range.Size = FoundIt->second.Size;
					}
					AppendPushConstantRange(OutReflection, Range);
					break;
				}
				case slang::BindingType::VaryingInput:
				case slang::BindingType::VaryingOutput:
					break;
				default:
					OutErrorMessage = std::format(
						"Unsupported Slang binding type {} for shader binding '{}'",
						static_cast<uint32>(BindingType),
						BindingName
					);
					return false;
				}
			}

			return true;
		}

		auto BuildReflectionData(
			slang::ProgramLayout* ProgramLayout,
			slang::IMetadata* Metadata,
			std::span<const uint32> SpirvWords,
			uint32 EntryPointIndex,
			EShaderFrequency Frequency,
			FShaderReflectionData& OutReflection,
			std::string& OutErrorMessage
		) -> bool
		{
			OutReflection = {};
			OutErrorMessage.clear();

			slang::EntryPointReflection* EntryPointReflection = ProgramLayout ? ProgramLayout->getEntryPointByIndex(EntryPointIndex) : nullptr;
			if (!ProgramLayout || !EntryPointReflection)
			{
				OutErrorMessage = "Failed to access Slang program layout reflection";
				return false;
			}

			const EShaderStageFlags StageFlags = ToStageFlags(Frequency);
			std::unordered_map<std::string, FPushConstantRange> PushConstantRanges;
			CollectPushConstantOffsets(ProgramLayout->getGlobalParamsVarLayout(), StageFlags, PushConstantRanges);
			CollectPushConstantOffsets(EntryPointReflection->getVarLayout(), StageFlags, PushConstantRanges);

			if (!ExtractBindingsFromTypeLayout(ProgramLayout->getGlobalParamsTypeLayout(), StageFlags, PushConstantRanges, Metadata, OutReflection, OutErrorMessage))
			{
				return false;
			}
			if (!ExtractBindingsFromTypeLayout(EntryPointReflection->getTypeLayout(), StageFlags, PushConstantRanges, Metadata, OutReflection, OutErrorMessage))
			{
				return false;
			}

			AppendUsedPushConstantRanges(PushConstantRanges, SpirvWords, OutReflection);

			return true;
		}
	}

	FSlangShaderCompiler::FSlangShaderCompiler()
	{
		InitGlobalSession();
	}

	FSlangShaderCompiler::~FSlangShaderCompiler()
	{
		GlobalSession.setNull();
	}

	auto FSlangShaderCompiler::CreateSession(const FShaderCompileOptions& Options, Slang::ComPtr<slang::ISession>& OutSession, std::string& OutErrorMessage) const -> bool
	{
		std::vector<FShaderMacroDefinition> NormalizedMacros;
		if (!ShaderCompileUtilities::NormalizeMacros(Options, NormalizedMacros, OutErrorMessage))
		{
			return false;
		}

		std::vector<slang::PreprocessorMacroDesc> SlangMacros;
		SlangMacros.reserve(NormalizedMacros.size());
		for (const FShaderMacroDefinition& Macro : NormalizedMacros)
		{
			slang::PreprocessorMacroDesc MacroDesc = {};
			MacroDesc.name = Macro.Name.c_str();
			MacroDesc.value = Macro.Value ? Macro.Value->c_str() : nullptr;
			SlangMacros.push_back(MacroDesc);
		}

		slang::TargetDesc TargetDesc = {};
		TargetDesc.format = SLANG_SPIRV;
		TargetDesc.profile = GlobalSession->findProfile(GSlangTargetProfile.data());

		slang::SessionDesc SessionDesc = {};
		SessionDesc.targets = &TargetDesc;
		SessionDesc.targetCount = 1;
		SessionDesc.preprocessorMacros = SlangMacros.empty() ? nullptr : SlangMacros.data();
		SessionDesc.preprocessorMacroCount = static_cast<SlangInt>(SlangMacros.size());

		if (SLANG_FAILED(GlobalSession->createSession(SessionDesc, OutSession.writeRef())))
		{
			OutErrorMessage = "createSession failed";
			return false;
		}

		return true;
	}

	auto FSlangShaderCompiler::CompileInternal(
		slang::ISession* InSession,
		const char8* InShaderFilePath,
		const std::span<const char8* const>& InEntryPoints,
		Slang::ComPtr<slang::IComponentType>& OutComposedProgram,
		Slang::ComPtr<slang::IBlob>& OutDiagnostics
	) const -> Slang::Result
	{
		slang::IModule* Module = InSession->loadModule(InShaderFilePath, OutDiagnostics.writeRef());
		if (!Module)
		{
			return SLANG_FAIL;
		}

		std::vector<slang::IComponentType*> ComponentTypes;
		ComponentTypes.push_back(Module);

		std::vector<Slang::ComPtr<slang::IEntryPoint>> EntryPointObjects;
		for (const char8* Name : InEntryPoints)
		{
			Slang::ComPtr<slang::IEntryPoint> EntryPoint;
			SLANG_RETURN_ON_FAIL(Module->findEntryPointByName(Name, EntryPoint.writeRef()));
			EntryPointObjects.push_back(EntryPoint);
			ComponentTypes.push_back(EntryPoint.get());
		}

		SLANG_RETURN_ON_FAIL(InSession->createCompositeComponentType(
			ComponentTypes.data(),
			ComponentTypes.size(),
			OutComposedProgram.writeRef(),
			OutDiagnostics.writeRef()
		));

		return SLANG_OK;
	}

	static auto ConvertBlobToArray(const Slang::ComPtr<slang::IBlob>& FromBlob, std::vector<std::byte>& OutCode) -> bool
	{
		const void* BufferPtr = FromBlob->getBufferPointer();
		const size_t BufferSize = FromBlob->getBufferSize();

		if (BufferSize == 0 || BufferSize % sizeof(uint32) != 0)
		{
			DURIN_ERROR("Invalid SPIR-V size: {} bytes", BufferSize);
			return false;
		}

		OutCode.clear();
		OutCode.resize(BufferSize);
		std::memcpy(OutCode.data(), BufferPtr, BufferSize);
		return true;
	}

	static auto FillCompiledShaderOutput(
		Slang::ComPtr<slang::IComponentType>& ComposedProgram,
		std::string_view VirtualShaderPath,
		const char8* EntryPointName,
		EShaderFrequency Frequency,
		FCompiledShader& OutCompiledShader,
		std::string& OutErrorMessage
	) -> bool
	{
		Slang::ComPtr<slang::IBlob> LayoutDiagnostics;
		slang::ProgramLayout* ProgramLayout = ComposedProgram->getLayout(0, LayoutDiagnostics.writeRef());
		if (!ProgramLayout)
		{
			if (LayoutDiagnostics)
			{
				OutErrorMessage = std::string(static_cast<const char*>(LayoutDiagnostics->getBufferPointer()));
			}
			else
			{
				OutErrorMessage = "Failed to acquire Slang program layout";
			}
			return false;
		}

		Slang::ComPtr<slang::IBlob> CodeBlob;
		if (SLANG_FAILED(ComposedProgram->getEntryPointCode(0, 0, CodeBlob.writeRef(), nullptr)))
		{
			OutErrorMessage = "Failed to generate SPIR-V for entry point";
			return false;
		}

		Slang::ComPtr<slang::IMetadata> Metadata;
		ComposedProgram->getEntryPointMetadata(0, 0, Metadata.writeRef(), nullptr);

		OutCompiledShader = {};
		OutCompiledShader.Frequency = Frequency;
		OutCompiledShader.SourceEntryPoint = EntryPointName ? std::string(EntryPointName) : std::string();
		// Slang's single-entry-point SPIR-V path currently emits `main` as the binary entry point.
		OutCompiledShader.BinaryEntryPoint = "main";
		OutCompiledShader.DebugName = VirtualShaderPath.empty()
			? OutCompiledShader.SourceEntryPoint
			: std::format("{}::{}", VirtualShaderPath, OutCompiledShader.SourceEntryPoint);
		OutCompiledShader.Code = std::make_shared<std::vector<std::byte>>();
		if (!ConvertBlobToArray(CodeBlob, *OutCompiledShader.Code))
		{
			OutErrorMessage = "Failed to convert Slang SPIR-V output";
			return false;
		}
		OutCompiledShader.Hash = FXxHash128::HashBuffer(*OutCompiledShader.Code);

		std::vector<uint32> SpirvWords(OutCompiledShader.Code->size() / sizeof(uint32));
		std::memcpy(SpirvWords.data(), OutCompiledShader.Code->data(), OutCompiledShader.Code->size());

		std::string ReflectionErrorMessage;
		if (!BuildReflectionData(ProgramLayout, Metadata.get(), SpirvWords, 0, OutCompiledShader.Frequency, OutCompiledShader.Reflection, ReflectionErrorMessage))
		{
			OutErrorMessage = std::format("Failed to reflect shader '{}': {}", OutCompiledShader.SourceEntryPoint, ReflectionErrorMessage);
			return false;
		}

		return true;
	}

	FShaderCompilerOutput FSlangShaderCompiler::Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options)
	{
		FShaderCompilerOutput Output;

		const auto& EntryPoints = Options.EntryPoints;
		const uint32 EntryPointCount = static_cast<uint32>(EntryPoints.size());
		if (EntryPointCount == 0)
		{
			Output.ErrorMessage = "No entry points found";
			return Output;
		}

		if (EntryPointCount != Options.Frequencies.size())
		{
			Output.ErrorMessage = "Entry point count does not match shader frequency count";
			return Output;
		}

		const std::string SourceFilePath(ShaderSourceFilePath);

		Slang::ComPtr<slang::ISession> CompileSession;
		if (!CreateSession(Options, CompileSession, Output.ErrorMessage))
		{
			return Output;
		}

		Output.CompiledShaders.resize(EntryPointCount);
		for (uint32 Index = 0; Index < EntryPointCount; ++Index)
		{
			const std::array<const char8*, 1> SingleEntryPoint = {EntryPoints[Index]};

			Slang::ComPtr<slang::IBlob> DiagnosticsBlob;
			Slang::ComPtr<slang::IComponentType> ComposedProgram;
			const Slang::Result CompileResult = CompileInternal(CompileSession.get(), SourceFilePath.data(), SingleEntryPoint, ComposedProgram, DiagnosticsBlob);
			if (SLANG_FAILED(CompileResult) && !ComposedProgram)
			{
				if (DiagnosticsBlob != nullptr)
				{
					Output.ErrorMessage = std::string{"Failed to compile shader. Diagnostics: \n"} + static_cast<const char*>(DiagnosticsBlob->getBufferPointer());
				}
				else
				{
					const std::string EntryPointName = EntryPoints[Index] ? std::string(EntryPoints[Index]) : std::string("<null>");
					Output.ErrorMessage = std::format("Failed to compile shader entry point '{}'", EntryPointName);
				}
				return Output;
			}
			if (SLANG_FAILED(CompileResult) && DiagnosticsBlob != nullptr)
			{
				const std::string EntryPointName = EntryPoints[Index] ? std::string(EntryPoints[Index]) : std::string("<null>");
				DURIN_WARN("Slang reported non-fatal diagnostics for shader entry point '{}': {}", EntryPointName, static_cast<const char*>(DiagnosticsBlob->getBufferPointer()));
			}

			if (!FillCompiledShaderOutput(
				ComposedProgram,
				Options.VirtualShaderPath,
				EntryPoints[Index],
				Options.Frequencies[Index],
				Output.CompiledShaders[Index],
				Output.ErrorMessage))
			{
				return Output;
			}
		}

		Output.bSucceeded = true;
		return Output;
	}

	auto FSlangShaderCompiler::GetEnvironmentIdentity() const -> std::string
	{
		const char* BuildTag = GlobalSession ? GlobalSession->getBuildTagString() : nullptr;
		return std::format("slang:{};target=spirv;profile={}", BuildTag ? BuildTag : "unknown", GSlangTargetProfile);
	}

	auto FSlangShaderCompiler::InitGlobalSession() -> void
	{
		if (SLANG_FAILED(slang_createGlobalSession(SLANG_API_VERSION, GlobalSession.writeRef())))
		{
			throw std::runtime_error("slang_createGlobalSession failed");
		}
	}
} // namespace Durin
