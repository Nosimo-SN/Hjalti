// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
//
// The read-only C API (include/CanineDrivesAPI.h): other DLLs resolve these with GetProcAddress. Both are safe from
// any thread: they read the snapshot Drives::Publish leaves behind its lock and touch nothing else.
#include "drives.h"

extern "C" {

CDAPI_EXPORT std::uint32_t CDAPI_GetVersion(void)
{
	return CDAPI_VERSION;
}

CDAPI_EXPORT std::uint32_t CDAPI_GetSnapshot(std::uint32_t actorRefID, void* output, std::uint32_t outputSize)
{
	if (!output || outputSize < sizeof(CDAPI_SnapshotV1)) {
		return 0;
	}
	CDAPI_SnapshotV1 snap{};
	if (!Canine::Drives::ApiSnapshot(actorRefID, snap)) {
		return 0;
	}
	std::memcpy(output, &snap, sizeof(snap));
	return 1;
}

}
