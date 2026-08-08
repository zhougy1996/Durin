#include "PackageV3Measurement.h"

#include "DObject/DurinPropertyTypes.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>

namespace Durin::Testing
{
	namespace
	{
		constexpr uint32 DastMagic = 0x54534144;
		constexpr uint32 DastV3 = 3;
		constexpr uint64 MaximumStringBytes = 1024 * 1024;
		constexpr uint64 MaximumDependencies = 100000;
		constexpr uint64 MaximumObjects = 1000000;
		constexpr uint64 MaximumFields = 100000;
		constexpr uint64 MaximumContainerElements = 1000000;
		constexpr uint32 MaximumDepth = 64;

		enum class ELogicalKind
		{
			Scalar,
			String,
			Name,
			Guid,
			Enum,
			Object,
			SoftObject,
			Struct,
			Array,
			Map
		};

		struct FLogicalType
		{
			ELogicalKind Kind = ELogicalKind::Scalar;
			uint64 ScalarBytes = 0;
			std::unique_ptr<FLogicalType> Element;
			std::unique_ptr<FLogicalType> Key;
			std::unique_ptr<FLogicalType> Value;
		};

		struct FReader
		{
			std::span<const uint8> Bytes;
			size_t Offset = 0;

			auto Remaining(size_t End) const -> size_t
			{
				return Offset <= End ? End - Offset : 0;
			}

			template<typename T>
			auto Read(T& Value, size_t End, uint64& Category) -> bool
			{
				if (Offset > End || sizeof(T) > End - Offset) return false;
				std::memcpy(&Value, Bytes.data() + Offset, sizeof(T));
				Offset += sizeof(T);
				Category += sizeof(T);
				return true;
			}

			auto Consume(size_t Size, size_t End, uint64& Category) -> bool
			{
				if (Offset > End || Size > End - Offset) return false;
				Offset += Size;
				Category += Size;
				return true;
			}
		};

		auto Fail(std::string& Error, std::string_view Message) -> bool
		{
			Error.assign(Message);
			return false;
		}

		auto ParseUnsigned(std::string_view Text, uint64& Out) -> bool
		{
			const char* First = Text.data();
			const char* Last = First + Text.size();
			const auto Result = std::from_chars(First, Last, Out);
			return Result.ec == std::errc{} && Result.ptr == Last;
		}

		auto FindTopLevelComma(std::string_view Text) -> size_t
		{
			uint32 Depth = 0;
			for (size_t Index = 0; Index < Text.size(); ++Index)
			{
				if (Text[Index] == '<') ++Depth;
				else if (Text[Index] == '>')
				{
					if (Depth == 0) return std::string_view::npos;
					--Depth;
				}
				else if (Text[Index] == ',' && Depth == 0) return Index;
			}
			return std::string_view::npos;
		}

		auto ParseLogicalType(std::string_view Signature, FLogicalType& Out) -> bool
		{
			if (Signature.starts_with("Native<"))
			{
				const size_t Version = Signature.rfind(">:v");
				if (Version == std::string_view::npos || Version < 7) return false;
				uint64 IgnoredVersion = 0;
				return ParseUnsigned(Signature.substr(Version + 3), IgnoredVersion)
					&& ParseLogicalType(Signature.substr(7, Version - 7), Out);
			}
			if (Signature.starts_with("Array<") && Signature.ends_with('>'))
			{
				Out.Kind = ELogicalKind::Array;
				Out.Element = std::make_unique<FLogicalType>();
				return ParseLogicalType(Signature.substr(6, Signature.size() - 7), *Out.Element);
			}
			if (Signature.starts_with("Map<") && Signature.ends_with('>'))
			{
				const std::string_view Body = Signature.substr(4, Signature.size() - 5);
				const size_t Comma = FindTopLevelComma(Body);
				if (Comma == std::string_view::npos) return false;
				Out.Kind = ELogicalKind::Map;
				Out.Key = std::make_unique<FLogicalType>();
				Out.Value = std::make_unique<FLogicalType>();
				return ParseLogicalType(Body.substr(0, Comma), *Out.Key)
					&& ParseLogicalType(Body.substr(Comma + 1), *Out.Value);
			}
			if (Signature.starts_with("Object:"))
			{
				Out.Kind = ELogicalKind::Object;
				return true;
			}
			if (Signature.starts_with("SoftObject:"))
			{
				Out.Kind = ELogicalKind::SoftObject;
				return true;
			}
			if (Signature.starts_with("Struct<") && Signature.ends_with('>'))
			{
				Out.Kind = ELogicalKind::Struct;
				return true;
			}
			if (Signature.starts_with("Enum:"))
			{
				const size_t Colon = Signature.rfind(':');
				Out.Kind = ELogicalKind::Enum;
				return Colon != std::string_view::npos
					&& ParseUnsigned(Signature.substr(Colon + 1), Out.ScalarBytes)
					&& Out.ScalarBytes != 0 && Out.ScalarBytes <= sizeof(uint64);
			}

			const size_t Colon = Signature.rfind(':');
			uint64 NativeKind = 0;
			if (Colon == std::string_view::npos
				|| !ParseUnsigned(Signature.substr(0, Colon), NativeKind)) return false;
			if (NativeKind == uint64(DurinCodeGen::EPropertyGenFlags::String))
				Out.Kind = ELogicalKind::String;
			else if (NativeKind == uint64(DurinCodeGen::EPropertyGenFlags::Name))
				Out.Kind = ELogicalKind::Name;
			else if (NativeKind == uint64(DurinCodeGen::EPropertyGenFlags::Guid))
				Out.Kind = ELogicalKind::Guid;
			else
			{
				Out.Kind = ELogicalKind::Scalar;
				if (!ParseUnsigned(Signature.substr(Colon + 1), Out.ScalarBytes)
					|| Out.ScalarBytes == 0 || Out.ScalarBytes > sizeof(uint64)) return false;
			}
			return true;
		}

