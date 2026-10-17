#include "Tools/PIEToolset.h"
#include "Tools/EditorToolService.h"
#include "Editor/EditorEngine.h"
#include "Editor/LevelEditorSubsystem.h"

namespace Durin
{
	namespace
	{
		auto Failure(std::string Message) -> FToolResult
		{
			return {EToolStatus::Failed, {}, {{EToolError::DomainFailure, std::move(Message), {}}}};
		}
		auto ToToolResult(FLevelEditorPlayResult Result) -> FToolResult
		{
			if (!Result.bSucceeded) return Failure(std::move(Result.Message));
			return {EToolStatus::Succeeded, FToolValue::FObject{{"message", std::move(Result.Message)}, {"state", std::move(Result.State)}}};
		}
	}
	auto CreatePIEToolset() -> std::unique_ptr<FNativeToolset>
	{
		std::vector<FToolBinding> Bindings;
		for (const auto& Name : {"status", "play", "stop", "pause"})
		{
			FToolDescriptor Descriptor;
			Descriptor.Name = Name;
			Descriptor.Description = Name == std::string_view("status") ? "Shows the current Play state."
				: Name == std::string_view("play") ? "Starts Play from the level start."
				: Name == std::string_view("stop") ? "Stops the active Play session." : "Sets whether Play is paused.";
			Descriptor.bExposeExternally = true;
			Descriptor.Effects = Descriptor.Name == "status" ? EToolEffect::ReadOnly : EToolEffect::MutatesMemory;
			Descriptor.RetryPolicy = Descriptor.Name == "status" || Descriptor.Name == "pause" ? EToolRetryPolicy::Idempotent : EToolRetryPolicy::Unsafe;
			Descriptor.Output.Properties.emplace("message", FToolSchema(EToolValueType::String));
			FToolSchema StateSchema(EToolValueType::String);
			StateSchema.Enum = {"stopped", "playing", "paused"};
			Descriptor.Output.Properties.emplace("state", std::move(StateSchema));
			Descriptor.Output.Required = {"message", "state"};
			if (Descriptor.Name == "pause")
			{
				Descriptor.Input.Properties.emplace("paused", FToolSchema(EToolValueType::Boolean));
				Descriptor.Input.Required = {"paused"};
			}
			FToolBinding Binding;
			Binding.Descriptor = std::move(Descriptor);
			Binding.Handler = [](const FToolRequest& Request, const FToolContext& Context) -> FToolResult {
				auto* Editor = GetToolEditor(Context);
				if (!Editor) return Failure("The admitting editor is unavailable.");
				auto* Levels = Editor->GetEditorSubsystem<DLevelEditorSubsystem>();
				if (!Levels) return Failure("The level editor service is unavailable.");
				if (Request.Address.Tool == "status") return ToToolResult(Levels->GetPlayStatus());
				if (Request.Address.Tool == "play") return ToToolResult(Levels->StartPlaySession());
				if (Request.Address.Tool == "stop") return ToToolResult(Levels->StopPlaySession());
				return ToToolResult(Levels->SetPlaySessionPaused(std::get<bool>(std::get<FToolValue::FObject>(Request.Arguments.Data).at("paused").Data)));
			};
			Bindings.push_back(std::move(Binding));
		}
		return std::make_unique<FNativeToolset>("editor.pie", "Play in Editor controls for the admitting editor.", std::move(Bindings));
	}
}
