#include "PropertyEditor/DetailPanel.h"
#include "DObject/WeakObjectPtr.h"
#include "Profiling/Profiling.h"
#include "MonaImGui.h"
#include "Widgets/MonaImGuiPropertyTable.h"

namespace Durin
{
	namespace
	{
		class FDetailsView final : public IDetailsView
		{
		public:
			auto SetObject(DObject* Object) -> void override { SelectedObject = TWeakObjectPtr<DObject>(Object); }

			auto Draw(const FPropertyViewContext& Context, const FDetailPanelOptions& Options) -> FDetailPanelResult override
			{
				return DrawDetailPanel(PropertyView, Context, SelectedObject.Get(), Options);
			}

			auto FinishActiveEdit(const FPropertyViewContext& Context, bool bCancel) -> bool override
			{
				return PropertyView.FinishActiveEdit(&Context, bCancel);
			}

		private:
			TWeakObjectPtr<DObject> SelectedObject;
			FPropertyView PropertyView;
		};
	}

	auto CreateDetailView() -> std::unique_ptr<IDetailsView>
	{
		return std::make_unique<FDetailsView>();
	}

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
			Layout.AddDefaultFunctions();
		}
		if (!MonaImGui::PropertyEdit::BeginTable(Options.PropertyTableId))
		{
			View.EndInteractionFrame(Context);
			return Result;
		}
		Result.bTableDrawn = true;
		const auto Declared = Layout.DrawRows(View, Context);
		Result.VisibleRowCount = Declared.VisibleRowCount;
		Result.bChanged = Declared.bChanged;
		MonaImGui::PropertyEdit::EndTable();
		View.EndInteractionFrame(Context, &Result.bChanged);
		if (Declared.FunctionRequest)
		{
			const auto Action = ExecuteEditorFunction(View, Context, *Declared.FunctionRequest);
			Result.bActionInvoked = Action.bInvoked;
			Result.bRefreshRequested = Action.bInvoked;
		}
		if (Options.bShowEmptyMessage && Result.VisibleRowCount == 0)
			ImGui::TextDisabled(!Options.SearchText.empty() ? "No properties or actions match the current search."
				: "This object has no reflected Edit properties or editor actions.");
		return Result;
	}
}
