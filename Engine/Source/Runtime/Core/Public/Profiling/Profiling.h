#pragma once

#include "CoreAPI.h"

#include <array>
#include <string>
#include <string_view>

#ifndef DURIN_WITH_TRACY
	#define DURIN_WITH_TRACY 0
#endif

namespace Durin::Profiling
{
	enum class EStartupMilestone : uint8
	{
		ProcessEntry,
		PreInitComplete,
		RHIReady,
		DefaultMaterialBegin,
		DefaultMaterialReady,
		RendererBegin,
		RendererReady,
		EditorShellBegin,
		EditorShellComplete,
		WorkspaceRegistrationBegin,
		WorkspaceRegistrationComplete,
		RegistryScanBegin,
		RegistryScanComplete,
		DefaultDocumentBegin,
		DefaultDocumentComplete,
		NativeViewportReady,
		FirstPresent,
		DefaultWorkspaceReady,
		Count,
	};

	CORE_API auto RecordStartupMilestone(EStartupMilestone Milestone) noexcept -> bool;
	CORE_API auto SetStartupProjectMode(bool bHasProject) noexcept -> void;
	CORE_API auto ArmEditorShellFirstPresent() noexcept -> void;
	CORE_API auto RecordEditorShellFirstPresent() noexcept -> bool;
	CORE_API auto TryLogStartupTimingSummary() -> bool;
	CORE_API auto GetStartupMilestoneMilliseconds(EStartupMilestone Milestone) noexcept -> double;

	CORE_API auto FormatProgramIdentity(
		std::string_view RuntimeVariant,
		std::string_view ProjectName,
		uint32 ProcessId
	) -> std::string;
	CORE_API auto SetProgramIdentity(
		std::string_view RuntimeVariant,
		std::string_view ProjectName,
		uint32 ProcessId
	) -> std::string_view;
	CORE_API auto GetProgramIdentity() -> std::string;
	CORE_API auto FormatPortOverrideDiagnostic(std::string_view Port) -> std::string;
}

