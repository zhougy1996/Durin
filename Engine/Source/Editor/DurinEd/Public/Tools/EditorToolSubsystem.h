#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "Tools/EditorToolService.h"
#include "EditorToolSubsystem.gen.h"

namespace Durin
{
	DCLASS(NoClassDefaultObject)
	class DEditorToolSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorToolSubsystem(const FObjectInitializer& Initializer);
		DURINED_API auto Initialize() -> FSubsystemResult override;
		DURINED_API auto Deinitialize() noexcept -> void override;
		// Acquire on GameThread, then retain the detached endpoint on other threads.
		DURINED_API auto GetService() const -> std::shared_ptr<FEditorToolService>;
		DURINED_API auto ListToolsets() -> FEditorToolCatalog;
		DURINED_API auto DescribeToolset(std::string_view Name) -> std::expected<FEditorToolsetCatalogEntry, FToolDiagnostic>;
		DURINED_API auto DescribeTool(const FToolAddress& Address) -> std::expected<FToolDescriptor, FToolDiagnostic>;
		DURINED_API auto CheckAvailability(const FToolRequest& Request, EToolSource Source = EToolSource::Native) -> FToolAvailability;
		DURINED_API auto CallNow(const FToolRequest& Request, EToolSource Source = EToolSource::Native,
			const FToolClientContext& Client = {}, FToolCancellationToken Cancellation = {}) -> FToolResult;
		DURINED_API auto SubmitNow(const FToolRequest& Request, EToolSource Source = EToolSource::Native,
			const FToolClientContext& Client = {}, FToolCancellationToken Cancellation = {}) -> std::expected<FEditorToolSubmission, FToolDiagnostic>;
		// Advisory scheduling check only; callers must never retry a provider result automatically.
		DURINED_API auto CanCallNow() const -> bool;
		// Editor tick safe point; also used by native hosts without rendering startup.
		DURINED_API auto TickTools() -> void;
		// Separate script boundary: native tool receipts are published before scripts run.
		DURINED_API auto TickScripts() -> void;
	private:
		std::shared_ptr<FEditorToolService> Service;
	};
}
