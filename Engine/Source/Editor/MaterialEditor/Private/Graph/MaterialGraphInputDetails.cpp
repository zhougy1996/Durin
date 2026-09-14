#include "MaterialGraphCanvas.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphExpressionState.h"
#include "MaterialExpressionParameters.h"
#include "MaterialExpressionInputs.h"
#include "MaterialGraphValueTypes.h"
#include "Editor/AssetPicker.h"
#include "Texture/Texture2D.h"
#include <cmath>

namespace Durin::Editor::Material
{
	auto FMaterialGraphCanvas::DrawSelectionDetails(DObject& Owner, DTransactor& Transactions,
		const FReportError& ReportError) -> void
	{
		const auto Selection = GetSelectedProgramNodes();
		if (Selection.size() != 1) return;
		FMaterialGraphDocument Document(Owner);
		const FMaterialExpressionCollection* Collection = nullptr;
		if (const auto* Material = Cast<DMaterial>(&Owner)) Collection = &Material->GetExpressionCollection();
		if (const auto* Function = Cast<DMaterialFunction>(&Owner)) Collection = &Function->GetExpressionCollection();
		if (!Collection) return;
		const auto Existing = std::ranges::find(Collection->Expressions, Selection.front(), [](const auto& E) { return E->Id; });
		if (Existing == Collection->Expressions.end()) return;
		const TStrongObjectPtr<DMaterialExpression> Expression(Existing->Get());
		GraphEditInternals::FOwnedGraphSnapshot State;
		const auto CaptureSelected = [&]() {
			State = {};
			if (!State.Capture(Owner)) return State.Expressions.end();
			return std::ranges::find(State.Expressions, Selection.front(), [](const auto& E) { return E->Id; });
		};
		const auto View = Document.Inspect();
		const auto Selected = std::ranges::find(View.Nodes, Selection.front(),
			[](const auto& Entry) { return Entry.Node.Id; });
		if (Selected == View.Nodes.end() || !ImGui::CollapsingHeader("Selected Node", ImGuiTreeNodeFlags_DefaultOpen)) return;
		ImGui::PushID(Selection.front().ToString().c_str());
		ImGui::TextUnformatted(Selected->PrimaryLabel.c_str());
		if (SelectedDiagnostic && SelectedDiagnostic->NodeId == Selected->Node.Id)
		{
			constexpr std::array UVNames{"Channel", "Scale", "Offset", "Rotation"};
			if (SelectedDiagnostic->UVFieldIndex && *SelectedDiagnostic->UVFieldIndex < UVNames.size())
				ImGui::TextColored({1, .65f, .25f, 1}, "Diagnostic UV field: %s", UVNames[*SelectedDiagnostic->UVFieldIndex]);
			else if (SelectedDiagnostic->InputIndex)
				ImGui::TextColored({1, .65f, .25f, 1}, "Diagnostic input: %u", *SelectedDiagnostic->InputIndex);
		}
		bool Changed = false;
		const auto Submit = [&](FMaterialGraphCommandResult Result) {
			if (!Result) ReportError(Result.Message);
			Changed = Result.Status == EMaterialGraphCommandStatus::Succeeded;
		};
		const auto EditText = [](const char* Label, std::string& Value) {
			std::array<char, MaterialProgramMaxDisplayNameBytes + 1> Buffer{};
			std::copy_n(Value.data(), std::min(Value.size(), Buffer.size() - 1), Buffer.data());
			if (!ImGui::InputText(Label, Buffer.data(), Buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) return false;
			Value = Buffer.data();
			return true;
		};
		const auto EditLiteral = [](const char* Label, EMaterialProgramValueType Type, FMaterialProgramLiteral& Value) {
			std::array<float, 4> Components{Value.X, Value.Y, Value.Z, Value.W};
			const int Count = static_cast<int>(Type) + 1;
			if (Count < 1 || Count > 4 || !ImGui::InputScalarN(Label, ImGuiDataType_Float, Components.data(), Count,
				nullptr, nullptr, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue)) return false;
			Value = {Components[0], Components[1], Components[2], Components[3]};
			return true;
		};
		auto* Material = Cast<DMaterial>(&Owner);
		if (const auto* ParameterExpression = Cast<DMaterialExpressionParameter>(Expression.Get()); Material && ParameterExpression)
		{
			auto Parameter = ParameterExpression->GetParameterDefinition();
			const auto CommitParameter = [&]() {
				auto Replacement = GraphEditInternals::MakeParameterExpression(Parameter);
				if (!Replacement) { ReportError("The parameter definition is invalid."); return; }
				Replacement->Id = Expression->Id;
				const auto ExpressionIt = CaptureSelected();
				if (ExpressionIt == State.Expressions.end()) return;
				if (auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(ExpressionIt->Get()); Sample && Parameter.Type == EMaterialParameterType::Texture)
				{
					const auto* Texture = Cast<DMaterialExpressionTextureParameter>(Replacement.Get());
					Sample->Metadata = Texture->Metadata; Sample->DefaultValue = Texture->DefaultValue; Sample->TextureUsage = Texture->TextureUsage;
				}
				else *ExpressionIt = Replacement.Get();
				Submit(GraphEditInternals::CommitOwnedExpressions(Owner, State, "Edit Parameter Expression", &Transactions));
			};
			int Type = static_cast<int>(Parameter.Type);
			if (ImGui::Combo("Type", &Type, "Scalar\0Vector3\0Texture\0Vector2\0Vector4\0"))
			{
				const auto Previous = ReadParameterLiteral(GetProgramType(Parameter.Type), Parameter.Value);
				Parameter.Type = static_cast<EMaterialParameterType>(Type);
				Parameter.Value = MakeParameterValue(GetProgramType(Parameter.Type), Previous);
				Parameter.Presentation = EMaterialParameterPresentation::Default;
				Parameter.bHasRange = false;
				CommitParameter();
			}
			int Presentation = static_cast<int>(Parameter.Presentation);
			if (!Changed && ImGui::Combo("Presentation", &Presentation, "Default\0Drag\0Integer\0Color\0Asset picker\0"))
			{
				Parameter.Presentation = static_cast<EMaterialParameterPresentation>(Presentation);
				CommitParameter();
			}
			std::string Name = Parameter.Name.ToString();
			if (!Changed && EditText("Name", Name)) { Parameter.Name = FName(Name); CommitParameter(); }
			if (!Changed && EditText("Display name", Parameter.DisplayName)) CommitParameter();
			std::string Group = Parameter.GroupName.ToString();
			if (!Changed && EditText("Group", Group)) { Parameter.GroupName = FName(Group); CommitParameter(); }
			if (!Changed && ImGui::InputInt("Order", &Parameter.SortOrder, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) CommitParameter();
			if (!Changed && Parameter.Type == EMaterialParameterType::Scalar && ImGui::Checkbox("Range hint", &Parameter.bHasRange)) CommitParameter();
			if (!Changed && Parameter.bHasRange)
			{
				float Range[2]{Parameter.MinimumValue, Parameter.MaximumValue};
				if (ImGui::InputFloat2("Min / Max", Range, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue))
				{
					Parameter.MinimumValue = Range[0]; Parameter.MaximumValue = Range[1];
					CommitParameter();
				}
			}
			if (!Changed && Parameter.Type == EMaterialParameterType::Texture)
			{
				int Usage = static_cast<int>(Parameter.TextureUsage);
				if (ImGui::Combo("Texture usage", &Usage, "Color\0Normal\0Data / Mask\0"))
				{
					Parameter.TextureUsage = static_cast<ETextureUsage>(Usage);
					CommitParameter();
				}
				const auto Picker = AssetPicker::Draw({.RequiredClass = DTexture2D::StaticClass(),
					.CurrentSelection = Parameter.Value.GetTexture().Texture.Get(), .SearchText = NodeTextureSearch,
					.AssignSelection = [&](DObject* Object, std::string& Error) {
						auto Value = Parameter.Value;
						Value.GetTexture().Texture = Cast<DTexture2D>(Object);
						const auto Result = FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id, Value, &Transactions);
						Error = Result.Message; Changed = Result.Status == EMaterialGraphCommandStatus::Succeeded;
						return static_cast<bool>(Result);
					}});
				if (!Picker.Error.empty()) ReportError(Picker.Error);
				const auto Combo = [&](const char* Label, auto& Value, const char* Names) {
					int Index = static_cast<int>(Value);
					if (Changed || !ImGui::Combo(Label, &Index, Names)) return;
					Value = static_cast<std::remove_reference_t<decltype(Value)>>(Index);
					Submit(FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id, Parameter.Value, &Transactions));
				};
				Combo("Fallback", Parameter.Value.GetTexture().TextureFallback, "White\0Black\0Flat normal\0");
				Combo("Min filter", Parameter.Value.GetTexture().SamplerState.MinFilter, "Nearest\0Linear\0Nearest mip nearest\0Linear mip nearest\0Nearest mip linear\0Linear mip linear\0");
				Combo("Mag filter", Parameter.Value.GetTexture().SamplerState.MagFilter, "Nearest\0Linear\0");
				Combo("Address U", Parameter.Value.GetTexture().SamplerState.AddressU, "Repeat\0Mirrored repeat\0Clamp\0");
				Combo("Address V", Parameter.Value.GetTexture().SamplerState.AddressV, "Repeat\0Mirrored repeat\0Clamp\0");
			}
			else if (!Changed)
			{
				auto Literal = ReadParameterLiteral(GetProgramType(Parameter.Type), Parameter.Value);
				if (EditLiteral("Default", GetProgramType(Parameter.Type), Literal))
				{
					if (Parameter.Presentation == EMaterialParameterPresentation::Integer)
					{
						Literal.X = std::round(Literal.X);
						if (Parameter.bHasRange) Literal.X = std::clamp(Literal.X, Parameter.MinimumValue, Parameter.MaximumValue);
					}
					Submit(FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id,
						MakeParameterValue(GetProgramType(Parameter.Type), Literal), &Transactions));
				}
			}
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
			{
				const auto Candidate = CaptureSelected();
				if (Candidate != State.Expressions.end())
				{
					if (auto* E = Cast<DMaterialExpressionScalarConstant>(Candidate->Get())) E->Value = Value.X;
					if (auto* E = Cast<DMaterialExpressionVector2Constant>(Candidate->Get())) E->Value = FVector2(Value.X, Value.Y);
					if (auto* E = Cast<DMaterialExpressionVector3Constant>(Candidate->Get())) E->Value = FVector3(Value.X, Value.Y, Value.Z);
					if (auto* E = Cast<DMaterialExpressionVector4Constant>(Candidate->Get())) E->Value = FVector4(Value.X, Value.Y, Value.Z, Value.W);
					Submit(GraphEditInternals::CommitOwnedExpressions(Owner, State, "Edit Constant Expression", &Transactions));
				}
			}
		}
		for (const auto& Pin : Selected->Inputs)
		{
			if (Changed) break;
			if (Selected->Node.IsSampleUVInput(Pin.InputIndex) || Pin.SourceType > EMaterialProgramValueType::Float4) continue;
			ImGui::PushID(static_cast<int>(Pin.InputIndex));
			ImGui::TextUnformatted(Pin.Name.c_str());
			auto Value = Pin.InlineDefault;
			if (Value.Kind == EMaterialInputDefaultKind::None) Value.Type = Pin.SourceType;
			if (Pin.Link.SourceNodeId.IsValid()) ImGui::TextDisabled("Connected; literal fallback is retained");
			if (EditLiteral("Fallback", Value.Type, Value.Literal))
			{
				Value.Kind = EMaterialInputDefaultKind::Literal;
				Submit(Document.SetInputDefault(Selected->Node.Id, Pin.InputIndex, Value, Pin.PortId, &Transactions));
			}
			if (!Changed && Pin.Link.SourceNodeId.IsValid())
			{
				if (ImGui::SmallButton("Inline constant")) Submit(Document.InlineInputNode(Selected->Node.Id, Pin.InputIndex, Pin.PortId, &Transactions));
				if (!Changed && ImGui::SmallButton("Disconnect")) Submit(Pin.PortId.IsValid()
					? Document.DisconnectCallInput(Selected->Node.Id, Pin.PortId, &Transactions)
					: Document.ConnectInput(Selected->Node.Id, Pin.InputIndex, {}, true, &Transactions));
			}
			else if (!Changed)
			{
				if (ImGui::SmallButton("Extract constant")) Submit(Document.ExtractInputDefault(Selected->Node.Id, Pin.InputIndex, Pin.PortId, &Transactions));
				if (!Changed && Material && ImGui::SmallButton("Promote to parameter"))
				{
					FMaterialParameterDefinition Definition;
					Definition.Id = FGuid::NewGuid(); Definition.Type = *GetParameterType(Value.Type);
					Definition.Value = MakeParameterValue(Value.Type, Value.Literal); Definition.Name = "Parameter";
					for (uint32 Suffix = 1; Material->FindParameterDefinition(Definition.Name); ++Suffix)
						Definition.Name = FName(std::format("Parameter{}", Suffix));
					Definition.DisplayName = Definition.Name.ToString();
					auto Parameter = GraphEditInternals::MakeParameterExpression(Definition);
					if (const auto Candidate = Parameter ? CaptureSelected() : State.Expressions.end(); Candidate != State.Expressions.end())
					{
						const FMaterialExpressionInput Link{Parameter->Id};
						if (Pin.PortId.IsValid())
						{
							auto* Call = Cast<DMaterialExpressionFunctionCall>(Candidate->Get());
							auto Input = std::ranges::find(Call->Inputs, Pin.PortId, &FMaterialExpressionFunctionInputBinding::InputId);
							if (Input == Call->Inputs.end()) Call->Inputs.push_back({Pin.PortId, Value.Type, Link});
							else Input->Input = Link;
						}
						else VisitMaterialExpressionInputs(**Candidate, [&](uint32 Index, FMaterialExpressionInput& Input) {
							if (Index == Pin.InputIndex) Input = Link;
						});
						const auto Position = std::ranges::find(State.Presentation.Nodes, Expression->Id, &FMaterialGraphNodePresentation::NodeId);
						const int32 X = Position == State.Presentation.Nodes.end() ? -320 : Position->X - 320;
						const int32 Y = Position == State.Presentation.Nodes.end() ? 0 : Position->Y;
						State.Presentation.Nodes.push_back({Parameter->Id, X, Y});
						State.Expressions.emplace_back(Parameter.Get());
						Submit(GraphEditInternals::CommitOwnedExpressions(Owner, State, "Promote Input Parameter", &Transactions));
					}
				}
			}
			ImGui::Separator(); ImGui::PopID();
		}
		FMaterialExpressionUVSettings* UVSettings = nullptr;
		FMaterialExpressionInput* UV = nullptr;
		uint32 UVIndex = 0;
		if (auto* E = Cast<DMaterialExpressionTextureSample2D>(Expression.Get())) { UVSettings = &E->UVSettings; UV = &E->UV; UVIndex = 1; }
		if (auto* E = Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get())) { UVSettings = &E->UVSettings; UV = &E->UV; }
		if (!Changed && UVSettings)
		{
			const bool Connected = UV->ExpressionId.IsValid();
			auto Settings = *UVSettings;
			ImGui::TextUnformatted("UV Settings (rotation in radians)");
			if (Connected && ImGui::SmallButton("Disconnect UV")) Submit(Document.ConnectInput(Expression->Id, UVIndex, {}, true, &Transactions));
			constexpr std::array Names{"Channel", "Scale", "Offset", "Rotation"};
			for (uint32 Index = 0; Index < 4 && !Changed; ++Index)
			{
				auto* Scalar = Index == 0 ? &Settings.Channel : Index == 3 ? &Settings.Rotation : nullptr;
				auto* Vector = Index == 1 ? &Settings.Scale : Index == 2 ? &Settings.Offset : nullptr;
				FMaterialProgramLiteral Value = Scalar ? FMaterialProgramLiteral{Scalar->Value}
					: FMaterialProgramLiteral{static_cast<float>(Vector->Value.x), static_cast<float>(Vector->Value.y)};
				if (EditLiteral(Names[Index], Scalar ? EMaterialProgramValueType::Float : EMaterialProgramValueType::Float2, Value))
				{
					if (Scalar) *Scalar = {true, Value.X};
					else *Vector = {true, FVector2(Value.X, Value.Y)};
					const auto Candidate = CaptureSelected();
					if (Candidate != State.Expressions.end())
					{
						if (auto* E = Cast<DMaterialExpressionTextureSample2D>(Candidate->Get())) E->UVSettings = Settings;
						if (auto* E = Cast<DMaterialExpressionTextureSampleParameter2D>(Candidate->Get())) E->UVSettings = Settings;
						Submit(GraphEditInternals::CommitOwnedExpressions(Owner, State, "Edit Local UV Settings", &Transactions));
					}
				}
			}
			if (!Connected && !Changed && ImGui::Button("Extract Texture Coordinates")) Submit(Document.ExtractUVSettings(Expression->Id, &Transactions));
		}
		ImGui::PopID();
	}
}
