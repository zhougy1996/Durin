#include "LaunchArguments.h"

#include "Misc/Build.h"

namespace Durin
{
	namespace
	{
		struct FParsedLaunchArguments
		{
			FLaunchRequest Request;
			std::unordered_set<std::string> SeenOptions;
		};

		auto Failure(std::string_view Option, std::string Message)
			-> FLaunchArgumentResult
		{
			return {.Error = FLaunchArgumentError{
				.ExitCode = 2,
				.Option = std::string(Option),
				.Message = std::move(Message)}};
		}

		auto MarkUnique(
			FParsedLaunchArguments& Parsed,
			std::string_view Option,
			FLaunchArgumentResult& OutFailure) -> bool
		{
			if (Parsed.SeenOptions.emplace(Option).second) return true;
			OutFailure = Failure(Option, "may be specified only once");
			return false;
		}

		template <typename T>
		auto ParsePositiveInteger(
			std::string_view Option,
			std::string_view Text,
			T& OutValue) -> FLaunchArgumentResult
		{
			if (Text.empty()) return Failure(Option, "requires a non-empty numeric value");
			const auto [End, Error] = std::from_chars(
				Text.data(), Text.data() + Text.size(), OutValue);
			if (Error == std::errc::result_out_of_range)
				return Failure(Option, "value is outside the supported range");
			if (Error != std::errc{} || End != Text.data() + Text.size() || OutValue == 0)
				return Failure(Option, "requires a positive integer without trailing text");
			return {.Request = FLaunchRequest{}};
		}

		auto ParseNativeCrashPhase(
			std::string_view Text,
			ENativeCrashPhase& OutPhase) -> bool
		{
			if (Text == "process-entry") OutPhase = ENativeCrashPhase::ProcessEntry;
			else if (Text == "pre-initialization") OutPhase = ENativeCrashPhase::PreInitialization;
			else if (Text == "logger-running") OutPhase = ENativeCrashPhase::LoggerRunning;
			else if (Text == "running") OutPhase = ENativeCrashPhase::Running;
			else if (Text == "object-collection") OutPhase = ENativeCrashPhase::ObjectCollection;
			else return false;
			return true;
		}

		auto ParseSyntax(std::span<const std::string_view> Arguments)
			-> std::variant<FParsedLaunchArguments, FLaunchArgumentError>
		{
			FParsedLaunchArguments Parsed;
			for (size_t Index = 0; Index < Arguments.size(); ++Index)
			{
				const std::string_view Argument = Arguments[Index];
				const size_t Equals = Argument.find('=');
				const std::string_view Option = Equals == std::string_view::npos
					? Argument : Argument.substr(0, Equals);
				const std::string_view Value = Equals == std::string_view::npos
					? std::string_view{} : Argument.substr(Equals + 1);
				FLaunchArgumentResult Duplicate;

				if (Option == "--startup-command-arg" && Equals != std::string_view::npos)
				{
					Parsed.Request.Automation.StartupCommand.Arguments.emplace_back(Value);
					continue;
				}
				if (!MarkUnique(Parsed, Option, Duplicate)) return *Duplicate.Error;

				if (Option == "--wait-for-process" && Equals != std::string_view::npos)
				{
					uint32 ProcessId = 0;
					if (auto Result = ParsePositiveInteger(Option, Value, ProcessId); !Result)
						return *Result.Error;
					Parsed.Request.ProcessCoordination.WaitForProcessId = ProcessId;
				}
				else if (Option == "--exit-after-ticks" && Equals != std::string_view::npos)
				{
					uint64 TickCount = 0;
					if (auto Result = ParsePositiveInteger(Option, Value, TickCount); !Result)
						return *Result.Error;
					Parsed.Request.Automation.ExitAfterTicks = TickCount;
				}
				else if (Argument == "--hidden-window") Parsed.Request.Host.bSuppressWindowDisplay = true;
				else if (Argument == "--project-browser") Parsed.Request.Host.bOpenProjectBrowser = true;
				else if (Argument == "--task-scheduler-lifecycle-smoke") Parsed.Request.Diagnostics.bRunTaskSchedulerLifecycleSmoke = true;
				else if (Argument == "--engine-asset-service-lifecycle-smoke") Parsed.Request.Diagnostics.bRunEngineAssetServiceLifecycleSmoke = true;
				else if (Argument == "--editor-pie-lifecycle-smoke") Parsed.Request.Diagnostics.bRunEditorPIELifecycleSmoke = true;
				else if (Argument == "--native-gameplay-lifecycle-smoke") Parsed.Request.Diagnostics.bRunNativeGameplayLifecycleSmoke = true;
				else if (Option == "--project" && Equals != std::string_view::npos)
					Parsed.Request.Host.ProjectFile = std::string(Value);
				else if (Argument == "--project")
				{
					if (Index + 1 >= Arguments.size()
						|| Arguments[Index + 1].empty()
						|| Arguments[Index + 1].starts_with("--"))
						return FLaunchArgumentError{2, "--project", "requires a following non-empty path"};
					Parsed.Request.Host.ProjectFile = std::string(Arguments[++Index]);
				}
				else if (Option == "--startup-command" && Equals != std::string_view::npos)
					Parsed.Request.Automation.StartupCommand.Name = std::string(Value);
				else if (Option == "--native-crash-fixture" && Equals != std::string_view::npos)
					Parsed.Request.Diagnostics.NativeCrashFixture = std::string(Value);
				else if (Option == "--native-crash-saved" && Equals != std::string_view::npos)
					Parsed.Request.Diagnostics.NativeCrashSavedRoot = std::string(Value);
				else if (Option == "--native-crash-at" && Equals != std::string_view::npos)
				{
					ENativeCrashPhase Phase;
					if (!ParseNativeCrashPhase(Value, Phase))
						return FLaunchArgumentError{2, "--native-crash-at", "has an unsupported lifecycle phase"};
					Parsed.Request.Diagnostics.NativeCrashPhase = Phase;
				}
				else if (Argument == "--native-crash-disable-dump") Parsed.Request.Diagnostics.bDisableNativeCrashDump = true;
				else if (Argument == "--native-crash-force-collision") Parsed.Request.Diagnostics.bForceNativeCrashCollision = true;
				else if (Argument == "--native-crash-log-gap") Parsed.Request.Diagnostics.bFillNativeCrashLogGap = true;
				else if (Argument == "--native-crash-fault-writer") Parsed.Request.Diagnostics.bFaultNativeCrashWriter = true;
				else return FLaunchArgumentError{2, std::string(Argument), "is not a recognized Launch option"};
			}
			return Parsed;
		}

