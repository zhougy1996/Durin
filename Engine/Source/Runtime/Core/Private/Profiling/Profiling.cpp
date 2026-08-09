#include "Profiling/Profiling.h"
#include "Logging/LogMacros.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

namespace Durin::Profiling
{
	namespace
	{
		constexpr int64 UnrecordedStartupTime = -1;

		struct FStartupTimingState
		{
			FStartupTimingState()
			{
				for (std::atomic<int64>& Milestone : Milestones)
					Milestone.store(UnrecordedStartupTime, std::memory_order_relaxed);
			}

			std::array<std::atomic<int64>, static_cast<size_t>(EStartupMilestone::Count)> Milestones;
			std::atomic<int64> ProcessStartNanoseconds{0};
			std::atomic<bool> bHasProject{false};
			std::atomic<bool> bFirstPresentArmed{false};
			std::atomic<bool> bSummaryLogged{false};
		};

		FStartupTimingState GStartupTiming;
		std::mutex GProgramIdentityMutex;
		std::deque<std::string> GProgramIdentityStorage;

		auto StartupNowNanoseconds() noexcept -> int64
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		auto GetStartupNanoseconds(EStartupMilestone Milestone) noexcept -> int64
		{
			return GStartupTiming.Milestones[static_cast<size_t>(Milestone)].load(std::memory_order_acquire);
		}

		auto StartupDurationMilliseconds(EStartupMilestone Begin, EStartupMilestone End) noexcept -> double
		{
			const int64 BeginNanoseconds = GetStartupNanoseconds(Begin);
			const int64 EndNanoseconds = GetStartupNanoseconds(End);
			return BeginNanoseconds >= 0 && EndNanoseconds >= BeginNanoseconds
				? static_cast<double>(EndNanoseconds - BeginNanoseconds) / 1'000'000.0
				: -1.0;
		}
	}

	auto RecordStartupMilestone(EStartupMilestone Milestone) noexcept -> bool
	{
		const int64 NowNanoseconds = StartupNowNanoseconds();
		int64 ProcessStartNanoseconds = GStartupTiming.ProcessStartNanoseconds.load(std::memory_order_acquire);
		if (ProcessStartNanoseconds == 0)
		{
			int64 Expected = 0;
			GStartupTiming.ProcessStartNanoseconds.compare_exchange_strong(
				Expected, NowNanoseconds, std::memory_order_acq_rel);
			ProcessStartNanoseconds = Expected == 0 ? NowNanoseconds : Expected;
		}
		std::atomic<int64>& Slot = GStartupTiming.Milestones[static_cast<size_t>(Milestone)];
		int64 Expected = UnrecordedStartupTime;
		return Slot.compare_exchange_strong(
			Expected, NowNanoseconds - ProcessStartNanoseconds, std::memory_order_acq_rel);
	}

	auto SetStartupProjectMode(bool bHasProject) noexcept -> void
	{
		GStartupTiming.bHasProject.store(bHasProject, std::memory_order_release);
	}

	auto ArmEditorShellFirstPresent() noexcept -> void
	{
		GStartupTiming.bFirstPresentArmed.store(true, std::memory_order_release);
	}

	auto RecordEditorShellFirstPresent() noexcept -> bool
	{
		if (!GStartupTiming.bFirstPresentArmed.load(std::memory_order_acquire)) return false;
		const bool bRecorded = RecordStartupMilestone(EStartupMilestone::FirstPresent);
		if (bRecorded) TryLogStartupTimingSummary();
		return bRecorded;
	}

	auto GetStartupMilestoneMilliseconds(EStartupMilestone Milestone) noexcept -> double
	{
		const int64 Nanoseconds = GetStartupNanoseconds(Milestone);
		return Nanoseconds >= 0 ? static_cast<double>(Nanoseconds) / 1'000'000.0 : -1.0;
	}

