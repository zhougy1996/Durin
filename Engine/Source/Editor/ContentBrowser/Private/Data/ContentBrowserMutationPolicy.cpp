#include "Data/ContentBrowserMutationPolicy.h"
namespace Durin::ContentBrowser::Private
{
	FContentBrowserMutationPolicy::FContentBrowserMutationPolicy(const FContentBrowserOperationContext& Host)
		: Diagnostics(Host.ReportDiagnostic)
		, MutationAllowed(Host.CanMutateContent)
		, ItemUpdates(Host.NotifyScopedContentMutation)
	{
		if (!ItemUpdates && Host.NotifyMountedContentMutation)
			ItemUpdates = [Notify = Host.NotifyMountedContentMutation](FContentChangeBatch) { Notify(); };
	}

	auto FContentBrowserMutationPolicy::Report(ELogLevel Level, std::string_view Message) const -> void
	{
		if (Diagnostics) Diagnostics(Level, Message);
		else FFeedbackContext::Get().Serialize(Level, Message);
	}
	auto FContentBrowserMutationPolicy::Feedback() const -> FFeedbackContext
	{ return FFeedbackContext({}, [this](ELogLevel Level, std::string_view Message) { Report(Level, Message); }); }
	auto FContentBrowserMutationPolicy::NotifyItemsUpdated(FContentChangeBatch Changes) -> void
	{
		if (!Changes.bFullRefresh && Changes.Changes.empty()) return;
		if (ItemUpdates) ItemUpdates(std::move(Changes));
	}
	auto FContentBrowserMutationPolicy::Complete(const FAssetWriteResult& Status, FContentChangeBatch Changes) -> bool
	{
		if (!Status) Report(ELogLevel::Error, Status.Message);
		NotifyItemsUpdated(std::move(Changes));
		return bool(Status);
	}

	auto FContentBrowserMutationPolicy::CanModify(std::string* OutErrorMsg) const -> bool
	{
		if (OutErrorMsg) OutErrorMsg->clear();
		if (!bAccepting) { if (OutErrorMsg) *OutErrorMsg = "Content operations are stopping."; return false; }
		if (MutationAllowed && !MutationAllowed()) { if (OutErrorMsg) *OutErrorMsg = "Content mutation is currently disabled."; return false; }
		return bAccepting;
	}

}
