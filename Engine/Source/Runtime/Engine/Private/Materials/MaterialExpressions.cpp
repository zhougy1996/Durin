#include "Materials/MaterialExpressionBuild.h"
#include "DObject/DurinPropertyTypes.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureParameter.h"
#include "Materials/MaterialExpressionVector4Parameter.h"
#include "Materials/MaterialCustomVersion.h"
#include "Serialization/Archive.h"

#include "Threading/RunnableThread.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialFunction.h"
#include "Materials/Material.h"
#include "DObject/Property.h"

namespace Durin
{
	auto FMaterialPropertyInputs::Find(EMaterialProperty Property) const -> const FMaterialNumericInput*
	{
		switch (Property)
		{
		case EMaterialProperty::MP_BaseColor: return &BaseColor;
		case EMaterialProperty::MP_Normal: return &Normal;
		case EMaterialProperty::MP_Metallic: return &Metallic;
		case EMaterialProperty::MP_Roughness: return &Roughness;
		case EMaterialProperty::MP_AmbientOcclusion: return &AmbientOcclusion;
		case EMaterialProperty::MP_EmissiveColor: return &EmissiveColor;
		case EMaterialProperty::MP_Opacity: return &Opacity;
		case EMaterialProperty::MP_OpacityMask: return &OpacityMask;
		case EMaterialProperty::MP_WorldPositionOffset: return &WorldPositionOffset;
		case EMaterialProperty::MP_Specular: return &Specular;
		case EMaterialProperty::MP_ClearCoat: return &ClearCoat;
		case EMaterialProperty::MP_ClearCoatRoughness: return &ClearCoatRoughness;
		default: return nullptr;
		}
	}

	auto FMaterialRootInputs::FindInvalidConstantWidth() const -> std::optional<EMaterialProperty>
	{
		for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
			if (Get(Field.Property).Constant.size() != uint32(Field.Semantics.Type) + 1) return Field.Property;
		return std::nullopt;
	}

	FMaterialInputDescription::FMaterialInputDescription(const FMaterialExpressionInput& Packed)
		: Connection(&Packed), Type(EMaterialProgramValueType::MaterialAttributes), Name("Material Attributes") {}

	FMaterialInputDescription::FMaterialInputDescription(EMaterialProperty Property, FMaterialNumericInputView Input)
		: Connection(&Input.Connection), Numeric(Input)
	{
		const auto* Field = FMaterialAttributeDefinitionMap::Find(Property);
		check(Field);
		Type = Field->Semantics.Type;
		Name = Field->Name;
	}

	auto FMaterialRootInputs::GetExpressionInputDescription(EMaterialProperty Property) const -> std::optional<FMaterialInputDescription>
	{
		if (Property == EMaterialProperty::MP_MaterialAttributes)
			return FMaterialInputDescription{Attributes};
		const auto* Input = Properties.Find(Property);
		if (!Input) return std::nullopt;
		return FMaterialInputDescription{Property, *Input};
	}

	auto FMaterialRootInputs::GetExpressionInputForProperty(EMaterialProperty Property) const -> const FMaterialExpressionInput*
	{
		const auto Description = GetExpressionInputDescription(Property);
		return Description ? Description->Connection : nullptr;
	}

	static auto CaptureBindings(const auto& Root) -> FMaterialOutputBindings
	{
		const auto& Attributes = *Root.GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes);
		FMaterialOutputBindings Result{Root.UsesMaterialAttributes(), Attributes.GetExpressionId(), Attributes.OutputIndex};
		Result.Properties.reserve(FMaterialAttributeDefinitionMap::GetDefinitions().size());
		for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
		{
			const auto Input = Root.GetNumericInputForProperty(Field.Property);
			Result.Properties.push_back({Input->Connection.GetExpressionId(), Input->Connection.OutputIndex,
				Input->UseConstant, {Input->Constant.begin(), Input->Constant.end()}});
		}
		return Result;
	}
	auto CaptureMaterialOutputBindings(const FMaterialRootInputs& Root) -> FMaterialOutputBindings
	{
		return CaptureBindings(Root);
	}
#if DURIN_WITH_EDITORONLY_DATA
	auto CaptureMaterialOutputBindings(const DMaterial& Material) -> FMaterialOutputBindings
	{
		return CaptureBindings(Material);
	}
