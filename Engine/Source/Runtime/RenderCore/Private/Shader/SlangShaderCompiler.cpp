#include "SlangShaderCompiler.h"

namespace Durin
{
	FSlangShaderCompiler::FSlangShaderCompiler()
	{
		InitGlobalSession();

		slang::TargetDesc TargetDesc = {};
		TargetDesc.format = SLANG_SPIRV;
		TargetDesc.profile = GlobalSession->findProfile("spirv_1_5");

		slang::SessionDesc SessionDesc = {};
		SessionDesc.targets = &TargetDesc;
		SessionDesc.targetCount = 1;

		if (SLANG_FAILED(GlobalSession->createSession(SessionDesc, Session.writeRef())))
		{
			throw std::runtime_error("createSession failed");
		}
	}

	FSlangShaderCompiler::~FSlangShaderCompiler()
	{
	}

	static auto ConvertBlobToArray(const Slang::ComPtr<slang::IBlob>& FromBlob, FShaderCode& OutCode) -> bool
	{
		// Get the raw pointer and size in bytes
		const void* BufferPtr = FromBlob->getBufferPointer();
		const size_t BufferSize = FromBlob->getBufferSize();

		if (BufferSize == 0 || BufferSize % sizeof(uint32) != 0)
		{
			DURIN_ERROR("Invalid SPIR-V size: {} bytes", BufferSize);
			return false;
		}

		// Minimize reallocations: clear and resize
		OutCode.clear();
		OutCode.resize(BufferSize);

		// Since SPIR-V is already a binary format, this is a direct bit-copy.
		std::memcpy(OutCode.data(), BufferPtr, BufferSize);

		return true;
	}


	FShaderCompilerOutput FSlangShaderCompiler::Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options)
	{
		FShaderCompilerOutput Output;

		const auto& EntryPoints = Options.EntryPoints;
		const uint32 EntryPointCount = EntryPoints.size();
		if (EntryPointCount == 0)
		{
			Output.ErrorMessage = "No entry points found";
			return Output;
		}

		Slang::ComPtr<slang::IBlob> DiagnosticsBlob;
		std::vector<Slang::ComPtr<slang::IBlob>> CompiledCodeBlobs;
		const Slang::Result CompileResult = CompileInternal(ShaderSourceFilePath.data(), EntryPoints, CompiledCodeBlobs, DiagnosticsBlob);

		// If compilation failed, fill the error message and return
		// The user might want to handle the error message in different ways, for example, showing it in the UI, or writing it to a log file, etc.
		// So we don't log error message here, just fill it in the output and let the user decide how to handle it.
		if (SLANG_FAILED(CompileResult))
		{
			if (DiagnosticsBlob != nullptr)
			{
				Output.ErrorMessage = static_cast<const char*>(DiagnosticsBlob->getBufferPointer());
			}
			return Output;
		}

		// If compilation succeeded, convert the compiled code blobs to arrays and fill the output
		check(CompiledCodeBlobs.size() == EntryPointCount);
		auto& OutCodes = Output.Codes;
		Output.bSucceeded = true;
		OutCodes.resize(EntryPointCount);
		for (size_t i = 0; i < EntryPointCount; ++i)
		{
			if (!ConvertBlobToArray(CompiledCodeBlobs[i], OutCodes[i]))
			{
				Output.bSucceeded = false;
				Output.ErrorMessage = "Failed to convert compiled code blob to array for shader: " + std::string(ShaderSourceFilePath) + ", entry point: " + std::string(EntryPoints[i]);
				OutCodes.clear();
				return Output;
			}
		}

		return Output;
	}

	auto FSlangShaderCompiler::CompileInternal(
		const char8* InShaderFilePath,
		const std::span<const char8* const>& InEntryPoints,
		std::vector<Slang::ComPtr<slang::IBlob>>& OutCodes,
		Slang::ComPtr<slang::IBlob>& OutDiagnostics
	) const -> Slang::Result
	{
		slang::IModule* Module = Session->loadModule(InShaderFilePath, OutDiagnostics.writeRef());
		if (!Module) return SLANG_FAIL;

		std::vector<slang::IComponentType*> ComponentTypes;
		ComponentTypes.push_back(Module);

		// Find entry point objects for the given entry point names
		std::vector<Slang::ComPtr<slang::IEntryPoint>> EntryPointObjects;
		for (const char8* Name : InEntryPoints)
		{
			Slang::ComPtr<slang::IEntryPoint> EntryPoint;
			SLANG_RETURN_ON_FAIL(Module->findEntryPointByName(Name, EntryPoint.writeRef()));
			EntryPointObjects.push_back(EntryPoint);
			ComponentTypes.push_back(EntryPoint.get());
		}

		// Compose the program from the module and entry points
		Slang::ComPtr<slang::IComponentType> ComposedProgram;
		SLANG_RETURN_ON_FAIL(Session->createCompositeComponentType(
			ComponentTypes.data(),
			ComponentTypes.size(),
			ComposedProgram.writeRef(),
			OutDiagnostics.writeRef()
		));

		// Get code for each entry point
		OutCodes.resize(InEntryPoints.size());
		for (size_t i = 0; i < InEntryPoints.size(); ++i)
		{
			SLANG_RETURN_ON_FAIL(ComposedProgram->getEntryPointCode(
				i,
				0,
				OutCodes[i].writeRef(),
				OutDiagnostics.writeRef()
			));
		}

		return SLANG_OK;
	}

	auto FSlangShaderCompiler::InitGlobalSession() -> void
	{
		if (SLANG_FAILED(slang_createGlobalSession(SLANG_API_VERSION, GlobalSession.writeRef())))
		{
			throw std::runtime_error("slang_createGlobalSession failed");
		}
	}

} // namespace Durin