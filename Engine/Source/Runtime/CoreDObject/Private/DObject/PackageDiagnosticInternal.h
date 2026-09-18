#pragma once
#include "Serialization/BinaryEnvelope.h"

namespace Durin::ObjectPackage
{
	auto FormatEnvelopeError(EBinaryEnvelopeError Error) -> std::string;
}
