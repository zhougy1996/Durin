#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "Misc/FeedbackContext.h"

namespace Durin::Editor
{
	// Shared startup/modal content; returns Cancel. Hide elapsed time for projections without timing data.
	DURINED_API auto DrawSlowTaskProgress(const FSlowTaskSnapshot& Task, bool bShowCancel = false, bool bShowElapsed = true) -> bool;

	// Modal presentation for either business snapshots or a feedback context; never pumps host work.
	class FSlowTaskDialogPresenter
	{
	public:
		// A missing or terminal snapshot closes the modal. A cancel click is returned to its owner.
		DURINED_API auto Draw(const std::optional<FSlowTaskSnapshot>& Task) -> bool;
		DURINED_API auto Draw(FFeedbackContext& Context) -> bool;
	};
}
