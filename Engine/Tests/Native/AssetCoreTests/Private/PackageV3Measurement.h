#pragma once

#include "Misc/CoreTypes.h"

#include <span>
#include <string>
#include <unordered_set>

namespace Durin::Testing
{
	struct FV3ByteCategories
	{
		uint64 Envelope = 0;
		uint64 PublicSummaryFraming = 0;
		uint64 PublicSummaryText = 0;
		uint64 DependencyFraming = 0;
		uint64 DependencyText = 0;
		uint64 ObjectFraming = 0;
		uint64 ObjectText = 0;
		uint64 FieldFraming = 0;
		uint64 FieldMetadataText = 0;
		uint64 NestedStructFraming = 0;
		uint64 NestedStructMetadataText = 0;
		uint64 ContainerFraming = 0;
		uint64 ReferenceFraming = 0;
		uint64 ReferenceText = 0;
		uint64 ScalarValues = 0;
		uint64 StringValueFraming = 0;
		uint64 StringValueText = 0;
		uint64 Unclassified = 0;

		auto Total() const -> uint64;
	};

	struct FV3PackageMeasurement
	{
		FV3ByteCategories Bytes;
		uint64 Objects = 0;
		uint64 Fields = 0;
		uint64 NestedFields = 0;
		uint64 ContainerElements = 0;
		uint64 References = 0;
		uint64 MetadataStringOccurrences = 0;
		uint64 TypeSignatureOccurrences = 0;
		uint64 SchemaOccurrences = 0;
		uint64 ParseOperations = 0;
		uint64 AllocationInputs = 0;
		uint32 MaximumNesting = 0;
		std::unordered_set<std::string> UniqueMetadataStrings;
		std::unordered_set<std::string> UniqueTypeSignatures;
		std::unordered_set<std::string> UniqueSchemas;
	};

	auto MeasureDastV3(
		std::span<const uint8> Bytes,
		FV3PackageMeasurement& OutMeasurement,
		std::string& OutError) -> bool;
}
