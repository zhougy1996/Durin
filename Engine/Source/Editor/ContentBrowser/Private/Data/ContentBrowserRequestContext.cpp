#include "Data/ContentBrowserRequestContext.h"

namespace Durin::ContentBrowser::Private
{
	FContentBrowserRequestContext::FContentBrowserRequestContext(const FContentBrowserHostContext& Host)
		: Diagnostics(Host.ReportDiagnostic)
		, MutationAllowed(Host.CanMutateContent)
		, ItemUpdates(Host.NotifyScopedContentMutation)
	{
		if (!ItemUpdates && Host.NotifyMountedContentMutation)
			ItemUpdates = [Notify = Host.NotifyMountedContentMutation](FContentChangeBatch) { Notify(); };
	}
	auto FContentBrowserRequestContext::Combine(std::vector<std::shared_ptr<FContentBrowserRequestContext>> Contexts) -> std::shared_ptr<FContentBrowserRequestContext>
	{
		if (Contexts.empty()) return {};
		if (Contexts.size() == 1) return Contexts.front();
		auto Result = std::make_shared<FContentBrowserRequestContext>();
		Result->ItemUpdates = Contexts.front()->ItemUpdates;
		Result->Diagnostics = Contexts.front()->Diagnostics;
		Result->MutationAllowed = [Contexts = std::move(Contexts)] {
			for (const auto& Context : Contexts) if (!Context || !Context->CanModify()) return false;
			return true;
		};
		return Result;
	}

}
