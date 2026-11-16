#pragma once
#include "CoreMinimal.h"
#include "ApplicationCoreAPI.h"

namespace Durin
{
	// Native synchronous decision; unavailable interaction never implies consent.
	enum class EMessageDialogResult : uint8 { Yes, No, Cancel, Error };
	APPLICATIONCORE_API auto ShowMessageDialog(std::string_view Title, std::string_view Message,
		bool bAllowDecline, bool bAllowUserInteraction) -> EMessageDialogResult;
}
