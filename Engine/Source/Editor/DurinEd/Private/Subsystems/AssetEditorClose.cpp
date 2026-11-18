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

	auto DAssetEditorSubsystem::QueueClose(FAssetEditorToolkit& Toolkit, FObjectKey Asset, std::string Label) -> void
	{
		if (!Toolkit.IsEditorOpen() || FindPendingClose(Toolkit)) return;
		PendingCloses.push_back(std::make_shared<FPendingClose>(FPendingClose{
			.Prompt = {NextCloseRequest++, std::string(Toolkit.GetWindowId()), std::move(Label)},
			.Toolkit = Toolkit.weak_from_this(),
			.WindowReason = Toolkit.ActiveCloseReason ? Toolkit.ActiveCloseReason
				: Toolkit.GetEditingObjects().size() == 1 ? std::optional(EAssetEditorCloseReason::AssetEditorHostClosed) : std::nullopt,
			.Asset = Asset}));
	}

	auto DAssetEditorSubsystem::GetPendingClosePrompt() const -> std::optional<FAssetEditorClosePrompt>
	{
		const auto Found = std::ranges::find_if(PendingCloses, [](const auto& Request) { return !Request->bResume; });
		return Found == PendingCloses.end() ? std::nullopt : std::optional((*Found)->Prompt);
	}

	auto DAssetEditorSubsystem::ResolvePendingClose(uint64 RequestId, EAssetEditorCloseResponse Response) -> EAssetEditorCloseResult
	{
		if (!CanCall()) return EAssetEditorCloseResult::Rejected;
		const auto Found = std::ranges::find_if(PendingCloses,
			[RequestId](const auto& Request) { return Request->Prompt.RequestId == RequestId; });
		if (Found == PendingCloses.end()) return EAssetEditorCloseResult::Rejected;
		const auto Request = *Found;
		if (Request->bResolving || Request->bResume) return EAssetEditorCloseResult::Rejected;
		const auto Toolkit = Request->Toolkit.lock();
		if (!Toolkit || !Toolkit->IsEditorOpen())
		{
			std::erase(PendingCloses, Request);
			return EAssetEditorCloseResult::Rejected;
		}
		if (Response == EAssetEditorCloseResponse::Cancel)
		{
			std::erase(PendingCloses, Request);
			return EAssetEditorCloseResult::Cancelled;
		}
		EAssetEditorCloseResult Result = EAssetEditorCloseResult::Rejected;
		Request->bResolving = true;
		DispatchEditorCallback([&] { Result = Toolkit->ResolveAssetClose(Request->Asset, Response); });
		Request->bResolving = false;
		if (Result == EAssetEditorCloseResult::Closed && Request->WindowReason && Toolkit->IsEditorOpen())
			Request->bResume = true;
		else if (Result != EAssetEditorCloseResult::PendingConfirmation || !Toolkit->IsEditorOpen())
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