		auto ValidateSemantics(FParsedLaunchArguments Parsed) -> FLaunchArgumentResult
		{
			FLaunchRequest& Request = Parsed.Request;
			if (Request.Host.ProjectFile && Request.Host.ProjectFile->empty())
				return Failure("--project", "requires a non-empty path");
			if (Request.Automation.StartupCommand.Name
				&& Request.Automation.StartupCommand.Name->empty())
				return Failure("--startup-command", "requires a non-empty command name");
			if (!Request.Automation.StartupCommand.Name
				&& !Request.Automation.StartupCommand.Arguments.empty())
				return Failure("--startup-command-arg", "requires exactly one --startup-command");

			const bool bLifecycleSmoke =
				Request.Diagnostics.bRunTaskSchedulerLifecycleSmoke
				|| Request.Diagnostics.bRunEngineAssetServiceLifecycleSmoke
				|| Request.Diagnostics.bRunEditorPIELifecycleSmoke
				|| Request.Diagnostics.bRunNativeGameplayLifecycleSmoke;
			if (Request.Automation.StartupCommand.Name
				&& (Request.Automation.ExitAfterTicks || Request.Host.bOpenProjectBrowser || bLifecycleSmoke))
				return Failure("--startup-command", "conflicts with tick-exit, project-browser, and lifecycle-smoke modes");

			FLaunchDiagnosticsRequest& Diagnostics = Request.Diagnostics;
			if (Diagnostics.NativeCrashFixture && Diagnostics.NativeCrashFixture->empty())
				return Failure("--native-crash-fixture", "requires a non-empty fixture name");
			if (Diagnostics.NativeCrashSavedRoot && Diagnostics.NativeCrashSavedRoot->empty())
				return Failure("--native-crash-saved", "requires a non-empty path");
			if (Diagnostics.NativeCrashPhase && !Diagnostics.NativeCrashFixture)
				return Failure("--native-crash-at", "requires --native-crash-fixture");
			if (Diagnostics.bFillNativeCrashLogGap
				&& (!Diagnostics.NativeCrashFixture || !Diagnostics.NativeCrashPhase
					|| (*Diagnostics.NativeCrashPhase != ENativeCrashPhase::LoggerRunning
						&& *Diagnostics.NativeCrashPhase != ENativeCrashPhase::Running)))
				return Failure("--native-crash-log-gap", "requires a fixture at logger-running or running");
#if DURIN_BUILD_SHIPPING
			if (bLifecycleSmoke || Diagnostics.NativeCrashFixture
				|| Diagnostics.NativeCrashSavedRoot || Diagnostics.NativeCrashPhase
				|| Diagnostics.bDisableNativeCrashDump
				|| Diagnostics.bForceNativeCrashCollision
				|| Diagnostics.bFillNativeCrashLogGap
				|| Diagnostics.bFaultNativeCrashWriter)
				return Failure("diagnostic option", "is unavailable in Shipping builds");
#endif
			return {.Request = std::move(Request)};
		}
	}

	auto ParseLaunchArguments(std::span<const std::string_view> Arguments)
		-> FLaunchArgumentResult
	{
		auto Parsed = ParseSyntax(Arguments);
		if (const auto* Error = std::get_if<FLaunchArgumentError>(&Parsed))
			return {.Error = *Error};
		return ValidateSemantics(std::get<FParsedLaunchArguments>(std::move(Parsed)));
	}
}
