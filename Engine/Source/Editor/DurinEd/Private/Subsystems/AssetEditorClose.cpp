#include "Subsystems/AssetEditorSubsystem.h"

#include "Toolkits/AssetEditorToolkit.h"

namespace Durin
{
	auto DAssetEditorSubsystem::FindPendingClose(const FAssetEditorToolkit& Toolkit) const -> std::shared_ptr<FPendingClose>
	{
		const auto Found = std::ranges::find_if(PendingCloses,
			[&](const auto& Request) { return Request->Prompt.WindowId == Toolkit.GetWindowId(); });
		return Found == PendingCloses.end() ? nullptr : *Found;
	}

	auto DAssetEditorSubsystem::QueueClose(FAssetEditorToolkit& Toolkit, uint64 DocumentId, std::string Label,
		std::function<EDocumentCloseResult(EDocumentCloseResponse)> Resolve) -> void
	{
		if (!Toolkit.IsEditorOpen() || FindPendingClose(Toolkit)) return;
		PendingCloses.push_back(std::make_shared<FPendingClose>(FPendingClose{
			.Prompt = {NextCloseRequest++, std::string(Toolkit.GetWindowId()), std::move(Label)},
			.Toolkit = Toolkit.weak_from_this(),
			.WindowReason = Toolkit.ActiveCloseReason,
			.Resolve = std::move(Resolve),
			.LegacyDocumentId = DocumentId}));
	}

	auto DAssetEditorSubsystem::GetPendingClosePrompt() const -> std::optional<FAssetEditorClosePrompt>
	{
		const auto Found = std::ranges::find_if(PendingCloses, [](const auto& Request) { return !Request->bResume; });
		return Found == PendingCloses.end() ? std::nullopt : std::optional((*Found)->Prompt);
	}

	auto DAssetEditorSubsystem::ResolvePendingClose(uint64 RequestId, EDocumentCloseResponse Response) -> EDocumentCloseResult
	{
		if (!CanCall()) return EDocumentCloseResult::Rejected;
		const auto Found = std::ranges::find_if(PendingCloses,
			[RequestId](const auto& Request) { return Request->Prompt.RequestId == RequestId; });
		if (Found == PendingCloses.end()) return EDocumentCloseResult::Rejected;
		const auto Request = *Found;
		if (Request->bResolving || Request->bResume) return EDocumentCloseResult::Rejected;
		const auto Toolkit = Request->Toolkit.lock();
		if (!Toolkit || !Toolkit->IsEditorOpen())
		{
			std::erase(PendingCloses, Request);
			return EDocumentCloseResult::Rejected;
		}
		if (Response == EDocumentCloseResponse::Cancel)
		{
			std::erase(PendingCloses, Request);
			return EDocumentCloseResult::Cancelled;
		}
		EDocumentCloseResult Result = EDocumentCloseResult::Rejected;
		Request->bResolving = true;
		DispatchEditorCallback([&] { Result = Request->Resolve(Response); });
		Request->bResolving = false;
		if (Result == EDocumentCloseResult::Closed && Request->WindowReason && Toolkit->IsEditorOpen())
			Request->bResume = true;
		else if (Result != EDocumentCloseResult::PendingConfirmation || !Toolkit->IsEditorOpen())
			std::erase(PendingCloses, Request);
		return Result;
	}

	auto DAssetEditorSubsystem::ResumeReadyCloseRequests() -> void
	{
		const auto Snapshot = PendingCloses;
		for (const auto& Request : Snapshot)
		{
			if (!CanCall()) return;
			if (!Request->bResume || !std::erase(PendingCloses, Request)) continue;
			if (const auto Toolkit = Request->Toolkit.lock())
				Toolkit->CloseWindowInternal(*Request->WindowReason, false);
		}
	}
}
