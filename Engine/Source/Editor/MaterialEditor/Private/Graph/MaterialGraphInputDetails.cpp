#include "MaterialGraphCanvas.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScene.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Widgets/MaterialDetailsStyle.h"
#include "Graph/MaterialEditingOperations.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphCommands.h"
#include "Materials/MaterialExpressionInputs.h"
#include "MaterialGraphValueTypes.h"
#include "Asset/AssetPicker.h"
#include "Math/Color.h"
#include "Texture/Texture2D.h"
#include "ThirdParty/ImGui/imgui_internal.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		auto GetNodeLiteralRevision(const DObject* Owner) -> uint64
		{
			if (const auto* Material = Cast<DMaterial>(Owner)) return Material->GetMaterialContentRevision();
			if (const auto* Function = Cast<DMaterialFunction>(Owner)) return Function->GetFunctionRevision();
			return 0;
		}
	}

	auto FMaterialGraphCanvas::CommitParameterValue(const FMaterialParameterDefinition& Parameter,
		const FMaterialParameterValue& Value) -> std::expected<void, std::string>
	{
		std::string Error;
		ParameterFrame = {};
		if (!ParameterControl.Finish(false, &Error)) return std::unexpected(std::move(Error));
		if (!Error.empty()) ReportError(std::move(Error));
		auto* Preview = PreviewParameters.Find(Parameter.Id);
		if (!Preview) return std::unexpected("The material parameter definition is unavailable.");
		const auto Handle = PropertyEditor::MakePropertyHandle(Preview->GetEditTarget());
		if (Handle->SetValue(Value.GetTexture()) != PropertyEditor::FPropertyAccess::Success)
			return std::unexpected(Handle->GetLastError());
		return {};
	}

	auto FMaterialGraphCanvas::DrawParameterTexture(const FMaterialParameterDefinition& Parameter) -> bool
	{
		bool bChanged = false;
		const auto& Texture = Parameter.Value.GetTexture();
		MonaImGui::PropertyEdit::BeginRow("Texture");
		const auto Interaction = AssetPicker::Draw({
			.RequiredClass = Texture.Dimension == EMaterialTextureDimension::TextureCube ? DTextureCube::StaticClass() : DTexture2D::StaticClass(),
			.CurrentSelection = Texture.Texture.Get(), .SearchText = NodeTextureSearch,
			.AssignSelection = [&](DObject* Object, std::string& Error) {
				auto Value = Parameter.Value;
				Value.GetTexture().Texture = Cast<DTexture>(Object);
				const auto Result = CommitParameterValue(Parameter, Value);
				if (!Result) Error = Result.error();
				bChanged = Result.has_value() && Value != Parameter.Value;
				return Result.has_value();
			}});
		MonaImGui::PropertyEdit::EndRow();
		if (!Interaction.Error.empty()) ReportError(Interaction.Error);
		return bChanged;
	}

	auto FMaterialGraphCanvas::DrawParameterValue(
		const FMaterialParameterDefinition& Parameter) -> void
	{
		auto* Material = Cast<DMaterial>(EditorGraph->GetMaterialOrFunction());
		if (!Material) { CancelInteraction(); return; }
		if (Parameter.Type == EMaterialParameterType::Texture)
		{
			DrawParameterTexture(Parameter);
			return;
		}
		auto* Preview = PreviewParameters.Find(Parameter.Id);
		const bool bEditing = Preview && ParameterControl.IsEditingObject(Preview);
		ImGui::PushID(Parameter.Id.ToString().c_str());
		MonaImGui::PropertyEdit::BeginRow("Default value");
		const ImGuiID Widget = ImGui::GetID("##Value");
		using namespace MonaImGui::PropertyEdit;
		FPropertyInteractionState Interaction;
		auto Value = bEditing ? Preview->GetValue() : Parameter.Value;
		bool Edited = false;
		const float Minimum = Parameter.bHasRange ? Parameter.MinimumValue : 0.0f;
		const float Maximum = Parameter.bHasRange ? Parameter.MaximumValue : 0.0f;
		const auto Flags = Parameter.bHasRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
		if (Parameter.Type == EMaterialParameterType::Scalar)
		{
			Edited = ImGui::DragFloat("##Value", &Value.GetScalar(),
				Parameter.Presentation == EMaterialParameterPresentation::Integer ? 1.0f : 0.01f,
				Minimum, Maximum, Parameter.Presentation == EMaterialParameterPresentation::Integer ? "%.0f" : "%.4g", Flags);
			if (Parameter.Presentation == EMaterialParameterPresentation::Integer)
			{
				Value.GetScalar() = std::round(Value.GetScalar());
				if (Parameter.bHasRange) Value.GetScalar() = std::clamp(Value.GetScalar(), Minimum, Maximum);
			}
		}
		else if (Parameter.Type == EMaterialParameterType::Vector4)
		{
			const auto V = Value.GetVector4();
			float Components[]{static_cast<float>(V.x), static_cast<float>(V.y), static_cast<float>(V.z), static_cast<float>(V.w)};
			if (Parameter.Presentation == EMaterialParameterPresentation::Color)
			{
				FLinearColor Color(Components[0], Components[1], Components[2], Components[3]);
				Edited = DrawColorPicker(Color, EColorPickerMode::LinearHDR, true, false, &Interaction);
				if (Edited) Value.GetVector4() = FVector4(Color.R, Color.G, Color.B, Color.A);
			}
			else
			{
				Edited = ImGui::DragFloat4("##Value", Components, 0.01f, Minimum, Maximum, "%.4g", Flags);
				Value.GetVector4() = FVector4(Components[0], Components[1], Components[2], Components[3]);
			}
		}
		const bool Active = ImGui::IsItemActive();
		const bool Deactivated = ImGui::IsItemDeactivatedAfterEdit();
		if (Interaction.Id == 0) Interaction = FPropertyInteractionState::FromWidget(Widget, {Active, Deactivated}, false, Edited);
		if (Preview && (Edited || bEditing))
		{
			std::optional<FParameterInput> Proposed;
			if (Edited)
			{
				if (Parameter.Type == EMaterialParameterType::Scalar) Proposed = Value.GetScalar();
				else Proposed = Value.GetVector4();
			}
			ParameterFrame.Observe(Interaction.Id, {PropertyEditor::MakePropertyHandle(Preview->GetEditTarget()),
				Interaction.Boundary == EPropertyInteractionBoundary::Popup}, Interaction.End, std::move(Proposed));
		}
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto FMaterialGraphCanvas::EndParameterFrame() -> void
	{
		EndParameterTextFrame();
		EndNodeLiteralFrame();
		EndRootInputFrame();
		std::string Error;
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ParameterFrame = {};
			ParameterControl.Finish(false, &Error);
		}
		else ApplyParameterFrame(Error);
		if (!Error.empty()) ReportError(std::move(Error));
	}

	auto FMaterialGraphCanvas::ApplyParameterFrame(std::string& Error) -> bool
	{
		return ParameterFrame.EndFrame(false,
			[&](ImGuiID, const FParameterWidget& Widget) {
				return ParameterControl.BeginGesture(Widget.Handle, true, Widget.bColorPicker, &Error);
			},
			[&](const FParameterInput& Value) {
				PropertyEditor::FPropertyControl::FInput Input(ParameterControl, &Error);
				if (!Input) return false;
				const auto Result = std::visit([&](const auto& V) { return Input->SetValue(V, PropertyEditor::FPropertyControl::InteractiveFlags); }, Value);
				if (Result == PropertyEditor::FPropertyAccess::Success) return true;
				Error = Input->GetLastError();
				ParameterFrame = {};
				ParameterControl.Finish(false);
				return false;
			},
			[&](bool bCancel) { ParameterControl.Finish(bCancel, &Error); return !ParameterControl.IsActive(); });
	}

	auto FMaterialGraphCanvas::DrawParameterText(const char* Label, EParameterTextField Field, const FGuid& TargetId,
		std::string_view Value) -> void
	{
		std::array<char, MaterialProgramMaxDisplayNameBytes + 1> Buffer{};
		std::copy_n(Value.data(), std::min(Value.size(), Buffer.size() - 1), Buffer.data());
		bool bActive = false;
		const bool bEdited = DetailsStyle::EditRow(Label, [&] {
			const bool bChanged = ImGui::InputText("##Value", Buffer.data(), Buffer.size());
			bActive = ImGui::IsItemActive();
			return bChanged;
		});
		const bool bSameTarget = ParameterTextDraft && ParameterTextDraft->TargetId == TargetId
			&& ParameterTextDraft->Field == Field;
		if (!bEdited)
		{
			if (bSameTarget) ParameterTextDraft.reset();
			return;
		}
		if (!bSameTarget) ParameterTextDraft = FParameterTextDraft{Field, TargetId, std::string(Value)};
		ParameterTextDraft->Text = Buffer.data();
		ParameterTextDraft->ActiveFrame = bActive ? ImGui::GetFrameCount() : -1;
	}

	auto FMaterialGraphCanvas::ReplaceParameterDefinition(const DMaterialExpressionParameter& Expression,
		const FMaterialParameterDefinition& Definition) -> FMaterialGraphCommandResult
	{
		const auto Duplicated = DuplicateObject(&Expression, nullptr, NAME_None);
		if (!Duplicated) return GraphEditInternals::RejectCommand(ToString(Duplicated.error()));
		TStrongObjectPtr<DMaterialExpressionParameter> Candidate(Duplicated.value());
		if (const auto Applied = Candidate->SetParameterDefinition(Definition); !Applied)
			return GraphEditInternals::RejectCommand(Applied.error());
		return EditorGraph->ReplaceExpression(*Candidate);
	}

	auto FMaterialGraphCanvas::EndParameterTextFrame() -> void
	{
		if (!ParameterTextDraft) return;
		auto* Material = Cast<DMaterial>(EditorGraph->GetMaterialOrFunction());
		if (!Material || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ParameterTextDraft.reset();
			return;
		}
		// Keep the draft attached to its original parameter or node when the row disappears.
		if (ParameterTextDraft->ActiveFrame == ImGui::GetFrameCount()) return;
		const auto Draft = *std::exchange(ParameterTextDraft, std::nullopt);
		const auto& Expressions = Material->GetExpressionCollection().Expressions;
		const auto It = std::ranges::find_if(Expressions, [&](const auto& Entry) {
			const auto* Parameter = Cast<DMaterialExpressionParameter>(Entry.Get());
			return Parameter && (Draft.Field == EParameterTextField::Binding
				? Parameter->Id : Parameter->Metadata.Id) == Draft.TargetId;
		});
		if (It == Expressions.end()) return;
		const auto& Expression = *Cast<DMaterialExpressionParameter>(It->Get());
		auto Definition = Expression.GetParameterDefinition();
		switch (Draft.Field)
		{
		case EParameterTextField::Name:
			if (Definition.Name.ToString() == Draft.Original)
				CheckCommand(MaterialEditing::RenameParameter(*Material, Definition.Id, FName(Draft.Text)));
			return;
		case EParameterTextField::DisplayName:
			if (Definition.DisplayName != Draft.Original) return;
			Definition.DisplayName = Draft.Text;
			break;
		case EParameterTextField::Group:
			if (Definition.GroupName.ToString() != Draft.Original) return;
			Definition.GroupName = FName(Draft.Text);
			break;
		case EParameterTextField::Binding:
			if (Definition.Name.ToString() != Draft.Original) return;
			Definition.Name = FName(Draft.Text);
			break;
		}
		CheckCommand(ReplaceParameterDefinition(Expression, Definition));
	}

	auto FMaterialGraphCanvas::EndRootInputFrame() -> void
	{
		const auto* Material = Cast<DMaterial>(EditorGraph->GetMaterialOrFunction());
		const bool bStale = RootInputDraft && (!Material || RootInputRevision != Material->GetMaterialContentRevision());
		if (!bStale && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			RootInputFrame.Finish(false, [this](bool bCancel) { return FinishRootInputEdit(bCancel); });
			return;
		}
		RootInputFrame.EndFrame(bStale,
			[this](const FRootInputControlId& Id, uint64 Revision) {
				RootInputRevision = Revision;
				RootInputDraft.emplace(Id.first, FMaterialInputDefault{});
				return true;
			},
			[this](const FMaterialInputDefault& Value) {
				RootInputDraft->second = Value;
				return true;
			},
			[this](bool bCancel) { return FinishRootInputEdit(bCancel); });
	}

	auto FMaterialGraphCanvas::FinishRootInputEdit(bool bCancel) -> bool
	{
		const auto Draft = std::exchange(RootInputDraft, std::nullopt);
		auto* Material = Cast<DMaterial>(EditorGraph->GetMaterialOrFunction());
		if (!bCancel && Draft && Material && RootInputRevision == Material->GetMaterialContentRevision())
		{
			const auto Result = GraphEditInternals::WriteInputDefault(Material, Draft->first, Draft->second);
			CheckCommand(Result);
			if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded && Services.RootInputCommitted)
				Services.RootInputCommitted(RootInputRevision);
		}
		return true;
	}

	auto FMaterialGraphCanvas::DrawRootInputValue(const FMaterialGraphPinView& Pin, uint64 Revision,
		FMaterialInputDefault Value, float Width) -> void
	{
		using namespace MonaImGui::PropertyEdit;
		using P = EMaterialProperty;
		if (RootInputDraft && RootInputDraft->first == Pin.MaterialProperty && RootInputRevision == Revision)
			Value = RootInputDraft->second;
		FPropertyInteractionState Interaction;
		const auto Widget = ImGui::GetID("##Value");
		bool bEdited = false;
		ImGui::BeginGroup();
		ImGui::SetNextItemWidth(Width);
		if (Pin.MaterialProperty == P::MP_BaseColor || Pin.MaterialProperty == P::MP_EmissiveColor)
		{
			FLinearColor Color(Value.Literal.X, Value.Literal.Y, Value.Literal.Z, 1.0f);
			const auto Mode = Pin.MaterialProperty == P::MP_EmissiveColor ? EColorPickerMode::LinearHDR : EColorPickerMode::Linear;
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
			if (ImGui::BeginTable("##Swatch", 1, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX, {Width, 0.0f}))
			{
				ImGui::TableNextColumn();
				bEdited = DrawColorPicker(Color, Mode, false, false, &Interaction);
				ImGui::EndTable();
			}
			ImGui::PopStyleVar();
			if (bEdited) Value.Literal = {Color.R, Color.G, Color.B};
		}
		else if (Value.Type == EMaterialProgramValueType::Float3)
		{
			FVector3f Vector(Value.Literal.X, Value.Literal.Y, Value.Literal.Z);
			FWidgetState State;
			bEdited = EditVectorValue("##Value", Vector, 0.01, &State,
				{.MaximumWidthInEm = Width / ImGui::GetFontSize(), .Format = "%.4g"});
			Interaction = FPropertyInteractionState::FromWidget(Widget, State, false, bEdited);
			if (bEdited) Value.Literal = {Vector.x, Vector.y, Vector.z};
		}
		else
		{
			bEdited = ImGui::DragFloat("##Value", &Value.Literal.X, 0.01f, 0.0f, 0.0f, "%.4g");
			Interaction = FPropertyInteractionState::FromWidget(Widget, {ImGui::IsItemActive(), ImGui::IsItemDeactivatedAfterEdit()}, false, bEdited);
		}
		ImGui::EndGroup();
		RootInputFrame.Observe({*Pin.MaterialProperty, Interaction.Id}, Revision, Interaction.End,
			bEdited ? std::optional(Value) : std::nullopt);
	}


	auto FMaterialGraphCanvas::GetSelectedDetailsNode() const -> DMaterialGraphNode*
	{
		return SelectedNodes.size() == 1 ? EditorGraph->ResolveNode(*SelectedNodes.begin()) : nullptr;
	}

	auto FMaterialGraphCanvas::DrawNodeLiteral(const FMaterialGraphPin* Pin,
		EMaterialProgramValueType Type, const FMaterialProgramLiteral& Value) -> void
	{
		const int Count = static_cast<int>(Type) + 1;
		if (!Pin || Count < 1 || Count > 4) return;
		std::array<float, 4> Components{Value.X, Value.Y, Value.Z, Value.W};
		const bool bEdited = ImGui::InputScalarN("##Value", ImGuiDataType_Float, Components.data(), Count,
			nullptr, nullptr, "%.4g");
		const bool bFinished = DetailsStyle::NumericInputFinished();
		if (bEdited)
		{
			NodeLiteralDraft = FNodeLiteralDraft{FPinHandle(*Pin),
				{.Kind = EMaterialInputDefaultKind::Literal, .Type = Type,
					.Literal = {Components[0], Components[1], Components[2], Components[3]}},
				GetNodeLiteralRevision(EditorGraph->GetMaterialOrFunction()), bFinished ? -1 : ImGui::GetFrameCount()};
		}
		else if (NodeLiteralDraft && NodeLiteralDraft->Pin == FPinHandle(*Pin))
			NodeLiteralDraft.reset();
	}

	auto FMaterialGraphCanvas::FinishDetailsEdit(bool bCancel) -> bool
	{
		return ExpressionDetails->FinishActiveEdit({.ReportPropertyError = Services.ReportError}, bCancel);
	}

	auto FMaterialGraphCanvas::EndNodeLiteralFrame() -> void
	{
		if (ExpressionDetailsFrame != ImGui::GetFrameCount())
		{
			if (FinishDetailsEdit(ImGui::IsKeyPressed(ImGuiKey_Escape, false))) ExpressionDetails->SetObject(nullptr);
		}
		if (!NodeLiteralDraft) return;
		const auto* Owner = EditorGraph->GetMaterialOrFunction();
		if (!Owner || ImGui::IsKeyPressed(ImGuiKey_Escape, false)
			|| NodeLiteralDraft->Revision != GetNodeLiteralRevision(Owner))
		{
			NodeLiteralDraft.reset();
			return;
		}
		// A canvas selection change can remove the input before Details draws its deactivation frame.
		if (NodeLiteralDraft->ActiveFrame == ImGui::GetFrameCount()) return;
		const auto Draft = *std::exchange(NodeLiteralDraft, std::nullopt);
		if (const auto* Pin = Draft.Pin.Resolve(*EditorGraph))
			CheckCommand(Pin->GetOwningNode()->PinDefaultValueChanged(Pin, Draft.Value));
	}

	auto FMaterialGraphCanvas::DrawSelectionDetails() -> void
	{
		if (!EditorGraph->GetMaterialOrFunction()) { CancelInteraction(); return; }
		auto& Owner = *EditorGraph->GetMaterialOrFunction();
		auto* SelectedNode = GetSelectedDetailsNode();
		if (!SelectedNode) return;
		const TStrongObjectPtr<DMaterialExpression> Expression(SelectedNode->GetMaterialExpression());
		if (!Expression && !SelectedNode->GetView().Node.bMaterialOutput) return;
		const auto& Selected = SelectedNode->GetView();
		ImGui::PushID(Selected.Node.Id.ToString().c_str());
		ImGui::TextWrapped("%s", Selected.PrimaryLabel.c_str());
		if (SelectedDiagnostic && SelectedDiagnostic->NodeId == Selected.Node.Id)
		{
			constexpr std::array UVNames{"Channel", "Scale", "Offset", "Rotation"};
			if (SelectedDiagnostic->UVFieldIndex && *SelectedDiagnostic->UVFieldIndex < UVNames.size())
				ImGui::TextColored({1, .65f, .25f, 1}, "Diagnostic UV field: %s", UVNames[*SelectedDiagnostic->UVFieldIndex]);
			else if (SelectedDiagnostic->InputIndex)
				ImGui::TextColored({1, .65f, .25f, 1}, "Diagnostic input: %u", *SelectedDiagnostic->InputIndex);
		}
		if (Cast<DMaterialExpressionScalarConstant>(Expression.Get()) || Cast<DMaterialExpressionVector2Constant>(Expression.Get())
			|| Cast<DMaterialExpressionVector3Constant>(Expression.Get()) || Cast<DMaterialExpressionVector4Constant>(Expression.Get()))
		{
			ExpressionDetailsFrame = ImGui::GetFrameCount();
			ExpressionDetails->SetObject(Expression.Get());
			ExpressionDetails->Draw({.ReportError = Services.ReportError, .ReportPropertyError = Services.ReportError});
			ImGui::PopID();
			return;
		}
		if (!MonaImGui::PropertyEdit::BeginTable("NodeProperties", DetailsStyle::MakeTableConfig()))
		{
			ImGui::PopID();
			return;
		}
		bool Changed = false;
		const auto Submit = [&](FMaterialGraphCommandResult Result) {
			CheckCommand(Result);
			Changed = Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded;
		};
		auto* Material = Cast<DMaterial>(&Owner);
		if (const auto* ParameterExpression = Cast<DMaterialExpressionParameter>(Expression.Get()); Material && ParameterExpression)
		{
			auto Parameter = ParameterExpression->GetParameterDefinition();
			DrawParameterText("Parameter name", EParameterTextField::Binding, Expression->Id, Parameter.Name.ToString());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Change this node's parameter binding. Existing names use the existing parameter's value.");
			if (!Changed && Parameter.Type == EMaterialParameterType::Texture)
			{
				const bool Cube = Parameter.Value.GetTexture().Dimension == EMaterialTextureDimension::TextureCube;
				int Usage = Cube ? (Parameter.TextureUsage == ETextureUsage::DataMask ? 1 : 0) : static_cast<int>(Parameter.TextureUsage);
				if (DetailsStyle::EditRow("Texture usage", [&] { return ImGui::Combo("##Value", &Usage, Cube ? "Color\0Data / Mask\0" : "Color\0Normal\0Data / Mask\0"); }))
				{
					Parameter.TextureUsage = Cube ? (Usage == 1 ? ETextureUsage::DataMask : ETextureUsage::Color) : static_cast<ETextureUsage>(Usage);
					Submit(ReplaceParameterDefinition(*ParameterExpression, Parameter));
				}
				Changed = DrawParameterTexture(Parameter);
				if (MonaImGui::PropertyEdit::BeginGroup("Sampler", "Sampler settings", ImGuiTreeNodeFlags_None))
				{
					const auto Combo = [&](const char* Label, auto& Value, const char* Names) {
						int Index = static_cast<int>(Value);
						if (Changed || !DetailsStyle::EditRow(Label, [&] { return ImGui::Combo("##Value", &Index, Names); })) return;
						Value = static_cast<std::remove_reference_t<decltype(Value)>>(Index);
						const auto Result = CommitParameterValue(Parameter, Parameter.Value);
						if (!Result) ReportError(Result.error());
						Changed = Result.has_value();
					};
					Combo("Missing texture", Parameter.Value.GetTexture().TextureFallback, Parameter.Value.GetTexture().Dimension == EMaterialTextureDimension::TextureCube ? "White\0Black\0" : "White\0Black\0Flat normal\0");
					Combo("Min filter", Parameter.Value.GetTexture().SamplerState.MinFilter, "Nearest\0Linear\0Nearest mip nearest\0Linear mip nearest\0Nearest mip linear\0Linear mip linear\0");
					Combo("Mag filter", Parameter.Value.GetTexture().SamplerState.MagFilter, "Nearest\0Linear\0");
					Combo("Address U", Parameter.Value.GetTexture().SamplerState.AddressU, "Repeat\0Mirrored repeat\0Clamp\0");
					Combo("Address V", Parameter.Value.GetTexture().SamplerState.AddressV, "Repeat\0Mirrored repeat\0Clamp\0");
					Combo("Address W", Parameter.Value.GetTexture().SamplerState.AddressW, "Repeat\0Mirrored repeat\0Clamp\0");
					MonaImGui::PropertyEdit::EndGroup();
				}
			}
			else if (!Changed) DrawParameterValue(Parameter);
		}
		std::optional<EMaterialTextureReadMode> SamplingMode;
		if (const auto* ReadMode = Expression ? Expression->GetTextureSampleReadMode() : nullptr; ReadMode && !Changed)
		{
			SamplingMode = *ReadMode;
			int Mode = static_cast<int>(*SamplingMode);
			if (DetailsStyle::EditRow("Sampling", [&] { return ImGui::Combo("##Value", &Mode, "Implicit\0Level\0Bias\0Grad\0"); }))
			{
				const auto Duplicated = DuplicateObject(Expression.Get(), nullptr, NAME_None);
				if (!Duplicated) Submit(GraphEditInternals::RejectCommand(ToString(Duplicated.error())));
				else
				{
					TStrongObjectPtr<DMaterialExpression> Copy(*Duplicated);
					*Copy->GetTextureSampleReadMode() = static_cast<EMaterialTextureReadMode>(Mode);
					Submit(EditorGraph->ReplaceExpression(*Copy));
				}
			}
		}
		if (const auto* Scene = Cast<DMaterialExpressionSceneTexture>(Expression.Get()); Scene && !Changed)
		{
			int Id = static_cast<int>(Scene->SceneTextureId);
			bool Filtered = Scene->bFiltered;
			const bool IdChanged = DetailsStyle::EditRow("Scene input", [&] { return ImGui::Combo("##Value", &Id, "PostProcessInput0\0Scene Depth\0"); });
			if (Id == static_cast<int>(EMaterialSceneTextureId::SceneDepth)) Filtered = false;
			const bool FilterChanged = Id == static_cast<int>(EMaterialSceneTextureId::PostProcessInput0)
				&& DetailsStyle::EditRow("Linear filtering", [&] { return ImGui::Checkbox("##Value", &Filtered); });
			if (IdChanged || FilterChanged)
			{
				TStrongObjectPtr<DMaterialExpressionSceneTexture> Copy(DuplicateObject(Scene, nullptr, NAME_None).value());
				Copy->SceneTextureId = static_cast<EMaterialSceneTextureId>(Id); Copy->bFiltered = Filtered;
				Submit(EditorGraph->ReplaceExpression(*Copy));
			}
		}
		if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get()); Swizzle && !Changed)
		{
			DetailsStyle::EditRow("Channels", [&] {
				constexpr std::array Labels{"R", "G", "B", "A"};
				for (uint8 Channel = 0; Channel < 4; ++Channel)
				{
					if (Channel) ImGui::SameLine();
					bool Enabled = std::ranges::contains(Swizzle->Components, Channel);
					ImGui::BeginDisabled(Enabled && Swizzle->Components.size() == 1);
					const bool Edited = ImGui::Checkbox(Labels[Channel], &Enabled);
					ImGui::EndDisabled();
					if (!Edited) continue;
					std::vector<uint8> Mask;
					for (uint8 Candidate = 0; Candidate < 4; ++Candidate)
						if (Candidate == Channel ? Enabled : std::ranges::contains(Swizzle->Components, Candidate))
							Mask.push_back(Candidate);
					Submit(SelectedNode->SetSwizzleComponents(Mask));
					return true;
				}
				return false;
			});
		}
		if (const auto* Surface = Cast<DMaterialExpressionGetMaterialAttributes>(Expression.Get()); Surface && !Changed)
		{
			for (uint32 Index = 0; Index < FMaterialAttributeDefinitionMap::GetDefinitions().size() && !Changed; ++Index)
			{
				const auto ID = FMaterialAttributeDefinitionMap::GetDefinitions()[Index].AttributeID;
				bool Enabled = std::ranges::contains(Surface->AttributeIDs, ID);
				if (!DetailsStyle::EditRow(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Name.data(), [&] { return ImGui::Checkbox("##Value", &Enabled); })) continue;
				Submit(SelectedNode->SetAttributeOutputEnabled(ID, Enabled));
			}
		}
		if (const auto* Surface = Cast<DMaterialExpressionSetMaterialAttributes>(Expression.Get()); Surface && !Changed)
		{
			for (uint32 Index = 0; Index < FMaterialAttributeDefinitionMap::GetDefinitions().size() && !Changed; ++Index)
			{
				const auto Attribute = FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property;
				const auto Binding = std::ranges::find(Surface->Attributes, Attribute, &FMaterialExpressionAttributeBinding::GetProperty);
				const bool bKeepBase = Binding == Surface->Attributes.end()
					|| !Binding->Input.IsConnected();
				DetailsStyle::EditRow(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Name.data(), [&] {
					if (!ImGui::BeginCombo("##Value", bKeepBase ? "Keep base value" : "Connected")) return false;
					if (ImGui::Selectable("Keep base value", bKeepBase)) Submit(SelectedNode->ClearAttributeInput(Attribute));
					if (Changed)
					{
						ImGui::EndCombo();
						return true;
					}
					for (const auto& SourceNode : EditorGraph->GetNodes())
					{
						const auto& Source = SourceNode->GetView();
						for (const auto& Output : SourceNode->GetPins())
						{
							if (Output.Direction != EMaterialGraphPinDirection::Output
								|| !SelectedNode->CanSetAttributeInput(Attribute, &Output).Allows(true)) continue;
							const auto& Pin = Source.Outputs[Output.DisplayIndex];
							if (!ImGui::Selectable(std::format("{}: {}##{}", Source.PrimaryLabel, Pin.Name,
								Output.GetId().ToString()).c_str())) continue;

							std::string Error;
							Changed = SelectedNode->SetAttributeInput(Attribute, Output, true, &Error);
							if (!Error.empty()) ReportError(std::move(Error));
							ImGui::EndCombo();
							return Changed;
						}
					}
					ImGui::EndCombo();
					return Changed;
				});
			}
		}
		const auto DrawInput = [&](size_t Index, bool bInactiveRoot = false) {
			// A property edit can synchronously reconstruct the node during this row.
			const auto Pin = Selected.Inputs[Index];
			const bool bRoot = Selected.Node.bMaterialOutput;
			const bool bRootAttributes = bRoot && Pin.MaterialProperty == EMaterialProperty::MP_MaterialAttributes;
			if (Pin.SourceType > EMaterialProgramValueType::Float4 && !bRootAttributes) return;
			ImGui::PushID(static_cast<int>(Pin.InputIndex));
			auto Value = Pin.InlineDefault;
			if (Value.Kind == EMaterialInputDefaultKind::None)
			{
				Value.Type = Pin.SourceType;
				if (Pin.PortId.IsValid() && !Pin.bRequired && Pin.Default.Kind == EMaterialFunctionDefaultKind::Numeric)
				{
					Value.Kind = EMaterialInputDefaultKind::Literal;
					Value.Literal = Pin.Default.Numeric;
				}
			}
			const bool bInheritedDynamic = Pin.PortId.IsValid() && !Pin.bRequired
				&& Value.Kind == EMaterialInputDefaultKind::None && Pin.Default.Kind != EMaterialFunctionDefaultKind::None;
			const auto* InputPin = SelectedNode->GetInputPin(static_cast<int32>(Index));
			const auto* Upstream = InputPin->LinkedTo.empty() ? nullptr : InputPin->LinkedTo.front();
			const bool bConnected = Upstream != nullptr;
			const bool bInheritedContext = Pin.bSupportsConstant && !Pin.bUseConstant
				&& (Selected.Node.IsSampleUVInput(Pin.InputIndex)
					|| (Selected.Node.Opcode == EMaterialProgramOpcode::SetMaterialAttributes && Pin.InputIndex != 0));
			const bool bRootNormal = bRoot && Pin.MaterialProperty == EMaterialProperty::MP_Normal;
			const auto EditValue = [&](float Width) {
				if (bRootNormal)
				{
					ImGui::TextUnformatted("Default tangent-space normal (0, 0, 1)");
					return;
				}
				if (bInheritedContext)
				{
					ImGui::TextUnformatted(Selected.Node.IsSampleUVInput(Pin.InputIndex) ? "Mesh UV0" : "Base Surface value");
					return;
				}
				if (bInheritedDynamic)
				{
					ImGui::TextWrapped("%s", DescribeFunctionDefault(Pin.Default).c_str());
					return;
				}
				if (bRootAttributes)
				{
					ImGui::TextDisabled("No connection");
					return;
				}
				if (bRoot && Pin.bSupportsConstant && Material)
				{
					DrawRootInputValue(Pin, Material->GetMaterialContentRevision(), Value, Width);
					return;
				}
				ImGui::SetNextItemWidth(Width);
				DrawNodeLiteral(InputPin, Value.Type, Value.Literal);
			};
			auto Label = Pin.Name;
			if (SamplingMode)
			{
				if (Pin.bActive && Pin.Name == "Mip Value")
					Label = *SamplingMode == EMaterialTextureReadMode::Level ? "Mip Level" : "Mip Bias";
			}
			else if (!bRoot && Pin.bSupportsConstant) Label += Pin.bUseConstant ? " (constant)" : " (inherited)";
			const char* Tooltip = nullptr;
			if (bInactiveRoot) Tooltip = "Inactive for the current material settings.";
			else if (bRoot && Pin.bSupportsConstant && !bConnected)
				Tooltip = Pin.bUseConstant ? "Local constant" : "Using the material definition default";
			MonaImGui::PropertyEdit::BeginRow(Label.c_str(), false, 0.0f, Tooltip);
			const float MenuWidth = ImGui::GetFrameHeight();
			const bool bReset = bRoot && !bConnected && Pin.bUseConstant && !bInactiveRoot;
			const auto& Style = ImGui::GetStyle();
			const float ResetWidth = bReset ? ImGui::CalcTextSize("Reset").x + 2.0f * Style.FramePadding.x + Style.ItemSpacing.x : 0.0f;
			const float ActionsWidth = bInactiveRoot ? 0.0f : MenuWidth + Style.ItemSpacing.x + ResetWidth;
			const float ValueWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - ActionsWidth);
			if (Upstream)
			{
				const auto& Source = Upstream->GetOwningNode()->GetView();
				const auto& Output = Source.Outputs[Upstream->DisplayIndex];
				const auto SourceLabel = std::format("{}.{}", Source.PrimaryLabel, Output.Name);
				if (ImGui::Button((SourceLabel + "###Source").c_str(), {ValueWidth, 0.0f}))
					SelectAndFrame(Source.Node.Id);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Connected to %s. Click to locate.", SourceLabel.c_str());
			}
			else if (bInactiveRoot)
			{
				const auto& Literal = Pin.RetainedConstant.Literal;
				if (Pin.SourceType == EMaterialProgramValueType::Float3)
					ImGui::TextDisabled("Constant (%g, %g, %g)", Literal.X, Literal.Y, Literal.Z);
				else ImGui::TextDisabled("Constant %g", Literal.X);
			}
			else EditValue(ValueWidth);
			if (bInactiveRoot)
			{
				MonaImGui::PropertyEdit::EndRow();
				ImGui::PopID();
				return;
			}
			bool bResetRequested = false;
			if (bReset)
			{
				ImGui::SameLine();
				bResetRequested = ImGui::Button("Reset");
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use the material definition default.");
			}
			ImGui::SameLine();
			if (ImGui::Button("...", {MenuWidth, 0.0f})) ImGui::OpenPopup("InputActions");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Input actions");
			MonaImGui::PropertyEdit::EndRow();
			if (bResetRequested) Submit(SelectedNode->SetPinUseConstant(InputPin, false));
			if (ImGui::BeginPopup("InputActions"))
			{
				if (!Changed && bConnected)
				{
					if (ImGui::MenuItem("Inline constant", nullptr, false, !bRootNormal && !bRootAttributes)) Submit(SelectedNode->InlineInputNode(InputPin));
					if (!Changed && ImGui::MenuItem("Disconnect")) Changed = BreakPinLinks(InputPin);
				}
				else if (!Changed)
				{
					if (ImGui::MenuItem("Extract constant", nullptr, false, Pin.InlineDefault.Kind == EMaterialInputDefaultKind::Literal)) Changed = CheckCommand(SelectedNode->ExtractInputDefault(InputPin));
					if (!Changed && Material && ImGui::MenuItem("Promote to parameter", nullptr, false, !bInheritedDynamic && !bInheritedContext
						&& (!Pin.MaterialProperty || MaterialEditing::CanPromoteSurfaceOutput(*Material, *Pin.MaterialProperty))))
						Changed = CheckCommand(EditorGraph->PromoteInputToParameter(InputPin));
				}
				if (!Changed && Pin.bSupportsConstant && ImGui::MenuItem(Pin.bUseConstant ? "Use definition default" : "Use retained constant"))
					Submit(SelectedNode->SetPinUseConstant(InputPin, !Pin.bUseConstant));
				if (!Changed && bInheritedDynamic && ImGui::MenuItem("Override with constant"))
				{
					Value.Kind = EMaterialInputDefaultKind::Literal;
					Submit(SelectedNode->PinDefaultValueChanged(InputPin, Value));
				}
				if (!Changed && Pin.PortId.IsValid() && !Pin.bRequired
					&& Pin.InlineDefault.Kind != EMaterialInputDefaultKind::None && ImGui::MenuItem("Use function default"))
					Submit(SelectedNode->PinDefaultValueChanged(InputPin, {}));
				if (!Changed && bConnected && !bRootAttributes && ImGui::BeginMenu("When disconnected"))
				{
					ImGui::TextUnformatted("Value used after disconnecting");
					EditValue(ImGui::GetFontSize() * 16.0f);
					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		};
		std::vector<size_t> UnusedConnectedInputs;
		std::vector<size_t> InactiveRootInputs;
		for (size_t Index = 0; !Changed && Index < Selected.Inputs.size(); ++Index)
		{
			if (Selected.Node.bMaterialOutput && !Selected.Inputs[Index].bActive)
			{
				if (Selected.Inputs[Index].bUseConstant || !SelectedNode->GetInputPin(static_cast<int32>(Index))->LinkedTo.empty())
					InactiveRootInputs.push_back(Index);
				continue;
			}
			if (SamplingMode && !Selected.Inputs[Index].bActive)
			{
				if (!SelectedNode->GetInputPin(static_cast<int32>(Index))->LinkedTo.empty())
					UnusedConnectedInputs.push_back(Index);
				continue;
			}
			DrawInput(Index);
		}
		if (!Changed && !UnusedConnectedInputs.empty()
			&& MonaImGui::PropertyEdit::BeginGroup("UnusedSamplingInputs", "Unused inputs", ImGuiTreeNodeFlags_None))
		{
			for (const size_t Index : UnusedConnectedInputs)
			{
				if (Changed) break;
				DrawInput(Index);
			}
			MonaImGui::PropertyEdit::EndGroup();
		}
		if (!Changed && !InactiveRootInputs.empty()
			&& MonaImGui::PropertyEdit::BeginGroup("InactiveRootInputs", "Inactive inputs", ImGuiTreeNodeFlags_None))
		{
			for (const size_t Index : InactiveRootInputs) DrawInput(Index, true);
			MonaImGui::PropertyEdit::EndGroup();
		}
		MonaImGui::PropertyEdit::EndTable();
		ImGui::PopID();
	}
}
