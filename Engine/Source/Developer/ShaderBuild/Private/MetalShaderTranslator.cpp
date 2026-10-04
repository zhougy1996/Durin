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

		auto RemapBindings(FCompiledShader& Shader, spvc_compiler Compiler,
			SpvExecutionModel Stage) -> FShaderOperationResult
		{
			auto Map = BuildMetalShaderBindingMap(Shader.Frequency, Shader.Reflection);
			if (!Map) return std::unexpected(std::move(Map.error()));
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
					return std::unexpected(FShaderError{.Code = EShaderError::MetalBindingRemapInvalid});
				}
				if (spvc_compiler_msl_add_resource_binding_2(Compiler, &NativeBinding) != SPVC_SUCCESS)
					return std::unexpected(FShaderError{.Code = EShaderError::MslConversionFailed});
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
					return std::unexpected(FShaderError{.Code = EShaderError::MslConversionFailed});
			}
			Shader.MetalBindings = std::move(Map->Bindings);
			Shader.MetalPushConstantBufferSlot = Map->PushConstantBufferSlot;
			Shader.BindingRemapIdentity = Map->Identity;
			return {};
		}

	}
	#endif

	auto TranslateMetalShader(FCompiledShader& Shader) -> FShaderOperationResult
	{
	#if defined(__APPLE__)
		if (!Shader.Code || Shader.CodeFormat != EShaderCodeFormat::Spirv15
			|| Shader.Code->size() < 20 || Shader.Code->size() % sizeof(uint32) != 0)
			return std::unexpected(FShaderError{.Code = EShaderError::SpirvConversionFailed});
		const auto Stage = ExecutionModel(Shader.Frequency);
		if (!Stage) return std::unexpected(FShaderError{.Code = EShaderError::MslConversionFailed});
		std::vector<uint32> Words(Shader.Code->size() / sizeof(uint32));
		std::memcpy(Words.data(), Shader.Code->data(), Shader.Code->size());
		FSpirvCrossContext Context;
		if (spvc_context_create(&Context.Handle) != SPVC_SUCCESS)
			return std::unexpected(FShaderError{.Code = EShaderError::MslConversionFailed});
		spvc_parsed_ir IR = nullptr;
		spvc_compiler Compiler = nullptr;
		spvc_compiler_options Options = nullptr;
		if (spvc_context_parse_spirv(Context.Handle, Words.data(), Words.size(), &IR) != SPVC_SUCCESS
			|| spvc_context_create_compiler(Context.Handle, SPVC_BACKEND_MSL, IR,
				SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &Compiler) != SPVC_SUCCESS
			|| spvc_compiler_create_compiler_options(Compiler, &Options) != SPVC_SUCCESS
			|| spvc_compiler_options_set_uint(Options, SPVC_COMPILER_OPTION_MSL_VERSION,
				SPVC_MAKE_MSL_VERSION(2, 0, 0)) != SPVC_SUCCESS
			|| spvc_compiler_install_compiler_options(Compiler, Options) != SPVC_SUCCESS)
			return std::unexpected(FShaderError{.Code = EShaderError::MslConversionFailed});
		if (auto Result = RemapBindings(Shader, Compiler, *Stage); !Result) return Result;
		const char* Source = nullptr;
		if (spvc_compiler_compile(Compiler, &Source) != SPVC_SUCCESS || !Source || !*Source)
			return std::unexpected(FShaderError{.Code = EShaderError::MslConversionFailed,
				.ActualIdentity = spvc_context_get_last_error_string(Context.Handle)});
		const char* EntryPoint = spvc_compiler_get_cleansed_entry_point_name(
			Compiler, Shader.BinaryEntryPoint.c_str(), *Stage);
		if (!EntryPoint || !*EntryPoint)
			return std::unexpected(FShaderError{.Code = EShaderError::MslConversionFailed});
		const auto SourceBytes = std::as_bytes(std::span(Source, std::strlen(Source)));
		Shader.Code = std::make_shared<const FSharedByteBuffer>(FSharedByteBuffer::Copy(SourceBytes));
		Shader.Hash = FXxHash128::HashBuffer(*Shader.Code);
		Shader.BinaryEntryPoint = EntryPoint;
		Shader.Target = MetalShaderTarget;
		Shader.CodeFormat = EShaderCodeFormat::Msl20Source;
		return {};
	#else
		return std::unexpected(FShaderError{.Code = EShaderError::MslConversionFailed});
	#endif
	}
}