		auto ReadString(
			FReader& Reader,
			size_t End,
			uint64& Framing,
			uint64& Text,
			std::string* Out,
			std::string& Error) -> bool
		{
			uint64 Size = 0;
			if (!Reader.Read(Size, End, Framing) || Size > MaximumStringBytes
				|| Size > Reader.Remaining(End)) return Fail(Error, "invalid v3 string extent");
			if (Out)
				Out->assign(reinterpret_cast<const char*>(Reader.Bytes.data() + Reader.Offset),
					static_cast<size_t>(Size));
			return Reader.Consume(static_cast<size_t>(Size), End, Text);
		}

		auto RecordMetadata(
			FV3PackageMeasurement& Measurement,
			std::string_view Value,
			bool bTypeSignature) -> void
		{
			++Measurement.MetadataStringOccurrences;
			Measurement.UniqueMetadataStrings.emplace(Value);
			if (bTypeSignature)
			{
				++Measurement.TypeSignatureOccurrences;
				Measurement.UniqueTypeSignatures.emplace(Value);
			}
			++Measurement.ParseOperations;
			++Measurement.AllocationInputs;
		}

		auto MeasureValue(
			FReader& Reader,
			size_t End,
			const FLogicalType& Type,
			FV3PackageMeasurement& Measurement,
			uint32 Depth,
			std::string& Error) -> bool;

		auto MeasureField(
			FReader& Reader,
			size_t End,
			FV3PackageMeasurement& Measurement,
			uint32 Depth,
			bool bNested,
			std::string& Error) -> bool
		{
			uint64& Framing = bNested
				? Measurement.Bytes.NestedStructFraming : Measurement.Bytes.FieldFraming;
			uint64& MetadataText = bNested
				? Measurement.Bytes.NestedStructMetadataText : Measurement.Bytes.FieldMetadataText;
			std::string DeclaringType, FieldName, Signature;
			uint8 Kind = 0;
			uint64 PayloadSize = 0;
			if (!ReadString(Reader, End, Framing, MetadataText, &DeclaringType, Error)
				|| !ReadString(Reader, End, Framing, MetadataText, &FieldName, Error)
				|| !Reader.Read(Kind, End, Framing)
				|| !ReadString(Reader, End, Framing, MetadataText, &Signature, Error)
				|| !Reader.Read(PayloadSize, End, Framing)
				|| PayloadSize > Reader.Remaining(End)) return Fail(Error, "invalid v3 field record");
			RecordMetadata(Measurement, DeclaringType, false);
			RecordMetadata(Measurement, FieldName, false);
			RecordMetadata(Measurement, Signature, true);
			std::string SchemaKey = DeclaringType;
			SchemaKey.push_back('\0');
			SchemaKey.append(FieldName);
			SchemaKey.push_back('\0');
			SchemaKey.append(Signature);
			++Measurement.SchemaOccurrences;
			Measurement.UniqueSchemas.insert(std::move(SchemaKey));
			FLogicalType Type;
			if (!ParseLogicalType(Signature, Type)) return Fail(Error, "unknown v3 logical type signature");
			const size_t PayloadEnd = Reader.Offset + static_cast<size_t>(PayloadSize);
			if (!MeasureValue(Reader, PayloadEnd, Type, Measurement, Depth, Error)) return false;
			if (Reader.Offset != PayloadEnd) return Fail(Error, "unconsumed v3 field payload");
			if (bNested) ++Measurement.NestedFields;
			else ++Measurement.Fields;
			return true;
		}

