#include "Asset/OfflinePreparation.h"
namespace Durin
{
	namespace { thread_local uint32 OfflinePreparationDepth = 0; }
	FScopedOfflinePreparation::FScopedOfflinePreparation() { ++OfflinePreparationDepth; }
	FScopedOfflinePreparation::~FScopedOfflinePreparation() { --OfflinePreparationDepth; }
	auto FScopedOfflinePreparation::IsActive() -> bool { return OfflinePreparationDepth != 0; }
}
