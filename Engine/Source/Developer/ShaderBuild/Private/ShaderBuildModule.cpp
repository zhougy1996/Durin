#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "Shader/ShaderBuildProvider.h"
#include "Shader/ShaderData.h"
#include "ShaderBuild/ShaderBuildLifecycle.h"
#include "ShaderBuild/ShaderPaths.h"
#include "ShaderCompileService.h"
#include "ShaderLibraryProducer.h"
#include "Misc/FileHelper.h"

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
			std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
			std::string& OutError) -> bool override
		{
			return BuildShaderSourceDependencyManifestFromService(
				VirtualShaderPath, Options, OutDependencies, OutError);
		}

		auto BuildSourceTreeFingerprint(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			FShaderSourceDependencyFingerprint& OutFingerprint,
			std::string& OutError) -> bool override
		{
			return BuildShaderSourceTreeFingerprintFromService(
				VirtualShaderPath, Options, OutFingerprint, OutError);
		}

		auto CaptureSourceArtifacts(std::shared_ptr<const FShaderSourceArtifacts>& OutArtifacts,
			std::string& OutError, const std::function<bool()>& IsCancelled) -> bool override
		{
			OutArtifacts.reset();
			const auto Mounts = FShaderPaths::GetRegisteredMountPoints();
			if (Mounts.size() > 256) { OutError = "Shader mount limit exceeded."; return false; }
			uint64 Entries = 0;
			std::map<std::string, FByteBuffer> Files;
			std::vector<std::string> SearchRoots;
			uint64 TotalBytes = 0;
			std::error_code Error;
			for (const auto& Mount : Mounts)
			{
				const std::filesystem::path Root(Mount.SourceDir);
				SearchRoots.push_back(Mount.VirtualRoot);
				std::filesystem::recursive_directory_iterator It(Root, Error), End;
				if (Error) { OutError = Error.message(); return false; }
				for (; It != End; It.increment(Error))
				{
					if (IsCancelled && IsCancelled()) { OutError = "Shader input capture cancelled."; return false; }
					if (++Entries > 131072) { OutError = "Shader directory entry limit exceeded."; return false; }
					if (Error) { OutError = Error.message(); return false; }
					if (It->is_symlink(Error))
					{ OutError = "Shader capture requires regular source files, not symbolic links."; return false; }
					if (Error) { OutError = Error.message(); return false; }
					if (!It->is_regular_file(Error))
					{
						if (Error) { OutError = Error.message(); return false; }
						continue;
					}
					const auto Name = (std::filesystem::path(Mount.VirtualRoot)
						/ It->path().lexically_relative(Root)).lexically_normal().generic_string();
					FFileHelper::FFileIoError ReadError;
					auto File = FFileHelper::OpenRead(It->path(), &ReadError);
					if (!File) { OutError = ReadError.ToString(); return false; }
					const auto Size = File->GetSize();
					if (Files.size() >= 65536 || Name.size() > 4096
						|| Size > 64ull * 1024 * 1024 || Size > 512ull * 1024 * 1024 - TotalBytes)
					{ OutError = "Shader capture input limit exceeded."; return false; }
					FByteBuffer Bytes(static_cast<size_t>(Size));
					constexpr size_t Chunk = 4 * 1024 * 1024;
					for (size_t Offset = 0; Offset < Bytes.size(); Offset += Chunk)
					{
						if (IsCancelled && IsCancelled()) { OutError = "Shader input capture cancelled."; return false; }
						if (!File->ReadAt(Offset, std::span(Bytes).subspan(Offset, std::min(Chunk, Bytes.size() - Offset)), &ReadError))
						{ OutError = ReadError.ToString(); return false; }
					}
					TotalBytes += Size;
					if (!Files.emplace(Name, std::move(Bytes)).second)
					{ OutError = "Shader capture contains duplicate logical files."; return false; }
				}
				if (Error) { OutError = Error.message(); return false; }
			}
			OutArtifacts = std::make_shared<FShaderSourceArtifacts>(Files, std::move(SearchRoots));
			return true;
		}

		auto GetStats() const -> FShaderBuildStats override
		{
			return GetShaderCompileServiceStats();
		}

		auto BuildCookedLibrary(
			EShaderTargetPlatform TargetPlatform,
			EShaderTargetProfile TargetProfile,
			FByteBuffer& OutBytes,
			std::string& OutError, std::shared_ptr<const FShaderSourceArtifacts> Artifacts,
			const std::function<bool()>& IsCancelled) -> bool override
		{
			return ProduceCookedShaderLibrary(
				TargetPlatform, TargetProfile, OutBytes, OutError, std::move(Artifacts), IsCancelled);
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
			std::string Error;
			requiref(InitializeShaderData(
				FShaderDataConfiguration::Authored(), Error),
				"Authored Shader data initialization failed: {}", Error);
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
