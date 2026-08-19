#include "DSPGCSUploadJobStateMirror.h"

void FDSPGCSUploadJobStateMirror::Reset()
{
	CurrentSnapshot = {};
	Routes.clear();
	PendingCommandID.clear();
	LastDeltaSequence = 0;
	NextRoutePageStart = 0;
	bHasSnapshot = false;
	bRecoveryRequired = false;
	bRoutePageComplete = false;
}

void FDSPGCSUploadJobStateMirror::SetPendingCommandID(
	const std::string& ClientCommandID)
{
	PendingCommandID = ClientCommandID;
}

bool FDSPGCSUploadJobStateMirror::ApplyCommandAck(
	const dspgcs::UploadJobCommandAckDTO& Ack)
{
	if (PendingCommandID.empty() || Ack.clientCommandId != PendingCommandID)
	{
		return false;
	}
	PendingCommandID.clear();
	return true;
}

bool FDSPGCSUploadJobStateMirror::ApplySnapshot(
	const dspgcs::UploadJobSnapshotDTO& Snapshot)
{
	if (Snapshot.resultCode != dspgcs::UploadJobErrorCode::None)
	{
		bRecoveryRequired = false;
		return false;
	}
	if (bHasSnapshot && Snapshot.uploadJobId != CurrentSnapshot.uploadJobId)
	{
		return false;
	}
	CurrentSnapshot = Snapshot;
	Routes.clear();
	LastDeltaSequence = Snapshot.routeDeltaSequence;
	NextRoutePageStart = 0;
	bHasSnapshot = true;
	bRecoveryRequired = false;
	bRoutePageComplete = Snapshot.matchedCount == 0;
	return true;
}

bool FDSPGCSUploadJobStateMirror::ApplyRoutePage(
	const dspgcs::UploadJobRouteStatePageDTO& Page)
{
	if (!bHasSnapshot || Page.uploadJobId != CurrentSnapshot.uploadJobId ||
		Page.snapshotToken != CurrentSnapshot.snapshotToken ||
		Page.jobRevision != CurrentSnapshot.jobRevision ||
		Page.pageStart != NextRoutePageStart)
	{
		bRecoveryRequired = true;
		return false;
	}
	for (const auto& Route : Page.routes)
	{
		Routes[Route.routeSlotIndex] = Route;
	}
	NextRoutePageStart = Page.nextPageStart;
	bRoutePageComplete = !Page.hasMore;
	return true;
}

bool FDSPGCSUploadJobStateMirror::ApplyRouteDelta(
	const dspgcs::UploadJobRouteStateDeltaDTO& Delta,
	bool& bOutDuplicateIgnored)
{
	bOutDuplicateIgnored = false;
	if (!bHasSnapshot || Delta.uploadJobId != CurrentSnapshot.uploadJobId ||
		Delta.jobRevision < CurrentSnapshot.jobRevision)
	{
		bRecoveryRequired = true;
		return false;
	}
	if (Delta.deltaSequence <= LastDeltaSequence)
	{
		bOutDuplicateIgnored = true;
		return true;
	}
	if (Delta.deltaSequence != LastDeltaSequence + 1)
	{
		bRecoveryRequired = true;
		return false;
	}
	for (const auto& Route : Delta.routes)
	{
		Routes[Route.routeSlotIndex] = Route;
	}
	LastDeltaSequence = Delta.deltaSequence;
	CurrentSnapshot.jobRevision = Delta.jobRevision;
	return true;
}

void FDSPGCSUploadJobStateMirror::MarkTransportReconnected()
{
	if (bHasSnapshot) bRecoveryRequired = true;
}

void FDSPGCSUploadJobStateMirror::MarkPageOpened()
{
	if (bHasSnapshot) bRecoveryRequired = true;
}

void FDSPGCSUploadJobStateMirror::ClearRecoveryRequired()
{
	bRecoveryRequired = false;
}

const dspgcs::UploadRouteStateDTO*
FDSPGCSUploadJobStateMirror::FindRoute(const int32 RouteSlotIndex) const
{
	const auto Item = Routes.find(RouteSlotIndex);
	return Item == Routes.end() ? nullptr : &Item->second;
}

void FDSPGCSUploadJobStateMirror::CopySortedRouteSlots(
	TArray<int32>& OutRouteSlots) const
{
	OutRouteSlots.Reset(static_cast<int32>(Routes.size()));
	for (const auto& Pair : Routes)
	{
		OutRouteSlots.Add(Pair.first);
	}
	OutRouteSlots.Sort();
}
