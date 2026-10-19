#include "Tools/EditorToolSubsystem.h"
#include "Editor/EditorEngine.h"
#include "Tools/EditorScriptPump.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto Unavailable(EToolError Code = EToolError::ToolUnavailable) -> FToolResult
		{
			return {EToolStatus::Failed, {}, {{Code, "Editor tool service is unavailable or called from the wrong thread.", {}}}};
		}
	}
	DEditorToolSubsystem::DEditorToolSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DEditorToolSubsystem::CanCallNow() const -> bool
	{
		return IsInGameThread() && Service && Service->CanDispatch();
	}
	auto DEditorToolSubsystem::Initialize() -> FSubsystemResult
	{
		Service = std::shared_ptr<FEditorToolService>(new FEditorToolService(*GetEditor(), GetWorkGate()));
		Service->RefreshCatalog();
		return {};
	}
	auto DEditorToolSubsystem::Deinitialize() noexcept -> void
	{
		if (Service)
		{
			Service->Close();
			(void)FModularFeatureRegistry::Get().InvokeAll<IEditorScriptPump>([&](auto& Pump) { Pump.CloseEndpoint(Service); });
		}
		Service.reset();
	}
	auto DEditorToolSubsystem::GetService() const -> std::shared_ptr<FEditorToolService>
	{
		require(IsInGameThread());
		return Service;
	}
	auto DEditorToolSubsystem::ListToolsets() -> FEditorToolCatalog
	{
		if (!IsInGameThread()) { FEditorToolCatalog Result; Result.Diagnostics = Unavailable(EToolError::WrongThread).Diagnostics; return Result; }
		if (!Service) { FEditorToolCatalog Result; Result.Diagnostics = Unavailable().Diagnostics; return Result; }
		Service->RefreshCatalog();
		return Service->GetCatalog();
	}
	auto DEditorToolSubsystem::DescribeToolset(std::string_view Name) -> std::expected<FEditorToolsetCatalogEntry, FToolDiagnostic>
	{
		auto Catalog = ListToolsets();
		for (auto& Entry : Catalog.Toolsets) if (Entry.Descriptor.Name == Name) return std::move(Entry);
		if (!Catalog.Diagnostics.empty() && Catalog.Diagnostics.front().Code == EToolError::WrongThread) return std::unexpected(Catalog.Diagnostics.front());
		return std::unexpected(FToolDiagnostic{EToolError::ToolUnavailable, "Toolset is not present in the editor catalog.", {}});
	}
	auto DEditorToolSubsystem::DescribeTool(const FToolAddress& Address) -> std::expected<FToolDescriptor, FToolDiagnostic>
	{
		auto Set = DescribeToolset(Address.Toolset);
		if (!Set) return std::unexpected(Set.error());
		for (auto& Tool : Set->Descriptor.Tools) if (Tool.Name == Address.Tool) return std::move(Tool);
		return std::unexpected(FToolDiagnostic{EToolError::ToolUnavailable, "Tool is not present in the toolset.", {}});
	}
	auto DEditorToolSubsystem::CheckAvailability(const FToolRequest& Request, EToolSource Source) -> FToolAvailability
	{
		if (!IsInGameThread()) return {false, Unavailable(EToolError::WrongThread).Diagnostics};
		if (!CanCallNow()) return {false, {{EToolError::Busy, "Editor is outside the tool dispatch safe point.", {}}}};
		FToolAvailability Availability{false, Unavailable().Diagnostics};
		GetEditor()->DispatchSubsystemCallback([&] {
			auto Result = Service->Call(Request, Source, true, &Availability);
			if (Result.Status == EToolStatus::Failed) Availability = {false, std::move(Result.Diagnostics)};
		});
		return Availability;
	}
	auto DEditorToolSubsystem::CallNow(const FToolRequest& Request, EToolSource Source,
		const FToolClientContext& Client, FToolCancellationToken Cancellation) -> FToolResult
	{
		if (!IsInGameThread()) return Unavailable(EToolError::WrongThread);
		if (!CanCallNow()) return {EToolStatus::Failed, {}, {{EToolError::Busy, "Editor is outside the tool dispatch safe point.", {}}}};
		auto Result = Unavailable();
		GetEditor()->DispatchSubsystemCallback([&] { Result = Service->Call(Request, Source, false, nullptr, Client, Cancellation); });
		return Result;
	}
	auto DEditorToolSubsystem::SubmitNow(const FToolRequest& Request, EToolSource Source,
		const FToolClientContext& Client, FToolCancellationToken Cancellation) -> std::expected<FEditorToolSubmission, FToolDiagnostic>
	{
		if (!IsInGameThread()) return std::unexpected(Unavailable(EToolError::WrongThread).Diagnostics.front());
		if (!CanCallNow()) return std::unexpected(Unavailable(EToolError::Busy).Diagnostics.front());
		std::expected<FEditorToolSubmission, FToolDiagnostic> Result = std::unexpected(Unavailable().Diagnostics.front());
		GetEditor()->DispatchSubsystemCallback([&] { Result = Service->SubmitNow(Request, Source, Client, Cancellation); });
		return Result;
	}
	auto DEditorToolSubsystem::TickTools() -> void
	{
		require(IsInGameThread());
		if (!CanCallNow()) return;
		GetEditor()->DispatchSubsystemCallback([&] { Service->Drain(); });
	}
	auto DEditorToolSubsystem::TickScripts() -> void
	{
		require(IsInGameThread());
		if (!CanCallNow()) return;
		GetEditor()->DispatchSubsystemCallback([&] { Service->PumpScripts(Service); });
	}
}
