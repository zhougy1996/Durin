#pragma once
#include "CoreMinimal.h"
#include "Data/ContentBrowserNativeItem.h"
#include "Asset/AssetWriteResult.h"
#include "Misc/FeedbackContext.h"
#include "ContentBrowserOperationContext.h"
namespace Durin::ContentBrowser::Private
{
	// Shared admission and reporting only; contains no mutation routing or I/O.
	class FContentBrowserMutationPolicy
	{
	public:
		explicit FContentBrowserMutationPolicy(const FContentBrowserOperationContext& Host = {});
		auto CanModify(std::string* Error = nullptr) const -> bool;
		auto StopRequestAdmission() -> void { bAccepting = false; }
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Diagnostics = std::move(Sink); }
	protected:
		auto Report(ELogLevel, std::string_view) const -> void;
		auto Feedback() const -> FFeedbackContext;
		auto NotifyItemsUpdated(FContentChangeBatch) -> void;
		auto Complete(const FAssetWriteResult&, FContentChangeBatch Changes = {}) -> bool;
	private:
		FFeedbackContext::FMessageSink Diagnostics;
		std::function<bool()> MutationAllowed;
		bool bAccepting = true;
		std::function<void(FContentChangeBatch)> ItemUpdates;
	};
}
