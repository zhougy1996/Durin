#include "SlangShaderCompiler.h"

namespace Doge
{
	static auto DiagnoseIfNeeded(const Slang::ComPtr<slang::IBlob>& DiagnosticsBlob, std::source_location SourceLocation = std::source_location::current())
	{
		if (DiagnosticsBlob != nullptr)
		{
			std::string LocationString = std::format("{}:{}:{}", SourceLocation.file_name(), SourceLocation.line(), SourceLocation.column());
			DOGE_WARN("Slang diagnostics in {} : \n{}", LocationString, static_cast<const char*>(DiagnosticsBlob->getBufferPointer()));
		}
	}

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

	auto FSlangShaderCompiler::Compile(const char8* InShaderFilename, const char8* InEntryPoint, std::vector<uint32>& OutCode) -> bool
	{
		std::vector<std::vector<uint32>> Codes;
		if (Compile(InShaderFilename, std::span(&InEntryPoint, 1), Codes))
		{
			OutCode = std::move(Codes[0]);
			return true;
		}
		return false;
	}

	static auto ConvertBlobToArray(const Slang::ComPtr<slang::IBlob>& FromBlob, std::vector<uint32>& OutCode) -> bool
	{
		// Get the raw pointer and size in bytes
		const void* BufferPtr = FromBlob->getBufferPointer();
		const size_t BufferSize = FromBlob->getBufferSize();

		if (BufferSize == 0 || BufferSize % sizeof(uint32) != 0)
		{
			DOGE_ERROR("Invalid SPIR-V size: {} bytes", BufferSize);
			return false;
		}

		// Calculate number of uint32 elements
		const size_t ElementCount = BufferSize / sizeof(uint32);

		// Minimize reallocations: clear and resize
		OutCode.clear();
		OutCode.resize(ElementCount);

		// Since SPIR-V is already a binary format, this is a direct bit-copy.
		std::memcpy(OutCode.data(), BufferPtr, BufferSize);

		return true;
	}

	auto FSlangShaderCompiler::Compile(const char8* InShaderFilename, const std::span<const char8*>& InEntryPoints, std::vector<std::vector<uint32>>& OutCodes) -> bool
	{
		const size_t EntryPointCount = InEntryPoints.size();
		if (EntryPointCount == 0)
		{
			DOGE_WARN("No entry point specified");
			return false;
		}

		Slang::ComPtr<slang::IBlob> DiagnosticsBlob;
		std::vector<Slang::ComPtr<slang::IBlob>> CompiledCodeBlobs;
		Slang::Result CompileResult = CompileInternal(InShaderFilename, InEntryPoints, CompiledCodeBlobs, DiagnosticsBlob);
		DiagnoseIfNeeded(DiagnosticsBlob);
		if (SLANG_FAILED(CompileResult))
		{
			std::stringstream EntryPointsStream;
			for (size_t i = 0; i < InEntryPoints.size(); ++i)
			{
				if (i > 0) EntryPointsStream << ", ";
				EntryPointsStream << InEntryPoints[i];
			}
			DOGE_ERROR("Failed to compile shader: {}, entry points: {}", InShaderFilename, EntryPointsStream.str());
			return false;
		}

		OutCodes.resize(EntryPointCount);
		for (size_t i = 0; i < EntryPointCount; ++i)
		{
			if (!ConvertBlobToArray(CompiledCodeBlobs[i], OutCodes[i]))
			{
				DOGE_ERROR("Failed to convert compiled code blob to array for shader: {}, entry point: {}", InShaderFilename, InEntryPoints[i]);
				OutCodes.clear();
				return false;
			}
		}
		return true;
	}

	auto FSlangShaderCompiler::CompileInternal(
		const char8* InShaderFilePath,
		const std::span<const char8*>& InEntryPoints,
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

} // namespace Doge