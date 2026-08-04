#include "Materials/MaterialTypes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_set>
#include <utility>

namespace Durin
{
	namespace
	{
		auto SetValidationFailure(
			FMaterialRenderValidationDiagnostic& OutDiagnostic,
			EMaterialRenderValidationFailure Failure,
			uint32 FieldIndex,
			std::string Message
		) -> bool
		{
			OutDiagnostic.Failure = Failure;
			OutDiagnostic.FieldIndex = FieldIndex;
			OutDiagnostic.Message = std::move(Message);
			return false;
		}

		auto GetUniformFieldSize(EMaterialRenderValueType Type) -> uint32
		{
			switch (Type)
			{
			case EMaterialRenderValueType::Scalar:
				return sizeof(float);
			case EMaterialRenderValueType::Vector3:
				return sizeof(float) * 3;
			case EMaterialRenderValueType::Vector4:
				return sizeof(float) * 4;
			case EMaterialRenderValueType::Texture2D:
				break;
			}
			return 0;
		}

		auto GetUniformFieldAlignment(EMaterialRenderValueType Type) -> uint32
		{
			switch (Type)
			{
			case EMaterialRenderValueType::Scalar:
				return sizeof(float);
			case EMaterialRenderValueType::Vector3:
			case EMaterialRenderValueType::Vector4:
				return 16;
			case EMaterialRenderValueType::Texture2D:
				break;
			}
			return 0;
		}

		auto IsUniformFieldType(EMaterialRenderValueType Type) -> bool
		{
			return Type != EMaterialRenderValueType::Texture2D;
		}

		auto WriteFloat(
			std::vector<std::byte>& Bytes,
			uint32 Offset,
			float Value
		) -> void
		{
			std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
		}

		auto MakeDefaultUniformPayload() -> std::vector<std::byte>
		{
			std::vector<std::byte> Result(32, std::byte{0});
			WriteFloat(Result, 0, 0.95f);
			WriteFloat(Result, 4, 0.62f);
			WriteFloat(Result, 8, 0.22f);
			WriteFloat(Result, 12, 1.0f);
			WriteFloat(Result, 16, 0.35f);
			WriteFloat(Result, 20, 32.0f);
			return Result;
		}

		auto IsRangeOverlapping(
			const std::vector<std::pair<uint32, uint32>>& Ranges,
			uint32 Begin,
			uint32 End
		) -> bool
		{
			return std::ranges::any_of(Ranges, [Begin, End](const auto& Range) {
				return Begin < Range.second && Range.first < End;
			});
		}
	}

	auto MakeDefaultMaterialRenderLayout() -> FMaterialRenderLayout
	{
		using namespace MaterialParameters;
		FMaterialRenderLayout Result;
		Result.Identity = {};
		Result.UniformPayloadSize = 32;
		Result.UniformFieldCount = 4;
		Result.ResourceFieldCount = 1;
		Result.Fields = {
			{
				.ParameterId = BaseColorId,
				.Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = EMaterialRenderValueType::Vector3,
				.CompactIndex = 0,
				.Offset = 0,
				.Size = 12,
			},
			{
				.ParameterId = OpacityId,
				.Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = EMaterialRenderValueType::Scalar,
				.CompactIndex = 1,
				.Offset = 12,
				.Size = 4,
			},
			{
				.ParameterId = SpecularStrengthId,
				.Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = EMaterialRenderValueType::Scalar,
				.CompactIndex = 2,
				.Offset = 16,
				.Size = 4,
			},
			{
				.ParameterId = ShininessId,
				.Storage = EMaterialRenderFieldStorage::Uniform,
				.Type = EMaterialRenderValueType::Scalar,
				.CompactIndex = 3,
				.Offset = 20,
				.Size = 4,
			},
			{
				.ParameterId = BaseColorTextureId,
				.Storage = EMaterialRenderFieldStorage::Resource,
				.Type = EMaterialRenderValueType::Texture2D,
				.CompactIndex = 0,
				.Offset = 0,
				.Size = 0,
			},
		};
		return Result;
	}

