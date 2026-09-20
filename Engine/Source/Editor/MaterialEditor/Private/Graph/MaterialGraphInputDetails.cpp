#include "MaterialGraphCanvas.h"
#include "Widgets/MaterialDetailsStyle.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphEditSession.h"
#include "MaterialExpressionParameters.h"
#include "MaterialExpressionInputs.h"
#include "MaterialGraphValueTypes.h"
#include "Editor/AssetPicker.h"
#include "Texture/Texture2D.h"
#include <cmath>

namespace Durin::Editor::Material
{
	auto FMaterialGraphCanvas::DrawParameterValue(
		const FMaterialParameterDefinition& Parameter, DTransactor& Transactions) -> void
	{
		const auto& ReportError = Services.ReportError;
		auto* MaterialOwner = Cast<DMaterial>(GraphDocument.GetOwner());
		if (!MaterialOwner) { CancelInteraction(); return; }
		auto& Material = *MaterialOwner;
		if (Parameter.Type == EMaterialParameterType::Texture) return;
		ImGui::PushID(Parameter.Id.ToString().c_str());
		MonaImGui::PropertyEdit::BeginRow("Default value");
		const ImGuiID Widget = ImGui::GetID("##Value");
		auto Value = Parameter.Value;
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
			Edited = Parameter.Presentation == EMaterialParameterPresentation::Color
				? ImGui::ColorEdit4("##Value", Components, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_HDR)
				: ImGui::DragFloat4("##Value", Components, 0.01f, Minimum, Maximum, "%.4g", Flags);
			Value.GetVector4() = FVector4(Components[0], Components[1], Components[2], Components[3]);
		}
		const bool Active = ImGui::IsItemActive();
		const bool Deactivated = ImGui::IsItemDeactivatedAfterEdit();
		const auto Report = [&](const FMaterialGraphCommandResult& Result) {
			if (!Result && ReportError) ReportError(FormatMaterialGraphCommandResult(Result));
			return static_cast<bool>(Result);
		};
		if (Edited)
		{
			if (ParameterSession.IsActive() && (ParameterWidget != Widget || EditingParameterId != Parameter.Id))
				Report(ParameterSession.Commit());
			if (!ParameterSession.IsActive() && Report(ParameterSession.Begin(Material, Parameter.Id, &Transactions)))
			{
				ParameterWidget = Widget;
				EditingParameterId = Parameter.Id;
			}
			if (ParameterSession.IsActive() && ParameterWidget == Widget) Report(ParameterSession.Apply(Value));
		}
		if (ParameterSession.IsActive() && ParameterWidget == Widget)
		{
			ParameterFrame = ImGui::GetFrameCount();
			// Escape may deactivate an ImGui input before this check; cancellation wins.
			if (ImGui::IsKeyPressed(ImGuiKey_Escape)) Report(ParameterSession.Cancel());
			else if (Deactivated || (!Active && Edited)) Report(ParameterSession.Commit());
		}
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto FMaterialGraphCanvas::EndParameterFrame() -> void
	{
		// A hidden/collapsed panel or a changed selection cannot retain a live draft.
		if (ParameterSession.IsActive() && ParameterFrame != ImGui::GetFrameCount()) ParameterSession.Cancel();
	}

	auto FMaterialGraphCanvas::PrepareDetailsView(DObject& Owner) -> const FMaterialGraphView&
	{
		// Refresh here too: Details can be drawn before the canvas or while it is hidden.
		if (auto* Material = Cast<DMaterial>(&Owner)) PrepareView(*Material);
		else if (auto* Function = Cast<DMaterialFunction>(&Owner)) PrepareFunctionView(*Function);
		return ReadModel.GetView();
	}

