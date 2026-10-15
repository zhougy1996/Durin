#include "Tools/Tool.h"

namespace Durin
{
	FToolSchema::FToolSchema(const FToolSchema& Other)
		: Type(Other.Type), Description(Other.Description), Properties(Other.Properties), Required(Other.Required),
		Items(Other.Items ? std::make_unique<FToolSchema>(*Other.Items) : nullptr), Enum(Other.Enum),
		Default(Other.Default), Minimum(Other.Minimum), Maximum(Other.Maximum), MinSize(Other.MinSize), MaxSize(Other.MaxSize) {}

	auto FToolSchema::operator=(const FToolSchema& Other) -> FToolSchema&
	{
		if (this != &Other) *this = FToolSchema(Other);
		return *this;
	}

	namespace
	{
		auto Error(EToolError Code, std::string Message, std::string Path = {}) -> std::unexpected<FToolDiagnostic>
		{
			return std::unexpected(FToolDiagnostic{Code, std::move(Message), std::move(Path)});
		}

		auto ChildPath(const std::string& Path, std::string_view Key) -> std::string
		{
			std::string Result = Path + '/';
			for (char C : Key)
			{
				if (C == '~') Result += "~0";
				else if (C == '/') Result += "~1";
				else Result += C;
			}
			return Result;
		}

		struct FBudget
		{
			const FToolLimits& Limits;
			size_t Nodes = 0;
			size_t StringBytes = 0;

			auto Node(size_t Depth, const std::string& Path) -> FToolValidationResult
			{
				if (Depth > Limits.MaxDepth || Nodes >= Limits.MaxNodes)
					return Error(EToolError::LimitExceeded, "Tool tree depth or node limit exceeded.", Path);
				++Nodes;
				return {};
			}
			auto String(std::string_view Value, const std::string& Path) -> FToolValidationResult
			{
				if (Value.size() > Limits.MaxStringBytes || Value.size() > Limits.MaxTotalStringBytes - StringBytes)
					return Error(EToolError::LimitExceeded, "Tool string byte limit exceeded.", Path);
				StringBytes += Value.size();
				return {};
			}
		};

		auto ValidateLimits(const FToolLimits& Limits) -> FToolValidationResult
		{
			// The fixed ceiling protects recursive traversal even with caller-supplied limits.
			if (Limits.MaxDepth > 64 || Limits.MaxNodes == 0)
				return Error(EToolError::InvalidSchema, "Limits require MaxDepth <= 64 and MaxNodes > 0.");
			return {};
		}

		auto MatchesType(const FToolValue& Value, EToolValueType Type) -> bool
		{
			return static_cast<size_t>(Type) == Value.Data.index();
		}

		auto IsNumeric(EToolValueType Type) -> bool
		{
			return Type == EToolValueType::Integer || Type == EToolValueType::UnsignedInteger || Type == EToolValueType::Number;
		}

		// Operands have already been checked to have the same numeric type.
		auto Less(const FToolValue& Left, const FToolValue& Right) -> bool
		{
			if (const auto* Value = std::get_if<int64>(&Left.Data)) return *Value < std::get<int64>(Right.Data);
			if (const auto* Value = std::get_if<uint64>(&Left.Data)) return *Value < std::get<uint64>(Right.Data);
			return std::get<double>(Left.Data) < std::get<double>(Right.Data);
		}

		// Bounds raw values before any normalization/copy and rejects non-finite numbers.
		auto CheckValue(const FToolValue& Value, FBudget& Budget, size_t Depth, const std::string& Path) -> FToolValidationResult
		{
			if (auto Check = Budget.Node(Depth, Path); !Check) return Check;
			if (Value.Data.valueless_by_exception()) return Error(EToolError::InvalidArguments, "Valueless tool value.", Path);
			if (const auto* Number = std::get_if<double>(&Value.Data); Number && !std::isfinite(*Number))
				return Error(EToolError::InvalidArguments, "Numbers must be finite.", Path);
			if (const auto* String = std::get_if<std::string>(&Value.Data)) return Budget.String(*String, Path);
			if (const auto* Array = std::get_if<FToolValue::FArray>(&Value.Data))
			{
				if (Array->size() > Budget.Limits.MaxContainerEntries) return Error(EToolError::LimitExceeded, "Array limit exceeded.", Path);
				for (size_t Index = 0; Index < Array->size(); ++Index)
					if (auto Check = CheckValue((*Array)[Index], Budget, Depth + 1, ChildPath(Path, std::to_string(Index))); !Check) return Check;
			}
			if (const auto* Object = std::get_if<FToolValue::FObject>(&Value.Data))
			{
				if (Object->size() > Budget.Limits.MaxContainerEntries) return Error(EToolError::LimitExceeded, "Object limit exceeded.", Path);
				for (const auto& [Key, Child] : *Object)
				{
					if (auto Check = Budget.String(Key, Path); !Check) return Check;
					if (auto Check = CheckValue(Child, Budget, Depth + 1, ChildPath(Path, Key)); !Check) return Check;
				}
			}
			return {};
		}

