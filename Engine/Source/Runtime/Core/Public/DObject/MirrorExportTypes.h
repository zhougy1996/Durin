#pragma once

// Mirror export types
// For use in generating code for other modules that need to reference core DObject types
#ifdef _DHT_EXPORTS_PARSER

DCLASS()
class DObject
{
};

DCLASS()
class DStructure : public DObject
{
};

DCLASS()
class DClass : public DStructure
{
};

DCLASS()
class DEnum
{
};

#endif