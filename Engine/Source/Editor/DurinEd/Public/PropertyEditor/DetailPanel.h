#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "PropertyEditor/DetailLayoutBuilder.h"

namespace Durin::Editor
{
	// Configures a shared object-details draw with optional host-specific rows.
	struct FDetailPanelOptions
	{
		std::string_view SearchText;
		const char* PropertyTableId = "DetailsProperties";
		IDetailHostContext* HostContext = nullptr;
		std::function<void(FDetailLayoutBuilder&)> ExtendLayout;
		bool bShowEmptyMessage = true;
	};

	// Reports draw readiness and changes without transferring ownership of the property view.
	struct FDetailPanelResult
	{
		uint32 VisibleRowCount = 0;
		bool bChanged = false;
		bool bOwnerReady = false;
		bool bTableDrawn = false;
		bool bActionInvoked = false;
		bool bRefreshRequested = false;
	};

	DURINED_API auto DrawDetailPanel(FPropertyView& View, const FPropertyViewContext& Context,
		DObject* Object, const FDetailPanelOptions& Options = {}) -> FDetailPanelResult;
}