#if DURIN_WITH_TRACY
	#include <algorithm>
	#include <array>
	#include <cstdio>
	#include <mutex>

	#include <tracy/Tracy.hpp>

	namespace Durin::Profiling
	{
		namespace Private
		{
			constexpr size_t TaskProfilerSlotCount = 1'024;
			constexpr size_t TaskProfilerLabelBytes = 64;
			constexpr size_t TaskProfilerPlotNameBytes = 160;
			constexpr size_t TaskProfilerMessageBytes = 320;

			enum class ETaskProfilerPlot : uint8
			{
				QueueDepth,
				Running,
				Rejected,
				CallableBytes,
				PayloadBytes,
				ResultBytes,
				RetainedResultBytes,
				Count,
			};

			struct FTaskProfilerSlot
			{
				std::array<char, TaskProfilerLabelBytes> Owner{};
				std::array<char, TaskProfilerLabelBytes> Category{};
				std::array<std::array<char, TaskProfilerPlotNameBytes>, static_cast<size_t>(ETaskProfilerPlot::Count)> PlotNames{};
				uint16 OwnerId = 1;
				bool bRegistered = false;
			};

			struct FTaskProfilerMessage
			{
				std::array<char, TaskProfilerMessageBytes> Bytes{};
				size_t Length = 0;
			};

			inline std::mutex GTaskProfilerRegistrationMutex;
			inline std::array<FTaskProfilerSlot, TaskProfilerSlotCount> GTaskProfilerSlots{};

			inline auto CopyTaskProfilerLabel(std::array<char, TaskProfilerLabelBytes>& Out, std::string_view Label) noexcept -> void
			{
				const size_t Length = std::min(Label.size(), Out.size() - 1);
				std::copy_n(Label.data(), Length, Out.data());
				Out[Length] = '\0';
			}

			inline auto GetTaskProfilerSlot(uint16 OwnerId, uint16 CategoryId) noexcept -> const FTaskProfilerSlot&
			{
				if (CategoryId < GTaskProfilerSlots.size())
				{
					const FTaskProfilerSlot& Slot = GTaskProfilerSlots[CategoryId];
					if (Slot.bRegistered && Slot.OwnerId == OwnerId) return Slot;
				}
				return GTaskProfilerSlots[1];
			}

			inline auto FormatTaskProfilerMessage(
				const char* Phase,
				uint64 TaskId,
				uint64 ScopeId,
				uint16 OwnerId,
				uint16 CategoryId,
				uint8 Target,
				uint8 TerminalReason,
				const char* DebugName = nullptr) noexcept -> FTaskProfilerMessage
			{
				const FTaskProfilerSlot& Slot = GetTaskProfilerSlot(OwnerId, CategoryId);
				FTaskProfilerMessage Message;
				const int Length = DebugName
					? std::snprintf(Message.Bytes.data(), Message.Bytes.size(),
						"task phase=%s id=%llu scope=%llu owner=%s category=%s target=%u reason=%u name=%s",
						Phase, static_cast<unsigned long long>(TaskId), static_cast<unsigned long long>(ScopeId), Slot.Owner.data(), Slot.Category.data(),
						static_cast<unsigned>(Target), static_cast<unsigned>(TerminalReason), DebugName)
					: std::snprintf(Message.Bytes.data(), Message.Bytes.size(),
						"task phase=%s id=%llu scope=%llu owner=%s category=%s target=%u reason=%u",
						Phase, static_cast<unsigned long long>(TaskId), static_cast<unsigned long long>(ScopeId), Slot.Owner.data(), Slot.Category.data(),
						static_cast<unsigned>(Target), static_cast<unsigned>(TerminalReason));
				Message.Length = Length <= 0 ? 0 : std::min(static_cast<size_t>(Length), Message.Bytes.size() - 1);
				return Message;
			}
		}

		inline auto RegisterTaskProfilerAttribution(
			uint16 OwnerId,
			uint16 CategoryId,
			std::string_view Owner,
			std::string_view Category) noexcept -> void
		{
			if (CategoryId >= Private::GTaskProfilerSlots.size()) return;
			std::lock_guard Lock(Private::GTaskProfilerRegistrationMutex);
			Private::FTaskProfilerSlot& Slot = Private::GTaskProfilerSlots[CategoryId];
			if (Slot.bRegistered) return;
			Slot.OwnerId = OwnerId;
			Private::CopyTaskProfilerLabel(Slot.Owner, Owner);
			Private::CopyTaskProfilerLabel(Slot.Category, Category);
			constexpr std::array<const char*, static_cast<size_t>(Private::ETaskProfilerPlot::Count)> Suffixes{
				"QueueDepth", "Running", "Rejected", "CallableBytes", "PayloadBytes", "ResultBytes", "RetainedResultBytes"
			};
			for (size_t Index = 0; Index < Suffixes.size(); ++Index)
			{
				std::snprintf(Slot.PlotNames[Index].data(), Slot.PlotNames[Index].size(),
					"Tasks.%s.%s.%s", Slot.Owner.data(), Slot.Category.data(), Suffixes[Index]);
			}
			Slot.bRegistered = true;
		}

		inline auto TaskEnqueued(uint64 TaskId, uint64 ScopeId, uint16 OwnerId, uint16 CategoryId, uint8 Target) noexcept -> void
		{
			const Private::FTaskProfilerMessage Message = Private::FormatTaskProfilerMessage(
				"enqueue", TaskId, ScopeId, OwnerId, CategoryId, Target, 0);
			TracyMessage(Message.Bytes.data(), Message.Length);
		}

		inline auto TaskExecution(uint64, uint64, uint16, uint16, uint8) noexcept -> void {}

		inline auto TaskTerminal(uint64 TaskId, uint64 ScopeId, uint16 OwnerId, uint16 CategoryId, uint8 Target, uint8 TerminalReason) noexcept -> void
		{
			const Private::FTaskProfilerMessage Message = Private::FormatTaskProfilerMessage(
				"terminal", TaskId, ScopeId, OwnerId, CategoryId, Target, TerminalReason);
			TracyMessage(Message.Bytes.data(), Message.Length);
		}

		inline auto TaskAggregatePlots(
			uint16 OwnerId,
			uint16 CategoryId,
			uint64 QueueDepth,
			uint64 Running,
			uint64 Rejected,
			uint64 CallableBytes,
			uint64 PayloadBytes,
			uint64 ResultBytes,
			uint64 RetainedResultBytes) noexcept -> void
		{
			const Private::FTaskProfilerSlot& Slot = Private::GetTaskProfilerSlot(OwnerId, CategoryId);
			const std::array<uint64, static_cast<size_t>(Private::ETaskProfilerPlot::Count)> Values{
				QueueDepth, Running, Rejected, CallableBytes, PayloadBytes, ResultBytes, RetainedResultBytes
			};
			for (size_t Index = 0; Index < Values.size(); ++Index)
			{
				TracyPlot(Slot.PlotNames[Index].data(), static_cast<int64>(Values[Index]));
			}
		}
	}

	#define DURIN_PROFILE_CPU_ZONE() ZoneScoped
	#define DURIN_PROFILE_CPU_ZONE_NAMED(Name) ZoneScopedN(Name)
	#define DURIN_PROFILE_TASK_EXECUTION_ZONE(DebugName, TaskId, ScopeId, OwnerId, CategoryId, Target) \
		ZoneScopedN("Task.Execute"); \
		const auto DurinTaskProfilerExecutionMessage = ::Durin::Profiling::Private::FormatTaskProfilerMessage( \
			"execute", TaskId, ScopeId, OwnerId, CategoryId, Target, 0, DebugName); \
		ZoneText(DurinTaskProfilerExecutionMessage.Bytes.data(), DurinTaskProfilerExecutionMessage.Length)
	#define DURIN_PROFILE_FRAME_MARK() FrameMark
	#define DURIN_PROFILE_STARTUP_FIRST_PRESENT() TracyMessageL("Startup.FirstPresent")
	#define DURIN_PROFILE_THREAD(Name) tracy::SetThreadName(Name)
	#define DURIN_PROFILE_PROGRAM_IDENTITY(RuntimeVariant, ProjectName, ProcessId) \
		::Durin::Profiling::SetProgramIdentity(RuntimeVariant, ProjectName, ProcessId)