		auto MeasureOneValue(
			FReader& Reader,
			size_t End,
			const FLogicalType& Type,
			FV3PackageMeasurement& Measurement,
			uint32 Depth,
			std::string& Error) -> bool
		{
			if (Depth > MaximumDepth) return Fail(Error, "v3 value nesting exceeds bound");
			Measurement.MaximumNesting = std::max(Measurement.MaximumNesting, Depth);
			++Measurement.ParseOperations;
			switch (Type.Kind)
			{
			case ELogicalKind::Scalar:
			case ELogicalKind::Enum:
				return Reader.Consume(static_cast<size_t>(Type.ScalarBytes), End,
					Measurement.Bytes.ScalarValues);
			case ELogicalKind::Guid:
				return Reader.Consume(16, End, Measurement.Bytes.ScalarValues);
			case ELogicalKind::String:
			case ELogicalKind::Name:
				++Measurement.AllocationInputs;
				return ReadString(Reader, End, Measurement.Bytes.StringValueFraming,
					Measurement.Bytes.StringValueText, nullptr, Error);
			case ELogicalKind::Object:
			{
				uint8 ReferenceKind = 0;
				if (!Reader.Read(ReferenceKind, End, Measurement.Bytes.ReferenceFraming)) return false;
				++Measurement.References;
				if (ReferenceKind == 0) return true;
				if (ReferenceKind == 1)
				{
					uint64 Id = 0;
					return Reader.Read(Id, End, Measurement.Bytes.ReferenceFraming);
				}
				if (ReferenceKind == 2)
				{
					++Measurement.AllocationInputs;
					return ReadString(Reader, End, Measurement.Bytes.ReferenceFraming,
						Measurement.Bytes.ReferenceText, nullptr, Error);
				}
				return Fail(Error, "invalid v3 hard-reference tag");
			}
			case ELogicalKind::SoftObject:
			{
				uint8 ReferenceKind = 0;
				if (!Reader.Read(ReferenceKind, End, Measurement.Bytes.ReferenceFraming)) return false;
				++Measurement.References;
				if (ReferenceKind == 0) return true;
				if (ReferenceKind != 1) return Fail(Error, "invalid v3 soft-reference tag");
				++Measurement.AllocationInputs;
				return ReadString(Reader, End, Measurement.Bytes.ReferenceFraming,
					Measurement.Bytes.ReferenceText, nullptr, Error);
			}
			case ELogicalKind::Struct:
			{
				std::string StructName;
				uint64 FieldCount = 0;
				if (!ReadString(Reader, End, Measurement.Bytes.NestedStructFraming,
						Measurement.Bytes.NestedStructMetadataText, &StructName, Error)
					|| !Reader.Read(FieldCount, End, Measurement.Bytes.NestedStructFraming)
					|| FieldCount > MaximumFields) return Fail(Error, "invalid v3 struct header");
				RecordMetadata(Measurement, StructName, false);
				for (uint64 Index = 0; Index < FieldCount; ++Index)
					if (!MeasureField(Reader, End, Measurement, Depth + 1, true, Error)) return false;
				return true;
			}
			case ELogicalKind::Array:
			case ELogicalKind::Map:
			{
				uint64 Count = 0;
				if (!Reader.Read(Count, End, Measurement.Bytes.ContainerFraming)
					|| Count > MaximumContainerElements) return Fail(Error, "invalid v3 container count");
				const uint64 ValueCount = Type.Kind == ELogicalKind::Map ? Count * 2 : Count;
				if (Count != 0 && ValueCount / Count != (Type.Kind == ELogicalKind::Map ? 2u : 1u))
					return Fail(Error, "v3 container count overflow");
				Measurement.ContainerElements += ValueCount;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					if (Type.Kind == ELogicalKind::Array)
					{
						if (!Type.Element || !MeasureOneValue(
							Reader, End, *Type.Element, Measurement, Depth + 1, Error)) return false;
					}
					else if (!Type.Key || !Type.Value
						|| !MeasureOneValue(Reader, End, *Type.Key, Measurement, Depth + 1, Error)
						|| !MeasureOneValue(Reader, End, *Type.Value, Measurement, Depth + 1, Error)) return false;
				}
				return true;
			}
			}
			return false;
		}