		auto CheckSchemaValue(const FToolValue& Value, FBudget& Budget, size_t Depth, const std::string& Path) -> FToolValidationResult
		{
			auto Check = CheckValue(Value, Budget, Depth, Path);
			if (!Check && Check.error().Code == EToolError::InvalidArguments) Check.error().Code = EToolError::InvalidSchema;
			return Check;
		}

		auto CheckAgainstSchema(const FToolSchema& Schema, const FToolValue& Value,
			FToolValue* Normalized, bool bDefaults, FBudget& Budget, size_t Depth, const std::string& Path)
			-> FToolValidationResult
		{
			if (auto Check = Budget.Node(Depth, Path); !Check) return Check;
			if (!MatchesType(Value, Schema.Type)) return Error(EToolError::InvalidArguments, "Value does not match the declared type.", Path);
			if (const auto* Number = std::get_if<double>(&Value.Data); Number && !std::isfinite(*Number))
				return Error(EToolError::InvalidArguments, "Numbers must be finite.", Path);
			if (!Schema.Enum.empty() && !std::ranges::contains(Schema.Enum, Value))
				return Error(EToolError::InvalidArguments, "Value is not in the declared enum.", Path);
			if ((Schema.Minimum && Less(Value, *Schema.Minimum)) || (Schema.Maximum && Less(*Schema.Maximum, Value)))
				return Error(EToolError::InvalidArguments, "Value is outside the numeric bounds.", Path);
			size_t Size = 0;
			if (const auto* String = std::get_if<std::string>(&Value.Data))
			{
				if (auto Check = Budget.String(*String, Path); !Check) return Check;
				Size = String->size();
			}
			if (const auto* Array = std::get_if<FToolValue::FArray>(&Value.Data))
			{
				Size = Array->size();
				if (Size > Budget.Limits.MaxContainerEntries) return Error(EToolError::LimitExceeded, "Array limit exceeded.", Path);
				FToolValue::FArray Output;
				if (Normalized) Output.reserve(Size);
				for (size_t Index = 0; Index < Size; ++Index)
				{
					FToolValue Child;
					if (auto Check = CheckAgainstSchema(*Schema.Items, (*Array)[Index], Normalized ? &Child : nullptr,
						bDefaults, Budget, Depth + 1, ChildPath(Path, std::to_string(Index))); !Check) return Check;
					if (Normalized) Output.push_back(std::move(Child));
				}
				if (Normalized) Normalized->Data = std::move(Output);
			}
			else if (const auto* Object = std::get_if<FToolValue::FObject>(&Value.Data))
			{
				if (Object->size() > Budget.Limits.MaxContainerEntries) return Error(EToolError::LimitExceeded, "Object limit exceeded.", Path);
				for (const auto& [Key, Child] : *Object)
					if (!Schema.Properties.contains(Key)) return Error(EToolError::InvalidArguments, "Unknown object field.", ChildPath(Path, Key));
				FToolValue::FObject Output;
				for (const auto& [Key, ChildSchema] : Schema.Properties)
				{
					const auto Found = Object->find(Key);
					const FToolValue* Child = Found == Object->end() ? nullptr : &Found->second;
					if (!Child && bDefaults && ChildSchema.Default) Child = &*ChildSchema.Default;
					if (!Child)
					{
						if (std::ranges::contains(Schema.Required, Key)) return Error(EToolError::InvalidArguments, "Required field is missing.", ChildPath(Path, Key));
						continue;
					}
					if (++Size > Budget.Limits.MaxContainerEntries) return Error(EToolError::LimitExceeded, "Normalized object limit exceeded.", Path);
					if (auto Check = Budget.String(Key, Path); !Check) return Check;
					FToolValue ChildOutput;
					if (auto Check = CheckAgainstSchema(ChildSchema, *Child, Normalized ? &ChildOutput : nullptr,
						bDefaults, Budget, Depth + 1, ChildPath(Path, Key)); !Check) return Check;
					if (Normalized) Output.emplace(Key, std::move(ChildOutput));
				}
				if (Normalized) Normalized->Data = std::move(Output);
			}
			else if (Normalized) *Normalized = Value;
			if ((Schema.MinSize && Size < *Schema.MinSize) || (Schema.MaxSize && Size > *Schema.MaxSize))
				return Error(EToolError::InvalidArguments, "Value is outside the size bounds.", Path);
			return {};
		}

