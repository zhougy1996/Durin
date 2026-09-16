#include "Materials/MaterialExpressions.h"
#include "Materials/MaterialExpressionBuild.h"

#include "Threading/RunnableThread.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialFunction.h"
#include "Materials/Material.h"
#include "DObject/Property.h"

namespace Durin
{
	auto GetMaterialDomainOutputPins(EMaterialDomain Domain) -> std::span<const FMaterialOutputPinDefinition>
	{
		using P = EMaterialOutputPin;
		using T = EMaterialProgramValueType;
		static constexpr std::array Pins{
			FMaterialOutputPinDefinition{P::Surface, "Surface", T::Surface},
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

	auto GetMaterialOutputInput(FMaterialExpressionSurfaceOutputs& Outputs, EMaterialOutputPin Pin) -> FMaterialExpressionInput*
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
		case EMaterialOutputPin::Surface: return &Outputs.Surface;
		default: return nullptr;
		}
	}

	auto ReadMaterialOutputDefault(const FMaterialExpressionSurfaceOutputs& Outputs, EMaterialOutputPin Pin) -> std::vector<float>
	{
		switch (Pin)
		{
		case EMaterialOutputPin::BaseColor: return {static_cast<float>(Outputs.BaseColorDefault.x), static_cast<float>(Outputs.BaseColorDefault.y), static_cast<float>(Outputs.BaseColorDefault.z)};
		case EMaterialOutputPin::Normal: return {static_cast<float>(Outputs.NormalDefault.x), static_cast<float>(Outputs.NormalDefault.y), static_cast<float>(Outputs.NormalDefault.z)};
		case EMaterialOutputPin::Metallic: return {Outputs.MetallicDefault};
		case EMaterialOutputPin::Roughness: return {Outputs.RoughnessDefault};
		case EMaterialOutputPin::AmbientOcclusion: return {Outputs.AmbientOcclusionDefault};
		case EMaterialOutputPin::Emissive: return {static_cast<float>(Outputs.EmissiveDefault.x), static_cast<float>(Outputs.EmissiveDefault.y), static_cast<float>(Outputs.EmissiveDefault.z)};
		case EMaterialOutputPin::Opacity: return {Outputs.OpacityDefault};
		case EMaterialOutputPin::OpacityMask: return {Outputs.OpacityMaskDefault};
		default: return {};
		}
	}

	auto WriteMaterialOutputDefault(FMaterialExpressionSurfaceOutputs& Outputs, EMaterialOutputPin Pin, std::span<const float> Value) -> bool
	{
		if (Value.size() != ReadMaterialOutputDefault(Outputs, Pin).size() || Value.empty()) return false;
		switch (Pin)
		{
		case EMaterialOutputPin::BaseColor: Outputs.BaseColorDefault = FVector3(Value[0], Value[1], Value[2]); return true;
		case EMaterialOutputPin::Normal: Outputs.NormalDefault = FVector3(Value[0], Value[1], Value[2]); return true;
		case EMaterialOutputPin::Metallic: Outputs.MetallicDefault = Value[0]; return true;
		case EMaterialOutputPin::Roughness: Outputs.RoughnessDefault = Value[0]; return true;
		case EMaterialOutputPin::AmbientOcclusion: Outputs.AmbientOcclusionDefault = Value[0]; return true;
		case EMaterialOutputPin::Emissive: Outputs.EmissiveDefault = FVector3(Value[0], Value[1], Value[2]); return true;
		case EMaterialOutputPin::Opacity: Outputs.OpacityDefault = Value[0]; return true;
		case EMaterialOutputPin::OpacityMask: Outputs.OpacityMaskDefault = Value[0]; return true;
		default: return false;
		}
	}

	auto DMaterialExpressionMaterialOutput::Build(FMaterialExpressionBuildContext& Context,
		uint8, FGuid) const -> FMaterialExpressionBuildValue
	{
		return Context.Fail("A material output is a sink and cannot be used as an expression source.");
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

	auto DMaterialExpressionParameter::SetParameterDefinition(const FMaterialParameterDefinition& Definition) -> bool
	{
		if (GetParameterDefinition().Type != Definition.Type
			|| !ValidateMaterialParameterDefinitions(std::span(&Definition, 1))) return false;
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
		else return false;
		Metadata = {Definition.Id, Definition.Name, Definition.DisplayName, Definition.GroupName,
			Definition.SortOrder, Definition.Presentation};
		return true;
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
