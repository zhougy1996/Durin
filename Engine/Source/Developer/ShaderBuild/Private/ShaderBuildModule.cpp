#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "Shader/ShaderBuildProvider.h"
#include "Shader/ShaderData.h"
#include "ShaderBuild/ShaderBuildLifecycle.h"
#include "ShaderBuild/ShaderPaths.h"
#include "ShaderCompileService.h"
#include "ShaderLibraryProducer.h"
#include "Misc/FileHelper.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	class FShaderBuildProvider final : public IShaderBuildProvider
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
			if (Mounts.size() > 256) { return {.Error = {.Code = EShaderError::CaptureMountLimit,
				.Expected = 256,
				.Actual = Mounts.size()}}; }
			uint64 Entries = 0;
			std::map<std::string, FXxHash128> Files;
			std::vector<std::string> SearchRoots;
			uint64 TotalBytes = 0;
			std::error_code Error;
			for (const auto& Mount : Mounts)
			{
				const std::filesystem::path Root(Mount.SourceDir);
				SearchRoots.push_back(Mount.VirtualRoot);
				std::filesystem::recursive_directory_iterator It(Root, Error), End;
				if (Error) { return {.Error = {.Code = EShaderError::FileSystemFailure,
					.ActualIdentity = Root.generic_string(),
					.SystemError = Error}}; }
				for (; It != End; It.increment(Error))
				{
					if (IsCancelled && IsCancelled()) { return {.Error = {.Code = EShaderError::Cancelled}}; }
					if (++Entries > 131072) { return {.Error = {.Code = EShaderError::CaptureDirectoryLimit,
						.Expected = 131072,
						.Actual = Entries}}; }
					if (Error) { return {.Error = {.Code = EShaderError::FileSystemFailure,
						.ActualIdentity = Root.generic_string(),
						.SystemError = Error}}; }
					if (It->is_symlink(Error))
					{ return {.Error = {.Code = EShaderError::CaptureSymlink, .ActualIdentity = It->path().generic_string()}}; }
					if (Error) { return {.Error = {.Code = EShaderError::FileSystemFailure,
						.ActualIdentity = Root.generic_string(),
						.SystemError = Error}}; }
					if (!It->is_regular_file(Error))
					{
						if (Error) { return {.Error = {.Code = EShaderError::FileSystemFailure,
							.ActualIdentity = Root.generic_string(),
							.SystemError = Error}}; }
						continue;
					}
					const auto Name = (std::filesystem::path(Mount.VirtualRoot)
						/ It->path().lexically_relative(Root)).lexically_normal().generic_string();
					FFileHelper::FFileIoError ReadError;
					auto File = FFileHelper::OpenRead(It->path(), &ReadError);
					if (!File) { return {.Error = {.Code = EShaderError::FileReadFailure, .FileError = ReadError}}; }
					const auto Size = File->GetSize();
					if (Files.size() >= 65536 || Name.size() > 4096
						|| Size > 64ull * 1024 * 1024 || Size > 512ull * 1024 * 1024 - TotalBytes)
					{ return {.Error = {.Code = EShaderError::CaptureInputLimit, .ActualIdentity = Name, .Actual = Size}}; }
					FByteBuffer Bytes(static_cast<size_t>(Size));
					constexpr size_t Chunk = 4 * 1024 * 1024;
					for (size_t Offset = 0; Offset < Bytes.size(); Offset += Chunk)
					{
						if (IsCancelled && IsCancelled()) { return {.Error = {.Code = EShaderError::Cancelled}}; }
						if (!File->ReadAt(Offset, std::span(Bytes).subspan(Offset, std::min(Chunk, Bytes.size() - Offset)), &ReadError))
						{ return {.Error = {.Code = EShaderError::FileReadFailure, .FileError = ReadError}}; }
					}
					TotalBytes += Size;
					if (!Files.emplace(Name, FXxHash128::HashBuffer(Bytes)).second)
					{ return {.Error = {.Code = EShaderError::CaptureDuplicateFile, .ActualIdentity = Name}}; }
				}
				if (Error) { return {.Error = {.Code = EShaderError::FileSystemFailure,
					.ActualIdentity = Root.generic_string(),
					.SystemError = Error}}; }
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
	};

	namespace
	{
		FShaderBuildProvider Provider;
		FModularFeatureRegistration ProviderRegistration;
		std::unique_ptr<FModuleTestOwner> TestOwner;

		auto InitializeShaderBuildServices() -> void
		{
			FShaderPaths::InitDefaultMountPoints();
			InitShaderCompileService();
			FShaderOperationResult Error;
			requiref((Error = InitializeShaderData(FShaderDataConfiguration::Authored())),
				"Authored Shader data initialization failed: {}", FormatShaderError(Error.Error));
		}
	}

	auto InitializeShaderBuild() -> void
	{
		requiref(!ProviderRegistration.IsValid(),
			"ShaderBuild is already initialized");
		ProviderRegistration =
			FModuleStartup::RegisterFeature<IShaderBuildProvider>(Provider);
		require(ProviderRegistration.IsValid());
		InitializeShaderBuildServices();
	}

	auto InitializeShaderBuildForTesting() -> void
	{
		requiref(!ProviderRegistration.IsValid(),
			"ShaderBuild is already initialized");
		TestOwner = std::make_unique<FModuleTestOwner>("ShaderBuildTestRoot");
		ProviderRegistration = TestOwner->RegisterFeature(Provider);
		require(ProviderRegistration.IsValid());
		InitializeShaderBuildServices();
	}

	auto ShutdownShaderBuild() -> void
	{
		const FModularFeatureRetirementResult Retirement =
			ProviderRegistration.Reset();
		requiref(Retirement.Succeeded(),
			"ShaderBuild provider retirement failed: {}", Retirement.Message);
		ShutdownShaderCompileService();
		ShutdownShaderData();
		TestOwner.reset();
	}

	class FShaderBuildModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			InitializeShaderBuild();
		}

		auto ShutdownModule() -> void override
		{
			ShutdownShaderBuild();
		}
	};

	IMPLEMENT_MODULE(FShaderBuildModule, ShaderBuild)
}
