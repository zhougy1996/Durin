#include "SlangShaderCompiler.h"

#include "ShaderCompileUtilities.h"

#include "Hash/XxHash.h"

#include <array>
#include <unordered_map>

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
			case EShaderFrequency::Pixel:
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

		auto ExtractBindingsFromTypeLayout(
			slang::TypeLayoutReflection* TypeLayout,
			EShaderStageFlags StageFlags,
			const std::unordered_map<std::string, FPushConstantRange>& PushConstantRanges,
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

				switch (BindingType)
				{
				case slang::BindingType::ConstantBuffer:
				case slang::BindingType::Texture:
				case slang::BindingType::MutableTexture:
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
					case slang::BindingType::MutableTexture:
						Binding.Type = ERHIBindingType::Texture;
						break;
					case slang::BindingType::Sampler:
						Binding.Type = ERHIBindingType::Sampler;
						break;
					default:
						break;
					}

					OutReflection.ResourceBindings.push_back(std::move(Binding));
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
					OutReflection.PushConstantRanges.push_back(Range);
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

			if (!ExtractBindingsFromTypeLayout(ProgramLayout->getGlobalParamsTypeLayout(), StageFlags, PushConstantRanges, OutReflection, OutErrorMessage))
			{
				return false;
			}
			if (!ExtractBindingsFromTypeLayout(EntryPointReflection->getTypeLayout(), StageFlags, PushConstantRanges, OutReflection, OutErrorMessage))
			{
				return false;
			}

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
			MacroDesc.value = Macro.bHasExplicitValue ? Macro.Value.c_str() : "1";
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

	static auto ConvertBlobToArray(const Slang::ComPtr<slang::IBlob>& FromBlob, FShaderCode& OutCode) -> bool
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

		OutCompiledShader = {};
		OutCompiledShader.Frequency = Frequency;
		OutCompiledShader.EntryPoint = EntryPointName ? std::string(EntryPointName) : std::string();
		OutCompiledShader.DebugName = VirtualShaderPath.empty()
			? OutCompiledShader.EntryPoint
			: std::format("{}::{}", VirtualShaderPath, OutCompiledShader.EntryPoint);
		OutCompiledShader.Code = std::make_shared<FShaderCode>();
		if (!ConvertBlobToArray(CodeBlob, *OutCompiledShader.Code))
		{
			OutErrorMessage = "Failed to convert Slang SPIR-V output";
			return false;
		}
		OutCompiledShader.Hash = FXxHash128::HashBuffer(*OutCompiledShader.Code);

		std::string ReflectionErrorMessage;
		if (!BuildReflectionData(ProgramLayout, 0, OutCompiledShader.Frequency, OutCompiledShader.Reflection, ReflectionErrorMessage))
		{
			OutErrorMessage = std::format("Failed to reflect shader '{}': {}", OutCompiledShader.EntryPoint, ReflectionErrorMessage);
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

	auto FSlangShaderCompiler::InitGlobalSession() -> void
	{
		if (SLANG_FAILED(slang_createGlobalSession(SLANG_API_VERSION, GlobalSession.writeRef())))
		{
			throw std::runtime_error("slang_createGlobalSession failed");
		}
	}
} // namespace Durin
