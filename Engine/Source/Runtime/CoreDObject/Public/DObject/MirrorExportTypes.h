#pragma once

// Mirror export types
// For use in generating code for other modules that need to reference core DObject types
#ifdef _DHT_EXPORTS_PARSER

namespace Durin
{
	// Mirrors the root object type for dependency export parsing.
	DCLASS()
	class DObject
	{
	};

	// Mirrors the base reflected metadata type for dependency export parsing.
	DCLASS()
	class DType : public DObject
	{
	};

	// Mirrors metadata with fields and memory layout for dependency export parsing.
	DCLASS()
	class DStructBase : public DType
	{
	};

	// Mirrors reflected object-class metadata for dependency export parsing.
	DCLASS()
	class DClass : public DStructBase
	{
	};

	// Mirrors reflected value-struct metadata for dependency export parsing.
	DCLASS()
	class DStruct : public DStructBase
	{
	};

	// Mirrors reflected enum metadata for dependency export parsing.
	DCLASS()
	class DEnum : public DType
	{
	};
}

#endif
