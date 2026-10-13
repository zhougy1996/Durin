#include "PropertyEditor/DetailPanel.h"
#include "Profiling/Profiling.h"
#include "MonaImGui.h"
#include "Widgets/MonaImGuiPropertyTable.h"

namespace Durin::Editor
{
	auto DrawDetailPanel(FPropertyView& View, const FPropertyViewContext& Context,
		DObject* Object, const FDetailPanelOptions& Options) -> FDetailPanelResult
	{
		FDetailPanelResult Result;
		Result.bOwnerReady = View.HandleOwnerContext(Context, Object);
		if (!Result.bOwnerReady) return Result;
		FDetailLayoutBuilder Layout(Options.SearchText, Options.HostContext);
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Details.BuildLayout");
			Layout.ApplyRegisteredCustomizations(Object);
			if (Options.ExtendLayout) Options.ExtendLayout(Layout);
			Layout.AddDefaultProperties(Object);
		}
		if (!MonaImGui::PropertyEdit::BeginTable(Options.PropertyTableId)) return Result;
		Result.bTableDrawn = true;
		const auto Declared = Layout.DrawRows(View, Context);
		Result.VisibleRowCount = Declared.VisibleRowCount;
		Result.bChanged = Declared.bChanged;
		MonaImGui::PropertyEdit::EndTable();
		if (Options.bShowEmptyMessage && Result.VisibleRowCount == 0)
			ImGui::TextDisabled(!Options.SearchText.empty() ? "No properties match the current search."
				: "This object has no reflected Edit properties.");
		return Result;
	}
}