#endif

	namespace
	{
		auto SameAuthoredFields(const DStructBase& Type, const void* Left, const void* Right) -> bool;

		auto SameField(const FProperty& Field, const void* Left, const void* Right, uint32 Index) -> bool
		{
			if (Field.GetKind() == DurinCodeGen::EPropertyGenFlags::Struct)
			{
				const auto* Type = static_cast<const FStructProperty&>(Field).GetStruct();
				const void* A = Field.GetValuePtr(Left, Index);
				const void* B = Field.GetValuePtr(Right, Index);
				if (Type == FMaterialExpressionInput::StaticStruct())
				{
					const auto& L = *static_cast<const FMaterialExpressionInput*>(A);
					const auto& R = *static_cast<const FMaterialExpressionInput*>(B);
					return L.IsConnected() == R.IsConnected() && L.GetExpressionId() == R.GetExpressionId()
						&& L.OutputIndex == R.OutputIndex;
				}
				if (ArePropertyValuesIdentical(&Field, Left, Index, Right, Index)) return true;
				// Only reflected structs with fields can contain a nested connection.
				return Type && SameAuthoredFields(*Type, A, B);
			}
			if (Field.GetKind() == DurinCodeGen::EPropertyGenFlags::Array)
			{
				const auto& Array = static_cast<const FArrayProperty&>(Field);
				uint64 LeftCount = 0, RightCount = 0;
				if (Array.GetNum(Left, LeftCount, Index) != EContainerOpResult::Success
					|| Array.GetNum(Right, RightCount, Index) != EContainerOpResult::Success || LeftCount != RightCount) return false;
				for (uint64 I = 0; I < LeftCount; ++I)
					if (!SameField(*Array.GetInner(), Array.GetElementPtr(Left, I, Index), Array.GetElementPtr(Right, I, Index), 0)) return false;
				return true;
			}
			return ArePropertyValuesIdentical(&Field, Left, Index, Right, Index);
		}

		auto SameAuthoredFields(const DStructBase& Type, const void* Left, const void* Right) -> bool
		{
			bool Equal = true, HasFields = false;
			Type.ForEachProperty([&](FProperty* Field) {
				HasFields = true;
				for (uint32 I = 0; Equal && I < Field->GetArrayDim(); ++I) Equal = SameField(*Field, Left, Right, I);
			});
			return HasFields && Equal;
		}
	}

	auto HaveSameMaterialExpressionFields(DMaterialExpression& Left, DMaterialExpression& Right) -> bool
	{
		return Left.GetClass() == Right.GetClass() && SameAuthoredFields(*Left.GetClass(), &Left, &Right);
	}

	FMaterialExpressionInput::FMaterialExpressionInput(DMaterialExpression* Upstream, int32 Index, FGuid NamedOutput)
		: FMaterialExpressionInput(Upstream, Index)
	{
		if (!NamedOutput.IsValid()) return;
		OutputIndex = -1;
		if (Index != 0) return;
		if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Upstream))
		{
			const auto It = std::ranges::find(Call->FunctionOutputs, NamedOutput, &FMaterialFunctionOutputBinding::OutputId);
			if (It != Call->FunctionOutputs.end()) OutputIndex = static_cast<int32>(It - Call->FunctionOutputs.begin());
		}
		else if (Cast<DMaterialExpressionGetMaterialAttributes>(Upstream))
		{
			if (const auto* Definition = FMaterialAttributeDefinitionMap::Find(NamedOutput))
				OutputIndex = static_cast<int32>(Definition->Property);
		}
	}

	auto FMaterialExpressionInput::GetOutputId() const -> FGuid
	{
		if (OutputIndex < 0) return {};
		if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get()))
			return static_cast<size_t>(OutputIndex) < Call->FunctionOutputs.size() ? Call->FunctionOutputs[OutputIndex].OutputId : FGuid{};
		if (Cast<DMaterialExpressionGetMaterialAttributes>(Expression.Get()) && std::in_range<uint8>(OutputIndex))
			if (const auto* Definition = FMaterialAttributeDefinitionMap::Find(static_cast<EMaterialProperty>(OutputIndex)))
				return Definition->AttributeID;
		return {};
	}

	auto FMaterialExpressionInput::GetExpressionId() const -> FGuid
	{
		return Expression ? Expression->Id : FGuid{};
	}

	DMaterialExpressionGetMaterialAttributes::DMaterialExpressionGetMaterialAttributes(const FObjectInitializer& Initializer) : Super(Initializer)
	{
		for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions()) AttributeIDs.push_back(Definition.AttributeID);
	}
	auto GetMaterialNumericInputFallback(EMaterialProgramOpcode Opcode,
		EMaterialProgramValueType Type, uint32 Slot) -> std::vector<float>
	{
		if (Opcode == EMaterialProgramOpcode::TextureCoordinates) Opcode = EMaterialProgramOpcode::UVChannel;
		if (Opcode == EMaterialProgramOpcode::AppendVector) return {0.f};
		if (Opcode == EMaterialProgramOpcode::BlendMaterialAttributes && Slot == 2) return {.5f};
		if (IsMaterialSamplingNode(Opcode))
		{
			const bool Cube = Opcode == EMaterialProgramOpcode::TextureSampleCube || Opcode == EMaterialProgramOpcode::TextureSampleParameterCube;
			const uint32 FirstOptional = Opcode == EMaterialProgramOpcode::TextureSample2D || Opcode == EMaterialProgramOpcode::TextureSampleCube ? 2 : 1;
			if (Cube && Slot + 1 == FirstOptional) return {1.f,0.f,0.f};
			if (Slot >= FirstOptional && Slot <= FirstOptional + 2)
				return std::vector<float>(Slot == FirstOptional ? 1 : Cube ? 3 : 2, 0.f);
		}
		const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
		if (!Signature || Slot >= Signature->InputCount) return {};
		if (Opcode == EMaterialProgramOpcode::MakeMaterialAttributes) return {}; // Connection-only pins.
		const auto& Accepted = Signature->Inputs[Slot];
		if (Accepted.empty() || Accepted.front() > EMaterialProgramValueType::Float4) return {};
		if (Opcode == EMaterialProgramOpcode::Swizzle) return std::vector<float>(static_cast<uint32>(Type) + 1, 0.f);
		const auto Width = IsMaterialAdaptiveNumeric(Opcode)
			&& !(Opcode == EMaterialProgramOpcode::Lerp && Slot == 2)
			? static_cast<uint32>(Type) + 1 : static_cast<uint32>(Accepted.front()) + 1;
		float Value = 0.f;
		if (((Opcode == EMaterialProgramOpcode::Multiply || Opcode == EMaterialProgramOpcode::Divide) && Slot == 1)
			|| (Opcode == EMaterialProgramOpcode::Clamp && Slot == 2)
			|| (Opcode == EMaterialProgramOpcode::Pow && Slot == 1)
			|| (Opcode == EMaterialProgramOpcode::Fmod && Slot == 1)
			|| (Opcode == EMaterialProgramOpcode::SmoothStep && Slot == 1)
			|| Opcode == EMaterialProgramOpcode::Normalize
			|| Opcode == EMaterialProgramOpcode::Log) Value = 1.f;
		if (Opcode == EMaterialProgramOpcode::Lerp) Value = Slot == 1 ? 1.f : Slot == 2 ? .5f : 0.f;
		return std::vector<float>(Width, Value);
	}

	auto DMaterialExpressionMakeMaterialAttributes::GetExpressionInputForProperty(EMaterialProperty Property) const -> const FMaterialExpressionInput*
	{
		switch (Property)
		{
		case EMaterialProperty::MP_BaseColor: return &BaseColor;
		case EMaterialProperty::MP_Normal: return &Normal;
		case EMaterialProperty::MP_Metallic: return &Metallic;
		case EMaterialProperty::MP_Roughness: return &Roughness;
		case EMaterialProperty::MP_AmbientOcclusion: return &AmbientOcclusion;
		case EMaterialProperty::MP_EmissiveColor: return &EmissiveColor;
		case EMaterialProperty::MP_Opacity: return &Opacity;
		case EMaterialProperty::MP_OpacityMask: return &OpacityMask;
		case EMaterialProperty::MP_WorldPositionOffset: return &WorldPositionOffset;
		case EMaterialProperty::MP_Specular: return &Specular;
		case EMaterialProperty::MP_ClearCoat: return &ClearCoat;
		case EMaterialProperty::MP_ClearCoatRoughness: return &ClearCoatRoughness;
		default: return nullptr;
		}
	}

	auto DMaterialExpressionSetMaterialAttributes::Serialize(FArchive& Ar) -> void
	{
		if (!FMaterialOutputVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
	}

	auto DMaterialExpressionMakeMaterialAttributes::Serialize(FArchive& Ar) -> void
	{
		if (!FMaterialOutputVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
	}

	auto GetMaterialDomainInputProperties(EMaterialDomain Domain) -> std::span<const EMaterialProperty>
	{
		static const auto Surface = [] {
			std::vector<EMaterialProperty> Properties{EMaterialProperty::MP_MaterialAttributes};
			for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions()) Properties.push_back(Field.Property);
			return Properties;
		}();
		static constexpr std::array PostProcess{EMaterialProperty::MP_EmissiveColor, EMaterialProperty::MP_MaterialAttributes};
		if (Domain == EMaterialDomain::PostProcess) return PostProcess;
		return Domain == EMaterialDomain::Surface ? std::span<const EMaterialProperty>(Surface) : std::span<const EMaterialProperty>{};
	}

	auto SupportsMaterialRootConstant(EMaterialProperty Property) -> bool
	{
		return Property != EMaterialProperty::MP_Normal && Property != EMaterialProperty::MP_MaterialAttributes;
	}

	auto ReadMaterialNumericDefault(FMaterialNumericInputView Input, EMaterialProperty Property) -> std::vector<float>
	{
		const auto* Field = FMaterialAttributeDefinitionMap::Find(Property);
		if (!Field) return {};
		if (SupportsMaterialRootConstant(Property) && Input.UseConstant) return {Input.Constant.begin(), Input.Constant.end()};
		const auto& Value = Field->DefaultValue;
		const std::array Components{Value.X, Value.Y, Value.Z, Value.W};
		return {Components.begin(), Components.begin() + static_cast<uint32>(Field->Semantics.Type) + 1};
	}

	auto ReadMaterialOutputDefault(const FMaterialRootInputs& Outputs, EMaterialProperty Property) -> std::vector<float>
	{
		return FMaterialAttributeDefinitionMap::Find(Property) ? ReadMaterialNumericDefault(Outputs.Get(Property), Property) : std::vector<float>{};
	}

	auto DMaterialExpression::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.Origin == EPropertyChangeOrigin::Undo || Event.Origin == EPropertyChangeOrigin::Redo) return;
		auto* Owner = GetOuter();
		if (Owner && (Owner->IsA(DMaterial::StaticClass()) || Owner->IsA(DMaterialFunction::StaticClass())))
		{
			auto OwnerEvent = Event;
			OwnerEvent.MemberProperty = Owner->GetClass()->FindPropertyByName("ExpressionCollection");
			Owner->PostEditChangeProperty(OwnerEvent);
		}
	}

	auto DMaterialExpressionParameter::SetParameterDefinition(const FMaterialParameterDefinition& Definition) -> std::expected<void, std::string>
	{
		const auto ExpectedType = GetParameterDefinition().Type;
		if (ExpectedType != Definition.Type)
		{
			return std::unexpected(std::format("{} [parameter {}]", "Material declaration type is invalid.", Definition.Id.ToString()));
		}
		const auto Validation = ValidateMaterialParameterDefinitions(std::span(&Definition, 1));
		if (!Validation) return std::unexpected(std::format("{} [parameter {}]", GetMaterialParameterErrorText(Validation.Error), Validation.ParameterId.ToString()));
		if (auto* Parameter = Cast<DMaterialExpressionScalarParameter>(this))
		{
			Parameter->DefaultValue = Definition.Value.GetScalar();
			Parameter->bHasRange = Definition.bHasRange;
			Parameter->MinimumValue = Definition.MinimumValue;
			Parameter->MaximumValue = Definition.MaximumValue;
		}
		else if (auto* Parameter = Cast<DMaterialExpressionVector4Parameter>(this)) Parameter->DefaultValue = Definition.Value.GetVector4();
		else if (auto* Parameter = Cast<DMaterialExpressionTextureParameter>(this))
		{
			const auto& Texture = Definition.Value.GetTexture();
			Parameter->DefaultValue = Texture;
			Parameter->TextureUsage = Definition.TextureUsage;
		}
		else return std::unexpected(std::format("{} [parameter {}]", "Parameter expression has an unsupported type.", Definition.Id.ToString()));
		Metadata = {Definition.Id, Definition.Name, Definition.DisplayName, Definition.GroupName,
			Definition.SortOrder, Definition.Presentation};
		return {};
	}

	auto DMaterialExpressionParameter::MakeDefinition(EMaterialParameterType Type, FMaterialParameterValue Value) const
		-> FMaterialParameterDefinition
	{
		return {.Id = Metadata.Id, .Name = Metadata.Name, .Type = Type, .Value = std::move(Value),
			.DisplayName = Metadata.DisplayName, .GroupName = Metadata.GroupName,
			.SortOrder = Metadata.SortOrder, .Presentation = Metadata.Presentation};
	}

	auto DMaterialExpressionScalarParameter::GetParameterDefinition() const -> FMaterialParameterDefinition
	{
		auto Definition = MakeDefinition(EMaterialParameterType::Scalar, FMaterialParameterValue::MakeScalar(DefaultValue));
		Definition.bHasRange = bHasRange;
		Definition.MinimumValue = MinimumValue;
		Definition.MaximumValue = MaximumValue;
		return Definition;
	}

	auto DMaterialExpressionVector4Parameter::GetParameterDefinition() const -> FMaterialParameterDefinition
	{
		auto Definition = MakeDefinition(EMaterialParameterType::Vector4, FMaterialParameterValue::MakeVector4(DefaultValue));
		return Definition;
	}

	auto DMaterialExpressionTextureParameter::GetParameterDefinition() const -> FMaterialParameterDefinition
	{
		auto Definition = MakeDefinition(EMaterialParameterType::Texture, FMaterialParameterValue(DefaultValue));
		Definition.TextureUsage = TextureUsage;
		return Definition;
	}

}