		auto CheckSchema(const FToolSchema& Schema, FBudget& Budget, size_t Depth, const std::string& Path) -> FToolValidationResult
		{
			auto Invalid = [&](std::string Message) -> FToolValidationResult { return Error(EToolError::InvalidSchema, std::move(Message), Path); };
			if (auto Check = Budget.Node(Depth, Path); !Check) return Check;
			if (static_cast<uint8>(Schema.Type) > static_cast<uint8>(EToolValueType::Object)) return Invalid("Unsupported schema type.");
			if (auto Check = Budget.String(Schema.Description, Path); !Check) return Check;
			const bool bObject = Schema.Type == EToolValueType::Object;
			const bool bArray = Schema.Type == EToolValueType::Array;
			const bool bSized = bObject || bArray || Schema.Type == EToolValueType::String;
			if ((!bObject && (!Schema.Properties.empty() || !Schema.Required.empty())) || (bArray != (Schema.Items != nullptr)))
				return Invalid("Properties/required belong to objects; exactly one item schema is required only for arrays.");
			if ((!IsNumeric(Schema.Type) && (Schema.Minimum || Schema.Maximum)) || (!bSized && (Schema.MinSize || Schema.MaxSize)))
				return Invalid("Bounds are not applicable to this schema type.");
			if (Schema.MinSize && Schema.MaxSize && *Schema.MinSize > *Schema.MaxSize) return Invalid("Inverted size bounds.");
			if (Schema.Properties.size() > Budget.Limits.MaxContainerEntries || Schema.Required.size() > Budget.Limits.MaxContainerEntries
				|| Schema.Enum.size() > Budget.Limits.MaxContainerEntries) return Error(EToolError::LimitExceeded, "Schema container limit exceeded.", Path);
			for (const auto* Bound : {&Schema.Minimum, &Schema.Maximum})
			{
				if (!*Bound) continue;
				if (!MatchesType(**Bound, Schema.Type)) return Invalid("Numeric bounds must have the exact schema type.");
				if (auto Check = CheckSchemaValue(**Bound, Budget, Depth + 1, Path); !Check) return Check;
			}
			if (Schema.Minimum && Schema.Maximum && Less(*Schema.Maximum, *Schema.Minimum)) return Invalid("Inverted numeric bounds.");
			if ((bObject || bArray) && !Schema.Enum.empty()) return Invalid("Enums require scalar schemas.");
			for (size_t Index = 0; Index < Schema.Enum.size(); ++Index)
			{
				const auto& Item = Schema.Enum[Index];
				if (!MatchesType(Item, Schema.Type)) return Invalid("Enum values must have the exact schema type.");
				if (auto Check = CheckSchemaValue(Item, Budget, Depth + 1, Path); !Check) return Check;
				if ((Schema.Minimum && Less(Item, *Schema.Minimum)) || (Schema.Maximum && Less(*Schema.Maximum, Item))) return Invalid("Enum value is outside numeric bounds.");
				if (const auto* String = std::get_if<std::string>(&Item.Data); String
					&& ((Schema.MinSize && String->size() < *Schema.MinSize) || (Schema.MaxSize && String->size() > *Schema.MaxSize))) return Invalid("Enum value is outside size bounds.");
				for (size_t Previous = 0; Previous < Index; ++Previous)
					if (Schema.Enum[Previous] == Item) return Invalid("Duplicate enum value.");
			}
			std::set<std::string_view> Required;
			for (const auto& Key : Schema.Required)
			{
				if (auto Check = Budget.String(Key, Path); !Check) return Check;
				if (!Schema.Properties.contains(Key) || !Required.insert(Key).second) return Invalid("Unknown or duplicate required field.");
			}
			for (const auto& [Key, Child] : Schema.Properties)
			{
				if (Key.empty()) return Invalid("Schema field names must not be empty.");
				if (auto Check = Budget.String(Key, Path); !Check) return Check;
				if (auto Check = CheckSchema(Child, Budget, Depth + 1, ChildPath(Path, Key)); !Check) return Check;
			}
			if (Schema.Items)
				if (auto Check = CheckSchema(*Schema.Items, Budget, Depth + 1, ChildPath(Path, "items")); !Check) return Check;
			if (Schema.Default)
			{
				if (auto Check = CheckSchemaValue(*Schema.Default, Budget, Depth + 1, Path); !Check) return Check;
				// Defaults must be fully valid without recursively adding further defaults.
				FBudget DefaultBudget{Budget.Limits};
				if (auto Check = CheckAgainstSchema(Schema, *Schema.Default, nullptr, false, DefaultBudget, 0, Path); !Check)
					return Invalid("Default does not satisfy its complete schema: " + Check.error().Message);
			}
			return {};
		}

