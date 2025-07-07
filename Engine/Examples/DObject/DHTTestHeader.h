#pragma once

#include "aaaa.h"

#ifdef _DHT_PARSER
#define DCLASS(...) __attribute__((annotate(#__VA_ARGS__)))
#define GENERATED_BODY(...)
#define DPROPERTY(...) __attribute__((annotate(#__VA_ARGS__)))
#define DFUNCTION(...) __attribute__((annotate(#__VA_ARGS__)))
#else
#define DCLASS(...)
#define GENERATED_BODY(...)
#define DPROPERTY(...)
#define DFUNCTION(...)
#endif

namespace a
{
class DCLASS() DTestObject
{
	GENERATED_BODY()

	DFUNCTION()
	void TestFuntion();

	DPROPERTY()
	int TestProperty;
};
}