	auto TryLogStartupTimingSummary() -> bool
	{
		constexpr std::array RequiredMilestones{
			EStartupMilestone::ProcessEntry,
			EStartupMilestone::PreInitComplete,
			EStartupMilestone::RHIReady,
			EStartupMilestone::DefaultMaterialReady,
			EStartupMilestone::RendererReady,
			EStartupMilestone::EditorShellComplete,
			EStartupMilestone::WorkspaceRegistrationComplete,
			EStartupMilestone::NativeViewportReady,
			EStartupMilestone::FirstPresent,
			EStartupMilestone::DefaultWorkspaceReady,
		};
		for (const EStartupMilestone Milestone : RequiredMilestones)
			if (GetStartupNanoseconds(Milestone) < 0) return false;

		bool bExpected = false;
		if (!GStartupTiming.bSummaryLogged.compare_exchange_strong(
			bExpected, true, std::memory_order_acq_rel)) return false;

		DURIN_INFO(
			"StartupTiming mode={} preinit_ms={:.3f} rhi_ready_ms={:.3f} default_material_ms={:.3f} renderer_ms={:.3f} shell_ms={:.3f} workspace_registration_ms={:.3f} registry_scan_ms={:.3f} default_document_ms={:.3f} default_document_asset_load_ms={:.3f} default_document_compatibility_ms={:.3f} default_document_activation_ms={:.3f} native_viewport_ready_ms={:.3f} first_present_ms={:.3f} default_workspace_ready_ms={:.3f}",
			GStartupTiming.bHasProject.load(std::memory_order_acquire) ? "project" : "project_browser",
			GetStartupMilestoneMilliseconds(EStartupMilestone::PreInitComplete),
			GetStartupMilestoneMilliseconds(EStartupMilestone::RHIReady),
			StartupDurationMilliseconds(EStartupMilestone::DefaultMaterialBegin, EStartupMilestone::DefaultMaterialReady),
			StartupDurationMilliseconds(EStartupMilestone::RendererBegin, EStartupMilestone::RendererReady),
			StartupDurationMilliseconds(EStartupMilestone::EditorShellBegin, EStartupMilestone::EditorShellComplete),
			StartupDurationMilliseconds(EStartupMilestone::WorkspaceRegistrationBegin, EStartupMilestone::WorkspaceRegistrationComplete),
			StartupDurationMilliseconds(EStartupMilestone::RegistryScanBegin, EStartupMilestone::RegistryScanComplete),
			StartupDurationMilliseconds(EStartupMilestone::DefaultDocumentBegin, EStartupMilestone::DefaultDocumentComplete),
			StartupDurationMilliseconds(EStartupMilestone::DefaultDocumentAssetLoadBegin, EStartupMilestone::DefaultDocumentAssetLoadComplete),
			StartupDurationMilliseconds(EStartupMilestone::DefaultDocumentCompatibilityBegin, EStartupMilestone::DefaultDocumentCompatibilityComplete),
			StartupDurationMilliseconds(EStartupMilestone::DefaultDocumentActivationBegin, EStartupMilestone::DefaultDocumentActivationComplete),
			GetStartupMilestoneMilliseconds(EStartupMilestone::NativeViewportReady),
			GetStartupMilestoneMilliseconds(EStartupMilestone::FirstPresent),
			GetStartupMilestoneMilliseconds(EStartupMilestone::DefaultWorkspaceReady));
		return true;
	}

	auto FormatProgramIdentity(
		std::string_view RuntimeVariant,
		std::string_view ProjectName,
		uint32 ProcessId
	) -> std::string
	{
		const std::string_view RuntimeLabel = RuntimeVariant.empty() ? std::string_view{"Durin"} : RuntimeVariant;
		const std::string_view ProjectLabel = ProjectName.empty() ? std::string_view{"No Project"} : ProjectName;
		return std::format("{} | {} | PID {}", RuntimeLabel, ProjectLabel, ProcessId);
	}

	auto SetProgramIdentity(
		std::string_view RuntimeVariant,
		std::string_view ProjectName,
		uint32 ProcessId
	) -> std::string_view
	{
		std::scoped_lock Lock(GProgramIdentityMutex);
		const std::string ProgramIdentity = FormatProgramIdentity(RuntimeVariant, ProjectName, ProcessId);
		if (GProgramIdentityStorage.empty() || GProgramIdentityStorage.back() != ProgramIdentity)
			GProgramIdentityStorage.emplace_back(ProgramIdentity);
		const std::string_view StoredIdentity = GProgramIdentityStorage.back();
#if DURIN_WITH_TRACY
		TracySetProgramName(StoredIdentity.data());
#endif
		return StoredIdentity;
	}

	auto GetProgramIdentity() -> std::string
	{
		std::scoped_lock Lock(GProgramIdentityMutex);
		return GProgramIdentityStorage.empty() ? std::string{} : GProgramIdentityStorage.back();
	}

	auto FormatPortOverrideDiagnostic(std::string_view Port) -> std::string
	{
		return std::format(
			"TRACY_PORT={} is explicitly set. This disables Tracy's automatic 8086-8105 port search; "
			"a second process inheriting the same fixed port cannot listen until the override changes or is removed.",
			Port.empty() ? std::string_view{"<empty>"} : Port
		);
	}
}
