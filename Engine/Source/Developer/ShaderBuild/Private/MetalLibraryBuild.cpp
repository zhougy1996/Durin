#include "MetalLibraryBuild.h"
#include "MetalShaderFormat.h"
#include "Misc/Guid.h"

#if defined(__APPLE__)
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Durin
{
	namespace
	{
		constexpr uint64 MaximumSourceBytes = 1024 * 1024;
		constexpr uint64 MaximumLibraryBytes = 64ull * 1024 * 1024;
		constexpr std::string_view FunctionName = "Durin.Metal.Library";
	#if defined(__APPLE__)
		struct FProcessOutput { int Code = -1; std::string Text; };
		std::mutex ProcessMutex;
		std::condition_variable ProcessChanged;
		uint32 ActiveProcesses = 0;
		auto RunTool(std::vector<std::string> Arguments, const std::filesystem::path& Directory,
			const DerivedData::FBuildCancellation& Cancel) -> std::expected<FProcessOutput, std::string>
		{
			{
				std::unique_lock Lock(ProcessMutex);
				while (ActiveProcesses >= 2)
				{
					if (Cancel.IsCancelled()) return std::unexpected("Metal tool request canceled.");
					ProcessChanged.wait_for(Lock, std::chrono::milliseconds(10));
				}
				++ActiveProcesses;
			}
			struct FProcessLease
			{
				~FProcessLease() { std::lock_guard Lock(ProcessMutex); --ActiveProcesses; ProcessChanged.notify_one(); }
			} Lease;
			Arguments.insert(Arguments.begin(), {"/usr/bin/xcrun", "--sdk", "macosx", "--toolchain", "Metal"});
			std::vector<char*> Argv;
			for (auto& Argument : Arguments) Argv.push_back(Argument.data());
			Argv.push_back(nullptr);
			if (Cancel.IsCancelled()) return std::unexpected("Metal compiler canceled.");
			struct FChild
			{
				int Pipe[2] = {-1, -1};
				pid_t Pid = 0;
				~FChild()
				{
					if (Pid > 0)
					{
						kill(-Pid, SIGKILL);
						int Status;
						while (waitpid(Pid, &Status, 0) < 0 && errno == EINTR) {}
					}
					for (int Descriptor : Pipe) if (Descriptor >= 0) close(Descriptor);
				}
			} Child;
			if (pipe(Child.Pipe)) return std::unexpected("Cannot create Metal compiler diagnostic pipe.");
			auto& Pipe = Child.Pipe;
			if (fcntl(Pipe[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(Pipe[1], F_SETFD, FD_CLOEXEC) < 0
				|| fcntl(Pipe[0], F_SETFL, O_NONBLOCK) < 0)
				return std::unexpected("Cannot configure Metal compiler diagnostic pipe.");
			posix_spawn_file_actions_t Actions;
			if (posix_spawn_file_actions_init(&Actions)) return std::unexpected("Cannot initialize Metal tool actions.");
			struct FActions { posix_spawn_file_actions_t& Value; ~FActions() { posix_spawn_file_actions_destroy(&Value); } } ActionsLease{Actions};
			if (posix_spawn_file_actions_adddup2(&Actions, Pipe[1], STDOUT_FILENO)
				|| posix_spawn_file_actions_adddup2(&Actions, Pipe[1], STDERR_FILENO)
				|| posix_spawn_file_actions_addclose(&Actions, Pipe[0])
				|| posix_spawn_file_actions_addclose(&Actions, Pipe[1])
				|| (!Directory.empty() && posix_spawn_file_actions_addchdir_np(&Actions, Directory.c_str())))
				return std::unexpected("Cannot configure Metal tool actions.");
			posix_spawnattr_t Attributes;
			if (posix_spawnattr_init(&Attributes)) return std::unexpected("Cannot initialize Metal tool attributes.");
			struct FAttributes { posix_spawnattr_t& Value; ~FAttributes() { posix_spawnattr_destroy(&Value); } } AttributesLease{Attributes};
			if (posix_spawnattr_setflags(&Attributes, POSIX_SPAWN_SETPGROUP) || posix_spawnattr_setpgroup(&Attributes, 0))
				return std::unexpected("Cannot configure Metal tool process group.");
			pid_t LaunchedPid = 0;
			if (posix_spawn(&LaunchedPid, Argv[0], &Actions, &Attributes, Argv.data(), environ))
				return std::unexpected("Cannot launch the Metal toolchain.");
			Child.Pid = LaunchedPid;
			close(Pipe[1]); Pipe[1] = -1;
			const pid_t Pid = Child.Pid;
			FProcessOutput Result;
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
			int Status = 0;
			for (;;)
			{
				char Buffer[2048]; ssize_t Count;
				while ((Count = read(Pipe[0], Buffer, sizeof(Buffer))) > 0)
					Result.Text.append(Buffer, std::min<size_t>(Count, 16384 - Result.Text.size()));
				const auto Waited = waitpid(Pid, &Status, WNOHANG);
				if (Waited == Pid)
				{
					while ((Count = read(Pipe[0], Buffer, sizeof(Buffer))) > 0)
						Result.Text.append(Buffer, std::min<size_t>(Count, 16384 - Result.Text.size()));
					Child.Pid = 0;
					break;
				}
				if ((Waited < 0 && errno != EINTR) || Cancel.IsCancelled() || std::chrono::steady_clock::now() >= Deadline)
				{
					return std::unexpected("Metal compiler canceled, timed out or failed while waiting.");
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			Result.Code = WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
			return Result;
		}
		std::mutex ProducerMutex;
		std::condition_variable ProducerChanged;
		uint32 ActiveProducers = 0;
	#endif
		class FMetalLibraryFunction final : public DerivedData::IBuildFunction
		{
		public:
			auto GetName() const -> std::string_view override { return FunctionName; }
			auto GetVersion() const -> uint32 override { return MetalLibraryBuildVersion; }
			auto Configure(DerivedData::FBuildConfigContext& Context) const -> void override
			{
				Context.SetConstantsSchema(1); Context.SetOutput("Metal.Library", 1);
				Context.SetCacheBucket(DerivedData::FCacheBucket::FromString("Shader"));
			}
			auto Build(DerivedData::FBuildContext& Context) const -> void override
			{
				const auto* Source = Context.FindConstant<std::string>("Source");
				const auto* Producer = Context.FindConstant<std::string>("Producer");
				const auto* Profile = Context.FindConstant<FXxHash128>("Profile");
				if (!Source || Source->empty() || Source->size() > MaximumSourceBytes
					|| Source->contains('\0') || !Producer || Producer->empty()
					|| !Profile || *Profile != MetalLibraryShaderTarget.SettingsDigest)
					return Context.FailExecution("Invalid Metal library build inputs.");
			#if defined(__APPLE__)
				const auto Actual = GetMetalLibraryProducerIdentity(Context.GetCancellation());
				if (!Actual || *Actual != *Producer)
					return Context.FailExecution(Actual ? "Metal toolchain changed after capture." : Actual.error());
				{
					std::unique_lock Lock(ProducerMutex);
					while (ActiveProducers >= 2)
					{
						if (Context.IsCancelled()) return;
						ProducerChanged.wait_for(Lock, std::chrono::milliseconds(10));
					}
					++ActiveProducers;
				}
				struct FRelease
				{
					~FRelease() { std::lock_guard Lock(ProducerMutex); --ActiveProducers; ProducerChanged.notify_one(); }
				} Release;
				const auto Scratch = std::filesystem::temp_directory_path() / ("DurinMetal-" + FGuid::NewGuid().ToString());
				struct FCleanup
				{
					std::filesystem::path Path;
					~FCleanup() { std::error_code Error; std::filesystem::remove_all(Path, Error); }
				} Cleanup{Scratch};
				std::error_code Error;
				if (!std::filesystem::create_directory(Scratch, Error))
					return Context.FailExecution("Cannot create Metal compiler scratch directory.");
				{
					std::ofstream File(Scratch / "shader.metal", std::ios::binary);
					File.write(Source->data(), Source->size());
					if (!File) return Context.FailExecution("Cannot capture Metal compiler source.");
				}
				const auto Compile = RunTool({"metal", "-std=macos-metal2.0", "-mmacosx-version-min=14.0", "-O2",
					"-c", "shader.metal", "-o", "shader.air"}, Scratch, Context.GetCancellation());
				if (!Compile || Compile->Code) return Context.FailExecution(Compile ? Compile->Text : Compile.error());
				const auto IntermediateSize = std::filesystem::file_size(Scratch / "shader.air", Error);
				if (Error || IntermediateSize > MaximumLibraryBytes)
					return Context.FailExecution("Metal intermediate exceeds output limit.");
				const auto Link = RunTool({"metallib", "shader.air", "-o", "shader.metallib"}, Scratch, Context.GetCancellation());
				if (!Link || Link->Code) return Context.FailExecution(Link ? Link->Text : Link.error());
				const auto Size = std::filesystem::file_size(Scratch / "shader.metallib", Error);
				if (Error || Size > MaximumLibraryBytes) return Context.FailExecution("Metal library exceeds output limit.");
				FByteBuffer Bytes(Size);
				std::ifstream File(Scratch / "shader.metallib", std::ios::binary);
				File.read(reinterpret_cast<char*>(Bytes.data()), Bytes.size());
				if (!File || !ShaderFormat::ValidateMetalLibrary(Bytes))
					return Context.FailExecution("Metal compiler produced an invalid library.");
				Context.AddValue(DerivedData::FValueId::FromName("Library"), FSharedByteBuffer::Take(std::move(Bytes)));
			#else
				Context.FailExecution("The Metal library producer is unavailable on this host.");
			#endif
			}
		};
	}
	auto GetMetalLibraryProducerIdentity(const DerivedData::FBuildCancellation& Cancel) -> std::expected<std::string, std::string>
	{
	#if defined(__APPLE__)
		const auto Version = RunTool({"metal", "--version"}, {}, Cancel);
		if (!Version || Version->Code) return std::unexpected(Version ? Version->Text : Version.error());
		const auto SDK = RunTool({"--show-sdk-version"}, {}, Cancel);
		if (!SDK || SDK->Code) return std::unexpected(SDK ? SDK->Text : SDK.error());
		const auto SDKBuild = RunTool({"--show-sdk-build-version"}, {}, Cancel);
		if (!SDKBuild || SDKBuild->Code) return std::unexpected(SDKBuild ? SDKBuild->Text : SDKBuild.error());
		const auto VersionStart = Version->Text.find("Apple metal version ");
		if (VersionStart == std::string::npos) return std::unexpected("Unrecognized Metal compiler version.");
		const auto LastLine = [](std::string Text) {
			while (!Text.empty() && (Text.back() == '\n' || Text.back() == '\r')) Text.pop_back();
			return Text.substr(Text.find_last_of('\n') == std::string::npos ? 0 : Text.find_last_of('\n') + 1);
		};
		const auto SDKVersion = LastLine(SDK->Text), SDKBuildVersion = LastLine(SDKBuild->Text);
		if (SDKVersion.empty() || SDKBuildVersion.empty()
			|| SDKVersion.find_first_not_of("0123456789.") != std::string::npos
			|| SDKBuildVersion.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != std::string::npos)
			return std::unexpected("Unrecognized Metal SDK provenance.");
		return Version->Text.substr(VersionStart, Version->Text.find('\n', VersionStart) - VersionStart)
			+ ";sdk=" + SDKVersion + ";sdk-build=" + SDKBuildVersion;
	#else
		return std::unexpected("The Metal library producer is unavailable on this host.");
	#endif
	}
	auto CreateMetalLibraryBuildFunction() -> std::shared_ptr<const DerivedData::IBuildFunction>
	{
		return std::make_shared<FMetalLibraryFunction>();
	}
	auto MakeMetalLibraryBuildRequest(const FShaderCode& Source, std::string ProducerIdentity)
		-> std::expected<FShaderSessionRequest, std::string>
	{
		if (Source.Target != MetalShaderTarget || Source.Code.size() > MaximumSourceBytes
			|| !ShaderFormat::ValidateShaderCode(Source) || ProducerIdentity.empty())
			return std::unexpected("Invalid source artifact for Metal library production.");
		DerivedData::FBuildDefinitionBuilder Builder{std::string(FunctionName)};
		Builder.AddConstant("Source", std::string(reinterpret_cast<const char*>(Source.Code.data()), Source.Code.size()));
		Builder.AddConstant("Producer", std::move(ProducerIdentity));
		Builder.AddConstant("Profile", MetalLibraryShaderTarget.SettingsDigest);
		auto Definition = std::move(Builder).Build();
		if (!Definition) return std::unexpected("Cannot encode Metal library build definition.");
		return FShaderSessionRequest{.Definition = std::move(*Definition)};
	}
}
