#include "DSPGCSUploadJobStateMirror.h"

void FDSPGCSUploadJobStateMirror::Reset()
{
	CurrentSnapshot = {};
	LastCommandAck = {};
	Routes.clear();
	PendingCommandID.clear();
	BoundJobID.clear();
	LastSnapshotSafeDiagnostic.clear();
	LastDeltaSequence = 0;
	NextRoutePageStart = 0;
	LastSnapshotResultCode = dspgcs::UploadJobErrorCode::None;
	bHasSnapshot = false;
	bHasLastCommandAck = false;
	bHasAuthoritativeError = false;
	bRecoveryRequired = false;
	bRoutePageComplete = false;
}

void FDSPGCSUploadJobStateMirror::PrepareForNewJob(
	const std::string& UploadJobID)
{
	CurrentSnapshot = {};
	Routes.clear();
	BoundJobID = UploadJobID;
	LastSnapshotSafeDiagnostic.clear();
	LastDeltaSequence = 0;
	NextRoutePageStart = 0;
	LastSnapshotResultCode = dspgcs::UploadJobErrorCode::None;
	bHasSnapshot = false;
	bHasAuthoritativeError = false;
	bRecoveryRequired = false;
	bRoutePageComplete = false;
}

void FDSPGCSUploadJobStateMirror::SetPendingCommandID(
	const std::string& ClientCommandID)
{
	PendingCommandID = ClientCommandID;
	LastCommandAck = {};
	bHasLastCommandAck = false;
}

void FDSPGCSUploadJobStateMirror::ClearPendingCommandID()
{
	PendingCommandID.clear();
}

bool FDSPGCSUploadJobStateMirror::ApplyCommandAck(
	const dspgcs::UploadJobCommandAckDTO& Ack)
{
	if (PendingCommandID.empty() || Ack.clientCommandId != PendingCommandID)
	{
		return false;
	}
	PendingCommandID.clear();
	LastCommandAck = Ack;
	bHasLastCommandAck = true;
	return true;
}

bool FDSPGCSUploadJobStateMirror::ApplySnapshot(
	const dspgcs::UploadJobSnapshotDTO& Snapshot)
{
	if (!BoundJobID.empty() && Snapshot.uploadJobId != BoundJobID)
	{
		bRecoveryRequired = true;
		return false;
	}
	if (Snapshot.resultCode != dspgcs::UploadJobErrorCode::None)
	{
		CurrentSnapshot = {};
		Routes.clear();
		LastDeltaSequence = 0;
		NextRoutePageStart = 0;
		LastSnapshotResultCode = Snapshot.resultCode;
		LastSnapshotSafeDiagnostic = Snapshot.safeDiagnostic;
		bHasSnapshot = false;
		bHasAuthoritativeError = true;
		bRecoveryRequired = false;
		bRoutePageComplete = false;
		return true;
	}
	if (bHasSnapshot && Snapshot.uploadJobId != CurrentSnapshot.uploadJobId)
	{
		return false;
	}
	if (BoundJobID.empty()) BoundJobID = Snapshot.uploadJobId;
	CurrentSnapshot = Snapshot;
	Routes.clear();
	LastSnapshotResultCode = dspgcs::UploadJobErrorCode::None;
	LastSnapshotSafeDiagnostic.clear();
	LastDeltaSequence = Snapshot.routeDeltaSequence;
	NextRoutePageStart = 0;
	bHasSnapshot = true;
	bHasAuthoritativeError = false;
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
