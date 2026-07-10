#pragma once

// Mirror export types
// For use in generating code for other modules that need to reference core DObject types
#ifdef _DHT_EXPORTS_PARSER

namespace Durin
{
	DCLASS()
	class DObject
	{
	};

	DCLASS()
	class DType : public DObject
	{
	};

	DCLASS()
	class DStructBase : public DType
	{
	};

	DCLASS()
	class DClass : public DStructBase
	{
	};

	DCLASS()
	class DStruct : public DStructBase
	{
	};

	DCLASS()
	class DEnum : public DType
	{
	};
}

#endif
