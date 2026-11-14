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
	FContentMutationOperation::FContentMutationOperation(const FContentMutationPolicy& InPolicy) : Policy(InPolicy) {}
	auto FContentMutationOperation::CanModify(std::string* Error) const -> bool
	{
		if (Error) Error->clear();
		if (Policy.CanMutateContent && !Policy.CanMutateContent())
		{ if (Error) *Error = "Content mutation is currently disabled."; return false; }
		return true;
	}
	auto FContentMutationOperation::Report(ELogLevel Level, std::string_view Message) const -> void
	{
		if (Policy.ReportDiagnostic) Policy.ReportDiagnostic(Level, Message);
		else FFeedbackContext::Get().Serialize(Level, Message);
	}
	auto FContentMutationOperation::Feedback() const -> FFeedbackContext
	{ return FFeedbackContext({}, [this](ELogLevel Level, std::string_view Message) { Report(Level, Message); }); }
	auto FContentMutationOperation::NotifyItemsUpdated(FContentChangeBatch Changes) -> void
	{
		if (!Changes.bFullRefresh && Changes.Changes.empty()) return;
		if (Policy.NotifyContentMutation) Policy.NotifyContentMutation(std::move(Changes));
	}
	auto FContentMutationOperation::Complete(const FAssetWriteResult& Status, FContentChangeBatch Changes) -> bool
	{
		if (!Status) Report(ELogLevel::Error, (Status.Error ? Status.Error->ToString() : std::string{}));
		NotifyItemsUpdated(std::move(Changes));
		return bool(Status);
	}
}
