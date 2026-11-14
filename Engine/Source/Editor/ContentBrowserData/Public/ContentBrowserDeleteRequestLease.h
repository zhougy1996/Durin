#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataSource.h"

namespace Durin::ContentBrowser::Private
{
	// Policy/feedback callbacks may dismiss or replace a modal. Execution retains
	// its capability locally and never resurrects a retired or replaced context.
	struct FContentBrowserDeleteRequestLease
	{
		explicit FContentBrowserDeleteRequestLease(FContentBrowserDeleteContext& InTarget)
			: Target(InTarget), Owner(InTarget.OwnerItem), Request(std::move(InTarget.Request)) {}
		~FContentBrowserDeleteRequestLease()
		{
			if (!bConsumed && Request && !Target.Request && Target.OwnerItem.GetContributionKey() == Owner.GetContributionKey()) Target.Request = std::move(Request);
		}
		FContentBrowserDeleteContext& Target;
		FContentBrowserItemData Owner;
		std::unique_ptr<IContentBrowserDeleteRequest> Request;
		bool bConsumed = false;
	};
}
