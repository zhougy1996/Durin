#include "DerivedDataBuildDefinition.h"
#include "Serialization/BinaryEncoding.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Durin::DerivedData
{
	namespace
	{
		constexpr size_t MaximumFields = 4096;
		constexpr size_t MaximumConstantBytes = 1024 * 1024;

		auto IsName(std::string_view Name) -> bool
		{
			return !Name.empty() && Name.size() <= 128
				&& std::ranges::all_of(Name, [](unsigned char Ch) {
					return (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z')
						|| (Ch >= '0' && Ch <= '9') || Ch == '_' || Ch == '.' || Ch == '-';
				});
		}

		// This wire encoding is intentionally independent of host ABI and Archive target.
		template<CBinaryInteger T>
		auto WriteInteger(FByteBuffer& Bytes, T Value) -> void
		{
			const auto Encoded = EncodeBinaryInteger(Value);
			Bytes.insert(Bytes.end(), Encoded.begin(), Encoded.end());
		}
		auto WriteString(FByteBuffer& Bytes, std::string_view Value) -> void
		{
			WriteInteger(Bytes, static_cast<uint64>(Value.size()));
			const auto View = std::as_bytes(std::span(Value.data(), Value.size()));
			Bytes.insert(Bytes.end(), View.begin(), View.end());
		}
		auto WriteHash(FByteBuffer& Bytes, FXxHash128 Hash) -> void
		{
			WriteInteger(Bytes, static_cast<uint64>(Hash.HashLow));
			WriteInteger(Bytes, static_cast<uint64>(Hash.HashHigh));
		}
	}

	auto FBuildDefinition::MatchesInputs(std::span<const FBuildInputReference> Bindings) const -> bool
	{
		if (Bindings.size() != Inputs.size()) return false;
		if (std::ranges::is_sorted(Bindings, {}, &FBuildInputReference::Name))
			return std::ranges::equal(Inputs, Bindings);
		// Preserve unordered adapter support without quadratic scans or copying strings.
		std::vector<const FBuildInputReference*> Ordered;
		Ordered.reserve(Bindings.size());
		for (const auto& Binding : Bindings) Ordered.push_back(&Binding);
		std::ranges::sort(Ordered, {}, [](const auto* Input) -> const std::string& { return Input->Name; });
		for (size_t Index = 0; Index < Inputs.size(); ++Index)
			if (Inputs[Index] != *Ordered[Index]) return false;
		return true;
	}

	auto FBuildDefinition::TryCreate(FBuildFunctionDescriptor Function,
		std::vector<FBuildConstant> Constants, std::vector<FBuildInputReference> Inputs)
		-> std::expected<FBuildDefinition, FBuildDefinitionError>
	{
		if (!IsName(Function.Name) || !IsName(Function.OutputType)
			|| !Function.Version || !Function.ConstantsSchema || !Function.OutputSchema
			|| !Function.Bucket.IsValid())
			return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::InvalidFunction, Function.Name});
		if (Constants.size() > MaximumFields || Inputs.size() > MaximumFields)
			return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::LimitExceeded, {}});
		std::ranges::sort(Constants, {}, &FBuildConstant::Name);
		std::ranges::sort(Inputs, {}, &FBuildInputReference::Name);
		size_t ConstantBytes = 0;
		for (size_t Index = 0; Index < Constants.size(); ++Index)
		{
			auto& Constant = Constants[Index];
			if (!IsName(Constant.Name))
				return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::InvalidName, Constant.Name});
			if (Index && Constants[Index - 1].Name == Constant.Name)
				return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::DuplicateName, Constant.Name});
			if (auto* Value = std::get_if<float>(&Constant.Value))
			{
				if (!std::isfinite(*Value))
					return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::InvalidConstant, Constant.Name});
				if (*Value == 0) *Value = 0.0f;
			}
			if (const auto* Value = std::get_if<std::string>(&Constant.Value))
			{
				if (Value->size() > MaximumConstantBytes - ConstantBytes)
					return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::LimitExceeded, Constant.Name});
				ConstantBytes += Value->size();
			}
		}
		for (size_t Index = 0; Index < Inputs.size(); ++Index)
		{
			const auto& Input = Inputs[Index];
			if (!IsName(Input.Name) || !IsName(Input.IdentityScheme) || !IsName(Input.Representation)
				|| Input.Identity.IsZero() || !Input.IdentityVersion || !Input.RepresentationVersion)
				return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::InvalidInput, Input.Name});
			if (Index && Inputs[Index - 1].Name == Input.Name)
				return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::DuplicateName, Input.Name});
		}

		FBuildDefinition Definition;
		auto& Bytes = Definition.CanonicalBytes;
		WriteString(Bytes, "Durin.DerivedData.BuildDefinition");
		WriteInteger(Bytes, static_cast<uint32>(1)); // Build-key schema; independent of runtime payload schemas.
		WriteString(Bytes, Function.Name);
		WriteInteger(Bytes, static_cast<uint32>(Function.Version));
		WriteInteger(Bytes, static_cast<uint32>(Function.ConstantsSchema));
		WriteString(Bytes, Function.OutputType);
		WriteInteger(Bytes, static_cast<uint32>(Function.OutputSchema));
		WriteString(Bytes, Function.Bucket.ToString());
		WriteInteger(Bytes, static_cast<uint32>(Constants.size()));
		for (const auto& Constant : Constants)
		{
			WriteString(Bytes, Constant.Name);
			WriteInteger(Bytes, static_cast<uint8>(Constant.Value.index() + 1));
			std::visit([&](const auto& Value) {
				using T = std::decay_t<decltype(Value)>;
				if constexpr (std::is_same_v<T, bool>) WriteInteger(Bytes, static_cast<uint8>(Value));
				else if constexpr (std::is_same_v<T, uint64>) WriteInteger(Bytes, static_cast<uint64>(Value));
				else if constexpr (std::is_same_v<T, float>) WriteInteger(Bytes, static_cast<uint32>(std::bit_cast<uint32>(Value)));
				else if constexpr (std::is_same_v<T, std::string>) WriteString(Bytes, Value);
				else WriteHash(Bytes, Value);
			}, Constant.Value);
		}
		WriteInteger(Bytes, static_cast<uint32>(Inputs.size()));
		for (const auto& Input : Inputs)
		{
			WriteString(Bytes, Input.Name);
			WriteHash(Bytes, Input.Identity);
			WriteString(Bytes, Input.IdentityScheme);
			WriteInteger(Bytes, static_cast<uint32>(Input.IdentityVersion));
			WriteString(Bytes, Input.Representation);
			WriteInteger(Bytes, static_cast<uint32>(Input.RepresentationVersion));
		}
		Definition.Key = FCacheKey::FromHash(Function.Bucket, FXxHash128::HashBuffer(Bytes));
		Definition.Function = std::move(Function);
		Definition.Constants = std::move(Constants);
		Definition.Inputs = std::move(Inputs);
		return Definition;
	}
}
