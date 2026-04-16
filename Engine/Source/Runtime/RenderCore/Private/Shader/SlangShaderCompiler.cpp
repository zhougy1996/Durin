#include "SlangShaderCompiler.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Doge
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

	auto FSlangShaderCompiler::CompileShader(const char8* InShaderFilename, const char8* InEntryPoint) -> bool
	{
		Slang::ComPtr<slang::IBlob> CompiledCode;
		Slang::Result CompileResult = CompileShaderInternal(InShaderFilename, InEntryPoint, CompiledCode);
		if (SLANG_FAILED(CompileResult))
		{
			DOGE_ERROR("Failed to compile shader: {}, entry point: {}", InShaderFilename, InEntryPoint);
			DOGE_ERROR("Slang error code: {}", CompileResult);
			return false;
		}

		const std::filesystem::path ShaderFilePath(InShaderFilename);
		const std::string ShaderCacheDir = FPaths::EngineDir() + "ShaderCache/SPIR-V/";
		std::string CompiledSpvFilePath = ShaderCacheDir + ShaderFilePath.stem().generic_string() + "_" + InEntryPoint + ".spv";

		FFileHelper::SaveArrayToFile(std::span{static_cast<const std::byte*>(CompiledCode->getBufferPointer()), CompiledCode->getBufferSize()}, CompiledSpvFilePath);
		return true;
	}

	auto FSlangShaderCompiler::Compile(const char8* InShaderFilename, const char8* InEntryPoint, std::vector<uint32>& OutCode) -> bool
	{
		Slang::ComPtr<slang::IBlob> CompiledCode;
		Slang::Result CompileResult = CompileShaderInternal(InShaderFilename, InEntryPoint, CompiledCode);

		if (SLANG_FAILED(CompileResult))
		{
			DOGE_ERROR("Failed to compile shader: {}, entry point: {}", InShaderFilename, InEntryPoint);
			return false;
		}

		// Get the raw pointer and size in bytes
		const void* BufferPtr = CompiledCode->getBufferPointer();
		const size_t BufferSize = CompiledCode->getBufferSize();

		if (BufferSize == 0 || BufferSize % sizeof(uint32) != 0)
		{
			DOGE_ERROR("Invalid SPIR-V size: {} bytes", BufferSize);
			return false;
		}

		check(BufferSize % sizeof(uint32) == 0); // SPIR-V should be a sequence of 32-bit words
		// Calculate number of uint32 elements
		const size_t ElementCount = BufferSize / sizeof(uint32);

		// Minimize reallocations: clear and resize
		OutCode.clear();
		OutCode.resize(ElementCount);

		// Since SPIR-V is already a binary format, this is a direct bit-copy.
		std::memcpy(OutCode.data(), BufferPtr, BufferSize);

		return true;
	}

	static auto DiagnoseIfNeeded(const Slang::ComPtr<slang::IBlob>& DiagnosticsBlob, std::source_location SourceLocation = std::source_location::current())
	{
		if (DiagnosticsBlob != nullptr)
		{
			std::string LocationString = std::format("{}:{}:{}", SourceLocation.file_name(), SourceLocation.line(), SourceLocation.column());
			DOGE_WARN("Slang diagnostics in {} : \n{}", LocationString, static_cast<const char*>(DiagnosticsBlob->getBufferPointer()));
		}
	}

	auto FSlangShaderCompiler::CompileShaderInternal(const char8* InShaderFilePath, const char8* InEntryPoint, Slang::ComPtr<slang::IBlob>& OutCode) const -> Slang::Result
	{
		Slang::ComPtr<slang::IBlob> DiagnosticsBlob;
		slang::IModule* Module = Session->loadModule(InShaderFilePath, DiagnosticsBlob.writeRef());
		DiagnoseIfNeeded(DiagnosticsBlob);

		if (Module == nullptr)
		{
			return SLANG_FAIL;
		}

		Slang::ComPtr<slang::IEntryPoint> EntryPoint;
		SLANG_RETURN_ON_FAIL(Module->findEntryPointByName(InEntryPoint, EntryPoint.writeRef()));

		std::vector<slang::IComponentType*> ComponentTypes;
		ComponentTypes.push_back(Module);
		ComponentTypes.push_back(EntryPoint);

		Slang::ComPtr<slang::IComponentType> ComposedProgram;
		SlangResult Result = Session->createCompositeComponentType(
			ComponentTypes.data(),
			ComponentTypes.size(),
			ComposedProgram.writeRef(),
			DiagnosticsBlob.writeRef());
		DiagnoseIfNeeded(DiagnosticsBlob);
		SLANG_RETURN_ON_FAIL(Result);

		SLANG_RETURN_ON_FAIL(ComposedProgram->getEntryPointCode(0, 0, OutCode.writeRef(), DiagnosticsBlob.writeRef()));
		DiagnoseIfNeeded(DiagnosticsBlob);

		if (!OutCode || OutCode->getBufferSize() == 0)
		{
			return SLANG_FAIL;
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

} // namespace Doge