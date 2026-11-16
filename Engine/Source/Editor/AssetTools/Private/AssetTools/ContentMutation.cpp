#include "AssetTools/ContentMutation.h"
namespace Durin::ContentStorage
{
	auto OnContentDirectoriesChanged() -> TMulticastDelegate<void(const FContentChangeBatch&, const DObject*, EContentDirectoryChangeReason)>&
	{
		static TMulticastDelegate<void(const FContentChangeBatch&, const DObject*, EContentDirectoryChangeReason)> Event;
		return Event;
	}
	auto PublishContentDirectoryChanges(const FContentChangeBatch& Batch, const DObject* Origin, EContentDirectoryChangeReason Reason) -> void
	{
		FContentChangeBatch Directories;
		Directories.bFullRefresh = Batch.bFullRefresh;
		for (const auto& Change : Batch.Changes) if (Change.bDirectory) Directories.Changes.push_back(Change);
		if (Directories.bFullRefresh || !Directories.Changes.empty()) OnContentDirectoriesChanged().Broadcast(Directories, Origin, Reason);
	}
	auto CanModifyContent(const FContentMutationPolicy& Policy, std::string* Error) -> bool
	{
		if (Error) Error->clear();
		if (Policy.CanMutateContent && !Policy.CanMutateContent())
		{ if (Error) *Error = "Content mutation is currently disabled."; return false; }
		return true;
	}
	auto MakeContentMutationFeedback(const FContentMutationPolicy& Policy) -> FFeedbackContext
	{
		const auto Sink = Policy.ReportDiagnostic;
		auto* Fallback = &FFeedbackContext::Get();
		return FFeedbackContext({}, [Sink, Fallback](ELogLevel Level, std::string_view Message) {
			if (Sink) Sink(Level, Message); else Fallback->Serialize(Level, Message);
		});
	}
	auto NotifyContentMutation(const FContentMutationPolicy& Policy, FContentChangeBatch Changes) -> void
	{
		if (!Changes.bFullRefresh && Changes.Changes.empty()) return;
		if (Policy.NotifyContentMutation) Policy.NotifyContentMutation(std::move(Changes));
	}
	auto ReportContentMutationFailure(const FContentMutationPolicy& Policy, const UnifiedError::FError& Error) -> bool
	{
		if (Policy.ReportDiagnostic) Policy.ReportDiagnostic(ELogLevel::Error, Error.ToString());
		else FFeedbackContext::Get().Serialize(ELogLevel::Error, Error.ToString());
		return false;
	}
}
