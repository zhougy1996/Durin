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
		auto Success(const DLevelEditorSubsystem& Levels, std::string Message) -> FToolResult
		{
			const auto State = !Levels.IsInPlayInEditor() ? "stopped" : Levels.IsPlaySessionPaused() ? "paused" : "playing";
			return {EToolStatus::Succeeded, FToolValue::FObject{{"message", std::move(Message)}, {"state", State}}};
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
				if (Request.Address.Tool == "status") return Success(*Levels,
					!Levels->IsInPlayInEditor() ? "Stopped" : Levels->IsPlaySessionPaused() ? "Paused" : "Playing");
				if (Request.Address.Tool == "play")
				{
					if (Levels->IsInPlayInEditor()) return Failure("A Play session is already active.");
					Levels->EditorRequestBeginPlay();
					return Levels->IsInPlayInEditor() ? Success(*Levels, "Play session started.")
						: Failure("Could not start the Play session. See editor diagnostics for details.");
				}
				if (!Levels->IsInPlayInEditor()) return Failure("No Play session is active.");
				if (Request.Address.Tool == "stop")
				{
					Levels->EditorRequestEndPlay();
					return !Levels->IsInPlayInEditor() ? Success(*Levels, "Play session stopped.") : Failure("Could not stop the Play session.");
				}
				const bool Paused = std::get<bool>(std::get<FToolValue::FObject>(Request.Arguments.Data).at("paused").Data);
				return Levels->SetPlaySessionPaused(Paused) ? Success(*Levels, Paused ? "Play paused." : "Play resumed.")
					: Failure("Could not change the Play pause state.");
			};
			Bindings.push_back(std::move(Binding));
		}
		return std::make_unique<FNativeToolset>("editor.pie", "Play in Editor controls for the admitting editor.", std::move(Bindings));
	}
}
