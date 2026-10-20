#include "MetalShaderFormat.h"
#include "MetalShaderTranslator.h"

#if defined(__APPLE__)
#include "spirv_cross_c.h"
#endif

namespace Durin
{
	#if defined(__APPLE__)
	namespace
	{
		struct FSpirvCrossContext
		{
			spvc_context Handle = nullptr;
			~FSpirvCrossContext() { if (Handle) spvc_context_destroy(Handle); }
		};

		auto ExecutionModel(EShaderFrequency Frequency) -> std::optional<SpvExecutionModel>
		{
			switch (Frequency)
			{
			case EShaderFrequency::Vertex: return SpvExecutionModelVertex;
			case EShaderFrequency::Fragment: return SpvExecutionModelFragment;
			case EShaderFrequency::Compute: return SpvExecutionModelGLCompute;
			default: return std::nullopt;
			}
		}

		auto RemapBindings(FShaderCode& Shader, spvc_compiler Compiler,
			SpvExecutionModel Stage) -> FShaderOperationResult
		{
			auto Map = ShaderFormat::BuildMetalBindingMap(Shader.Frequency, ShaderFormat::BuildLogicalBindings(Shader.Reflection), !Shader.Reflection.PushConstantRanges.empty());
			if (!Map) return std::unexpected(std::string("Metal shader binding remap exceeds or conflicts with native slots"));
			for (const auto& Binding : Map->Bindings)
			{
				spvc_msl_resource_binding_2 NativeBinding;
				spvc_msl_resource_binding_init_2(&NativeBinding);
				NativeBinding.stage = Stage;
				NativeBinding.desc_set = Binding.SetIndex;
				NativeBinding.binding = Binding.BindingIndex;
				NativeBinding.count = Binding.Count;
				switch (Binding.Type)
				{
				case ERHIBindingType::UniformBuffer:
				case ERHIBindingType::UniformBufferDynamic:
				case ERHIBindingType::StorageBuffer:
					NativeBinding.msl_buffer = Binding.Slot; break;
				case ERHIBindingType::Texture:
				case ERHIBindingType::StorageImage:
					NativeBinding.msl_texture = Binding.Slot; break;
				case ERHIBindingType::Sampler:
					NativeBinding.msl_sampler = Binding.Slot; break;
				default:
					return std::unexpected(std::string("Metal shader binding remap exceeds or conflicts with native slots"));
				}
				if (spvc_compiler_msl_add_resource_binding_2(Compiler, &NativeBinding) != SPVC_SUCCESS)
					return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
			}
			if (Map->PushConstantBufferSlot != UINT32_MAX)
			{
				spvc_msl_resource_binding_2 PushBinding;
				spvc_msl_resource_binding_init_2(&PushBinding);
				PushBinding.stage = Stage;
				PushBinding.desc_set = SPVC_MSL_PUSH_CONSTANT_DESC_SET;
				PushBinding.binding = SPVC_MSL_PUSH_CONSTANT_BINDING;
				PushBinding.msl_buffer = Map->PushConstantBufferSlot;
				if (spvc_compiler_msl_add_resource_binding_2(Compiler, &PushBinding) != SPVC_SUCCESS)
					return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
			}
			const auto Metadata = ShaderFormat::EncodeMetalMetadata(Shader.Frequency,
				{std::move(Map->Bindings), Map->PushConstantBufferSlot, Map->Identity});
			if (!Metadata) return std::unexpected(std::string("Metal shader binding remap exceeds or conflicts with native slots"));
			Shader.PlatformData = *Metadata;
			return {};
		}

	}
	#endif

