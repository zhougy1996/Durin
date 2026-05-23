#include "SlangShaderCompiler.h"

#include "Hash/XxHash.h"
#include "Misc/AppConfigCache.h"
#include "Misc/FileHelper.h"
#include "Shader/ShaderPaths.h"

namespace Durin
{
	static auto ToTicks(const std::filesystem::file_time_type& InTime) -> int64
	{
		return InTime.time_since_epoch().count();
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

	auto FSlangShaderCompiler::NormalizePath(const std::filesystem::path& InPath) const -> std::string
	{
		std::error_code ErrorCode;
		const std::filesystem::path CanonicalPath = std::filesystem::weakly_canonical(InPath, ErrorCode);
		if (!ErrorCode)
		{
			return CanonicalPath.generic_string();
		}
		return InPath.lexically_normal().generic_string();
	}

	auto FSlangShaderCompiler::GetDependencyMetaRootDir(std::string_view ShaderName) const -> std::filesystem::path
	{
		for (const auto& MountPoint : FShaderPaths::GetRegisteredMountPoints())
		{
			if (ShaderName.starts_with(MountPoint.VirtualRoot))
			{
				return std::filesystem::path(MountPoint.BinaryDir).parent_path() / "DependencyHashCache";
			}
		}
		return std::filesystem::path("ShaderCache") / "DependencyHashCache";
	}

	auto FSlangShaderCompiler::GetDependencyMetaFilePath(std::string_view ShaderName, std::string_view ShaderSourceFilePath) const -> std::filesystem::path
	{
		const std::filesystem::path MetaRootDir = GetDependencyMetaRootDir(ShaderName);
		std::filesystem::path RelativeShaderPath;
		for (const auto& MountPoint : FShaderPaths::GetRegisteredMountPoints())
		{
			if (ShaderName.starts_with(MountPoint.VirtualRoot))
			{
				RelativeShaderPath = std::filesystem::path(std::string(ShaderName.substr(MountPoint.VirtualRoot.size())));
				break;
			}
		}

		if (RelativeShaderPath.empty())
		{
			RelativeShaderPath = std::filesystem::path(ShaderSourceFilePath).filename();
		}

		if (RelativeShaderPath.is_absolute())
		{
			RelativeShaderPath = RelativeShaderPath.relative_path();
		}

		RelativeShaderPath = RelativeShaderPath.lexically_normal();
		return (MetaRootDir / RelativeShaderPath).replace_extension(".meta");
	}

	auto FSlangShaderCompiler::SaveShaderDependencyMeta(
		std::string_view ShaderName,
		std::string_view ShaderSourceFilePath,
		FXxHash64 SourceSignatureHash,
		const std::vector<std::string>& DependencyPaths
	) const -> void
	{
		const std::filesystem::path MetaPath = GetDependencyMetaFilePath(ShaderName, ShaderSourceFilePath);
		std::ostringstream MetaContent;
		MetaContent << "version=1\n";
		MetaContent << "shader=" << ShaderName << "\n";
		MetaContent << "source=" << ShaderSourceFilePath << "\n";
		MetaContent << "source_tree_hash=" << std::format("{:016x}", SourceSignatureHash.HashValue) << "\n";
		MetaContent << "dependency_count=" << DependencyPaths.size() << "\n";

		for (const std::string& DependencyPath : DependencyPaths)
		{
			MetaContent << "dep=" << DependencyPath << "\n";
		}

		const std::string MetaString = MetaContent.str();

		const std::span<const std::byte> MetaBytes(
			reinterpret_cast<const std::byte*>(MetaString.data()),
			MetaString.size()
		);

		if (!FFileHelper::SaveArrayToFile(MetaBytes, MetaPath))
		{
			DURIN_WARN("Failed to write shader dependency meta file: {}", MetaPath.string());
		}
	}

	auto FSlangShaderCompiler::ResolveDependencyFiles(const char8* InShaderFilePath, std::vector<std::string>& OutDependencyPaths, std::string& OutDiagnostics) const -> Slang::Result
	{
		Slang::ComPtr<slang::IBlob> DiagnosticsBlob;
		slang::IModule* Module = Session->loadModule(InShaderFilePath, DiagnosticsBlob.writeRef());
		if (!Module)
		{
			if (DiagnosticsBlob)
			{
				OutDiagnostics = static_cast<const char*>(DiagnosticsBlob->getBufferPointer());
			}
			return SLANG_FAIL;
		}

		OutDependencyPaths.clear();
		OutDependencyPaths.push_back(NormalizePath(std::filesystem::path(InShaderFilePath)));

		const SlangInt DependencyCount = Module->getDependencyFileCount();
		for (SlangInt DependencyIndex = 0; DependencyIndex < DependencyCount; ++DependencyIndex)
		{
			const char* DependencyPath = Module->getDependencyFilePath(DependencyIndex);
			if (DependencyPath && DependencyPath[0] != '\0')
			{
				OutDependencyPaths.push_back(NormalizePath(std::filesystem::path(DependencyPath)));
			}
		}

		std::ranges::sort(OutDependencyPaths);
		const auto UniqueEnd = std::ranges::unique(OutDependencyPaths).begin();
		OutDependencyPaths.erase(UniqueEnd, OutDependencyPaths.end());
		return SLANG_OK;
	}

	auto FSlangShaderCompiler::GetOrComputeFileHash(std::string_view InPath) -> FXxHash64
	{
		std::error_code ErrorCode;
		const std::filesystem::path FilePath(InPath);
		const bool bExists = std::filesystem::exists(FilePath, ErrorCode);
		if (ErrorCode || !bExists)
		{
			DURIN_WARN("Shader dependency file does not exist: {}", InPath);
			return {};
		}

		const uint64 FileSize = std::filesystem::file_size(FilePath, ErrorCode);
		if (ErrorCode)
		{
			DURIN_WARN("Failed to get shader dependency file size: {}", InPath);
			return {};
		}

		const int64 LastWriteTicks = ToTicks(std::filesystem::last_write_time(FilePath, ErrorCode));
		if (ErrorCode)
		{
			DURIN_WARN("Failed to get shader dependency file timestamp: {}", InPath);
			return {};
		}

		const std::string PathKey(InPath);
		if (const auto FoundIt = FileHashCache.find(PathKey); FoundIt != FileHashCache.end())
		{
			if (FoundIt->second.LastWriteTicks == LastWriteTicks && FoundIt->second.FileSize == FileSize)
			{
				return FoundIt->second.ContentHash;
			}
		}

		std::vector<uint8> FileBytes;
		if (!FFileHelper::LoadFileToArray(FileBytes, InPath))
		{
			DURIN_WARN("Failed to read shader dependency file: {}", InPath);
			return {};
		}

		FXxHash64 Hash = FXxHash64::HashBuffer(FileBytes.data(), static_cast<uint64>(FileBytes.size()));
		FileHashCache[PathKey] = {LastWriteTicks, FileSize, Hash};
		return Hash;
	}

	auto FSlangShaderCompiler::ComputeShaderSourceSignatureHash(const std::vector<std::string>& InDependencyPaths, const FShaderCompileOptions& Options) -> FXxHash64
	{
		FXxHash64Builder Builder;

		auto UpdateString = [&Builder](std::string_view Value) {
			const uint64 Length = static_cast<uint64>(Value.size());
			Builder.Update(&Length, sizeof(Length));
			if (Length > 0)
			{
				Builder.Update(Value.data(), Length);
			}
		};

		UpdateString("DurinShaderSourceSignature_v1");
		UpdateString(Options.ShaderName);
		UpdateString("spirv_1_5");

		for (uint32 EntryPointIndex = 0; EntryPointIndex < Options.EntryPoints.size(); ++EntryPointIndex)
		{
			UpdateString(Options.EntryPoints[EntryPointIndex]);
			const int32 FrequencyValue = static_cast<int32>(Options.Frequencies[EntryPointIndex]);
			Builder.Update(&FrequencyValue, sizeof(FrequencyValue));
		}

		for (const std::string& DependencyPath : InDependencyPaths)
		{
			UpdateString(DependencyPath);
			const FXxHash64 FileHash = GetOrComputeFileHash(DependencyPath);
			Builder.Update(&FileHash.HashValue, sizeof(FileHash.HashValue));
		}

		return Builder.Finalize();
	}

	auto FSlangShaderCompiler::TryLoadShaderCache(
		std::string_view ShaderName,
		const FShaderCompileOptions& Options,
		FXxHash64 SourceSignatureHash,
		FShaderCompilerOutput& OutOutput
	) -> bool
	{
		const uint32 EntryPointCount = static_cast<uint32>(Options.EntryPoints.size());
		OutOutput.CompiledShaders.resize(EntryPointCount);

		for (uint32 EntryPointIndex = 0; EntryPointIndex < EntryPointCount; ++EntryPointIndex)
		{
			const std::string CachePath = FShaderPaths::BinaryPath(ShaderName, Options.EntryPoints[EntryPointIndex], SourceSignatureHash.HashValue);
			if (!FFileHelper::FileExists(CachePath))
			{
				return false;
			}

			std::vector<uint8> ShaderBytes;
			if (!FFileHelper::LoadFileToArray(ShaderBytes, CachePath))
			{
				return false;
			}

			auto& CompiledShader = OutOutput.CompiledShaders[EntryPointIndex];
			CompiledShader.Frequency = Options.Frequencies[EntryPointIndex];
			CompiledShader.Code = std::make_shared<FShaderCode>();
			CompiledShader.Code->resize(ShaderBytes.size());
			if (!ShaderBytes.empty())
			{
				std::memcpy(CompiledShader.Code->data(), ShaderBytes.data(), ShaderBytes.size());
				CompiledShader.Hash = FXxHash64::HashBuffer(ShaderBytes.data(), static_cast<uint64>(ShaderBytes.size()));
			}
		}

		OutOutput.bSucceeded = true;
		return true;
	}

	auto FSlangShaderCompiler::SaveCompiledShaderCache(
		std::string_view ShaderName,
		const FShaderCompileOptions& Options,
		FXxHash64 SourceSignatureHash,
		const FShaderCompilerOutput& Output
	) const -> void
	{
		for (uint32 EntryPointIndex = 0; EntryPointIndex < Output.CompiledShaders.size(); ++EntryPointIndex)
		{
			const std::string CachePath = FShaderPaths::BinaryPath(ShaderName, Options.EntryPoints[EntryPointIndex], SourceSignatureHash.HashValue);
			FFileHelper::SaveArrayToFile(*Output.CompiledShaders[EntryPointIndex].Code, CachePath);
		}
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

	static auto FillCompilerOutput(Slang::ComPtr<slang::IComponentType>& ComposedProgram, const FShaderCompileOptions& Options, FShaderCompilerOutput& Output) -> Slang::Result
	{
		const uint32 EntryPointCount = Options.EntryPoints.size();
		Output.CompiledShaders.resize(EntryPointCount);
		for (uint32 i = 0; i < EntryPointCount; ++i)
		{
			Slang::ComPtr<slang::IBlob> CodeBlob;
			Slang::ComPtr<slang::IBlob> CodeHashBlob;
			ComposedProgram->getEntryPointCode(i, 0, CodeBlob.writeRef(), nullptr);
			ComposedProgram->getEntryPointHash(i, 0, CodeHashBlob.writeRef());

			auto& CompiledShader = Output.CompiledShaders[i];
			CompiledShader.Frequency = Options.Frequencies[i];
			CompiledShader.Code = std::make_shared<FShaderCode>();
			CompiledShader.Hash = FXxHash64(*static_cast<const uint64_t*>(CodeHashBlob->getBufferPointer()));
			if (!ConvertBlobToArray(CodeBlob, *CompiledShader.Code))
			{
				return SLANG_FAIL;
			}
		}
		return SLANG_OK;
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

		if (EntryPointCount != Options.Frequencies.size())
		{
			Output.ErrorMessage = "Entry point count does not match shader frequency count";
			return Output;
		}

		const bool bForceRecompile = Options.bForceRecompile || Settings.bForceRecompile;
		const std::string ShaderName = !Options.ShaderName.empty() ? Options.ShaderName : std::string(ShaderSourceFilePath);

		std::vector<std::string> DependencyPaths;
		std::string DependencyDiagnostics;
		if (SLANG_FAILED(ResolveDependencyFiles(ShaderSourceFilePath.data(), DependencyPaths, DependencyDiagnostics)))
		{
			Output.ErrorMessage = DependencyDiagnostics.empty() ? "Failed to parse shader dependency graph" : DependencyDiagnostics;
			return Output;
		}

		const FXxHash64 SourceSignatureHash = ComputeShaderSourceSignatureHash(DependencyPaths, Options);
		SaveShaderDependencyMeta(ShaderName, ShaderSourceFilePath, SourceSignatureHash, DependencyPaths);
		if (!bForceRecompile && TryLoadShaderCache(ShaderName, Options, SourceSignatureHash, Output))
		{
			return Output;
		}

		Slang::ComPtr<slang::IBlob> DiagnosticsBlob;
		Slang::ComPtr<slang::IComponentType> ComposedProgram;
		const Slang::Result CompileResult = CompileInternal(ShaderSourceFilePath.data(), EntryPoints, ComposedProgram, DiagnosticsBlob);

		// If compilation failed, fill the error message and return
		// The user might want to handle the error message in different ways, for example, showing it in the UI, or writing it to a log file, etc.
		// So we don't log error message here, just fill it in the output and let the user decide how to handle it.
		if (SLANG_FAILED(CompileResult))
		{
			if (DiagnosticsBlob != nullptr)
			{
				Output.ErrorMessage = std::string{"Failed to compile shader. Diagnostics: \n"} + static_cast<const char*>(DiagnosticsBlob->getBufferPointer());
			}
			return Output;
		}

		// If compilation succeeded, convert the compiled code blobs to arrays and fill the output
		Slang::Result OutputFillResult = FillCompilerOutput(ComposedProgram, Options, Output);
		if (SLANG_FAILED(OutputFillResult))
		{
			Output.ErrorMessage = "Failed to fill shader compiler output";
			return Output;
		}

		SaveCompiledShaderCache(ShaderName, Options, SourceSignatureHash, Output);

		Output.bSucceeded = true;
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

	auto FSlangShaderCompiler::CompileInternal(
		const char8* InShaderFilePath,
		const std::span<const char8* const>& InEntryPoints,
		Slang::ComPtr<slang::IComponentType>& OutComposedProgram,
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
		SLANG_RETURN_ON_FAIL(Session->createCompositeComponentType(
			ComponentTypes.data(),
			ComponentTypes.size(),
			OutComposedProgram.writeRef(),
			OutDiagnostics.writeRef()
		));

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