#include "MacOSProcessCrashHandler.h"

#include "Diagnostics/ProcessCrashContext.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Build.h"

#include <csignal>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Durin
{
	namespace
	{
		constexpr uint32 CrashContextVersion = 1;
		constexpr uint32 CrashDirectoryCollisionLimit = 16;
		constexpr std::array<int, 5> CrashSignals = {
			SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV};

		struct FCrashEvent
		{
			int Signal = 0;
			int Code = 0;
			uintptr_t Address = 0;
		};

		struct FCrashHandlerState
		{
			std::mutex Mutex;
			std::jthread Reporter;
			int Events[2] = {-1, -1};
			int Acknowledgements[2] = {-1, -1};
			std::array<struct sigaction, CrashSignals.size()> PreviousActions{};
			std::terminate_handler PreviousTerminate = nullptr;
			std::array<std::array<char, PATH_MAX>, 2> RootSlots{};
			std::atomic<uint32> ActiveRootSlot{0};
			std::string ExecutablePath;
			std::atomic<bool> bDisableDump{false};
			std::atomic<bool> bForceCollision{false};
			std::atomic<bool> bExplicitRootOverride{false};
			std::atomic<bool> bFaultCrashWriter{false};
			bool bInstalled = false;
		};

		FCrashHandlerState GCrashHandler;
		volatile sig_atomic_t GCrashWriter = 0;
		int GEventWrite = -1;
		int GAcknowledgementRead = -1;

		auto PublishRootValue(const std::filesystem::path& Root) -> bool
		{
			const std::string Value = Root.generic_string();
			if (Value.empty() || Value.size() >= PATH_MAX) return false;
			const uint32 Active = GCrashHandler.ActiveRootSlot.load(std::memory_order_relaxed);
			const uint32 Next = 1u - Active;
			auto& Slot = GCrashHandler.RootSlots[Next];
			std::memcpy(Slot.data(), Value.data(), Value.size());
			Slot[Value.size()] = '\0';
			GCrashHandler.ActiveRootSlot.store(Next, std::memory_order_release);
			return true;
		}

		auto WriteAll(int File, const void* Data, size_t Size) -> bool
		{
			const auto* Cursor = static_cast<const char*>(Data);
			while (Size > 0)
			{
				const ssize_t Written = write(File, Cursor, Size);
				if (Written > 0)
				{
					Cursor += Written;
					Size -= static_cast<size_t>(Written);
					continue;
				}
				if (Written < 0 && errno == EINTR) continue;
				return false;
			}
			return true;
		}

		auto ReadAll(int File, void* Data, size_t Size) -> bool
		{
			auto* Cursor = static_cast<char*>(Data);
			while (Size > 0)
			{
				const ssize_t Read = read(File, Cursor, Size);
				if (Read > 0)
				{
					Cursor += Read;
					Size -= static_cast<size_t>(Read);
					continue;
				}
				if (Read < 0 && errno == EINTR) continue;
				return false;
			}
			return true;
		}

		auto SignalName(int Signal) -> const char*
		{
			switch (Signal)
			{
			case SIGABRT: return "SIGABRT";
			case SIGBUS: return "SIGBUS";
			case SIGFPE: return "SIGFPE";
			case SIGILL: return "SIGILL";
			case SIGSEGV: return "SIGSEGV";
			default: return "Unknown";
			}
		}

		auto MakeTimestamp() -> std::string
		{
			const auto Now = std::chrono::system_clock::now();
			const auto Milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
				Now.time_since_epoch()) % 1000;
			const std::time_t Time = std::chrono::system_clock::to_time_t(Now);
			std::tm Utc{};
			gmtime_r(&Time, &Utc);
			std::array<char, 40> Buffer{};
			std::snprintf(Buffer.data(), Buffer.size(),
				"%04d%02d%02dT%02d%02d%02d.%03lldZ",
				Utc.tm_year + 1900, Utc.tm_mon + 1, Utc.tm_mday,
				Utc.tm_hour, Utc.tm_min, Utc.tm_sec,
				static_cast<long long>(Milliseconds.count()));
			return Buffer.data();
		}

		auto CreateCrashDirectory(
			const std::filesystem::path& Root,
			std::string& OutCrashId,
			int& OutError) -> std::filesystem::path
		{
			std::error_code Error;
			std::filesystem::create_directories(Root, Error);
			if (Error)
			{
				OutError = Error.value();
				return {};
			}

			if (GCrashHandler.bForceCollision.load(std::memory_order_relaxed))
				std::filesystem::create_directory(Root / OutCrashId, Error);
			for (uint32 Collision = 0; Collision < CrashDirectoryCollisionLimit; ++Collision)
			{
				const std::string CandidateId = Collision == 0
					? OutCrashId : std::format("{}-{}", OutCrashId, Collision);
				const std::filesystem::path Candidate = Root / CandidateId;
				Error.clear();
				if (std::filesystem::create_directory(Candidate, Error))
				{
					OutCrashId = CandidateId;
					return Candidate;
				}
				if (Error && Error != std::errc::file_exists)
				{
					OutError = Error.value();
					return {};
				}
			}
			OutError = EEXIST;
			return {};
		}

		auto WriteCrashArtifacts(const FCrashEvent& Event) -> void
		{
			if (GCrashHandler.bFaultCrashWriter.load(std::memory_order_relaxed)) return;
			const FProcessCrashContextSnapshot Snapshot = ReadProcessCrashContext();
			const std::string Timestamp = MakeTimestamp();
			std::string CrashId = std::format("{}-{}-{}",
				Snapshot.RuntimeVariant[0] ? Snapshot.RuntimeVariant.data() : DURIN_RUNTIME_VARIANT,
				Timestamp, getpid());
			int DirectoryError = 0;
			const uint32 RootSlot = GCrashHandler.ActiveRootSlot.load(std::memory_order_acquire);
			const std::filesystem::path CrashDirectory = CreateCrashDirectory(
				GCrashHandler.RootSlots[RootSlot].data(), CrashId, DirectoryError);
			if (CrashDirectory.empty()) return;

			const std::filesystem::path ContextPath = CrashDirectory
				/ std::format("{}-CrashContext-v1.txt", CrashId);
			std::ofstream Context(ContextPath, std::ios::binary | std::ios::trunc);
			if (!Context) return;
			const uint64 NowMicros = static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
			Context << "FormatVersion=" << CrashContextVersion << '\n'
				<< "CompletionState=PendingMarker\n"
				<< "CrashId=" << CrashId << '\n'
				<< "ReasonCode=" << Event.Signal << '\n'
				<< "ReasonKind=POSIXSignal\n"
				<< "SignalName=" << SignalName(Event.Signal) << '\n'
				<< "SignalCode=" << Event.Code << '\n'
				<< "ExceptionAddress=0x" << std::hex << Event.Address << std::dec << '\n'
				<< "ProcessId=" << getpid() << '\n'
				<< "FaultingThreadId=Unavailable\n"
				<< "RuntimeVariant=" << Snapshot.RuntimeVariant.data() << '\n'
				<< "BuildConfiguration=" << Snapshot.BuildConfiguration.data() << '\n'
				<< "BuildIdentity=" << Snapshot.BuildIdentity.data() << '\n'
				<< "ExecutableImagePath=" << GCrashHandler.ExecutablePath << '\n'
				<< "UtcTimestamp=" << Timestamp << '\n'
				<< "ProcessUptimeMicroseconds="
				<< (NowMicros >= Snapshot.ProcessStartMonotonicMicroseconds
					? NowMicros - Snapshot.ProcessStartMonotonicMicroseconds : 0) << '\n'
				<< "ProcessPhase=" << ProcessCrashPhaseName(Snapshot.Phase) << '\n'
				<< "BreadcrumbWriteSequence=" << Snapshot.BreadcrumbWriteSequence << '\n'
				<< "BreadcrumbFirstSequence=" << Snapshot.FirstBreadcrumbSequence << '\n'
				<< "BreadcrumbCount=" << Snapshot.BreadcrumbCount << '\n'
				<< "ActiveLogPath=" << Snapshot.ActiveLogPath.data() << '\n'
				<< "LastAcceptedLogSequence=" << Snapshot.LastAcceptedLogSequence << '\n'
				<< "LastProcessedLogSequence=" << Snapshot.LastProcessedLogSequence << '\n'
				<< "LastDurableLogSequence=" << Snapshot.LastDurableLogSequence << '\n'
				<< "DumpPath=SystemDiagnosticReports\n"
				<< "DumpResult=" << (GCrashHandler.bDisableDump.load(std::memory_order_relaxed)
					? "Disabled" : "SystemManaged") << '\n'
				<< "DirectoryError=" << DirectoryError << '\n'
				<< "ContextError=0\n"
				<< "DumpError=0\n";
			for (uint32 Index = 0; Index < Snapshot.BreadcrumbCount; ++Index)
			{
				const FProcessCrashBreadcrumb& Record = Snapshot.Breadcrumbs[Index];
				Context << "Breadcrumb=" << Record.Sequence << ','
					<< ProcessCrashBreadcrumbName(Record.Event) << ','
					<< Record.ThreadId << ',' << Record.MonotonicMicroseconds << ','
					<< Record.Argument0 << ',' << Record.Argument1 << '\n';
			}
			Context.flush();
			Context.close();
			std::ofstream Marker(CrashDirectory / "Complete.marker", std::ios::binary);
			Marker << "CrashContextVersion=1\n";
			Marker.flush();
		}

		auto ReporterMain() -> void
		{
			for (;;)
			{
				FCrashEvent Event;
				if (!ReadAll(GCrashHandler.Events[0], &Event, sizeof(Event))) return;
				if (Event.Signal == 0) return;
				WriteCrashArtifacts(Event);
				constexpr char Acknowledged = 1;
				if (!WriteAll(GCrashHandler.Acknowledgements[1], &Acknowledged, 1)) return;
			}
		}

		auto CrashSignalHandler(int Signal, siginfo_t* Info, void*) -> void
		{
			if (GCrashWriter != 0) _exit(128 + Signal);
			GCrashWriter = 1;
			const FCrashEvent Event{
				.Signal = Signal,
				.Code = Info ? Info->si_code : 0,
				.Address = reinterpret_cast<uintptr_t>(Info ? Info->si_addr : nullptr)};
			if (GEventWrite >= 0 && WriteAll(GEventWrite, &Event, sizeof(Event)))
			{
				char Acknowledged = 0;
				(void)ReadAll(GAcknowledgementRead, &Acknowledged, 1);
			}
			struct sigaction DefaultAction{};
			DefaultAction.sa_handler = SIG_DFL;
			sigemptyset(&DefaultAction.sa_mask);
			(void)sigaction(Signal, &DefaultAction, nullptr);
			(void)kill(getpid(), Signal);
		}

		auto TerminateHandler() -> void
		{
			raise(SIGABRT);
			std::abort();
		}

		auto ClosePipe(int (&Pipe)[2]) -> void
		{
			for (int& File : Pipe)
			{
				if (File >= 0) close(File);
				File = -1;
			}
		}
	}

	auto InstallMacOSProcessCrashHandler() -> bool
	{
		std::scoped_lock Lock(GCrashHandler.Mutex);
		if (GCrashHandler.bInstalled) return true;
		if (pipe(GCrashHandler.Events) != 0) return false;
		if (pipe(GCrashHandler.Acknowledgements) != 0)
		{
			ClosePipe(GCrashHandler.Events);
			return false;
		}
		GCrashHandler.ExecutablePath = FPlatformProcess::ExecutablePath();
		if (GCrashHandler.ExecutablePath.empty())
		{
			ClosePipe(GCrashHandler.Events);
			ClosePipe(GCrashHandler.Acknowledgements);
			return false;
		}
		if (!PublishRootValue(
			std::filesystem::path(GCrashHandler.ExecutablePath).parent_path() / "Crashes"))
		{
			ClosePipe(GCrashHandler.Events);
			ClosePipe(GCrashHandler.Acknowledgements);
			return false;
		}
		GCrashWriter = 0;
		GEventWrite = GCrashHandler.Events[1];
		GAcknowledgementRead = GCrashHandler.Acknowledgements[0];
		GCrashHandler.Reporter = std::jthread(ReporterMain);

		struct sigaction Action{};
		Action.sa_sigaction = CrashSignalHandler;
		sigemptyset(&Action.sa_mask);
		for (const int Signal : CrashSignals) sigaddset(&Action.sa_mask, Signal);
		Action.sa_flags = SA_SIGINFO | SA_RESTART;
		for (size_t Index = 0; Index < CrashSignals.size(); ++Index)
		{
			if (sigaction(CrashSignals[Index], &Action, &GCrashHandler.PreviousActions[Index]) == 0)
				continue;
			for (size_t Restore = 0; Restore < Index; ++Restore)
				sigaction(CrashSignals[Restore], &GCrashHandler.PreviousActions[Restore], nullptr);
			const FCrashEvent Stop{};
			(void)WriteAll(GCrashHandler.Events[1], &Stop, sizeof(Stop));
			GCrashHandler.Reporter.join();
			ClosePipe(GCrashHandler.Events);
			ClosePipe(GCrashHandler.Acknowledgements);
			GEventWrite = -1;
			GAcknowledgementRead = -1;
			return false;
		}
		GCrashHandler.PreviousTerminate = std::set_terminate(TerminateHandler);
		GCrashHandler.bInstalled = true;
		return true;
	}

	auto PublishMacOSProcessCrashRoot(
		std::string_view SavedDirectory,
		bool bExplicitDiagnosticOverride) -> bool
	{
		std::scoped_lock Lock(GCrashHandler.Mutex);
		if (!GCrashHandler.bInstalled) return false;
		if (!bExplicitDiagnosticOverride
			&& GCrashHandler.bExplicitRootOverride.load(std::memory_order_acquire))
			return true;
		const std::filesystem::path Saved = std::filesystem::path(SavedDirectory).lexically_normal();
		if (!Saved.is_absolute()) return false;
		if (!PublishRootValue(Saved / "Crashes")) return false;
		if (bExplicitDiagnosticOverride)
			GCrashHandler.bExplicitRootOverride.store(true, std::memory_order_release);
		return true;
	}

	auto UninstallMacOSProcessCrashHandler() -> void
	{
		std::scoped_lock Lock(GCrashHandler.Mutex);
		if (!GCrashHandler.bInstalled) return;
		for (size_t Index = 0; Index < CrashSignals.size(); ++Index)
			sigaction(CrashSignals[Index], &GCrashHandler.PreviousActions[Index], nullptr);
		std::set_terminate(GCrashHandler.PreviousTerminate);
		const FCrashEvent Stop{};
		(void)WriteAll(GCrashHandler.Events[1], &Stop, sizeof(Stop));
		GCrashHandler.Reporter.join();
		GEventWrite = -1;
		GAcknowledgementRead = -1;
		ClosePipe(GCrashHandler.Events);
		ClosePipe(GCrashHandler.Acknowledgements);
		GCrashHandler.bExplicitRootOverride.store(false, std::memory_order_release);
		GCrashHandler.bInstalled = false;
	}

	auto ConfigureMacOSProcessCrashTestOptions(
		bool bDisableDump,
		bool bForceCollision,
		bool bFaultCrashWriter) -> void
	{
#if DURIN_BUILD_SHIPPING
		(void)bDisableDump;
		(void)bForceCollision;
		(void)bFaultCrashWriter;
#else
		GCrashHandler.bDisableDump.store(bDisableDump, std::memory_order_relaxed);
		GCrashHandler.bForceCollision.store(bForceCollision, std::memory_order_relaxed);
		GCrashHandler.bFaultCrashWriter.store(bFaultCrashWriter, std::memory_order_relaxed);
#endif
	}

	auto RunMacOSProcessCrashFixture(std::string_view Fixture) -> bool
	{
#if DURIN_BUILD_SHIPPING
		(void)Fixture;
		return false;
#else
		if (Fixture == "access-read")
		{
			const volatile uint32 Value = *reinterpret_cast<volatile uint32*>(static_cast<uintptr_t>(1));
			(void)Value;
			return true;
		}
		if (Fixture == "access-write")
		{
			*reinterpret_cast<volatile uint32*>(static_cast<uintptr_t>(1)) = 7;
			return true;
		}
		if (Fixture == "access-execute")
		{
			reinterpret_cast<void(*)()>(static_cast<uintptr_t>(1))();
			return true;
		}
		if (Fixture == "terminate") std::terminate();
		if (Fixture == "worker-access-read")
		{
			std::jthread Worker([] {
				const volatile uint32 Value = *reinterpret_cast<volatile uint32*>(static_cast<uintptr_t>(1));
				(void)Value;
			});
			Worker.join();
			return true;
		}
		if (Fixture == "simultaneous-access")
		{
			std::atomic<uint32> Ready{0};
			const auto Fault = [&Ready] {
				Ready.fetch_add(1, std::memory_order_release);
				while (Ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
				const volatile uint32 Value = *reinterpret_cast<volatile uint32*>(static_cast<uintptr_t>(1));
				(void)Value;
			};
			std::jthread First(Fault);
			std::jthread Second(Fault);
			First.join();
			Second.join();
			return true;
		}
		return false;
#endif
	}
}