		auto MeasureValue(
			FReader& Reader,
			size_t End,
			const FLogicalType& Type,
			FV3PackageMeasurement& Measurement,
			uint32 Depth,
			std::string& Error) -> bool
		{
			if (Depth > MaximumDepth) return Fail(Error, "v3 value nesting exceeds bound");
			while (Reader.Offset < End)
			{
				const size_t Before = Reader.Offset;
				if (!MeasureOneValue(Reader, End, Type, Measurement, Depth, Error))
					return Error.empty() ? Fail(Error, "truncated v3 value") : false;
				if (Reader.Offset <= Before) return Fail(Error, "v3 value made no progress");
			}
			return Reader.Offset == End;
		}
	}

	auto FV3ByteCategories::Total() const -> uint64
	{
		return Envelope + PublicSummaryFraming + PublicSummaryText
			+ DependencyFraming + DependencyText + ObjectFraming + ObjectText
			+ FieldFraming + FieldMetadataText + NestedStructFraming
			+ NestedStructMetadataText + ContainerFraming + ReferenceFraming
			+ ReferenceText + ScalarValues + StringValueFraming + StringValueText
			+ Unclassified;
	}

	auto MeasureDastV3(
		std::span<const uint8> Bytes,
		FV3PackageMeasurement& OutMeasurement,
		std::string& OutError) -> bool
	{
		OutMeasurement = {};
		OutError.clear();
		FReader Reader{Bytes};
		uint32 Magic = 0, Version = 0;
		if (!Reader.Read(Magic, Bytes.size(), OutMeasurement.Bytes.Envelope)
			|| !Reader.Read(Version, Bytes.size(), OutMeasurement.Bytes.Envelope)
			|| Magic != DastMagic || Version != DastV3) return Fail(OutError, "not a DAST v3 package");

		std::string AssetClass;
		if (!ReadString(Reader, Bytes.size(), OutMeasurement.Bytes.PublicSummaryFraming,
				OutMeasurement.Bytes.PublicSummaryText, &AssetClass, OutError)) return false;
		RecordMetadata(OutMeasurement, AssetClass, false);
		uint8 EntryKind = 0;
		if (!Reader.Read(EntryKind, Bytes.size(), OutMeasurement.Bytes.PublicSummaryFraming)
			|| EntryKind > 1) return Fail(OutError, "invalid v3 entry kind");
		if (!ReadString(Reader, Bytes.size(), OutMeasurement.Bytes.PublicSummaryFraming,
				OutMeasurement.Bytes.PublicSummaryText, nullptr, OutError)) return false;

		uint64 DependencyCount = 0;
		if (!Reader.Read(DependencyCount, Bytes.size(), OutMeasurement.Bytes.DependencyFraming)
			|| DependencyCount > MaximumDependencies) return Fail(OutError, "invalid v3 dependency count");
		for (uint64 Index = 0; Index < DependencyCount; ++Index)
		{
			++OutMeasurement.AllocationInputs;
			if (!ReadString(Reader, Bytes.size(), OutMeasurement.Bytes.DependencyFraming,
					OutMeasurement.Bytes.DependencyText, nullptr, OutError)) return false;
		}

		uint64 ObjectCount = 0;
		if (!Reader.Read(ObjectCount, Bytes.size(), OutMeasurement.Bytes.ObjectFraming)
			|| ObjectCount == 0 || ObjectCount > MaximumObjects) return Fail(OutError, "invalid v3 object count");
		for (uint64 ObjectIndex = 0; ObjectIndex < ObjectCount; ++ObjectIndex)
		{
			uint64 Id = 0, OuterId = 0, FieldCount = 0;
			std::string ClassName, ObjectName;
			if (!Reader.Read(Id, Bytes.size(), OutMeasurement.Bytes.ObjectFraming)
				|| !Reader.Read(OuterId, Bytes.size(), OutMeasurement.Bytes.ObjectFraming)
				|| !ReadString(Reader, Bytes.size(), OutMeasurement.Bytes.ObjectFraming,
					OutMeasurement.Bytes.ObjectText, &ClassName, OutError)
				|| !ReadString(Reader, Bytes.size(), OutMeasurement.Bytes.ObjectFraming,
					OutMeasurement.Bytes.ObjectText, &ObjectName, OutError)
				|| !Reader.Read(FieldCount, Bytes.size(), OutMeasurement.Bytes.ObjectFraming)
				|| FieldCount > MaximumFields) return Fail(OutError, "invalid v3 object record");
			RecordMetadata(OutMeasurement, ClassName, false);
			RecordMetadata(OutMeasurement, ObjectName, false);
			for (uint64 FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex)
				if (!MeasureField(Reader, Bytes.size(), OutMeasurement, 1, false, OutError)) return false;
			++OutMeasurement.Objects;
		}
		if (Reader.Offset != Bytes.size()) return Fail(OutError, "trailing v3 package bytes");
		if (OutMeasurement.Bytes.Total() != Bytes.size()) return Fail(OutError, "v3 byte accounting does not conserve input");
		return true;
	}
}
