#include "WindowsProcessCrashHandler.h"
#include "WindowsProcessCrashPolicy.h"

#include "Diagnostics/ProcessCrashContext.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Build.h"

#include <DbgHelp.h>

namespace Durin
{
	namespace
	{
		constexpr uint32 CrashContextVersion = 1;
		constexpr uint32 CrashDirectoryCollisionLimit = 16;
		constexpr uint32 CrashRetentionCount = 16;
		constexpr uint32 CrashRetentionDays = 30;
		constexpr uint32 PartialCrashRetentionDays = 7;
		constexpr DWORD DurinTerminateStatus = 0xE0000001u;
		constexpr MINIDUMP_TYPE DurinMiniDumpType = static_cast<MINIDUMP_TYPE>(
			MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

		struct FFixedTextBuilder
		{
			std::array<char, 65536> Buffer{};
			size_t Size = 0;

			auto Append(std::string_view Text) -> void
			{
				const size_t Count = std::min(Text.size(), Buffer.size() - Size);
				std::memcpy(Buffer.data() + Size, Text.data(), Count);
				Size += Count;
			}

			auto AppendUnsigned(uint64 Value, uint32 Base = 10) -> void
			{
				std::array<char, 32> Digits{};
				const auto Result = std::to_chars(Digits.data(), Digits.data() + Digits.size(), Value, static_cast<int>(Base));
				if (Result.ec == std::errc{}) Append({Digits.data(), static_cast<size_t>(Result.ptr - Digits.data())});
			}

			auto Key(std::string_view Name, std::string_view Value) -> void
			{
				Append(Name); Append("="); Append(Value.empty() ? "Unavailable" : Value); Append("\r\n");
			}

			auto KeyUnsigned(std::string_view Name, uint64 Value) -> void
			{
				Append(Name); Append("="); AppendUnsigned(Value); Append("\r\n");
			}

			auto KeyHex(std::string_view Name, uint64 Value) -> void
			{
				Append(Name); Append("=0x"); AppendUnsigned(Value, 16); Append("\r\n");
			}
		};

		struct FCrashHandlerState
		{
			std::atomic<uint32> Writer{0};
			std::atomic<uint32> RootIndex{0};
			std::atomic<bool> bDisableDump{false};
			std::atomic<bool> bForceCollision{false};
			std::atomic<bool> bExplicitRootOverride{false};
			std::atomic<bool> bFaultCrashWriter{false};
			std::array<wchar_t, 32768> Roots[2]{};
			std::array<wchar_t, 32768> ExecutablePath{};
			std::array<char, ProcessCrashPathCapacity> ExecutablePathUtf8{};
			LPTOP_LEVEL_EXCEPTION_FILTER PreviousFilter = nullptr;
			std::terminate_handler PreviousTerminate = nullptr;
			bool bInstalled = false;
		};

		FCrashHandlerState GCrashHandler;

		auto CopyWide(std::wstring_view Source, wchar_t* Destination, size_t Capacity) -> bool
		{
			if (Source.empty() || Source.size() >= Capacity) return false;
			std::memcpy(Destination, Source.data(), Source.size() * sizeof(wchar_t));
			Destination[Source.size()] = L'\0';
			return true;
		}

		auto WideToUtf8(std::wstring_view Source, char* Destination, size_t Capacity) -> bool
		{
			if (Capacity == 0) return false;
			const int Count = WideCharToMultiByte(CP_UTF8, 0, Source.data(), static_cast<int>(Source.size()),
				Destination, static_cast<int>(Capacity - 1), nullptr, nullptr);
			if (Count <= 0) { Destination[0] = '\0'; return false; }
			Destination[Count] = '\0';
			return true;
		}

		auto AppendWidePath(std::array<wchar_t, 32768>& Path, std::wstring_view Component) -> bool
		{
			size_t Size = std::wcslen(Path.data());
			if (Size > 0 && Path[Size - 1] != L'\\' && Path[Size - 1] != L'/')
			{
				if (Size + 1 >= Path.size()) return false;
				Path[Size++] = L'\\';
			}
			if (Size + Component.size() >= Path.size()) return false;
			std::memcpy(Path.data() + Size, Component.data(), Component.size() * sizeof(wchar_t));
			Path[Size + Component.size()] = L'\0';
			return true;
		}

		auto MakeTimestamp(SYSTEMTIME Time, char* Destination, size_t Capacity) -> bool
		{
			const int Count = std::snprintf(Destination, Capacity, "%04u%02u%02uT%02u%02u%02u.%03uZ",
				Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond, Time.wMilliseconds);
			return Count > 0 && static_cast<size_t>(Count) < Capacity;
		}

		auto WriteBytes(HANDLE File, const char* Data, size_t Size, DWORD& OutError) -> bool
		{
			while (Size > 0)
			{
				const DWORD Chunk = static_cast<DWORD>(std::min<size_t>(Size, MAXDWORD));
				DWORD Written = 0;
				if (!WriteFile(File, Data, Chunk, &Written, nullptr) || Written == 0)
				{
					OutError = GetLastError();
					return false;
				}
				Data += Written;
				Size -= Written;
			}
			return true;
		}

		auto WriteFallback(std::string_view Text) -> void
		{
			DWORD Written = 0;
			const HANDLE Error = GetStdHandle(STD_ERROR_HANDLE);
			if (Error != nullptr && Error != INVALID_HANDLE_VALUE)
				WriteFile(Error, Text.data(), static_cast<DWORD>(Text.size()), &Written, nullptr);
		}

		auto CaptureCrashArtifacts(DWORD ReasonCode, EXCEPTION_POINTERS* ExceptionPointers) -> void
		{
			uint32 ExpectedWriter = 0;
			if (!GCrashHandler.Writer.compare_exchange_strong(ExpectedWriter, 1, std::memory_order_acq_rel))
			{
				WriteFallback("Durin crash capture skipped on a second faulting thread.\r\n");
				TerminateProcess(GetCurrentProcess(), ReasonCode);
			}
			if (GCrashHandler.bFaultCrashWriter.load(std::memory_order_relaxed))
			{
				const volatile uint32 Value = *reinterpret_cast<volatile uint32*>(static_cast<uintptr_t>(1));
				(void)Value;
			}

			const FProcessCrashContextSnapshot Snapshot = ReadProcessCrashContext();
			const uint32 ProcessId = GetCurrentProcessId();
			const uint32 ThreadId = GetCurrentThreadId();
			SYSTEMTIME Time{};
			GetSystemTime(&Time);
			std::array<char, 32> Timestamp{};
			MakeTimestamp(Time, Timestamp.data(), Timestamp.size());
			std::array<char, 160> CrashId{};
			const int CrashIdLength = std::snprintf(CrashId.data(), CrashId.size(), "%s-%s-%u",
				Snapshot.RuntimeVariant[0] ? Snapshot.RuntimeVariant.data() : DURIN_RUNTIME_VARIANT,
				Timestamp.data(), ProcessId);
			if (CrashIdLength <= 0 || static_cast<size_t>(CrashIdLength) >= CrashId.size())
			{
				WriteFallback("Durin crash capture could not create a crash id.\r\n");
				TerminateProcess(GetCurrentProcess(), ReasonCode);
			}

			const uint32 RootIndex = GCrashHandler.RootIndex.load(std::memory_order_acquire);
			std::array<wchar_t, 32768> CrashDirectory = GCrashHandler.Roots[RootIndex];
			DWORD DirectoryError = 0;
			if (!CreateDirectoryW(CrashDirectory.data(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
				DirectoryError = GetLastError();

			std::array<wchar_t, 160> WideCrashId{};
			MultiByteToWideChar(CP_UTF8, 0, CrashId.data(), -1, WideCrashId.data(), static_cast<int>(WideCrashId.size()));
			bool bDirectoryReady = false;
			if (DirectoryError == 0)
			{
				if (GCrashHandler.bForceCollision.load(std::memory_order_relaxed))
				{
					std::array<wchar_t, 32768> Occupied = GCrashHandler.Roots[RootIndex];
					if (AppendWidePath(Occupied, WideCrashId.data())) CreateDirectoryW(Occupied.data(), nullptr);
				}
				for (uint32 Collision = 0; Collision < CrashDirectoryCollisionLimit; ++Collision)
				{
					CrashDirectory = GCrashHandler.Roots[RootIndex];
					std::array<wchar_t, 192> Component{};
					if (Collision == 0) std::wcsncpy(Component.data(), WideCrashId.data(), Component.size() - 1);
					else std::swprintf(Component.data(), Component.size(), L"%s-%u", WideCrashId.data(), Collision);
					if (!AppendWidePath(CrashDirectory, Component.data())) break;
					if (CreateDirectoryW(CrashDirectory.data(), nullptr))
					{
						bDirectoryReady = true;
						if (Collision != 0)
						{
							std::array<char, 192> CollisionId{};
							WideToUtf8(Component.data(), CollisionId.data(), CollisionId.size());
							std::strncpy(CrashId.data(), CollisionId.data(), CrashId.size() - 1);
						}
						break;
					}
					if (GetLastError() != ERROR_ALREADY_EXISTS) { DirectoryError = GetLastError(); break; }
				}
			}

			std::array<wchar_t, 32768> ContextPath = CrashDirectory;
			std::array<wchar_t, 32768> DumpPath = CrashDirectory;
			std::array<wchar_t, 32768> MarkerPath = CrashDirectory;
			std::array<wchar_t, 256> ContextName{};
			std::array<wchar_t, 256> DumpName{};
			std::swprintf(ContextName.data(), ContextName.size(), L"%S-CrashContext-v1.txt", CrashId.data());
			std::swprintf(DumpName.data(), DumpName.size(), L"%S.dmp", CrashId.data());
			AppendWidePath(ContextPath, ContextName.data());
			AppendWidePath(DumpPath, DumpName.data());
			AppendWidePath(MarkerPath, L"Complete.marker");

			std::array<char, ProcessCrashPathCapacity> ContextPathUtf8{};
			std::array<char, ProcessCrashPathCapacity> DumpPathUtf8{};
			WideToUtf8(ContextPath.data(), ContextPathUtf8.data(), ContextPathUtf8.size());
			WideToUtf8(DumpPath.data(), DumpPathUtf8.data(), DumpPathUtf8.size());

			HANDLE ContextFile = INVALID_HANDLE_VALUE;
			DWORD ContextError = 0;
			if (bDirectoryReady)
			{
				ContextFile = CreateFileW(ContextPath.data(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
				if (ContextFile == INVALID_HANDLE_VALUE) ContextError = GetLastError();
			}

			FFixedTextBuilder Context;
			Context.KeyUnsigned("FormatVersion", CrashContextVersion);
			Context.Key("CompletionState", "PendingMarker");
			Context.Key("CrashId", CrashId.data());
			Context.KeyHex("ReasonCode", ReasonCode);
			Context.Key("ReasonKind", ExceptionPointers ? "UnhandledSEH" : "StdTerminate");
			Context.KeyHex("ExceptionAddress", ExceptionPointers && ExceptionPointers->ExceptionRecord
				? reinterpret_cast<uint64>(ExceptionPointers->ExceptionRecord->ExceptionAddress) : 0);
			Context.KeyUnsigned("ProcessId", ProcessId);
			Context.KeyUnsigned("FaultingThreadId", ThreadId);
			Context.Key("RuntimeVariant", Snapshot.RuntimeVariant.data());
			Context.Key("BuildConfiguration", Snapshot.BuildConfiguration.data());
			Context.Key("BuildIdentity", Snapshot.BuildIdentity.data());
			Context.Key("ExecutableImagePath", GCrashHandler.ExecutablePathUtf8.data());
			Context.Key("UtcTimestamp", Timestamp.data());
			const uint64 NowMicros = static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
			Context.KeyUnsigned("ProcessUptimeMicroseconds", NowMicros >= Snapshot.ProcessStartMonotonicMicroseconds
				? NowMicros - Snapshot.ProcessStartMonotonicMicroseconds : 0);
			Context.Key("ProcessPhase", ProcessCrashPhaseName(Snapshot.Phase));
			Context.KeyUnsigned("BreadcrumbWriteSequence", Snapshot.BreadcrumbWriteSequence);
			Context.KeyUnsigned("BreadcrumbFirstSequence", Snapshot.FirstBreadcrumbSequence);
			Context.KeyUnsigned("BreadcrumbCount", Snapshot.BreadcrumbCount);
			Context.Key("ActiveLogPath", Snapshot.ActiveLogPath.data());
			Context.KeyUnsigned("LastAcceptedLogSequence", Snapshot.LastAcceptedLogSequence);
			Context.KeyUnsigned("LastProcessedLogSequence", Snapshot.LastProcessedLogSequence);
			Context.KeyUnsigned("LastDurableLogSequence", Snapshot.LastDurableLogSequence);
			Context.Key("DumpPath", DumpPathUtf8.data());
			if (ExceptionPointers && ExceptionPointers->ExceptionRecord
				&& ReasonCode == EXCEPTION_ACCESS_VIOLATION
				&& ExceptionPointers->ExceptionRecord->NumberParameters >= 2)
			{
				Context.Key("AccessViolationOperation", WindowsAccessViolationOperationName(ExceptionPointers->ExceptionRecord->ExceptionInformation[0]));
				Context.KeyHex("AccessViolationAddress", ExceptionPointers->ExceptionRecord->ExceptionInformation[1]);
			}
			else
			{
				Context.Key("AccessViolationOperation", "Unavailable");
				Context.Key("AccessViolationAddress", "Unavailable");
			}
			for (uint32 Index = 0; Index < Snapshot.BreadcrumbCount; ++Index)
			{
				const FProcessCrashBreadcrumb& Record = Snapshot.Breadcrumbs[Index];
				Context.Append("Breadcrumb=");
				Context.AppendUnsigned(Record.Sequence); Context.Append(",");
				Context.Append(ProcessCrashBreadcrumbName(Record.Event)); Context.Append(",");
				Context.AppendUnsigned(Record.ThreadId); Context.Append(",");
				Context.AppendUnsigned(Record.MonotonicMicroseconds); Context.Append(",");
				Context.AppendUnsigned(Record.Argument0); Context.Append(",");
				Context.AppendUnsigned(Record.Argument1); Context.Append("\r\n");
			}
			if (ContextFile != INVALID_HANDLE_VALUE) WriteBytes(ContextFile, Context.Buffer.data(), Context.Size, ContextError);

			DWORD DumpError = 0;
			bool bDumpWritten = false;
			if (bDirectoryReady && !GCrashHandler.bDisableDump.load(std::memory_order_relaxed))
			{
				const HANDLE DumpFile = CreateFileW(DumpPath.data(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
				if (DumpFile != INVALID_HANDLE_VALUE)
				{
					MINIDUMP_EXCEPTION_INFORMATION ExceptionInfo{
						.ThreadId = ThreadId,
						.ExceptionPointers = ExceptionPointers,
						.ClientPointers = FALSE};
					bDumpWritten = MiniDumpWriteDump(GetCurrentProcess(), ProcessId, DumpFile, DurinMiniDumpType,
						ExceptionPointers ? &ExceptionInfo : nullptr, nullptr, nullptr) != FALSE;
					if (!bDumpWritten) DumpError = GetLastError();
					FlushFileBuffers(DumpFile);
					CloseHandle(DumpFile);
				}
				else DumpError = GetLastError();
			}
			else if (GCrashHandler.bDisableDump.load(std::memory_order_relaxed)) DumpError = ERROR_MOD_NOT_FOUND;

			FFixedTextBuilder Results;
			Results.Key("DumpResult", bDumpWritten ? "Written" : "Failed");
			Results.KeyUnsigned("DirectoryError", DirectoryError);
			Results.KeyUnsigned("ContextError", ContextError);
			Results.KeyUnsigned("DumpError", DumpError);
			if (ContextFile != INVALID_HANDLE_VALUE)
			{
				WriteBytes(ContextFile, Results.Buffer.data(), Results.Size, ContextError);
				FlushFileBuffers(ContextFile);
				CloseHandle(ContextFile);
			}

			if (bDirectoryReady)
			{
				const HANDLE Marker = CreateFileW(MarkerPath.data(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
				if (Marker != INVALID_HANDLE_VALUE)
				{
					DWORD MarkerError = 0;
					constexpr std::string_view MarkerText = "CrashContextVersion=1\r\n";
					WriteBytes(Marker, MarkerText.data(), MarkerText.size(), MarkerError);
					FlushFileBuffers(Marker);
					CloseHandle(Marker);
				}
			}
			WriteFallback("Durin captured native crash artifacts.\r\n");
		}

		auto WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* ExceptionPointers) -> LONG
		{
			const DWORD Code = ExceptionPointers && ExceptionPointers->ExceptionRecord
				? ExceptionPointers->ExceptionRecord->ExceptionCode : EXCEPTION_NONCONTINUABLE_EXCEPTION;
			CaptureCrashArtifacts(Code, ExceptionPointers);
			TerminateProcess(GetCurrentProcess(), Code);
			return EXCEPTION_EXECUTE_HANDLER;
		}

		auto TerminateHandler() -> void
		{
			CaptureCrashArtifacts(DurinTerminateStatus, nullptr);
			TerminateProcess(GetCurrentProcess(), DurinTerminateStatus);
		}

		__declspec(noinline) auto OverflowStack(uint64 Depth) -> uint64
		{
			volatile uint8 StackUse[4096]{};
			StackUse[Depth & 4095] = static_cast<uint8>(Depth);
			return OverflowStack(Depth + 1) + StackUse[(Depth + 1) & 4095];
		}
	}

	auto InstallWindowsProcessCrashHandler() -> bool
	{
		if (GCrashHandler.bInstalled) return true;
		const DWORD Length = GetModuleFileNameW(nullptr, GCrashHandler.ExecutablePath.data(),
			static_cast<DWORD>(GCrashHandler.ExecutablePath.size()));
		if (Length == 0 || Length >= GCrashHandler.ExecutablePath.size()) return false;
		WideToUtf8({GCrashHandler.ExecutablePath.data(), Length}, GCrashHandler.ExecutablePathUtf8.data(),
			GCrashHandler.ExecutablePathUtf8.size());
		std::array<wchar_t, 32768> FallbackRoot = GCrashHandler.ExecutablePath;
		wchar_t* LastSlash = std::wcsrchr(FallbackRoot.data(), L'\\');
		if (LastSlash == nullptr) return false;
		*(LastSlash + 1) = L'\0';
		if (!AppendWidePath(FallbackRoot, L"Crashes")) return false;
		GCrashHandler.Roots[0] = FallbackRoot;
		ULONG StackGuarantee = 64 * 1024;
		SetThreadStackGuarantee(&StackGuarantee);
		const UINT PreviousErrorMode = SetErrorMode(0);
		SetErrorMode(PreviousErrorMode | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
		GCrashHandler.PreviousFilter = SetUnhandledExceptionFilter(UnhandledExceptionFilter);
		GCrashHandler.PreviousTerminate = std::set_terminate(TerminateHandler);
		GCrashHandler.bInstalled = true;
		return true;
	}

	auto PublishWindowsProcessCrashRoot(std::string_view SavedDirectory, bool bExplicitDiagnosticOverride) -> bool
	{
		if (!GCrashHandler.bInstalled) return false;
		if (!bExplicitDiagnosticOverride && GCrashHandler.bExplicitRootOverride.load(std::memory_order_acquire)) return true;
		std::filesystem::path Root = std::filesystem::path(SavedDirectory) / "Crashes";
		Root = Root.lexically_normal();
		if (!Root.is_absolute()) return false;
		if (!IsValidWindowsProcessCrashSavedDirectory(std::filesystem::path(SavedDirectory))) return false;
		ApplyWindowsProcessCrashRetention(Root, CrashRetentionCount, CrashRetentionDays, PartialCrashRetentionDays);
		const std::wstring WideRoot = Root.wstring();
		const uint32 NextIndex = 1 - GCrashHandler.RootIndex.load(std::memory_order_relaxed);
		if (!CopyWide(WideRoot, GCrashHandler.Roots[NextIndex].data(), GCrashHandler.Roots[NextIndex].size())) return false;
		GCrashHandler.RootIndex.store(NextIndex, std::memory_order_release);
		if (bExplicitDiagnosticOverride) GCrashHandler.bExplicitRootOverride.store(true, std::memory_order_release);
		return true;
	}

	auto UninstallWindowsProcessCrashHandler() -> void
	{
		if (!GCrashHandler.bInstalled) return;
		SetUnhandledExceptionFilter(GCrashHandler.PreviousFilter);
		std::set_terminate(GCrashHandler.PreviousTerminate);
		GCrashHandler.bInstalled = false;
	}

	auto ConfigureWindowsProcessCrashTestOptions(bool bDisableDump, bool bForceCollision, bool bFaultCrashWriter) -> void
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

	auto RunWindowsProcessCrashFixture(std::string_view Fixture) -> bool
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
		if (Fixture == "terminate")
		{
			std::terminate();
		}
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
				while (Ready.load(std::memory_order_acquire) != 2) YieldProcessor();
				const volatile uint32 Value = *reinterpret_cast<volatile uint32*>(static_cast<uintptr_t>(1));
				(void)Value;
			};
			std::jthread First(Fault);
			std::jthread Second(Fault);
			First.join();
			Second.join();
			return true;
		}
		if (Fixture == "stack-overflow")
		{
			const volatile uint64 Result = OverflowStack(1);
			(void)Result;
			return true;
		}
		return false;
#endif
	}
}
