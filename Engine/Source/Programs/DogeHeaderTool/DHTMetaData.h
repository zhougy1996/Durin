#pragma once

namespace DHT
{
using DHTNamespaceStack = std::vector<std::string>;

struct DHTProperty
{
	std::string Name;
	std::string Type;
	std::string DefaultValue;
	std::string Description;

	bool bIsStatic = false;
	bool bIsConst = false;
	bool bIsVolatile = false;
};

struct DHTFunction
{
	std::string Name;
	std::string ReturnType;
	std::string Description;
	std::vector<DHTProperty> Parameters;
	bool bIsStatic = false;
	bool bIsConst = false;
	bool bIsVirtual = false;
	bool bIsPureVirtual = false;
	bool bIsInline = false;
};
	
struct DHTClass
{
	std::string ClassName;
	std::string Namespace;
	std::vector<std::string> BaseClasses;
	std::string Description;

	std::vector<DHTProperty> Properties;
	std::vector<DHTFunction> Functions;
};

struct DHTFile
{
	std::string Filename;

	std::string FilePath;

	std::vector<DHTClass> Classes;

	bool IsEmpty() { return Filename.empty(); }
};
} // namespace DHT
