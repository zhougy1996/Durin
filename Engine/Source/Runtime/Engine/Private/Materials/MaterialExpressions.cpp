#include "Materials/MaterialExpressions.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialCustomVersion.h"
#include "Serialization/Archive.h"

#include "Threading/RunnableThread.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialFunction.h"
#include "Materials/Material.h"
#include "DObject/Property.h"

namespace Durin
{
	auto GetMaterialNumericInputFallback(EMaterialProgramOpcode Opcode,
		EMaterialProgramValueType Type, uint32 Slot) -> std::vector<float>
	{
		if (Opcode == EMaterialProgramOpcode::TextureCoordinates) Opcode = EMaterialProgramOpcode::UVChannel;
		if (Opcode == EMaterialProgramOpcode::AppendVector) return {0.f};
		const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
		if (!Signature || Slot >= Signature->InputCount) return {};
		if (Opcode == EMaterialProgramOpcode::MakeSurface)
		{
			const FMaterialSurfaceOutputs Defaults;
			const auto Value = GetMaterialSurfaceOutputDefault(Defaults, static_cast<EMaterialSurfaceOutput>(Slot));
			const std::array Components{Value.X, Value.Y, Value.Z, Value.W};
			return {Components.begin(), Components.begin() + static_cast<uint32>(GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Slot))) + 1};
		}
		const auto& Accepted = Signature->Inputs[Slot];
		if (Accepted.empty() || Accepted.front() > EMaterialProgramValueType::Float4) return {};
		if (Opcode == EMaterialProgramOpcode::Swizzle) return std::vector<float>(static_cast<uint32>(Type) + 1, 0.f);
		const auto Width = IsMaterialAdaptiveNumeric(Opcode) && !(Opcode == EMaterialProgramOpcode::Lerp && Slot == 2)
			? static_cast<uint32>(Type) + 1 : static_cast<uint32>(Accepted.front()) + 1;
		float Value = 0.f;
		if (((Opcode == EMaterialProgramOpcode::Multiply || Opcode == EMaterialProgramOpcode::Divide) && Slot == 1)
			|| (Opcode == EMaterialProgramOpcode::Clamp && Slot == 2) || Opcode == EMaterialProgramOpcode::Normalize) Value = 1.f;
		if (Opcode == EMaterialProgramOpcode::Lerp) Value = Slot == 1 ? 1.f : Slot == 2 ? .5f : 0.f;
		return std::vector<float>(Width, Value);
	}

	auto DMaterialExpressionMaterialOutput::Serialize(FArchive& Ar) -> void
	{
		if (!FMaterialOutputVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
	}

	auto GetMaterialDomainOutputPins(EMaterialDomain Domain) -> std::span<const FMaterialOutputPinDefinition>
	{
		using P = EMaterialOutputPin;
		using T = EMaterialProgramValueType;
		static constexpr std::array Pins{
			FMaterialOutputPinDefinition{P::Surface, "Material Attributes", T::Surface},
			FMaterialOutputPinDefinition{P::BaseColor, "Base Color", T::Float3},
			FMaterialOutputPinDefinition{P::Normal, "Normal", T::Float3},
			FMaterialOutputPinDefinition{P::Metallic, "Metallic", T::Float},
			FMaterialOutputPinDefinition{P::Roughness, "Roughness", T::Float},
			FMaterialOutputPinDefinition{P::AmbientOcclusion, "Ambient Occlusion", T::Float},
			FMaterialOutputPinDefinition{P::Emissive, "Emissive", T::Float3},
			FMaterialOutputPinDefinition{P::Opacity, "Opacity", T::Float},
			FMaterialOutputPinDefinition{P::OpacityMask, "Opacity Mask", T::Float}};
		return Domain == EMaterialDomain::Surface ? std::span<const FMaterialOutputPinDefinition>(Pins) : std::span<const FMaterialOutputPinDefinition>{};
	}

	auto GetMaterialOutputNumericInput(FMaterialExpressionSurfaceOutputs& Outputs, EMaterialOutputPin Pin) -> FMaterialNumericInput*
	{
		switch (Pin)
		{
		case EMaterialOutputPin::BaseColor: return &Outputs.BaseColor;
		case EMaterialOutputPin::Normal: return &Outputs.Normal;
		case EMaterialOutputPin::Metallic: return &Outputs.Metallic;
		case EMaterialOutputPin::Roughness: return &Outputs.Roughness;
		case EMaterialOutputPin::AmbientOcclusion: return &Outputs.AmbientOcclusion;
		case EMaterialOutputPin::Emissive: return &Outputs.Emissive;
		case EMaterialOutputPin::Opacity: return &Outputs.Opacity;
		case EMaterialOutputPin::OpacityMask: return &Outputs.OpacityMask;
		default: return nullptr;
		}
	}

	auto GetMaterialOutputInput(FMaterialExpressionSurfaceOutputs& Outputs, EMaterialOutputPin Pin) -> FMaterialExpressionInput*
	{
		if (Pin == EMaterialOutputPin::Surface) return &Outputs.Surface;
		auto* Numeric = GetMaterialOutputNumericInput(Outputs, Pin);
		return Numeric ? &Numeric->Connection : nullptr;
	}

	auto ReadMaterialOutputDefault(const FMaterialExpressionSurfaceOutputs& Outputs, EMaterialOutputPin Pin) -> std::vector<float>
	{
		const auto* Numeric = GetMaterialOutputNumericInput(const_cast<FMaterialExpressionSurfaceOutputs&>(Outputs), Pin);
		if (!Numeric) return {};
		return Numeric->UseConstant ? Numeric->Constant : GetMaterialNumericInputFallback(
			EMaterialProgramOpcode::MakeSurface, EMaterialProgramValueType::Surface, static_cast<uint32>(Pin));
	}

	auto WriteMaterialOutputDefault(FMaterialExpressionSurfaceOutputs& Outputs, EMaterialOutputPin Pin, std::span<const float> Value) -> bool
	{
		auto* Numeric = GetMaterialOutputNumericInput(Outputs, Pin);
		if (!Numeric) return false;
		if (Value.empty()) { Numeric->UseConstant = false; return true; }
		const auto Width = static_cast<uint32>(GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Pin))) + 1;
		if (Value.size() != Width) return false;
		Numeric->SetConstant({Value.begin(), Value.end()});
		return true;
	}

	auto DMaterialExpressionMaterialOutput::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Fail(EMaterialExpressionError::MaterialOutputSinkUsedAsExpressionSource);
	}

	auto DMaterialExpression::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		auto* Owner = GetOuter();
		if (Owner && (Owner->IsA(DMaterial::StaticClass()) || Owner->IsA(DMaterialFunction::StaticClass())))
		{
			auto OwnerEvent = Event;
			OwnerEvent.MemberProperty = Owner->GetClass()->FindPropertyByName("ExpressionCollection");
			Owner->PostEditChangeProperty(OwnerEvent);
		}
	}

	auto DMaterialExpressionParameter::SetParameterDefinition(const FMaterialParameterDefinition& Definition) -> FMaterialOperationResult
	{
		const auto ExpectedType = GetParameterDefinition().Type;
		if (ExpectedType != Definition.Type)
		{
			FMaterialError Error(EMaterialParameterError::InvalidType, Definition.Id);
			Error.ParameterName = Definition.Name.ToString();
			Error.ExpectedParameterType = ExpectedType;
			Error.ActualParameterType = Definition.Type;
			return {std::move(Error)};
		}
		const auto Validation = ValidateMaterialParameterDefinitions(std::span(&Definition, 1));
		if (!Validation) return {FMaterialError(Validation)};
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
			Parameter->DefaultValue = {Texture.Texture, Texture.SamplerState, Texture.TextureFallback};
			Parameter->TextureUsage = Definition.TextureUsage;
		}
		else return {FMaterialError(EMaterialExpressionError::ParameterExpressionUnsupportedType, Definition.Id)};
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
		auto Definition = MakeDefinition(EMaterialParameterType::Texture,
			FMaterialParameterValue::MakeTexture(DefaultValue.Texture.Get(), DefaultValue.SamplerState, DefaultValue.TextureFallback));
		Definition.TextureUsage = TextureUsage;
		return Definition;
	}

}