	auto FMaterialGraphCanvas::DrawSelectionDetails(DTransactor& Transactions) -> void
	{
		const auto& ReportError = Services.ReportError;
		if (!GraphDocument.GetOwner()) { CancelInteraction(); return; }
		auto& Owner = *GraphDocument.GetOwner();
		const auto Selection = GetSelectedProgramNodes();
		if (Selection.size() != 1) return;
		const auto& Document = GraphDocument;
		const FMaterialExpressionCollection* Collection = nullptr;
		if (const auto* Material = Cast<DMaterial>(&Owner)) Collection = &Material->GetExpressionCollection();
		if (const auto* Function = Cast<DMaterialFunction>(&Owner)) Collection = &Function->GetExpressionCollection();
		if (!Collection) return;
		const auto Existing = std::ranges::find(Collection->Expressions, Selection.front(), [](const auto& E) { return E->Id; });
		if (Existing == Collection->Expressions.end()) return;
		const TStrongObjectPtr<DMaterialExpression> Expression(Existing->Get());
		std::optional<GraphEditInternals::FGraphEditSession> Session;
		const auto BeginSelectedEdit = [&]() {
			Session.emplace(Owner);
			const auto It = std::ranges::find(Session->Expressions, Selection.front(), [](const auto& E) { return E->Id; });
			if (It != Session->Expressions.end()) Session->Modify(**It);
			return It;
		};
		const auto& View = PrepareDetailsView(Owner);
		const auto Selected = std::ranges::find(View.Nodes, Selection.front(),
			[](const auto& Entry) { return Entry.Node.Id; });
		if (Selected == View.Nodes.end()) return;
		ImGui::PushID(Selection.front().ToString().c_str());
		ImGui::TextWrapped("%s", Selected->PrimaryLabel.c_str());
		if (SelectedDiagnostic && SelectedDiagnostic->NodeId == Selected->Node.Id)
		{
			constexpr std::array UVNames{"Channel", "Scale", "Offset", "Rotation"};
			if (SelectedDiagnostic->UVFieldIndex && *SelectedDiagnostic->UVFieldIndex < UVNames.size())
				ImGui::TextColored({1, .65f, .25f, 1}, "Diagnostic UV field: %s", UVNames[*SelectedDiagnostic->UVFieldIndex]);
			else if (SelectedDiagnostic->InputIndex)
				ImGui::TextColored({1, .65f, .25f, 1}, "Diagnostic input: %u", *SelectedDiagnostic->InputIndex);
		}
		if (!MonaImGui::PropertyEdit::BeginTable("NodeProperties", DetailsStyle::MakeTableConfig()))
		{
			ImGui::PopID();
			return;
		}
		bool Changed = false;
		const auto Submit = [&](FMaterialGraphCommandResult Result) {
			if (!Result) ReportError(FormatMaterialGraphCommandResult(Result));
			Changed = Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded;
		};
		const auto EditText = [](const char* Label, std::string& Value) {
			std::array<char, MaterialProgramMaxDisplayNameBytes + 1> Buffer{};
			std::copy_n(Value.data(), std::min(Value.size(), Buffer.size() - 1), Buffer.data());
			if (!DetailsStyle::EditRow(Label, [&] { return ImGui::InputText("##Value", Buffer.data(), Buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue); })) return false;
			Value = Buffer.data();
			return true;
		};
		const auto EditLiteralValue = [](EMaterialProgramValueType Type, FMaterialProgramLiteral& Value) {
			std::array<float, 4> Components{Value.X, Value.Y, Value.Z, Value.W};
			const int Count = static_cast<int>(Type) + 1;
			if (Count < 1 || Count > 4 || !ImGui::InputScalarN("##Value", ImGuiDataType_Float, Components.data(), Count,
				nullptr, nullptr, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue)) return false;
			Value = {Components[0], Components[1], Components[2], Components[3]};
			return true;
		};
		const auto EditLiteral = [&](const char* Label, EMaterialProgramValueType Type, FMaterialProgramLiteral& Value) {
			return DetailsStyle::EditRow(Label, [&] { return EditLiteralValue(Type, Value); });
		};
		auto* Material = Cast<DMaterial>(&Owner);
		if (const auto* ParameterExpression = Cast<DMaterialExpressionParameter>(Expression.Get()); Material && ParameterExpression)
		{
			auto Parameter = ParameterExpression->GetParameterDefinition();
			const auto CommitParameter = [&]() {
				auto Created = GraphEditInternals::MakeParameterExpression(Parameter);
				if (!Created) { ReportError(Created.Message); return; }
				auto Replacement = std::move(Created.Expression);
				Replacement->Id = Expression->Id;
				if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get()); Sample && Parameter.Type == EMaterialParameterType::Texture)
				{
					TStrongObjectPtr<DMaterialExpressionTextureSampleParameter2D> Copy(DuplicateObject(Sample, nullptr, NAME_None).Object);
					if (!Copy) { ReportError("Unable to copy the parameter expression."); return; }
					if (const auto Applied = Copy->SetParameterDefinition(Parameter); !Applied) { ReportError(FormatMaterialError(Applied.Error)); return; }
					Submit(Document.ReplaceExpression(*Copy.Get(), &Transactions));
				}
				else Submit(Document.ReplaceExpression(*Replacement.Get(), &Transactions));
			};
			std::string Name = Parameter.Name.ToString();
			if (!Changed && EditText("Parameter name", Name)) { Parameter.Name = FName(Name); CommitParameter(); }
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Change this node's parameter binding. Existing names use the existing parameter's value.");
			if (!Changed && Parameter.Type == EMaterialParameterType::Texture)
			{
				int Usage = static_cast<int>(Parameter.TextureUsage);
				if (DetailsStyle::EditRow("Texture usage", [&] { return ImGui::Combo("##Value", &Usage, "Color\0Normal\0Data / Mask\0"); }))
				{
					Parameter.TextureUsage = static_cast<ETextureUsage>(Usage);
					CommitParameter();
				}
				MonaImGui::PropertyEdit::BeginRow("Texture");
				const auto Picker = AssetPicker::Draw({.RequiredClass = DTexture2D::StaticClass(),
					.CurrentSelection = Parameter.Value.GetTexture().Texture.Get(), .SearchText = NodeTextureSearch,
					.AssignSelection = [&](DObject* Object, std::string& Error) {
						auto Value = Parameter.Value;
						Value.GetTexture().Texture = Cast<DTexture2D>(Object);
						const auto Result = FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id, Value, &Transactions);
						Error = FormatMaterialGraphCommandResult(Result); Changed = Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded;
						return static_cast<bool>(Result);
					}});
				MonaImGui::PropertyEdit::EndRow();
				if (!Picker.Error.empty()) ReportError(Picker.Error);
				if (MonaImGui::PropertyEdit::BeginGroup("Sampler", "Sampler settings", ImGuiTreeNodeFlags_None))
				{
					const auto Combo = [&](const char* Label, auto& Value, const char* Names) {
						int Index = static_cast<int>(Value);
						if (Changed || !DetailsStyle::EditRow(Label, [&] { return ImGui::Combo("##Value", &Index, Names); })) return;
						Value = static_cast<std::remove_reference_t<decltype(Value)>>(Index);
						Submit(FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id, Parameter.Value, &Transactions));
					};
					Combo("Missing texture", Parameter.Value.GetTexture().TextureFallback, "White\0Black\0Flat normal\0");
					Combo("Min filter", Parameter.Value.GetTexture().SamplerState.MinFilter, "Nearest\0Linear\0Nearest mip nearest\0Linear mip nearest\0Nearest mip linear\0Linear mip linear\0");
					Combo("Mag filter", Parameter.Value.GetTexture().SamplerState.MagFilter, "Nearest\0Linear\0");
					Combo("Address U", Parameter.Value.GetTexture().SamplerState.AddressU, "Repeat\0Mirrored repeat\0Clamp\0");
					Combo("Address V", Parameter.Value.GetTexture().SamplerState.AddressV, "Repeat\0Mirrored repeat\0Clamp\0");
					MonaImGui::PropertyEdit::EndGroup();
				}
			}
			else if (!Changed) DrawParameterValue(Parameter, Transactions);
		}
		else
		{
			FMaterialProgramLiteral Value;
			EMaterialProgramValueType Type = EMaterialProgramValueType::Float;
			bool bConstant = true;
			if (const auto* E = Cast<DMaterialExpressionScalarConstant>(Expression.Get())) Value = {E->Value};
			else if (const auto* E = Cast<DMaterialExpressionVector2Constant>(Expression.Get())) { Type = EMaterialProgramValueType::Float2; Value = {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y)}; }
			else if (const auto* E = Cast<DMaterialExpressionVector3Constant>(Expression.Get())) { Type = EMaterialProgramValueType::Float3; Value = {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y), static_cast<float>(E->Value.z)}; }
			else if (const auto* E = Cast<DMaterialExpressionVector4Constant>(Expression.Get())) { Type = EMaterialProgramValueType::Float4; Value = {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y), static_cast<float>(E->Value.z), static_cast<float>(E->Value.w)}; }
			else bConstant = false;
			if (bConstant && EditLiteral("Value", Type, Value))
				Submit(Document.SetConstantValue(Expression->Id, MakeParameterValue(Type, Value), &Transactions));
		}
		if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get()); Swizzle && !Changed)
		{
			DetailsStyle::EditRow("Channels", [&] {
				constexpr std::array Labels{"R", "G", "B", "A"};
				for (uint8 Channel = 0; Channel < 4; ++Channel)
				{
					if (Channel) ImGui::SameLine();
					bool Enabled = std::ranges::find(Swizzle->Components, Channel) != Swizzle->Components.end();
					ImGui::BeginDisabled(Enabled && Swizzle->Components.size() == 1);
					const bool Edited = ImGui::Checkbox(Labels[Channel], &Enabled);
					ImGui::EndDisabled();
					if (!Edited) continue;
					std::vector<uint8> Mask;
					for (uint8 Candidate = 0; Candidate < 4; ++Candidate)
						if (Candidate == Channel ? Enabled : std::ranges::find(Swizzle->Components, Candidate) != Swizzle->Components.end())
							Mask.push_back(Candidate);
					Submit(Document.SetSwizzleComponents(Expression->Id, Mask, &Transactions));
					return true;
				}
				return false;
			});
		}
		if (const auto* Surface = Cast<DMaterialExpressionGetSurfaceAttributes>(Expression.Get()); Surface && !Changed)
		{
			for (uint32 Index = 0; Index < 8 && !Changed; ++Index)
			{
				bool Enabled = (Surface->AttributeMask & (1 << Index)) != 0;
				if (!DetailsStyle::EditRow(MaterialSurfaceNames[Index], [&] { return ImGui::Checkbox("##Value", &Enabled); })) continue;
				const auto Edited = BeginSelectedEdit();
				if (Edited == Session->Expressions.end()) continue;
				auto* Draft = Cast<DMaterialExpressionGetSurfaceAttributes>(Edited->Get());
				Draft->AttributeMask = Enabled ? Draft->AttributeMask | (1 << Index) : Draft->AttributeMask & ~(1 << Index);
				Submit(Session->Commit("Edit Surface Outputs", &Transactions));
			}
		}
		if (const auto* Surface = Cast<DMaterialExpressionSetSurfaceAttributes>(Expression.Get()); Surface && !Changed)
		{
			for (uint32 Index = 0; Index < 8 && !Changed; ++Index)
			{
				const auto Attribute = static_cast<EMaterialSurfaceOutput>(Index);
				const auto Binding = std::ranges::find(Surface->Attributes, Attribute, &FMaterialExpressionSurfaceAttributeBinding::Attribute);
				const auto SetSource = [&](FMaterialProgramLink Link) {
					const auto Edited = BeginSelectedEdit();
					if (Edited == Session->Expressions.end()) return;
					auto& Bindings = Cast<DMaterialExpressionSetSurfaceAttributes>(Edited->Get())->Attributes;
					std::erase_if(Bindings, [&](const auto& Value) { return Value.Attribute == Attribute; });
					if (Link.SourceNodeId.IsValid()) Bindings.push_back({Attribute, {Link.SourceNodeId, Link.SourceOutputIndex, Link.SourceOutputId}});
					Submit(Session->Commit("Edit Surface Input", &Transactions));
				};
				DetailsStyle::EditRow(MaterialSurfaceNames[Index], [&] {
					if (!ImGui::BeginCombo("##Value", Binding == Surface->Attributes.end() ? "Keep base value" : "Connected")) return false;
					if (ImGui::Selectable("Keep base value", Binding == Surface->Attributes.end())) SetSource({});
					for (const auto& Source : View.Nodes)
					{
						if (Source.Node.Id == Expression->Id || Changed) continue;
						for (const auto& Pin : Source.Outputs)
							if (!Changed && Pin.Type == GetMaterialSurfaceOutputType(Attribute)
								&& ImGui::Selectable(std::format("{}: {}##{}{}", Source.PrimaryLabel, Pin.Name,
									Source.Node.Id.ToString(), Pin.PortId.ToString()).c_str()))
								SetSource({Source.Node.Id, Pin.OutputIndex, Pin.PortId});
					}
					ImGui::EndCombo();
					return Changed;
				});
			}
		}
		for (const auto& Pin : Selected->Inputs)
		{
			if (Changed) break;
			if (Selected->Node.IsSampleUVInput(Pin.InputIndex) || Pin.SourceType > EMaterialProgramValueType::Float4) continue;
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
			const bool bConnected = Pin.Link.SourceNodeId.IsValid();
			const auto EditValue = [&]() {
				if (bInheritedDynamic)
				{
					ImGui::TextWrapped("%s", DescribeFunctionDefault(Pin.Default).c_str());
					return;
				}
				if (EditLiteralValue(Value.Type, Value.Literal))
				{
					Value.Kind = EMaterialInputDefaultKind::Literal;
					Submit(Document.SetInputDefault(Selected->Node.Id, Pin.InputIndex, Value, Pin.PortId, &Transactions));
				}
			};
			MonaImGui::PropertyEdit::BeginRow(Pin.Name.c_str());
			const float MenuWidth = ImGui::GetFrameHeight();
			const float ValueWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - MenuWidth - ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetNextItemWidth(ValueWidth);
			if (bConnected)
			{
				const auto Source = std::ranges::find(View.Nodes, Pin.Link.SourceNodeId,
					[](const auto& Entry) { return Entry.Node.Id; });
				std::string SourceLabel = Source != View.Nodes.end() ? Source->PrimaryLabel : "Unavailable node";
				if (Source != View.Nodes.end())
				{
					const auto Output = std::ranges::find_if(Source->Outputs, [&](const auto& Port) {
						return Pin.Link.SourceOutputId.IsValid() ? Port.PortId == Pin.Link.SourceOutputId
							: Port.OutputIndex == Pin.Link.SourceOutputIndex;
					});
					if (Output != Source->Outputs.end()) SourceLabel += "." + Output->Name;
				}
				if (ImGui::Button((SourceLabel + "###Source").c_str(), {ValueWidth, 0.0f}) && Source != View.Nodes.end())
					SelectAndFrame(Pin.Link.SourceNodeId);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Connected to %s. Click to locate.", SourceLabel.c_str());
			}
			else EditValue();
			ImGui::SameLine();
			if (ImGui::Button("...", {MenuWidth, 0.0f})) ImGui::OpenPopup("InputActions");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Input actions");
			MonaImGui::PropertyEdit::EndRow();
			if (ImGui::BeginPopup("InputActions"))
			{
				if (!Changed && bConnected)
				{
					if (ImGui::MenuItem("Inline constant")) Submit(Document.InlineInputNode(Selected->Node.Id, Pin.InputIndex, Pin.PortId, &Transactions));
					if (!Changed && ImGui::MenuItem("Disconnect")) Submit(Document.Disconnect(
						Selected->InputAddress(Pin), &Transactions));
				}
				else if (!Changed)
				{
					if (ImGui::MenuItem("Extract constant", nullptr, false, Pin.InlineDefault.Kind == EMaterialInputDefaultKind::Literal)) Submit(Document.ExtractInputDefault(Selected->Node.Id, Pin.InputIndex, Pin.PortId, &Transactions));
					if (!Changed && Material && ImGui::MenuItem("Promote to parameter", nullptr, false, !bInheritedDynamic))
					{
						FMaterialParameterDefinition Definition;
						Definition.Id = FGuid::NewGuid(); Definition.Type = *GetParameterType(Value.Type);
						Definition.Value = MakeParameterValue(Value.Type, Value.Literal); Definition.Name = "Parameter";
						for (uint32 Suffix = 1; Material->FindParameterDefinition(Definition.Name); ++Suffix)
							Definition.Name = FName(std::format("Parameter{}", Suffix));
						Definition.DisplayName = Definition.Name.ToString();
						auto Created = GraphEditInternals::MakeParameterExpression(Definition);
						if (!Created) ReportError(Created.Message);
						auto Parameter = std::move(Created.Expression);
						if (Created)
						if (const auto SelectedExpression = BeginSelectedEdit(); SelectedExpression != Session->Expressions.end())
						{
							auto* Destination = SelectedExpression->Get();
							FMaterialExpressionInput Link{Parameter->Id};
							if (Value.Type == EMaterialProgramValueType::Float2 || Value.Type == EMaterialProgramValueType::Float3)
							{
								auto Mask = GraphEditInternals::MakeParameterMask(*Parameter.Get(), Value.Type);
								Link = {Mask->Id}; Session->Expressions.emplace_back(Mask.Get());
							}
							if (Pin.PortId.IsValid())
							{
								auto* Call = Cast<DMaterialExpressionFunctionCall>(Destination);
								auto Input = std::ranges::find(Call->Inputs, Pin.PortId, &FMaterialExpressionFunctionInputBinding::InputId);
								if (Input == Call->Inputs.end()) Call->Inputs.push_back({Pin.PortId, Value.Type, Link});
								else Input->Input = Link;
							}
							else VisitMaterialExpressionInputs(*Destination, [&](uint32 Index, FMaterialExpressionInput& Input) {
								if (Index == Pin.InputIndex) Input = Link;
							});
							const auto Position = std::ranges::find(Session->Presentation.Nodes, Expression->Id, &FMaterialGraphNodePresentation::NodeId);
							const int32 X = Position == Session->Presentation.Nodes.end() ? -320 : Position->X - 320;
							const int32 Y = Position == Session->Presentation.Nodes.end() ? 0 : Position->Y;
							Session->Presentation.Nodes.push_back({Parameter->Id, X, Y});
							Session->Expressions.emplace_back(Parameter.Get());
							Submit(Session->Commit("Promote Input Parameter", &Transactions));
						}
					}
				}
				if (!Changed && bInheritedDynamic && ImGui::MenuItem("Override with constant"))
				{
					Value.Kind = EMaterialInputDefaultKind::Literal;
					Submit(Document.SetInputDefault(Selected->Node.Id, Pin.InputIndex, Value, Pin.PortId, &Transactions));
				}
				if (!Changed && Pin.PortId.IsValid() && !Pin.bRequired
					&& Pin.InlineDefault.Kind != EMaterialInputDefaultKind::None && ImGui::MenuItem("Use function default"))
					Submit(Document.SetInputDefault(Selected->Node.Id, Pin.InputIndex, {}, Pin.PortId, &Transactions));
				if (!Changed && bConnected && ImGui::BeginMenu("When disconnected"))
				{
					ImGui::TextUnformatted("Value used after disconnecting");
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
					EditValue();
					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		MonaImGui::PropertyEdit::EndTable();
		ImGui::PopID();
	}
}