	auto TranslateMetalShader(FShaderCode& Shader) -> FShaderOperationResult
	{
	#if defined(__APPLE__)
		const auto Settings = ShaderFormat::ResolveMetalCompileSettings(MetalShaderTarget);
		if (!Settings) return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
		if (Shader.Code.IsEmpty() || Shader.CodeFormat != EShaderCodeFormat::Spirv15
			|| Shader.Code.size() < 20 || Shader.Code.size() % sizeof(uint32) != 0)
			return std::unexpected(std::string("Failed to convert Slang SPIR-V output"));
		const auto Stage = ExecutionModel(Shader.Frequency);
		if (!Stage) return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
		std::vector<uint32> Words(Shader.Code.size() / sizeof(uint32));
		std::memcpy(Words.data(), Shader.Code.data(), Shader.Code.size());
		FSpirvCrossContext Context;
		if (spvc_context_create(&Context.Handle) != SPVC_SUCCESS)
			return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
		spvc_parsed_ir IR = nullptr;
		spvc_compiler Compiler = nullptr;
		spvc_compiler_options Options = nullptr;
		if (spvc_context_parse_spirv(Context.Handle, Words.data(), Words.size(), &IR) != SPVC_SUCCESS
			|| spvc_context_create_compiler(Context.Handle, SPVC_BACKEND_MSL, IR,
				SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &Compiler) != SPVC_SUCCESS
			|| spvc_compiler_create_compiler_options(Compiler, &Options) != SPVC_SUCCESS
			|| spvc_compiler_options_set_uint(Options, SPVC_COMPILER_OPTION_MSL_VERSION,
				SPVC_MAKE_MSL_VERSION(Settings->MslLanguageVersion / 100,
					(Settings->MslLanguageVersion % 100) / 10, Settings->MslLanguageVersion % 10)) != SPVC_SUCCESS
			// Common RHI uses Vulkan's positive-height viewport: clip Y=-1 is top.
			// Metal maps clip Y=+1 to top, so adapt vertex output before rasterization.
			|| spvc_compiler_options_set_bool(Options, SPVC_COMPILER_OPTION_FLIP_VERTEX_Y,
				Shader.Frequency == EShaderFrequency::Vertex ? SPVC_TRUE : SPVC_FALSE) != SPVC_SUCCESS
			|| spvc_compiler_install_compiler_options(Compiler, Options) != SPVC_SUCCESS)
			return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
		if (Shader.Frequency == EShaderFrequency::Compute)
		{
			const SpvExecutionMode* Modes = nullptr;
			size_t ModeCount = 0;
			if (spvc_compiler_get_execution_modes(Compiler, &Modes, &ModeCount)
				!= SPVC_SUCCESS || !Modes)
				return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
			bool bLocalSize = false;
			for (size_t Index = 0; Index < ModeCount; ++Index)
			{
				if (Modes[Index] == SpvExecutionModeLocalSizeId)
					return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
				bLocalSize |= Modes[Index] == SpvExecutionModeLocalSize;
			}
			if (!bLocalSize)
				return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
			for (uint32 Axis = 0; Axis < 3; ++Axis)
				if (spvc_compiler_get_execution_mode_argument_by_index(
					Compiler, SpvExecutionModeLocalSize, Axis)
					!= Shader.ComputeThreadGroupSize[Axis])
					return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
		}
		if (auto Result = RemapBindings(Shader, Compiler, *Stage); !Result) return Result;
		const char* Source = nullptr;
		if (spvc_compiler_compile(Compiler, &Source) != SPVC_SUCCESS || !Source || !*Source)
			return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
		const char* EntryPoint = spvc_compiler_get_cleansed_entry_point_name(
			Compiler, Shader.BinaryEntryPoint.c_str(), *Stage);
		if (!EntryPoint || !*EntryPoint)
			return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
		const auto SourceBytes = std::as_bytes(std::span(Source, std::strlen(Source)));
		Shader.Code = FSharedByteBuffer::Copy(SourceBytes);
		Shader.CodeHash = FXxHash128::HashBuffer(Shader.Code);
		Shader.BinaryEntryPoint = EntryPoint;
		Shader.Target = MetalShaderTarget;
		Shader.CodeFormat = EShaderCodeFormat::Msl20Source;
		return {};
	#else
		return std::unexpected(std::string("Failed to translate SPIR-V to Metal shading language"));
	#endif
	}
}
