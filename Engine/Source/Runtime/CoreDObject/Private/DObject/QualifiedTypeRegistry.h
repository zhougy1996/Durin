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
	}
}
