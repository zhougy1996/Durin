#include "ContentBrowserOperationContext.h"
#include "ContentBrowserDataSource.h"
#include "Editor/EditorEngine.h"
#include "Misc/ContentChanges.h"
namespace Durin::ContentBrowser
{
	auto ComposeOperationContext(const FContentBrowserOperationContext& Existing, const FContentBrowserOperationContext& Requested) -> FContentBrowserOperationContext
	{
		auto Result = Existing;
		if (Requested.OpenAsset) Result.OpenAsset = Requested.OpenAsset;
		if (Requested.NotifyContentMutation) Result.NotifyContentMutation = Requested.NotifyContentMutation;
		if (Requested.ReportDiagnostic) Result.ReportDiagnostic = Requested.ReportDiagnostic;
		if (Requested.CanMutateContent)
		{
			if (Existing.CanMutateContent) Result.CanMutateContent = [Before = Existing.CanMutateContent, Added = Requested.CanMutateContent] { return Before() && Added(); };
			else Result.CanMutateContent = Requested.CanMutateContent;
		}
		return Result;
	}
	auto ScopeOperationContext(DContentBrowserDataSource& Source, FContentBrowserOperationContext Host,
		std::function<void(DContentBrowserDataSource&, const FContentChangeBatch&)> OnCommitted) -> FContentBrowserOperationContext
	{
		const TWeakObjectPtr<DContentBrowserDataSource> Weak(&Source);
		auto Current = [Weak]() -> DContentBrowserDataSource* {
			auto* Owner = Weak.Get();
			return Owner && Owner->IsActive() ? Owner : nullptr;
		};
		Host.NotifyContentMutation = [Current, OnCommitted = std::move(OnCommitted), Notify = Host.NotifyContentMutation](FContentChangeBatch Batch) {
			if (auto* Owner = Current(); Owner && OnCommitted)
			{
				TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Owner);
				OnCommitted(*Owner, Batch);
			}
			if (Notify) Notify(std::move(Batch));
		};
		Host.CanMutateContent = [Current, Allow = Host.CanMutateContent] {
			auto Admitted = [&] {
				auto* Owner = Current();
				if (!Owner) return false;
				const auto* Editor = Owner->GetOwningEditor();
				return !Editor || Editor->GetCapabilities().bEditWorld;
			};
			return Admitted() && (!Allow || Allow()) && Admitted();
		};
		return Host;
	}
}
