#include "PropertyEditor/DetailPanel.h"
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
		Layout.ApplyRegisteredCustomizations(Object);
		if (Options.ExtendLayout) Options.ExtendLayout(Layout);
		if (!MonaImGui::PropertyEdit::BeginTable(Options.PropertyTableId)) return Result;
		Result.bTableDrawn = true;
		const auto Declared = Layout.DrawRows(View, Context);
		Result.VisibleRowCount = Declared.VisibleRowCount;
		Result.bChanged = Declared.bChanged;
		if (!Layout.IsReplacingDefaultProperties())
		{
			const auto Defaults = View.EditObject(Context, Object, {
				.SearchText = Options.SearchText,
				.Filter = [&Layout, Object](const FProperty& Property, uint32 Index) {
					return Layout.ShouldDrawDefaultProperty(Object, Property, Index);
				}, .bCreatePropertyTable = false, .bShowEmptyMessage = false});
			Result.VisibleRowCount += Defaults.VisiblePropertyCount;
			Result.bChanged |= Defaults.bChanged;
		}
		MonaImGui::PropertyEdit::EndTable();
		if (Options.bShowEmptyMessage && Result.VisibleRowCount == 0)
			ImGui::TextDisabled(!Options.SearchText.empty() ? "No properties match the current search."
				: "This object has no reflected Edit properties.");
		return Result;
	}
}
