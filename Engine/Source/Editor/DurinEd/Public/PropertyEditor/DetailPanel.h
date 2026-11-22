#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "PropertyEditor/DetailLayoutBuilder.h"

namespace Durin
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

	class IDetailsView
	{
	public:
		virtual ~IDetailsView() = default;
		virtual auto SetObject(DObject* Object) -> void = 0;
		virtual auto Draw(const FPropertyViewContext& Context, const FDetailPanelOptions& Options = {}) -> FDetailPanelResult = 0;
		virtual auto FinishActiveEdit(const FPropertyViewContext& Context, bool bCancel) -> bool = 0;
	};

	DURINED_API auto CreateDetailView() -> std::unique_ptr<IDetailsView>;

	DURINED_API auto DrawDetailPanel(FPropertyView& View, const FPropertyViewContext& Context,
		DObject* Object, const FDetailPanelOptions& Options = {}) -> FDetailPanelResult;
}