	auto ValidateMaterialRenderLayout(
		const FMaterialRenderLayout& Layout,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		OutDiagnostic = {};
		if (Layout.Identity.Version != CurrentMaterialRenderLayoutVersion)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::UnsupportedVersion,
				0,
				std::format(
					"Material render layout version {} is unsupported; expected {}.",
					Layout.Identity.Version,
					CurrentMaterialRenderLayoutVersion));
		}
		if (Layout.Identity.Id != MaterialRenderLayoutV1Id)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::UnsupportedIdentity,
				0,
				"Material render layout identity is unsupported.");
		}
		if (Layout.UniformPayloadSize == 0
			|| Layout.UniformPayloadSize > MaterialRenderMaxUniformPayloadBytes
			|| Layout.UniformPayloadSize % 16 != 0
			|| Layout.Fields.size() > MaterialRenderMaxFieldCount
			|| Layout.UniformFieldCount > MaterialRenderMaxFieldCount
			|| Layout.ResourceFieldCount > MaterialRenderMaxResourceCount
			|| Layout.Fields.size()
				!= static_cast<size_t>(Layout.UniformFieldCount)
					+ Layout.ResourceFieldCount)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidCounts,
				0,
				"Material render layout counts or uniform payload size are invalid.");
		}

		std::unordered_set<FGuid> ParameterIds;
		std::vector<bool> UniformIndices(Layout.UniformFieldCount, false);
		std::vector<bool> ResourceIndices(Layout.ResourceFieldCount, false);
		std::vector<std::pair<uint32, uint32>> UniformRanges;
		UniformRanges.reserve(Layout.UniformFieldCount);
		for (uint32 FieldIndex = 0; FieldIndex < Layout.Fields.size(); ++FieldIndex)
		{
			const FMaterialRenderField& Field = Layout.Fields[FieldIndex];
			if (!Field.ParameterId.IsValid())
			{
				return SetValidationFailure(
					OutDiagnostic,
					EMaterialRenderValidationFailure::InvalidField,
					FieldIndex,
					"Material render field has an invalid parameter GUID.");
			}
			if (!ParameterIds.insert(Field.ParameterId).second)
			{
				return SetValidationFailure(
					OutDiagnostic,
					EMaterialRenderValidationFailure::DuplicateField,
					FieldIndex,
					"Material render layout contains a duplicate parameter GUID.");
			}

			if (Field.Storage == EMaterialRenderFieldStorage::Uniform)
			{
				if (!IsUniformFieldType(Field.Type)
					|| Field.CompactIndex >= UniformIndices.size()
					|| UniformIndices[Field.CompactIndex])
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::InvalidField,
						FieldIndex,
						"Material render uniform field type or compact index is invalid.");
				}
				UniformIndices[Field.CompactIndex] = true;
				const uint32 ExpectedSize = GetUniformFieldSize(Field.Type);
				const uint32 ExpectedAlignment = GetUniformFieldAlignment(Field.Type);
				if (Field.Size != ExpectedSize)
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::InvalidField,
						FieldIndex,
						"Material render uniform field size does not match its type.");
				}
				if (Field.Offset % ExpectedAlignment != 0)
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::InvalidAlignment,
						FieldIndex,
						"Material render uniform field offset is misaligned.");
				}
				if (Field.Offset > Layout.UniformPayloadSize
					|| Field.Size > Layout.UniformPayloadSize - Field.Offset)
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::InvalidOffset,
						FieldIndex,
						"Material render uniform field exceeds the payload.");
				}
				const uint32 End = Field.Offset + Field.Size;
				if (IsRangeOverlapping(UniformRanges, Field.Offset, End))
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::OverlappingFields,
						FieldIndex,
						"Material render uniform fields overlap.");
				}
				UniformRanges.emplace_back(Field.Offset, End);
			}
			else if (Field.Storage == EMaterialRenderFieldStorage::Resource)
			{
				if (Field.Type != EMaterialRenderValueType::Texture2D
					|| Field.CompactIndex >= ResourceIndices.size()
					|| ResourceIndices[Field.CompactIndex]
					|| Field.Offset != 0
					|| Field.Size != 0)
				{
					return SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::InvalidResource,
						FieldIndex,
						"Material render resource field type, index, or range is invalid.");
				}
				ResourceIndices[Field.CompactIndex] = true;
			}
			else
			{
				return SetValidationFailure(
					OutDiagnostic,
					EMaterialRenderValidationFailure::InvalidField,
					FieldIndex,
					"Material render field storage class is unsupported.");
			}
		}

		if (std::ranges::any_of(UniformIndices, [](bool Used) { return !Used; })
			|| std::ranges::any_of(ResourceIndices, [](bool Used) { return !Used; }))
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidCounts,
				0,
				"Material render compact indices are not contiguous.");
		}
		return true;
	}

	FMaterialRenderRepresentation::FMaterialRenderRepresentation()
		: Layout(MakeDefaultMaterialRenderLayout())
		, UniformPayload(MakeDefaultUniformPayload())
		, Resources(1)
		, bFallback(true)
	{
	}

	FMaterialRenderRepresentation::FMaterialRenderRepresentation(
		FMaterialRenderLayout InLayout,
		std::vector<std::byte> InUniformPayload,
		std::vector<FRHITextureReferenceRef> InResources,
		bool bInFallback)
		: Layout(std::move(InLayout))
		, UniformPayload(std::move(InUniformPayload))
		, Resources(std::move(InResources))
		, bFallback(bInFallback)
	{
	}

	auto FMaterialRenderRepresentation::TryCreate(
		FMaterialRenderRepresentationInput Input,
		FMaterialRenderRepresentation& OutRepresentation,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		OutRepresentation = FMaterialRenderRepresentation();
		OutDiagnostic = {};
		if (!ValidateMaterialRenderLayout(Input.Layout, OutDiagnostic)) return false;
		if (Input.UniformPayload.size() != Input.Layout.UniformPayloadSize)
		{
			SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidPayloadSize,
				0,
				"Material render uniform payload size does not match its layout.");
			return false;
		}
		if (Input.Resources.size() != Input.Layout.ResourceFieldCount)
		{
			SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidResource,
				0,
				"Material render resource count does not match its layout.");
			return false;
		}

		std::vector<bool> Covered(Input.UniformPayload.size(), false);
		for (uint32 FieldIndex = 0; FieldIndex < Input.Layout.Fields.size(); ++FieldIndex)
		{
			const FMaterialRenderField& Field = Input.Layout.Fields[FieldIndex];
			if (Field.Storage != EMaterialRenderFieldStorage::Uniform) continue;
			for (uint32 ByteIndex = Field.Offset;
				ByteIndex < Field.Offset + Field.Size;
				++ByteIndex)
			{
				Covered[ByteIndex] = true;
			}
			for (uint32 ValueIndex = 0;
				ValueIndex < Field.Size / sizeof(float);
				++ValueIndex)
			{
				float Value = 0.0f;
				std::memcpy(
					&Value,
					Input.UniformPayload.data()
						+ Field.Offset + ValueIndex * sizeof(float),
					sizeof(Value));
				if (!std::isfinite(Value))
				{
					SetValidationFailure(
						OutDiagnostic,
						EMaterialRenderValidationFailure::NonFiniteValue,
						FieldIndex,
						"Material render uniform payload contains a non-finite value.");
					return false;
				}
			}
		}
		for (uint32 ByteIndex = 0; ByteIndex < Input.UniformPayload.size(); ++ByteIndex)
		{
			if (!Covered[ByteIndex] && Input.UniformPayload[ByteIndex] != std::byte{0})
			{
				SetValidationFailure(
					OutDiagnostic,
					EMaterialRenderValidationFailure::NonZeroPadding,
					0,
					"Material render uniform padding must be zero.");
				return false;
			}
		}

		OutRepresentation = FMaterialRenderRepresentation(
			std::move(Input.Layout),
			std::move(Input.UniformPayload),
			std::move(Input.Resources),
			false);
		return true;
	}

	auto FMaterialRenderRepresentation::GetLayout() const
		-> const FMaterialRenderLayout&
	{
		return Layout;
	}

	auto FMaterialRenderRepresentation::GetUniformPayload() const
		-> std::span<const std::byte>
	{
		return UniformPayload;
	}

	auto FMaterialRenderRepresentation::GetResources() const
		-> std::span<const FRHITextureReferenceRef>
	{
		return Resources;
	}

	auto FMaterialRenderRepresentation::IsFallback() const -> bool
	{
		return bFallback;
	}

	auto TryGetMaterialRenderV1Binding(
		const FMaterialRenderRepresentation& Representation,
		FMaterialRenderV1Binding& OutBinding,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		OutBinding = FMaterialRenderV1Binding{};
		OutDiagnostic = {};

		static const FMaterialRenderLayout ExpectedLayout =
			MakeDefaultMaterialRenderLayout();
		const FMaterialRenderLayout& Layout = Representation.GetLayout();
		if (Layout.Identity != ExpectedLayout.Identity)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::UnsupportedIdentity,
				0,
				"Material render binding layout identity is unsupported.");
		}
		if (Layout != ExpectedLayout)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidField,
				0,
				"Material render binding layout does not match the v1 contract.");
		}

		const std::span<const std::byte> Payload =
			Representation.GetUniformPayload();
		if (Payload.size() != ExpectedLayout.UniformPayloadSize)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidPayloadSize,
				0,
				"Material render binding payload size is invalid.");
		}
		const std::span<const FRHITextureReferenceRef> Resources =
			Representation.GetResources();
		if (Resources.size() != ExpectedLayout.ResourceFieldCount)
		{
			return SetValidationFailure(
				OutDiagnostic,
				EMaterialRenderValidationFailure::InvalidResource,
				0,
				"Material render binding resource count is invalid.");
		}

		auto ReadFloat = [&Payload](uint32 Offset) {
			float Value = 0.0f;
			std::memcpy(&Value, Payload.data() + Offset, sizeof(Value));
			return Value;
		};
		OutBinding.BaseColor = FVector4f(
			ReadFloat(ExpectedLayout.Fields[0].Offset),
			ReadFloat(ExpectedLayout.Fields[0].Offset + 4),
			ReadFloat(ExpectedLayout.Fields[0].Offset + 8),
			ReadFloat(ExpectedLayout.Fields[1].Offset));
		OutBinding.SpecularStrength =
			ReadFloat(ExpectedLayout.Fields[2].Offset);
		OutBinding.Shininess = ReadFloat(ExpectedLayout.Fields[3].Offset);
		OutBinding.BaseColorTexture = Resources[0];
		return true;
	}

	FMaterialRenderRepresentationBuilder::FMaterialRenderRepresentationBuilder(
		const FMaterialRenderRepresentation& Source)
		: Input{.Layout = Source.GetLayout()}
	{
		Input.UniformPayload.assign(
			Source.GetUniformPayload().begin(),
			Source.GetUniformPayload().end());
		Input.Resources.assign(
			Source.GetResources().begin(),
			Source.GetResources().end());
	}

	auto FMaterialRenderRepresentationBuilder::FindField(
		const FGuid& ParameterId) const -> const FMaterialRenderField*
	{
		const auto It = std::ranges::find(
			Input.Layout.Fields,
			ParameterId,
			&FMaterialRenderField::ParameterId);
		return It == Input.Layout.Fields.end() ? nullptr : &*It;
	}

	auto FMaterialRenderRepresentationBuilder::RejectField(
		const FGuid& ParameterId) -> bool
	{
		bInvalid = true;
		InvalidParameterId = ParameterId;
		return false;
	}

	auto FMaterialRenderRepresentationBuilder::SetScalar(
		const FGuid& ParameterId,
		float Value
	) -> bool
	{
		const FMaterialRenderField* Field = FindField(ParameterId);
		if (Field == nullptr) return false;
		if (Field->Storage != EMaterialRenderFieldStorage::Uniform
			|| Field->Type != EMaterialRenderValueType::Scalar)
		{
			return RejectField(ParameterId);
		}
		WriteFloat(Input.UniformPayload, Field->Offset, Value);
		return true;
	}

	auto FMaterialRenderRepresentationBuilder::SetVector(
		const FGuid& ParameterId,
		const FVector3& Value
	) -> bool
	{
		const FMaterialRenderField* Field = FindField(ParameterId);
		if (Field == nullptr) return false;
		if (Field->Storage != EMaterialRenderFieldStorage::Uniform
			|| Field->Type != EMaterialRenderValueType::Vector3)
		{
			return RejectField(ParameterId);
		}
		WriteFloat(Input.UniformPayload, Field->Offset, static_cast<float>(Value.x));
		WriteFloat(Input.UniformPayload, Field->Offset + 4, static_cast<float>(Value.y));
		WriteFloat(Input.UniformPayload, Field->Offset + 8, static_cast<float>(Value.z));
		return true;
	}

	auto FMaterialRenderRepresentationBuilder::SetTexture(
		const FGuid& ParameterId,
		const FRHITextureReferenceRef& Value
	) -> bool
	{
		const FMaterialRenderField* Field = FindField(ParameterId);
		if (Field == nullptr) return false;
		if (Field->Storage != EMaterialRenderFieldStorage::Resource
			|| Field->Type != EMaterialRenderValueType::Texture2D
			|| Field->CompactIndex >= Input.Resources.size())
		{
			return RejectField(ParameterId);
		}
		Input.Resources[Field->CompactIndex] = Value;
		return true;
	}

	auto FMaterialRenderRepresentationBuilder::Build(
		FMaterialRenderRepresentation& OutRepresentation,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool
	{
		if (bInvalid)
		{
			OutRepresentation = FMaterialRenderRepresentation();
			OutDiagnostic = {
				.Failure = EMaterialRenderValidationFailure::InvalidField,
				.FieldIndex = 0,
				.Message = std::format(
					"Material render parameter {} does not match the selected layout.",
					InvalidParameterId.ToString()),
			};
			return false;
		}
		return FMaterialRenderRepresentation::TryCreate(
			Input,
			OutRepresentation,
			OutDiagnostic);
	}

	auto UpgradeMaterialParameterSchemaVersion(
		FMaterialParameterSchemaVersion& InOutVersion,
		std::string& OutWarning,
		std::string& OutError
	) -> bool
	{
		OutWarning.clear();
		OutError.clear();
		if (InOutVersion == 0)
		{
			InOutVersion = CurrentMaterialParameterSchemaVersion;
			OutWarning = "Material parameter schema version was missing; assumed version 1.";
			return true;
		}
		if (InOutVersion == CurrentMaterialParameterSchemaVersion) return true;
		OutError = std::format(
			"Material parameter schema version {} is unsupported; expected {}.",
			InOutVersion,
			CurrentMaterialParameterSchemaVersion);
		return false;
	}

	auto FMaterialParameterValue::MakeScalar(float Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.ScalarValue = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeVector(const FVector3& Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.VectorValue = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeTexture(DTexture2D* Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.TextureValue = Value;
		return Result;
	}

	namespace MaterialParameters
	{
		auto BaseColorName() -> const FName&
		{
			static const FName Name("BaseColor");
			return Name;
		}

		auto BaseColorTextureName() -> const FName&
		{
			static const FName Name("BaseColorTexture");
			return Name;
		}

		auto OpacityName() -> const FName&
		{
			static const FName Name("Opacity");
			return Name;
		}

		auto SpecularStrengthName() -> const FName&
		{
			static const FName Name("SpecularStrength");
			return Name;
		}

		auto ShininessName() -> const FName&
		{
			static const FName Name("Shininess");
			return Name;
		}
	}

	namespace
	{
		auto MakeDefinition(
			FGuid Id,
			FName Name,
			EMaterialParameterType Type,
			FMaterialParameterValue Value,
			std::string DisplayName,
			int32 SortOrder,
			EMaterialParameterPresentation Presentation,
			bool bHasRange = false,
			float MinimumValue = 0.0f,
			float MaximumValue = 0.0f,
			ETextureUsage TextureUsage = ETextureUsage::Color
		) -> FMaterialParameterDefinition
		{
			FMaterialParameterDefinition Result;
			Result.Id = Id;
			Result.Name = Name;
			Result.Type = Type;
			Result.Value = std::move(Value);
			Result.DisplayName = std::move(DisplayName);
			Result.GroupName = FName("Surface");
			Result.SortOrder = SortOrder;
			Result.Presentation = Presentation;
			Result.bHasRange = bHasRange;
			Result.MinimumValue = MinimumValue;
			Result.MaximumValue = MaximumValue;
			Result.TextureUsage = TextureUsage;
			return Result;
		}

		auto HasCanonicalMetadata(
			const FMaterialParameterDefinition& Definition,
			const FMaterialParameterDefinition& Canonical
		) -> bool
		{
			return Definition.Id == Canonical.Id
				&& Definition.Name == Canonical.Name
				&& Definition.Type == Canonical.Type
				&& Definition.DisplayName == Canonical.DisplayName
				&& Definition.GroupName == Canonical.GroupName
				&& Definition.SortOrder == Canonical.SortOrder
				&& Definition.Presentation == Canonical.Presentation
				&& Definition.bHasRange == Canonical.bHasRange
				&& Definition.MinimumValue == Canonical.MinimumValue
				&& Definition.MaximumValue == Canonical.MaximumValue
				&& Definition.TextureUsage == Canonical.TextureUsage;
		}
	}

	auto MakeCanonicalMaterialParameterDefinitions() -> std::vector<FMaterialParameterDefinition>
	{
		using namespace MaterialParameters;
		std::vector<FMaterialParameterDefinition> Result;
		Result.reserve(5);
		Result.push_back(MakeDefinition(BaseColorId, BaseColorName(), EMaterialParameterType::Vector,
			FMaterialParameterValue::MakeVector({0.95, 0.62, 0.22}), "Base Color", 0,
			EMaterialParameterPresentation::Color, true, 0.0f, 1.0f));
		Result.push_back(MakeDefinition(BaseColorTextureId, BaseColorTextureName(), EMaterialParameterType::Texture,
			FMaterialParameterValue::MakeTexture(nullptr), "Base Color Texture", 1,
			EMaterialParameterPresentation::AssetPicker, false, 0.0f, 0.0f, ETextureUsage::Color));
		Result.push_back(MakeDefinition(OpacityId, OpacityName(), EMaterialParameterType::Scalar,
			FMaterialParameterValue::MakeScalar(1.0f), "Opacity", 2,
			EMaterialParameterPresentation::Drag, true, 0.0f, 1.0f));
		Result.push_back(MakeDefinition(SpecularStrengthId, SpecularStrengthName(), EMaterialParameterType::Scalar,
			FMaterialParameterValue::MakeScalar(0.35f), "Specular Strength", 3,
			EMaterialParameterPresentation::Drag, true, 0.0f, 1.0f));
		Result.push_back(MakeDefinition(ShininessId, ShininessName(), EMaterialParameterType::Scalar,
			FMaterialParameterValue::MakeScalar(32.0f), "Shininess", 4,
			EMaterialParameterPresentation::Drag, true, 1.0f, 256.0f));
		return Result;
	}

	auto GetCanonicalMaterialParameterDefinitions() -> std::span<const FMaterialParameterDefinition>
	{
		static const std::vector<FMaterialParameterDefinition> Definitions = MakeCanonicalMaterialParameterDefinitions();
		return Definitions;
	}

	auto ValidateCanonicalMaterialParameterDefinitions(
		std::span<const FMaterialParameterDefinition> Definitions,
		std::string& OutError
	) -> bool
	{
		OutError.clear();
		const std::span Canonical = GetCanonicalMaterialParameterDefinitions();
		if (Definitions.size() != Canonical.size())
		{
			OutError = std::format("Material parameter schema contains {} definitions; expected {}.", Definitions.size(), Canonical.size());
			return false;
		}

		std::unordered_set<FGuid> Ids;
		std::unordered_set<FName> Names;
		for (size_t Index = 0; Index < Definitions.size(); ++Index)
		{
			const FMaterialParameterDefinition& Definition = Definitions[Index];
			if (!Definition.Id.IsValid())
			{
				OutError = std::format("Material parameter definition {} has an invalid GUID.", Index);
				return false;
			}
			if (!Ids.insert(Definition.Id).second)
			{
				OutError = std::format("Material parameter schema contains duplicate GUID {}.", Definition.Id.ToString());
				return false;
			}
			if (Definition.Name.IsNone())
			{
				OutError = std::format("Material parameter definition {} has a None name.", Index);
				return false;
			}
			if (!Names.insert(Definition.Name).second)
			{
				OutError = std::format("Material parameter schema contains duplicate name '{}'.", Definition.Name.ToString());
				return false;
			}
			if (!HasCanonicalMetadata(Definition, Canonical[Index]))
			{
				OutError = std::format("Material parameter definition {} ('{}') does not match the canonical identity, type, order, or metadata.",
					Index, Definition.Name.ToString());
				return false;
			}
		}
		return true;
	}

	auto ValidateMaterialStaticProperties(
		const FMaterialStaticProperties& Properties,
		std::string& OutError
	) -> bool
	{
		OutError.clear();
		switch (Properties.BlendMode)
		{
		case EMaterialBlendMode::Opaque:
		case EMaterialBlendMode::Masked:
		case EMaterialBlendMode::Translucent:
			break;
		default:
			OutError = "Material blend mode is invalid.";
			return false;
		}
		switch (Properties.ShadingModel)
		{
		case EMaterialShadingModel::Lit:
		case EMaterialShadingModel::Unlit:
			break;
		default:
			OutError = "Material shading model is invalid.";
			return false;
		}
		switch (Properties.DepthWritePolicy)
		{
		case EMaterialDepthWritePolicy::Automatic:
		case EMaterialDepthWritePolicy::Enabled:
		case EMaterialDepthWritePolicy::Disabled:
			break;
		default:
			OutError = "Material depth-write policy is invalid.";
			return false;
		}
		if (!std::isfinite(Properties.OpacityMaskThreshold)
			|| Properties.OpacityMaskThreshold < 0.0f
			|| Properties.OpacityMaskThreshold > 1.0f)
		{
			OutError = "Material opacity-mask threshold must be finite and in the inclusive range [0, 1].";
			return false;
		}
		return true;
	}
}
