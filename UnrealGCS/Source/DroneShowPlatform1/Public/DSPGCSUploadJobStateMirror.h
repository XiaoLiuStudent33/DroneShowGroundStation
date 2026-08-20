#pragma once

#include "CoreMinimal.h"
#include "DSPGCSUploadJobProtocol.h"

#include <unordered_map>

/** Value-only UE mirror of Console-owned Upload Job authority. */
class DRONESHOWPLATFORM1_API FDSPGCSUploadJobStateMirror final
{
public:
	void Reset();
	/** Explicitly clears the old terminal/error projection and binds the next requested Job ID. */
	void PrepareForNewJob(const std::string& UploadJobID);
	void SetPendingCommandID(const std::string& ClientCommandID);
	void ClearPendingCommandID();
	bool ApplyCommandAck(const dspgcs::UploadJobCommandAckDTO& Ack);
	bool ApplySnapshot(const dspgcs::UploadJobSnapshotDTO& Snapshot);
	bool ApplyRoutePage(const dspgcs::UploadJobRouteStatePageDTO& Page);
	bool ApplyRouteDelta(
		const dspgcs::UploadJobRouteStateDeltaDTO& Delta,
		bool& bOutDuplicateIgnored);
	void MarkTransportReconnected();
	void MarkPageOpened();
	void ClearRecoveryRequired();

	bool HasAuthoritativeSnapshot() const { return bHasSnapshot; }
	bool HasAuthoritativeError() const { return bHasAuthoritativeError; }
	bool IsRecoveryRequired() const { return bRecoveryRequired; }
	bool IsRoutePageComplete() const { return bRoutePageComplete; }
	const dspgcs::UploadJobSnapshotDTO* GetSnapshot() const
	{
		return bHasSnapshot ? &CurrentSnapshot : nullptr;
	}
	const dspgcs::UploadJobCommandAckDTO* GetLastCommandAck() const
	{
		return bHasLastCommandAck ? &LastCommandAck : nullptr;
	}
	dspgcs::UploadJobErrorCode GetLastSnapshotResultCode() const
	{
		return LastSnapshotResultCode;
	}
	const std::string& GetLastSnapshotSafeDiagnostic() const
	{
		return LastSnapshotSafeDiagnostic;
	}
	const std::string& GetBoundJobID() const { return BoundJobID; }
	const dspgcs::UploadRouteStateDTO* FindRoute(int32 RouteSlotIndex) const;
	/** Copies only deterministic ascending RouteSlot identifiers for one completed UI model build. */
	void CopySortedRouteSlots(TArray<int32>& OutRouteSlots) const;
	int32 GetRouteStateCount() const { return static_cast<int32>(Routes.size()); }
	std::uint32_t GetNextRoutePageStart() const { return NextRoutePageStart; }
	const std::string& GetPendingCommandID() const { return PendingCommandID; }

private:
	dspgcs::UploadJobSnapshotDTO CurrentSnapshot;
	dspgcs::UploadJobCommandAckDTO LastCommandAck;
	std::unordered_map<std::int32_t, dspgcs::UploadRouteStateDTO> Routes;
	std::string PendingCommandID;
	std::string BoundJobID;
	std::string LastSnapshotSafeDiagnostic;
	std::uint64_t LastDeltaSequence = 0;
	std::uint32_t NextRoutePageStart = 0;
	dspgcs::UploadJobErrorCode LastSnapshotResultCode =
		dspgcs::UploadJobErrorCode::None;
	bool bHasSnapshot = false;
	bool bHasLastCommandAck = false;
	bool bHasAuthoritativeError = false;
	bool bRecoveryRequired = false;
	bool bRoutePageComplete = false;
};
