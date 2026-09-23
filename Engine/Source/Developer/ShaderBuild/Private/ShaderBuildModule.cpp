#include "Modules/ModuleManager.h"
#include "Shader/IShaderBuildModule.h"
#include "Shader/ShaderData.h"
#include "ShaderBuild/ShaderPaths.h"
#include "ShaderCompileService.h"
#include "ShaderLibraryProducer.h"
#include "Misc/FileHelper.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	namespace
	{
		constexpr uint64 MaximumMounts = 256;
		constexpr uint64 MaximumDirectoryEntries = 131072;
		constexpr uint64 MaximumFiles = 65536;
		constexpr uint64 MaximumPathBytes = 4096;
		constexpr uint64 MaximumFileBytes = 64ull * 1024 * 1024;
		constexpr uint64 MaximumTotalBytes = 512ull * 1024 * 1024;
		constexpr size_t ReadChunkBytes = 4 * 1024 * 1024;

		auto CaptureLimitFailure(EShaderError Code, EShaderCaptureLimit Kind,
			uint64 Maximum, uint64 Actual, std::string_view Path = {}) -> FShaderOperationResult
		{
			return std::unexpected(FShaderError{.Code = Code, .ActualIdentity = std::string(Path),
				.CaptureLimit = FShaderCaptureLimitContext{Kind, Maximum, Actual}});
		}
	}

	class FShaderBuildModule final : public IShaderBuildModule
	{
	public:
		auto CompileMounted(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options) -> FShaderCompilerOutput override
		{
			return GetOrCompileShader(VirtualShaderPath, Options);
		}

		auto CompileGenerated(const FGeneratedShaderCompileRequest& Request)
			-> FShaderCompilerOutput override
		{
			return GetOrCompileGeneratedShader(Request);
		}

		auto GetCompilerEnvironmentIdentity() -> std::string override
		{
			return GetShaderCompilerEnvironmentIdentityFromService();
		}

		auto BuildSourceDependencyManifest(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult override
		{
			return BuildShaderSourceDependencyManifestFromService(VirtualShaderPath, Options, OutDependencies);
		}

		auto BuildSourceTreeFingerprint(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult override
		{
			return BuildShaderSourceTreeFingerprintFromService(VirtualShaderPath, Options, OutFingerprint);
		}

		auto GetCookInputIdentity(std::string& OutIdentity, const std::function<bool()>& IsCancelled) -> FShaderOperationResult override
		{
			OutIdentity.clear();
			const auto Mounts = FShaderPaths::GetRegisteredMountPoints();
			if (Mounts.size() > MaximumMounts)
				return CaptureLimitFailure(EShaderError::CaptureMountLimit,
					EShaderCaptureLimit::Mounts, MaximumMounts, Mounts.size());
			uint64 Entries = 0;
			std::map<std::string, FXxHash128> Files;
			std::vector<std::string> SearchRoots;
			uint64 TotalBytes = 0;
			std::error_code Error;
			for (const auto& Mount : Mounts)
			{
				const std::filesystem::path Root(Mount.SourceDir);
				SearchRoots.push_back(Mount.VirtualRoot);
				auto FailurePath = Root;
				std::filesystem::recursive_directory_iterator It(Root, Error), End;
				if (Error) { return std::unexpected(FShaderError::FromFileSystem(FailurePath, Error)); }
				for (; It != End; It.increment(Error))
				{
					if (IsCancelled && IsCancelled()) { return std::unexpected(FShaderError{.Code = EShaderError::Cancelled}); }
					if (++Entries > MaximumDirectoryEntries)
						return CaptureLimitFailure(EShaderError::CaptureDirectoryLimit,
							EShaderCaptureLimit::DirectoryEntries, MaximumDirectoryEntries, Entries, Root.generic_string());
					if (Error) { return std::unexpected(FShaderError::FromFileSystem(FailurePath, Error)); }
					FailurePath = It->path();
					if (It->is_symlink(Error))
					{ return std::unexpected(FShaderError{.Code = EShaderError::CaptureSymlink, .ActualIdentity = It->path().generic_string()}); }
					if (Error) { return std::unexpected(FShaderError::FromFileSystem(FailurePath, Error)); }
					if (!It->is_regular_file(Error))
					{
						if (Error) { return std::unexpected(FShaderError::FromFileSystem(FailurePath, Error)); }
						continue;
					}
					const auto Name = (std::filesystem::path(Mount.VirtualRoot)
						/ It->path().lexically_relative(Root)).lexically_normal().generic_string();
					auto File = FFileHelper::OpenRead(It->path());
					if (!File) { return std::unexpected(FShaderError{.Code = EShaderError::FileReadFailure, .FileError = File.error()}); }
					const auto Size = (*File)->GetSize();
					if (Files.size() >= MaximumFiles)
						return CaptureLimitFailure(EShaderError::CaptureInputLimit,
							EShaderCaptureLimit::Files, MaximumFiles, Files.size() + 1, Name);
					if (Name.size() > MaximumPathBytes)
						return CaptureLimitFailure(EShaderError::CaptureInputLimit,
							EShaderCaptureLimit::PathBytes, MaximumPathBytes, Name.size(), Name);
					if (Size > MaximumFileBytes)
						return CaptureLimitFailure(EShaderError::CaptureInputLimit,
							EShaderCaptureLimit::FileBytes, MaximumFileBytes, Size, Name);
					if (Size > MaximumTotalBytes - TotalBytes)
						return CaptureLimitFailure(EShaderError::CaptureInputLimit,
							EShaderCaptureLimit::TotalBytes, MaximumTotalBytes, TotalBytes + Size, Name);
					FByteBuffer Bytes(static_cast<size_t>(Size));
					for (size_t Offset = 0; Offset < Bytes.size(); Offset += ReadChunkBytes)
					{
						if (IsCancelled && IsCancelled()) { return std::unexpected(FShaderError{.Code = EShaderError::Cancelled}); }
						if (auto Read = (*File)->ReadAt(Offset, std::span(Bytes).subspan(Offset, std::min(ReadChunkBytes, Bytes.size() - Offset))); !Read)
						{ return std::unexpected(FShaderError{.Code = EShaderError::FileReadFailure, .FileError = Read.error()}); }
					}
					TotalBytes += Size;
					if (!Files.emplace(Name, FXxHash128::HashBuffer(Bytes)).second)
					{ return std::unexpected(FShaderError{.Code = EShaderError::CaptureDuplicateFile, .ActualIdentity = Name}); }
				}
				if (Error) { return std::unexpected(FShaderError::FromFileSystem(FailurePath, Error)); }
			}
			FBinaryWriter Identity;
			Identity.WriteString(GetCompilerEnvironmentIdentity());
			Identity.WriteU32(static_cast<uint32>(SearchRoots.size()));
			for (const auto& Root : SearchRoots) Identity.WriteString(Root);
			Identity.WriteU32(static_cast<uint32>(Files.size()));
			for (const auto& [Name, Digest] : Files)
			{
				Identity.WriteString(Name); Identity.WriteHash128(Digest);
			}
			const auto Digest = FXxHash128::HashBuffer(Identity.GetBytes());
			OutIdentity = std::format("{:016x}{:016x}", Digest.HashHigh, Digest.HashLow);
			return {};
		}

		auto GetStats() const -> FShaderBuildStats override
		{
			return GetShaderCompileServiceStats();
		}

		auto BuildCookedLibrary(
			EShaderTargetPlatform TargetPlatform,
			EShaderTargetProfile TargetProfile,
			FByteBuffer& OutBytes,
			std::shared_ptr<const FShaderSourceArtifacts> Artifacts,
			const std::function<bool()>& IsCancelled) -> FShaderOperationResult override
		{
			return ProduceCookedShaderLibrary(
				TargetPlatform, TargetProfile, OutBytes, std::move(Artifacts), IsCancelled);
		}

		// Resident until normal editor/tool shutdown; dynamic reloading is unsupported.
		auto StartupModule() -> void override
		{
			FShaderPaths::InitDefaultMountPoints();
			InitShaderCompileService();
			const auto Result = InitializeShaderData(FShaderDataConfiguration::Authored());
			requiref(Result,
				"Authored Shader data initialization failed: {}", FormatShaderError(Result.error()));
		}

		auto ShutdownModule() -> void override
		{
			ShutdownShaderCompileService();
			ShutdownShaderData();
		}
	};

	IMPLEMENT_MODULE(FShaderBuildModule, ShaderBuild)
}
