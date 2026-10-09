#pragma once

#include "SharedPCH_Engine.h"
#include "MonaImGui.h"

#include "Factories/Factory.h"
#include "Import/EditorReimportHandler.h"
#include "Notifications/Notification.h"
#include "Settings/EditorSettings.h"
#include "Transactions/Transaction.h"
#include "Transactions/TransactionRecord.h"
#include "Transactions/TransactionObjectRecord.h"
#include "Workspace/WorkspaceTypes.h"

// Keep DurinEd.h, EditorEngine.h and PropertyView.h outside the shared PCH:
// thumbnail rendering and package reload/material compilation pull in RHI headers.
