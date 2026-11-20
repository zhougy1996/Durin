#include "Materials/MaterialExpressionBuild.h"
#include "DObject/DurinPropertyTypes.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionMaterialOutput.h"
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
	auto CaptureMaterialOutputBindings(FMaterialAttributeRoot Root) -> std::pair<FMaterialAttributeRoot, std::vector<FGuid>>
	{
		std::vector<FGuid> Sources;
		const auto Capture = [&](FMaterialExpressionInput& Input) {
			Sources.push_back(Input.GetExpressionId());
			Input.Expression.Reset();
		};
		Capture(Root.Attributes);
		for (auto& Binding : Root.Properties.Bindings) Capture(Binding.Input.Connection);
		return {std::move(Root), std::move(Sources)};
	}

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
	FMaterialAttributeRoot::FMaterialAttributeRoot()
	{
		Properties.Bindings.reserve(64);
		for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions())
			Properties.Bindings.push_back({Definition.AttributeID,
				FMaterialNumericInput(uint32(Definition.Semantics.Type) + 1)});
	}
	auto FMaterialAttributeRoot::Get(EMaterialProperty Property) -> FMaterialNumericInput&
	{
		const auto* Definition = FMaterialAttributeDefinitionMap::Find(Property);
		check(Definition);
		for (auto& Binding : Properties.Bindings)
			if (Binding.AttributeID == Definition->AttributeID) return Binding.Input;
		Properties.Bindings.reserve(64);
		FMaterialNumericInput Input(static_cast<uint32>(Definition->Semantics.Type) + 1);
		Properties.Bindings.push_back({Definition->AttributeID, std::move(Input)});
		return Get(Property);
	}
	auto FMaterialAttributeRoot::Get(EMaterialProperty Property) const -> const FMaterialNumericInput&
	{
		const auto ID = FMaterialAttributeDefinitionMap::GetID(Property);
		for (const auto& Binding : Properties.Bindings) if (Binding.AttributeID == ID) return Binding.Input;
		static const auto Defaults = [] {
			FMaterialAttributeRoot Root;
			for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions()) Root.Get(Definition.Property);
			return Root;
		}();
		for (const auto& Binding : Defaults.Properties.Bindings) if (Binding.AttributeID == ID) return Binding.Input;
		check(false);
		return Defaults.Properties.Bindings.front().Input;
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
		if (Opcode == EMaterialProgramOpcode::MakeMaterialAttributes)
		{
			const FMaterialAttributeValues Defaults;
			const auto Value = GetMaterialSurfaceOutputDefault(Defaults, FMaterialAttributeDefinitionMap::GetDefinitions()[Slot].Property);
			const std::array Components{Value.X, Value.Y, Value.Z, Value.W};
			return {Components.begin(), Components.begin() + static_cast<uint32>(GetMaterialSurfaceOutputType(FMaterialAttributeDefinitionMap::GetDefinitions()[Slot].Property)) + 1};
		}
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

	auto DMaterialExpressionMaterialOutput::Serialize(FArchive& Ar) -> void
	{
		if (!FMaterialOutputVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
	}

	auto GetMaterialDomainOutputPins(EMaterialDomain Domain) -> std::span<const FMaterialOutputPinDefinition>
	{
		static const auto Pins = [] {
			std::vector<FMaterialOutputPinDefinition> Result{{EMaterialOutputPin::Surface, "Material Attributes", EMaterialProgramValueType::MaterialAttributes}};
			for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions())
				Result.push_back({static_cast<EMaterialOutputPin>(Definition.Property), Definition.Name, Definition.Semantics.Type});
			return Result;
		}();
		static const std::array PostProcessPins{FMaterialOutputPinDefinition{EMaterialOutputPin::Emissive, "Emissive Color", EMaterialProgramValueType::Float3}};
		if (Domain == EMaterialDomain::PostProcess) return PostProcessPins;
		return Domain == EMaterialDomain::Surface ? std::span<const FMaterialOutputPinDefinition>(Pins) : std::span<const FMaterialOutputPinDefinition>{};
	}

	auto GetMaterialOutputNumericInput(FMaterialAttributeRoot& Outputs, EMaterialOutputPin Pin) -> FMaterialNumericInput*
	{
		const auto Property = static_cast<EMaterialProperty>(Pin);
		return FMaterialAttributeDefinitionMap::Find(Property) ? &Outputs.Get(Property) : nullptr;
	}

	auto GetMaterialOutputInput(FMaterialAttributeRoot& Outputs, EMaterialOutputPin Pin) -> FMaterialExpressionInput*
	{
		if (Pin == EMaterialOutputPin::Surface) return &Outputs.Attributes;
		auto* Numeric = GetMaterialOutputNumericInput(Outputs, Pin);
		return Numeric ? &Numeric->Connection : nullptr;
	}

	auto ReadMaterialOutputDefault(const FMaterialAttributeRoot& Outputs, EMaterialOutputPin Pin) -> std::vector<float>
	{
		const auto* Definition = FMaterialAttributeDefinitionMap::Find(static_cast<EMaterialProperty>(Pin));
		if (!Definition) return {};
		const auto& Numeric = Outputs.Get(Definition->Property);
		return Numeric.UseConstant ? Numeric.Constant : GetMaterialNumericInputFallback(
			EMaterialProgramOpcode::MakeMaterialAttributes, EMaterialProgramValueType::MaterialAttributes, static_cast<uint32>(Pin));
	}

	auto WriteMaterialOutputDefault(FMaterialAttributeRoot& Outputs, EMaterialOutputPin Pin, std::span<const float> Value) -> bool
	{
		auto* Numeric = GetMaterialOutputNumericInput(Outputs, Pin);
		if (!Numeric) return false;
		if (Value.empty()) { Numeric->UseConstant = false; return true; }
		const auto Width = Pin == EMaterialOutputPin::WorldPositionOffset ? 3u
			: static_cast<uint32>(GetMaterialSurfaceOutputType(static_cast<EMaterialProperty>(Pin))) + 1;
		if (Value.size() != Width) return false;
		Numeric->SetConstant({Value.begin(), Value.end()});
		return true;
	}

	auto DMaterialExpressionMaterialOutput::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Fail("A material output is a sink and cannot be used as an expression source.");
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