#else
	namespace Durin::Profiling
	{
		inline auto TaskEnqueued(uint64, uint64, uint16, uint16, uint8) noexcept -> void {}
		inline auto TaskExecution(uint64, uint64, uint16, uint16, uint8) noexcept -> void {}
		inline auto TaskTerminal(uint64, uint64, uint16, uint16, uint8, uint8) noexcept -> void {}
		inline auto TaskAggregatePlots(uint16, uint16, uint64, uint64, uint64, uint64, uint64, uint64, uint64) noexcept -> void {}
	}

	#define DURIN_PROFILE_CPU_ZONE() ((void)0)
	#define DURIN_PROFILE_CPU_ZONE_NAMED(Name) ((void)0)
	#define DURIN_PROFILE_TASK_EXECUTION_ZONE(DebugName, TaskId, ScopeId, OwnerId, CategoryId, Target) \
		::Durin::Profiling::TaskExecution(TaskId, ScopeId, OwnerId, CategoryId, Target)
	#define DURIN_PROFILE_FRAME_MARK() ((void)0)
	#define DURIN_PROFILE_STARTUP_FIRST_PRESENT() ((void)0)
	#define DURIN_PROFILE_THREAD(Name) ((void)0)
	#define DURIN_PROFILE_PROGRAM_IDENTITY(RuntimeVariant, ProjectName, ProcessId) ((void)0)
#endif
