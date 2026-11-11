#include "Tools/TracyToolset.h"
#include "ProfilingToolService.h"

namespace Durin::MainFrame
{
	auto CreateTracyToolset(std::shared_ptr<FProfilingToolService> Service) -> std::unique_ptr<FNativeToolset>
	{
		std::vector<FToolBinding> Bindings;
		FToolBinding Status;
		Status.Descriptor.Name = "status";
		Status.Descriptor.Description = "Reports managed Tracy tool installation and this process's connection readiness.";
		Status.Descriptor.Effects = EToolEffect::ReadOnly;
		Status.Descriptor.RetryPolicy = EToolRetryPolicy::Idempotent;
		for (const auto* Name : {"manifest_valid", "platform_supported", "version_matches", "available", "can_connect"})
		{
			Status.Descriptor.Output.Properties.emplace(Name, FToolSchema(EToolValueType::Boolean));
			Status.Descriptor.Output.Required.emplace_back(Name);
		}
		for (const auto* Name : {"message", "connection_diagnostic", "expected_version", "client_version",
			"package_path", "profiler_path", "repair_command"})
		{
			Status.Descriptor.Output.Properties.emplace(Name, FToolSchema(EToolValueType::String));
			Status.Descriptor.Output.Required.emplace_back(Name);
		}
		FToolSchema Missing(EToolValueType::Array);
		Missing.Items = std::make_unique<FToolSchema>(EToolValueType::String);
		Status.Descriptor.Output.Properties.emplace("missing_files", std::move(Missing));
		Status.Descriptor.Output.Required.emplace_back("missing_files");
		Status.Handler = [Service](const FToolRequest&, const FToolContext&) -> FToolResult {
			const auto State = Service->QueryStatus();
			FToolValue::FArray MissingFiles;
			for (const auto& File : State.MissingFiles) MissingFiles.emplace_back(File);
			return {EToolStatus::Succeeded, FToolValue::FObject{
				{"manifest_valid", State.bManifestValid}, {"platform_supported", State.bPlatformSupported},
				{"version_matches", State.bVersionMatches}, {"available", State.bAvailable}, {"can_connect", State.bCanConnect},
				{"message", State.Diagnostic}, {"connection_diagnostic", State.ConnectionDiagnostic},
				{"expected_version", State.ExpectedVersion}, {"client_version", State.ClientVersion},
				{"package_path", State.PackagePath}, {"profiler_path", State.ProfilerPath},
				{"repair_command", State.RepairCommand},
				{"missing_files", std::move(MissingFiles)}}};
		};
		Bindings.push_back(std::move(Status));
		for (const auto* Name : {"launch", "open_capture"})
		{
			FToolBinding Binding;
			auto& Descriptor = Binding.Descriptor;
			Descriptor.Name = Name;
			Descriptor.Description = Descriptor.Name == "launch" ? "Launches the managed Tracy profiler for this editor's actual localhost port."
				: "Opens an existing .tracy file in the managed profiler.";
			Descriptor.Effects = EToolEffect::External;
			Descriptor.RetryPolicy = EToolRetryPolicy::Unsafe;
			Descriptor.Output.Properties.emplace("message", FToolSchema(EToolValueType::String));
			Descriptor.Output.Required = {"message"};
			if (Descriptor.Name == "open_capture")
			{
				FToolSchema Path(EToolValueType::String);
				Path.MinSize = 1;
				Descriptor.Input.Properties.emplace("path", std::move(Path));
				Descriptor.Input.Required = {"path"};
			}
			Binding.Handler = [Service](const FToolRequest& Request, const FToolContext&) -> FToolResult {
				std::string Error;
				bool Success;
				if (Request.Address.Tool == "launch") Success = Service->LaunchProfiler(&Error);
				else Success = Service->OpenCapture(std::get<std::string>(
					std::get<FToolValue::FObject>(Request.Arguments.Data).at("path").Data), &Error);
				if (!Success) return {EToolStatus::Failed, {}, {{EToolError::DomainFailure, std::move(Error), {}}}};
				return {EToolStatus::Succeeded, FToolValue::FObject{{"message",
					Request.Address.Tool == "launch" ? "Profiler launched; connection requested, handshake not confirmed."
					: "Capture open requested."}}};
			};
			Bindings.push_back(std::move(Binding));
		}
		return std::make_unique<FNativeToolset>("editor.tracy", "Managed Tracy profiler tools.", std::move(Bindings));
	}
}