		auto ValidName(std::string_view Name) -> bool
		{
			bool bStart = true;
			for (char C : Name)
			{
				if (C == '.') { if (bStart) return false; bStart = true; continue; }
				if (bStart ? (C < 'a' || C > 'z') : !((C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_')) return false;
				bStart = false;
			}
			return !bStart;
		}
	}

	auto ValidateToolName(std::string_view Name) -> bool { return Name.size() <= 256 && ValidName(Name); }

	auto ValidateToolsetDescriptor(const FToolsetDescriptor& Descriptor, const FToolLimits& Limits) -> FToolValidationResult
	{
		if (auto Check = ValidateLimits(Limits); !Check) return Check;
		if (!ValidateToolName(Descriptor.Name)) return Error(EToolError::InvalidDescriptor, "Invalid toolset name.");
		FBudget Budget{Limits};
		if (auto Check = Budget.String(Descriptor.Name, "/name"); !Check) return Check;
		return Budget.String(Descriptor.Description, "/description");
	}

	auto ValidateToolValue(const FToolValue& Value, const FToolLimits& Limits) -> FToolValidationResult
	{
		if (auto Check = ValidateLimits(Limits); !Check) return Check;
		FBudget Budget{Limits};
		return CheckValue(Value, Budget, 0, {});
	}

	auto ValidateToolDescriptor(const FToolDescriptor& Descriptor, const FToolLimits& Limits) -> FToolValidationResult
	{
		if (auto Check = ValidateLimits(Limits); !Check) return Check;
		if (!ValidateToolName(Descriptor.Name) || Descriptor.ContractVersion == 0 || Descriptor.Input.Type != EToolValueType::Object
			|| (static_cast<uint8>(Descriptor.Effects) & ~uint8{7}) != 0
			|| static_cast<uint8>(Descriptor.RetryPolicy) > static_cast<uint8>(EToolRetryPolicy::Idempotent))
			return Error(EToolError::InvalidDescriptor, "Invalid name, version, input root, effects, or retry policy.");
		FBudget Budget{Limits};
		if (auto Check = Budget.String(Descriptor.Name, "/name"); !Check) return Check;
		if (auto Check = Budget.String(Descriptor.Description, "/description"); !Check) return Check;
		if (auto Check = CheckSchema(Descriptor.Input, Budget, 0, "/input"); !Check) return Check;
		return CheckSchema(Descriptor.Output, Budget, 0, "/output");
	}

	auto PrepareToolArguments(const FToolDescriptor& Descriptor, const FToolRequest& Request, const FToolLimits& Limits)
		-> std::expected<FToolRequest, FToolDiagnostic>
	{
		if (auto Check = ValidateToolDescriptor(Descriptor, Limits); !Check) return std::unexpected(Check.error());
		if (!ValidateToolName(Request.Address.Toolset)) return Error(EToolError::InvalidArguments, "Invalid toolset name.");
		if (Request.Address.Tool != Descriptor.Name) return Error(EToolError::ToolUnavailable, "Request does not name this tool.");
		if (Request.ContractVersion != Descriptor.ContractVersion) return Error(EToolError::VersionMismatch, "Tool contract version does not match.");
		FBudget RawBudget{Limits};
		if (auto Check = CheckValue(Request.Arguments, RawBudget, 0, {}); !Check) return std::unexpected(Check.error());
		FBudget Budget{Limits};
		FToolRequest Prepared{Request.Address, Request.ContractVersion};
		if (auto Check = CheckAgainstSchema(Descriptor.Input, Request.Arguments, &Prepared.Arguments, true, Budget, 0, {}); !Check)
			return std::unexpected(Check.error());
		return Prepared;
	}

	auto ValidateToolOutput(const FToolDescriptor& Descriptor, const FToolValue& Output, const FToolLimits& Limits) -> FToolValidationResult
	{
		if (auto Check = ValidateToolDescriptor(Descriptor, Limits); !Check) return Check;
		FBudget RawBudget{Limits};
		if (auto Check = CheckValue(Output, RawBudget, 0, {}); !Check)
		{
			if (Check.error().Code == EToolError::InvalidArguments) Check.error().Code = EToolError::InvalidOutput;
			return Check;
		}
		FBudget Budget{Limits};
		auto Check = CheckAgainstSchema(Descriptor.Output, Output, nullptr, false, Budget, 0, {});
		if (!Check && Check.error().Code == EToolError::InvalidArguments) Check.error().Code = EToolError::InvalidOutput;
		return Check;
	}
}
