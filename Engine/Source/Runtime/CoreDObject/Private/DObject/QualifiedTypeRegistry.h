#pragma once

namespace Durin
{
	class DClass;
	class DStruct;
	class DEnum;
	class FName;

	namespace Private
	{
		auto UpdateQualifiedClassName(DClass* Class, FName PreviousName) -> void;
		auto RegisterQualifiedStruct(DStruct* Struct) -> void;
		auto RegisterQualifiedEnum(DEnum* Enum) -> void;
		auto RegisterLegacyClassNames(DClass* Class, std::span<const char* const> LegacyNames) -> void;
		auto RegisterLegacyStructNames(DStruct* Struct, std::span<const char* const> LegacyNames) -> void;
		auto RegisterLegacyEnumNames(DEnum* Enum, std::span<const char* const> LegacyNames) -> void;
	}
}
