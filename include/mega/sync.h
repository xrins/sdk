/**
 * @file mega/sync.h
 * @brief Class for synchronizing local and remote trees
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#ifndef MEGA_SYNC_H
#define MEGA_SYNC_H 1

#include <future>
#include <unordered_set>

#include "db.h"
#include "waiter.h"

#ifdef ENABLE_SYNC
#include "node.h"

namespace mega {

struct MegaApp;
class HeartBeatSyncInfo;
class BackupInfoSync;
class BackupMonitor;
class MegaClient;
struct JSON;
class JSONWriter;

// How should the sync engine detect filesystem changes?
enum ChangeDetectionMethod
{
    // Via filesystem event notifications.
    //
    // If the filesystem notification subsystem encounters an unrecoverable
    // error then all asssociated syncs will be failed unless the user has
    // specified a scan frequency.
    CDM_NOTIFICATIONS,
    // Via periodic rescanning.
    //
    // The user must specify a scan frequency in order to use this mode.
    CDM_PERIODIC_SCANNING,
    // Unknown change detection method.
    //
    // A possible result of importing a user-edited sync config.
    CDM_UNKNOWN
}; // ChangeDetectionMethod

ChangeDetectionMethod changeDetectionMethodFromString(const string& method);

string changeDetectionMethodToString(const ChangeDetectionMethod method);

class SyncConfig
{
public:

    enum Type
    {
        TYPE_UP = 0x01, // sync up from local to remote
        TYPE_DOWN = 0x02, // sync down from remote to local
        TYPE_TWOWAY = TYPE_UP | TYPE_DOWN, // Two-way sync
        TYPE_BACKUP, // special sync up from local to remote, automatically disabled when remote changed
    };

    SyncConfig() = default;

    SyncConfig(LocalPath localPath,
        string syncName,
        NodeHandle remoteNode,
        const string& remotePath,
        const fsfp_t localFingerprint,
        const LocalPath& externalDrivePath,
        const bool enabled = true,
        const Type syncType = TYPE_TWOWAY,
        const SyncError error = NO_SYNC_ERROR,
        const SyncWarning warning = NO_SYNC_WARNING,
        handle hearBeatID = UNDEF
    );

    // the local path of the sync root folder
    const LocalPath& getLocalPath() const;

    // returns the type of the sync
    Type getType() const;

    // If the sync is enabled, we will auto-start it
    bool getEnabled() const;
    void setEnabled(bool enabled);

    // Whether this is a backup sync.
    bool isBackup() const;

    // Whether this sync is backed by an external device.
    bool isExternal() const;
    bool isInternal() const;

    // check if we need to notify the App about error/enable flag changes
    bool stateFieldsChanged();

    string syncErrorToStr();
    static string syncErrorToStr(SyncError errorCode);

    void setBackupState(SyncBackupState state);
    SyncBackupState getBackupState() const;

    // enabled/disabled by the user
    bool mEnabled = true;

    // the local path of the sync
    LocalPath mLocalPath;

    // name of the sync (if localpath is not adequate)
    string mName;

    // the remote handle of the sync
    NodeHandle mRemoteNode;

    // the path to the remote node, as last known (not definitive)
    string mOriginalPathOfRemoteRootNode;

    // uniquely identifies the filesystem, we check this is unchanged.
    fsfp_t mFilesystemFingerprint;

    // uniquely identifies the local folder.  This ID is also a component of the sync database's filename
    // so if it changes, we would lose sync state.  So, we cannot allow that
    handle mLocalPathFsid = UNDEF;

    // type of the sync, defaults to bidirectional
    Type mSyncType;

    // failure cause (disable/failure cause).
    SyncError mError;

    // Warning if creation was successful but the user should know something
    SyncWarning mWarning;

    // Unique identifier. any other field can change (even remote handle),
    // and we want to keep disabled configurations saved: e.g: remote handle changed
    // id for heartbeating
    handle mBackupId;

    // Path to the volume containing this backup (only for external backups).
    // This one is not serialized
    LocalPath mExternalDrivePath;

    // Whether this backup is monitoring or mirroring.
    SyncBackupState mBackupState;

    // Prevent applying old settings dialog based exclusion when creating .megaignore, for newer syncs.  We set this true for new syncs or after upgrade.
    bool mLegacyExclusionsIneligigble = true;

    // If the database exists then its running/suspended. Not serialized.
    bool mDatabaseExists = false;

    // Maintained as we transition
    SyncRunState mRunState = SyncRunState::Pending;

    // not serialized.  Prevent re-enabling sync after removal
    bool mSyncDeregisterSent = false;

    // not serialized.  Prevent notifying the client app for this sync's state changes and prevent reentry removing sync by sds
    bool mRemovingSyncBySds = false;

    // not serialized.  Prevent notifying the client app for this sync's state changes
    bool mFinishedInitialScanning = false;

    // Name of this sync's state cache.
    string getSyncDbStateCacheName(handle fsid, NodeHandle nh, handle userId) const;

    // How should the engine detect filesystem changes?
    ChangeDetectionMethod mChangeDetectionMethod = CDM_NOTIFICATIONS;

    // Only meaningful when a sync is in CDM_PERIODIC_SCANNING mode.
    unsigned mScanIntervalSec = 0;

    // enum to string conversion
    static const char* synctypename(const Type type);
    static bool synctypefromname(const string& name, Type& type);

    SyncError knownError() const;

    // True if this sync is operating in scan-only mode.
    bool isScanOnly() const;

private:
    // If mError or mEnabled have changed from these values, we need to notify the app.
    SyncError mKnownError = NO_SYNC_ERROR;
    bool mKnownEnabled = false;
    SyncRunState mKnownRunState = SyncRunState::Pending;
};

// Convenience.
using SyncConfigVector = vector<SyncConfig>;
struct Syncs;

struct PerSyncStats
{
    // Data that we report per running sync for display alongside the sync
    bool scanning = false;
    bool syncing = false;
    int32_t numFiles = 0;
    int32_t numFolders = 0;
    int32_t numUploads = 0;
    int32_t numDownloads = 0;

    bool operator==(const PerSyncStats&);
    bool operator!=(const PerSyncStats&);
};

struct UnifiedSync
{
    // Reference to containing Syncs object
    Syncs& syncs;

    // We always have a config
    SyncConfig mConfig;

    // If the config is good, the sync can be running
    unique_ptr<Sync> mSync;

    // High level info about this sync, sent to backup centre
    std::unique_ptr<BackupInfoSync> mBackupInfo;

    // The next detail heartbeat to send to the backup centre
    std::shared_ptr<HeartBeatSyncInfo> mNextHeartbeat;

    // ctor/dtor
    UnifiedSync(Syncs&, const SyncConfig&);

    // Update state and signal to application
    void changeState(SyncError newSyncError, bool newEnableFlag, bool notifyApp, bool keepSyncDb);

    shared_ptr<bool> sdsUpdateInProgress;

    PerSyncStats lastReportedDisplayStats;

private:
    friend class Sync;
    friend struct Syncs;
    void changedConfigState(bool save, bool notifyApp);
};

enum SyncRowType : unsigned {
    SRT_XXX,
    SRT_XXF,
    SRT_XSX,
    SRT_XSF,
    SRT_CXX,
    SRT_CXF,
    SRT_CSX,
    SRT_CSF
}; // C(cloud) S(sync) F(file) as elements of the triplet. X means that element of the triplet is missing

struct SyncRow
{
    SyncRow(CloudNode* node, LocalNode* syncNode, FSNode* fsNode)
        : cloudNode(node)
        , syncNode(syncNode)
        , fsNode(fsNode)
    {
    };

    // needs to be move constructable/assignable for sorting (note std::list of non-copyable below)
    SyncRow(SyncRow&&) = default;
    SyncRow& operator=(SyncRow&&) = default;

    CloudNode* cloudNode;
    LocalNode* syncNode;
    FSNode* fsNode;

    NodeHandle cloudHandleOpt() { return cloudNode ? cloudNode->handle : NodeHandle(); }

    vector<CloudNode*> cloudClashingNames;
    vector<FSNode*> fsClashingNames;

    // True if we've recorded any clashing names.
    bool hasClashes() const;

    // True if this row exists in the cloud.
    bool hasCloudPresence() const;

    // True if this row exists on disk.
    bool hasLocalPresence() const;

    bool suppressRecursion = false;
    bool itemProcessed = false;

    bool recurseBelowRemovedCloudNode = false;
    bool recurseBelowRemovedFsNode = false;

    vector<SyncRow>* rowSiblings = nullptr;
    const LocalPath& comparisonLocalname() const;

    // This list stores "synthesized" FSNodes: That is, nodes that we
    // haven't scanned yet but we know must exist.
    //
    // An example would be when we download a directory from the cloud.
    // Here, we create directory locally and push an FSNode representing it
    // to this list so that we recurse into it immediately.
    list<FSNode> fsAddedSiblings;

    void inferOrCalculateChildSyncRows(bool wasSynced, vector<SyncRow>& childRows, vector<FSNode>& fsInferredChildren, vector<FSNode>& fsChildren, vector<CloudNode>& cloudChildren,
            bool belowRemovedFsNode, fsid_localnode_map& localnodeByScannedFsid);

    bool empty() { return !cloudNode && !syncNode && !fsNode && cloudClashingNames.empty() && fsClashingNames.empty(); }

    // What type of sync row is this?
    SyncRowType type() const;

    // Convenience specializations.
    ExclusionState exclusionState(const CloudNode& node) const;
    ExclusionState exclusionState(const FSNode& node) const;
    ExclusionState exclusionState(const LocalPath& name, nodetype_t type, m_off_t size) const;

    bool hasCaseInsensitiveLocalNameChange() const;
    bool hasCaseInsensitiveCloudNameChange() const;

    // Does this row represent an ignore file?
    bool isIgnoreFile() const;
    bool isLocalOnlyIgnoreFile() const;

    // Does this row represent a "no name" triplet?
    bool isNoName() const;
};

struct SyncPath
{
    // Tracks both local and remote absolute paths (whether they really exist or not) as we recurse the sync nodes
    LocalPath localPath;
    string cloudPath;

    // this one purely from the sync root (using cloud name, to avoid escaped names)
    string syncPath;

    bool appendRowNames(const SyncRow& row, FileSystemType filesystemType);

    SyncPath(Syncs& s, const LocalPath& fs, const string& cloud) : localPath(fs), cloudPath(cloud), syncs(s) {}
private:
    Syncs& syncs;
};

struct SyncStatusInfo
{
    handle mBackupID = UNDEF;
    string mName;
    size_t mTotalSyncedBytes = 0;
    size_t mTotalSyncedNodes = 0;
    SyncTransferCounts mTransferCounts;
}; // SyncStatusInfo

class SyncThreadsafeState
{
    // This class contains things that are read/written from either the Syncs thread,
    // or the MegaClient thread.  A mutex is used to keep the data consistent.
    // Referred to by shared_ptr so transfers etc don't have awkward lifetime issues.
    mutable mutex mMutex;

    // If we make a LocalNode for an upload we created ourselves,
    // it's because the local file is no longer at the matching position
    // and we need to set it up to be moved correspondingly
    map<string, weak_ptr<SyncUpload_inClient>> mExpectedUploads;

    // Transfers update these from the client thread
    void adjustTransferCounts(bool upload, int32_t adjustQueued, int32_t adjustCompleted, m_off_t adjustQueuedBytes, m_off_t adjustCompletedBytes);

    // track uploads/downloads
    SyncTransferCounts mTransferCounts;

    int32_t mFolderCount = 0;
    int32_t mFileCount = 0;

    // know where the sync's tmp folder is
    LocalPath mSyncTmpFolder;

    MegaClient* mClient = nullptr;
    handle mBackupId = 0;

public:

    const bool mCanChangeVault;

    // Remember which Nodes we created from upload,
    // until the corresponding LocalNodes are updated.
    void addExpectedUpload(NodeHandle parentHandle, const string& name, weak_ptr<SyncUpload_inClient>);
    void removeExpectedUpload(NodeHandle parentHandle, const string& name);
    shared_ptr<SyncUpload_inClient> isNodeAnExpectedUpload(NodeHandle parentHandle, const string& name);

    void transferBegin(direction_t direction, m_off_t numBytes);
    void transferComplete(direction_t direction, m_off_t numBytes);
    void transferFailed(direction_t direction, m_off_t numBytes);

    // Return a snapshot of this sync's current transfer counts.
    SyncTransferCounts transferCounts() const;

    void incrementSyncNodeCount(nodetype_t type, int32_t count);
    void getSyncNodeCounts(int32_t& files, int32_t& folders);

    std::atomic<unsigned> neverScannedFolderCount{};

    LocalPath syncTmpFolder() const;
    void setSyncTmpFolder(const LocalPath&);

    SyncThreadsafeState(handle backupId, MegaClient* client, bool canChangeVault) : mClient(client), mBackupId(backupId), mCanChangeVault(canChangeVault)  {}
    handle backupId() const { return mBackupId; }
    MegaClient* client() const { return mClient; }
};

class MEGA_API Sync
{
public:

    // returns the sync config
    SyncConfig& getConfig();
    const SyncConfig& getConfig() const;

    Syncs& syncs;

    // for logging
    string syncname;

    // sync-wide directory notification provider
    std::unique_ptr<DirNotify> dirnotify;

    // track how recent the last received fs noticiation was
    dstime lastFSNotificationTime = 0;

    // root of local filesystem tree, holding the sync's root folder.  Never null except briefly in the destructor (to ensure efficient db usage)
    unique_ptr<LocalNode> localroot;

    // Cloud node to sync to
    CloudNode cloudRoot;
    string cloudRootPath;
    handle cloudRootOwningUser;

    FileSystemType mFilesystemType = FS_UNKNOWN;

    // We test the sync root folder, and assume the rest of the filesystem tree has the same case sensitivity
    bool mCaseInsensitive = false;

    // syncing to an inbound share?
    bool inshare = false;

    // insertion/update queue
    localnode_set insertq;

    // adds an entry to the delete queue - removes it from insertq
    void statecachedel(LocalNode*);

    // adds an entry to the insert queue - removes it from deleteq
    void statecacheadd(LocalNode*);

    // recursively add children
    void addstatecachechildren(uint32_t, idlocalnode_map*, LocalPath&, LocalNode*, int);

    // Caches all synchronized LocalNode
    void cachenodes();

    // change state, signal to application
    void changestate(SyncError newSyncError, bool newEnableFlag, bool notifyApp, bool keepSyncDb);

    // process all outstanding filesystem notifications (mark sections of the sync tree to visit)
    dstime procscanq();

    // helper for checking moves etc
    bool checkIfFileIsChanging(FSNode& fsNode, const LocalPath& fullPath);

    // look up LocalNode relative to localroot
    LocalNode* localnodebypath(LocalNode*, const LocalPath&, LocalNode** parent, LocalPath* outpath, bool fromOutsideThreadAlreadyLocked);

    void combineTripletSet(vector<SyncRow>::iterator a, vector<SyncRow>::iterator b) const;

    vector<SyncRow> computeSyncTriplets(
        vector<CloudNode>& cloudNodes,
        const LocalNode& root,
        vector<FSNode>& fsNodes) const;
    bool inferRegeneratableTriplets(
        vector<CloudNode>& cloudNodes,
        const LocalNode& root,
        vector<FSNode>& fsNodes,
        vector<SyncRow>& inferredRows) const;

    struct PerFolderLogSummaryCounts
    {
        // in order to not swamp the logs, but still be able to diagnose.
        // mention one specfic item, then the count of the rest
        int alreadySyncedCount = 0;
        int alreadyUploadingCount = 0;
        int alreadyDownloadingCount = 0;
        bool report(string&);
    };

    bool recursiveSync(SyncRow& row, SyncPath& fullPath, bool belowRemovedCloudNode, bool belowRemovedFsNode, unsigned depth);
    bool syncItem_checkMoves(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool belowRemovedCloudNode, bool belowRemovedFsNode);
    bool syncItem_checkFilenameClashes(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath);
    bool syncItem_checkBackupCloudNameClash(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath);
    bool syncItem_checkDownloadCompletion(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath);
    bool syncItem(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, PerFolderLogSummaryCounts& pflsc);

    string logTriplet(SyncRow& row, SyncPath& fullPath);

    // resolve_* functions are to do with managing the various cases syncing a single item
    // they all return true/false depending on whether the node is now in sync
    // and therefore does not need to be visited again (until another change arrives).
    bool resolve_checkMoveDownloadComplete(SyncRow& row, SyncPath& fullPath);
    bool resolve_checkMoveComplete(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath);
    bool resolve_rowMatched(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, PerFolderLogSummaryCounts& pflsc);
    bool resolve_userIntervention(SyncRow& row, SyncPath& fullPath);
    bool resolve_makeSyncNode_fromFS(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool considerSynced);
    bool resolve_makeSyncNode_fromCloud(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool considerSynced);
    bool resolve_delSyncNode(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, unsigned deleteCounter);
    bool resolve_upsync(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, PerFolderLogSummaryCounts& pflsc);
    bool resolve_downsync(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool alreadyExists, PerFolderLogSummaryCounts& pflsc);
    bool resolve_cloudNodeGone(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath);
    bool resolve_fsNodeGone(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath);

    bool syncEqual(const CloudNode&, const FSNode&);
    bool syncEqual(const CloudNode&, const LocalNode&);
    bool syncEqual(const FSNode&, const LocalNode&);

    bool checkSpecialFile(SyncRow& child, SyncRow& parent, SyncPath& path);

    bool checkLocalPathForMovesRenames(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool& rowResult, bool belowRemovedCloudNode);
    bool checkCloudPathForMovesRenames(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool& rowResult, bool belowRemovedFsNode);
    bool checkForCompletedCloudMoveToHere(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool& rowResult);
    bool processCompletedUploadFromHere(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool& rowResult, shared_ptr<SyncUpload_inClient>);
    bool checkForCompletedFolderCreateHere(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool& rowResult);
    bool checkForCompletedCloudMovedToDebris(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool& rowResult);

    void recursiveCollectNameConflicts(SyncRow& row, list<NameConflict>& nc, SyncPath& fullPath);
    bool recursiveCollectNameConflicts(list<NameConflict>& nc);

    void purgeStaleDownloads();
    bool makeSyncNode_fromFS(SyncRow& row, SyncRow& parentRow, SyncPath& fullPath, bool considerSynced);

    // debris path component relative to the base path
    string debris;
    LocalPath localdebris;
    LocalPath localdebrisname;

    // state cache table
    unique_ptr<DbTable> statecachetable;

    // move file or folder to localdebris
    bool movetolocaldebris(const LocalPath& localpath);
    bool movetolocaldebrisSubfolder(const LocalPath& localpath, const LocalPath& targetFolder, bool logFailReason, bool& failedDueToTargetExists);

private:
    string mLastDailyDateTimeDebrisName;
    unsigned mLastDailyDateTimeDebrisCounter = 0;

public:
    // does the filesystem have stable IDs? (FAT does not)
    bool fsstableids = false;

    // true if the local synced folder is a network folder
    bool isnetwork = false;

    // flag to optimize destruction by skipping calls to treestate()
    bool mDestructorRunning = false;

    // How deep is this sync's cloud root?
    unsigned mCurrentRootDepth = 0;

    Sync(UnifiedSync&, const string&, const LocalPath&, bool, const string& logname, SyncError& e);
    ~Sync();

    // Asynchronous scan request / result.
    std::shared_ptr<ScanService::ScanRequest> mActiveScanRequestGeneral;

    // we can additionally be scanning one more yet-unscanned folder
    // in order to always be progressing even when downloads are
    // triggering rescans of their target folder
    std::shared_ptr<ScanService::ScanRequest> mActiveScanRequestUnscanned;

    static const int SCANNING_DELAY_DS;
    static const int EXTRA_SCANNING_DELAY_DS;
    static const int FILE_UPDATE_DELAY_DS;
    static const int FILE_UPDATE_MAX_DELAY_SECS;
    static const dstime RECENT_VERSION_INTERVAL_SECS;

    // We don't officially support the synchronization of trees greater than this depth.
    // Note that the depth is from the cloud root, not from the sync root.
    static const unsigned MAX_CLOUD_DEPTH;

    // Whether this is a backup sync.
    bool isBackup() const;

    // Whether this is a backup sync and it is mirroring.
    bool isBackupAndMirroring() const;

    // Whether this is a backup sync and it is monitoring.
    bool isBackupMonitoring() const;

    // Move the sync into the monitoring state.
    void setBackupMonitoring();

    // True if this sync should have a state cache database.
    bool shouldHaveDatabase() const;

    // What filesystem is this sync running on?
    const fsfp_t& fsfp() const;

    UnifiedSync& mUnifiedSync;

    // timer for whole-sync rescan in case of notifications failing or not being available
    BackoffTimer syncscanbt;

    shared_ptr<SyncThreadsafeState> threadSafeState;

protected :
    void readstatecache();

private:
    LocalPath mLocalPath;

    // permanent lock on the debris/tmp folder
    void createDebrisTmpLockOnce();

    // actually test the filesystem to be sure
    bool determineCaseInsenstivity(bool secondTry);

    // permanent lock on the debris/tmp folder
    unique_ptr<FileAccess> tmpfa;
    LocalPath tmpfaPath;
};

class SyncConfigIOContext;

class SyncConfigStore {
public:
    // How we compare drive paths.
    struct DrivePathComparator
    {
        bool operator()(const LocalPath& lhs, const LocalPath& rhs) const
        {
            return platformCompareUtf(lhs, false, rhs, false) < 0;
        }
    }; // DrivePathComparator

    using DriveSet = set<LocalPath, DrivePathComparator>;

    SyncConfigStore(const LocalPath& dbPath, SyncConfigIOContext& ioContext);
    ~SyncConfigStore();

    // Remember whether we need to update the file containing configs on this drive.
    void markDriveDirty(const LocalPath& drivePath);

    // Retrieve a drive's unique backup ID.
    handle driveID(const LocalPath& drivePath) const;

    // Whether any config data has changed and needs to be written to disk
    bool dirty() const;

    // Reads a database from disk.
    error read(const LocalPath& drivePath, SyncConfigVector& configs, bool isExternal);

    // Write the configs with this drivepath to disk.
    error write(const LocalPath& drivePath, const SyncConfigVector& configs);

    // Check whether we read configs from a particular drive
    bool driveKnown(const LocalPath& drivePath) const;

    // What drives do we know about?
    vector<LocalPath> knownDrives() const;

    // Remove a known drive.
    bool removeDrive(const LocalPath& drivePath);

    // update configs on disk for any drive marked as dirty
    auto writeDirtyDrives(const SyncConfigVector& configs) -> DriveSet;

private:
    // Metadata regarding a given drive.
    struct DriveInfo
    {
        // Path to the drive itself.
        LocalPath drivePath;

        // The drive's unique backup ID.
        // Meaningful only for external backups.
        handle driveID = UNDEF;

        // Tracks which 'slot' we're writing to.
        unsigned int slot = 0;

        bool dirty = false;
    }; // DriveInfo

    using DriveInfoMap = map<LocalPath, DriveInfo, DrivePathComparator>;

    // Checks whether two paths are equal.
    bool equal(const LocalPath& lhs, const LocalPath& rhs) const;

    // Computes a suitable DB path for a given drive.
    LocalPath dbPath(const LocalPath& drivePath) const;

    // Reads a database from the specified slot on disk.
    error read(DriveInfo& driveInfo, SyncConfigVector& configs, unsigned int slot, bool isExternal);

    // Where we store databases for internal syncs.
    const LocalPath mInternalSyncStorePath;

    // What drives are known to the store.
    DriveInfoMap mKnownDrives;

    // IO context used to read and write from disk.
    SyncConfigIOContext& mIOContext;
}; // SyncConfigStore


class MEGA_API SyncConfigIOContext
{
public:
    SyncConfigIOContext(FileSystemAccess& fsAccess,
                            const string& authKey,
                            const string& cipherKey,
                            const string& name,
                            PrnGen& rng);

    virtual ~SyncConfigIOContext();

    MEGA_DISABLE_COPY_MOVE(SyncConfigIOContext)

    // Deserialize configs from JSON (with logging.)
    bool deserialize(const LocalPath& dbPath,
                     SyncConfigVector& configs,
                     JSON& reader,
                     unsigned int slot,
                     bool isExternal) const;

    bool deserialize(SyncConfigVector& configs,
                     JSON& reader,
                     bool isExternal) const;

    // Retrieve a drive's unique backup ID.
    virtual handle driveID(const LocalPath& drivePath) const;

    // Return a reference to this context's filesystem access.
    FileSystemAccess& fsAccess() const;

    // Determine which slots are present.
    virtual error getSlotsInOrder(const LocalPath& dbPath,
                                  vector<unsigned int>& confSlots);

    // Read data from the specified slot.
    virtual error read(const LocalPath& dbPath,
                       string& data,
                       unsigned int slot);

    // Remove an existing slot from disk.
    virtual error remove(const LocalPath& dbPath,
                         unsigned int slot);

    // Remove all existing slots from disk.
    virtual error remove(const LocalPath& dbPath);

    // Serialize configs to JSON.
    void serialize(const SyncConfigVector &configs,
                   JSONWriter& writer) const;

    // Write data to the specified slot.
    virtual error write(const LocalPath& dbPath,
                        const string& data,
                        unsigned int slot);

    // Prefix applied to configuration database names.
    static const string NAME_PREFIX;

private:
    // Generate complete database path.
    LocalPath dbFilePath(const LocalPath& dbPath,
                         unsigned int slot) const;

    // Decrypt data.
    bool decrypt(const string& in, string& out);

    // Deserialize a config from JSON.
    bool deserialize(SyncConfig& config, JSON& reader, bool isExternal) const;

    // Encrypt data.
    string encrypt(const string& data);

    // Serialize a config to JSON.
    void serialize(const SyncConfig& config, JSONWriter& writer) const;

    // The cipher protecting the user's configuration databases.
    SymmCipher mCipher;

    // How we access the filesystem.
    FileSystemAccess& mFsAccess;

    // Name of this user's configuration databases.
    LocalPath mName;

    // Pseudo-random number generator.
    PrnGen& mRNG;

    // Hash used to authenticate configuration databases.
    HMACSHA256 mSigner;
}; // SyncConfigIOContext

/**
 * A Synchronization operation detected a problem and is
 * not able to continue (a stall)
 */
struct SyncStallEntry
{
    // Gives an overall reason for the stall
    // There may be a more specific pathProblem in one of the paths
    SyncWaitReason reason = SyncWaitReason::NoReason;

    // Set this true if there's no way this stall will be resolved
    // automatically.  That way, we can alert the user at the first
    // opportunity, instead of waiting until we run out of sync
    // activity that can occur.
    bool alertUserImmediately = false;

    // Indicates if we detected the issue from a user change in the cloud
    // otherwise, it was from a local change.
    // Showing it in the GUI helps the user understand what happened, especially for moves.
    bool detectionSideIsMEGA = false;

    struct StallCloudPath
    {
        PathProblem problem = PathProblem::NoProblem;
        string cloudPath;
        NodeHandle cloudHandle;

        StallCloudPath() {}

        StallCloudPath(NodeHandle h, const string& cp, PathProblem pp = PathProblem::NoProblem)
            : problem(pp), cloudPath(cp), cloudHandle(h)
        {
        }

        string debugReport()
        {
            string r = cloudPath;
            if (problem != PathProblem::NoProblem)
                r += " (" + string(syncPathProblemDebugString(problem)) + ")";
            return r;
        }
    };

    struct StallLocalPath
    {
        PathProblem problem = PathProblem::NoProblem;
        LocalPath localPath;

        StallLocalPath() {}

        StallLocalPath(const LocalPath& lp, PathProblem pp = PathProblem::NoProblem)
            : problem(pp), localPath(lp)
        {
        }

        string debugReport()
        {
            string r = localPath.toPath(false);
            if (problem != PathProblem::NoProblem)
                r += " (" + string(syncPathProblemDebugString(problem)) + ")";
            return r;
        }
    };

    // These are the paths involved with the stall case.
    // If a path is empty, it's irrelevant to the case
    // The problem might be local or remote, check the correpsonding PathProblem.
    // Typically we tried to do something on the problem side
    // The paths on the other side are what motivated the attempt.
    // Eg. Saw in the cloud cloudPath1 moved to cloudPath2
    //     Tried to move localPath1 to localPath2, got error localPath2Problem.
    StallCloudPath cloudPath1;
    StallCloudPath cloudPath2;
    StallLocalPath localPath1;
    StallLocalPath localPath2;

    SyncStallEntry(SyncWaitReason r, bool immediate, bool dueTocloudSideChange, StallCloudPath&& cp1, StallCloudPath&& cp2, StallLocalPath&& lp1, StallLocalPath&& lp2)
        : reason(r)
        , alertUserImmediately(immediate)
        , detectionSideIsMEGA(dueTocloudSideChange)
        , cloudPath1(move(cp1))
        , cloudPath2(move(cp2))
        , localPath1(move(lp1))
        , localPath2(move(lp2))
    {
    }
};

struct SyncStallInfo
{
    using StalledSyncsSet = std::unordered_set<handle>;
    using CloudStallInfoMap = map<string, SyncStallEntry>;
    using LocalStallInfoMap = map<LocalPath, SyncStallEntry>;

    /** No stalls detected */
    bool empty() const;

    bool waitingCloud(const string& mapKeyPath,
                      SyncStallEntry&& e);

    bool waitingLocal(const LocalPath& mapKeyPath,
                      SyncStallEntry&& e);

    bool isSyncStalled(handle backupId) const;

    CloudStallInfoMap cloud;
    LocalStallInfoMap local;
    StalledSyncsSet stalledSyncs;

    /** Requires user action to resolve */
    bool hasImmediateStallReason() const;
};

struct SyncProblems
{
    list<NameConflict> mConflicts;
    SyncStallInfo mStalls;
    bool mConflictsDetected = false;
    bool mStallsDetected = false;
}; // SyncProblems

struct SyncFlags
{
    // we can only perform moves after scanning is complete
    bool scanningWasComplete = false;

    // track whether all our reachable nodes have been scanned
    bool reachableNodesAllScannedThisPass = true;
    bool reachableNodesAllScannedLastPass = true;

    // true anytime we have just added a new sync, or unsuspended (unpaused) one
    bool isInitialPass = true;

    // we can only delete/upload/download after moves are complete
    bool movesWereComplete = false;

    // stall detection (for incompatible local and remote changes, eg file added locally in a folder removed remotely)
    bool noProgress = true;
    int noProgressCount = 0;

    bool earlyRecurseExitRequested = false;

    // to help with slowing down retries in stall state
    dstime recursiveSyncLastCompletedDs = 0;

    SyncStallInfo stall;
};

// This interface exists to give tests a little more control over how the
// sync engine behaves.
//
// The basic idea is that the sync will will ask an outside entity whether
// it should proceed with a certain action. Say, whether it should upload a
// particular file or complete a putnodes request (binding.)
//
// This is necessary as some tests require very specific sequencing in order
// to pass reliably.
class SyncController
{
protected:
    SyncController();

public:
    virtual ~SyncController();

    // Should we defer sending a putnodes for the specified file?
    virtual bool deferPutnode(const LocalPath& path) const;

    // Should we defer the completion of a putnodes sent for the specified file?
    virtual bool deferPutnodeCompletion(const LocalPath& path) const;

    // Should we defer uploading of the specified file?
    virtual bool deferUpload(const LocalPath& path) const;
}; // SyncController

// Convenience.
using HasImmediateStallPredicate =
  std::function<bool(const SyncStallInfo&)>;

using IsImmediateStallPredicate =
  std::function<bool(const SyncStallEntry& entry)>;

using SyncControllerPtr = std::shared_ptr<SyncController>;
using SyncControllerWeakPtr = std::weak_ptr<SyncController>;

struct Syncs
{
    // Retrieve a copy of configured sync settings (thread safe)
    SyncConfigVector getConfigs(bool onlyActive) const;
    bool configById(handle backupId, SyncConfig&) const;
    SyncConfigVector configsForDrive(const LocalPath& drive) const;
    SyncConfigVector selectedSyncConfigs(std::function<bool(SyncConfig&, Sync*)> selector) const;
    handle getSyncIdContainingActivePath(const LocalPath& lp) const;

    // Add new sync setups
    void appendNewSync(const SyncConfig&, bool startSync, std::function<void(error, SyncError, handle)> completion, bool completionInClient, const string& logname, const string& excludedPath = string());


    // only for use in tests; not really thread safe
    Sync* runningSyncByBackupIdForTests(handle backupId) const;

    void transferPauseFlagsUpdated(bool downloadsPaused, bool uploadsPaused);

    // returns a copy of the config, for thread safety
    bool syncConfigByBackupId(handle backupId, SyncConfig&) const;

    void purgeRunningSyncs();
    void loadSyncConfigsOnFetchnodesComplete(bool resetSyncConfigStore);
    void resumeSyncsOnStateCurrent();

    void enableSyncByBackupId(handle backupId, bool setOriginalPath, std::function<void(error, SyncError, handle)> completion, bool completionInClient, const string& logname);
    void disableSyncByBackupId(handle backupId, SyncError syncError, bool newEnabledFlag, bool keepSyncDb, std::function<void()> completion);

    // disable all active syncs.  Cache is kept
    void disableSyncs(SyncError syncError, bool newEnabledFlag, bool keepSyncDb);

    // Called via MegaApi::removeSync - cache files are deleted and syncs unregistered.  Synchronous (for now)
    void deregisterThenRemoveSync(handle backupId, std::function<void(Error)> completion, std::function<void(MegaClient&, TransferDbCommitter&)> clientRemoveSdsEntryFunction);
    
    // async, callback on client thread
    void renameSync(handle backupId, const string& newname, std::function<void(Error e)> result);

    void prepareForLogout(bool keepSyncsConfigFile, std::function<void()> clientCompletion);

    void locallogout(bool removecaches, bool keepSyncsConfigFile, bool reopenStoreAfter);

    // get snapshots of the sync configs

    // synchronous for now as that's a constraint from the intermediate layer
    NodeHandle getSyncedNodeForLocalPath(const LocalPath&);

    // synchronous and requires first locking mLocalNodeChangeMutex
    treestate_t getSyncStateForLocalPath(handle backupId, const LocalPath&);

    // a variant for recovery after not being able to lock the mutex in time, all on the sync thread
    bool getSyncStateForLocalPath(const LocalPath& lp, treestate_t& ts, nodetype_t& nt, SyncConfig& sc);

    Syncs(MegaClient& mc);
    ~Syncs();

    void getSyncProblems(std::function<void(unique_ptr<SyncProblems>)> completion,
                         bool completionInClient);

    // Retrieve status information about sync(s).
    using SyncStatusInfoCompletion =
      std::function<void(vector<SyncStatusInfo>)>;

    void getSyncStatusInfo(handle backupID,
                           SyncStatusInfoCompletion completion,
                           bool completionInClient);

    void getSyncStatusInfoInThread(handle backupID,
                                   SyncStatusInfoCompletion completion);

    /**
     * @brief
     * Removes previously opened backup databases from that drive from memory.
     *
     * Note that this function will:
     * - Flush any pending database changes.
     * - Remove all contained backup configs from memory.
     * - Remove the database itself from memory.
     *
     * @param drivePath
     * The drive containing the database to remove.
     */
    void backupCloseDrive(const LocalPath& drivePath, std::function<void(Error)> clientCallback);

    /**
     * @brief
     * Restores backups from an external drive.
     */
    void backupOpenDrive(const LocalPath& drivePath, std::function<void(Error)> clientCallback);


    // Add a config directly to the internal sync config DB.
    //
    // Note that configs added in this way bypass the usual sync mechanism.
    // That is, they are added directly to the JSON DB on disk.
    error syncConfigStoreAdd(const SyncConfig& config);

    // Move path to .debris folder associated to the backupId
    // Note: This method is thread safe.
    // completion is executed in sync thread, completionInClient is executed in client thread
    void moveToSyncDebrisByBackupID(const string& path, handle backupId, std::function<void(Error)> completion, std::function<void (Error)> completionInClient);

private:  // anything to do with loading/saving/storing configs etc is done on the sync thread

    // Returns a reference to this user's internal configuration database.
    SyncConfigStore* syncConfigStore();

    // Whether the internal database has changes that need to be written to disk.
    bool syncConfigStoreDirty();

    // Attempts to flush the internal configuration database to disk.
    bool syncConfigStoreFlush();

    // Load internal sync configs from disk.
    error syncConfigStoreLoad(SyncConfigVector& configs);

    // updates in state & error
    void saveSyncConfig(const SyncConfig& config);


public:

    string exportSyncConfigs(const SyncConfigVector configs) const;
    string exportSyncConfigs() const;

    void importSyncConfigs(const char* data, std::function<void(error)> completion);

    // maps local fsid to corresponding LocalNode* (s)
    fsid_localnode_map localnodeBySyncedFsid;
    fsid_localnode_map localnodeByScannedFsid;
    LocalNode* findLocalNodeBySyncedFsid(const fsfp_t& fsfp, mega::handle fsid, const LocalPath& originalpath, nodetype_t type, const FileFingerprint& fp,
                                         std::function<bool(LocalNode* ln)> extraCheck, handle owningUser, bool& foundExclusionUnknown);
    LocalNode* findLocalNodeByScannedFsid(const fsfp_t& fsfp, mega::handle fsid, const LocalPath& originalpath, nodetype_t type, const FileFingerprint* fp,
                                         std::function<bool(LocalNode* ln)> extraCheck, handle owningUser, bool& foundExclusionUnknown);

    void setSyncedFsidReused(const fsfp_t& fsfp, mega::handle fsid);
    void setScannedFsidReused(const fsfp_t& fsfp, mega::handle fsid);

    // maps nodehandle to corresponding LocalNode* (s)
    nodehandle_localnode_map localnodeByNodeHandle;
    bool findLocalNodeByNodeHandle(NodeHandle h, LocalNode*& sourceSyncNodeOriginal, LocalNode*& sourceSyncNodeCurrent, bool& unsureDueToIncompleteScanning, bool& unsureDueToUnknownExclusionMoveSource);

    // manage syncdown flags inside the syncs
    // backupId of UNDEF to rescan all
    void setSyncsNeedFullSync(bool andFullScan, bool andReFingerprint, handle backupId);

    // retrieves information about any detected name conflicts.
    bool conflictsDetected(list<NameConflict>* conflicts) const;

    bool syncStallDetected(SyncStallInfo& si) const;

    // Get name conficts - pass UNDEF to collect for all syncs.
    void collectSyncNameConflicts(handle backupId, std::function<void(list<NameConflict>&& nc)>, bool completionInClient);

    list<weak_ptr<LocalNode::RareFields::ScanBlocked>> scanBlockedPaths;
    list<weak_ptr<LocalNode::RareFields::BadlyFormedIgnore>> badlyFormedIgnoreFilePaths;

    typedef std::function<void(MegaClient&, TransferDbCommitter&)> QueuedClientFunc;
    ThreadSafeDeque<QueuedClientFunc> clientThreadActions;

    typedef std::pair<std::function<void()>, string> QueuedSyncFunc;
    ThreadSafeDeque<QueuedSyncFunc> syncThreadActions;

    void syncRun(std::function<void()>, const string& actionName);
    void queueSync(std::function<void()>&&, const string& actionName);
    void queueClient(QueuedClientFunc&&, bool fromAnyThread = false);

    bool onSyncThread() const { return std::this_thread::get_id() == syncThreadId; }

    // Update remote location
    bool checkSyncRemoteLocationChange(UnifiedSync&, bool exists, string cloudPath);

    // Cause periodic-scan syncs to scan now (waiting for the next periodic scan is impractical for tests)
    std::future<size_t> triggerPeriodicScanEarly(handle backupID);

    // mark nodes as needing to be checked for sync actions
    void triggerSync(NodeHandle, bool recurse = false);
    void triggerSync(const LocalPath& lp, bool scan);

    // set default permission for file system access
    void setdefaultfilepermissions(int permissions);
    void setdefaultfolderpermissions(int permissions);

    // ------ public data members (thread safe)

    // waiter for sync loop on thread
    shared_ptr<Waiter> waiter;
    bool skipWait = false;

    // These rules are used to generate ignore files for newly added syncs.
    DefaultFilterChain mNewSyncFilterChain;
    DefaultFilterChain mLegacyUpgradeFilterChain;

    // todo: move relevant code to this class later
    // this mutex protects the LocalNode trees while MEGAsync receives requests from the filesystem browser for icon indicators
    std::timed_mutex mLocalNodeChangeMutex;  // needs to be locked when making changes on this thread; or when accessing from another thread

    // flags matching the state we have reported to the app via callbacks
    bool syncscanstate = false;
    bool syncBusyState = false;
    bool syncStallState = false;
    bool syncConflictState = false;

    bool mSyncsLoaded = false;
    bool mSyncsResumed = false;

    // for quick lock free reference by MegaApiImpl::syncPathState (don't slow down windows explorer)
    bool mSyncVecIsEmpty = true;

    // directly accessed flag that makes sync-related logging a lot more detailed
    bool mDetailedSyncLogging = true;

    // total number of LocalNode objects (only updated by syncs thread)
    std::atomic<int32_t> totalLocalNodes{0};

    // backup rework implies certain restrictions that can be skipped
    // by setting this flag
    bool mBackupRestrictionsEnabled = true;

    std::atomic<int> completedPassCount{0};

private:

    // functions for internal use on-thread only
    void stopSyncsInErrorState();

    void processTriggerHandles();
    void processTriggerLocalpaths();

    void exportSyncConfig(JSONWriter& writer, const SyncConfig& config) const;

    bool importSyncConfig(JSON& reader, SyncConfig& config);
    bool importSyncConfigs(const string& data, SyncConfigVector& configs);

    // Returns a reference to this user's sync config IO context.
    SyncConfigIOContext* syncConfigIOContext();

    void proclocaltree(LocalNode* n, LocalTreeProc* tp);

    bool mightAnySyncsHaveMoves();
    bool isAnySyncSyncing();
    bool isAnySyncScanning_inThread();

    // actually start the sync (on sync thread)
    void startSync_inThread(UnifiedSync& us, const string& debris, const LocalPath& localdebris,
        bool inshare, bool isNetwork, const LocalPath& rootpath,
        std::function<void(error, SyncError, handle)> completion, const string& logname);
    void prepareForLogout_inThread(bool keepSyncsConfigFile, std::function<void()> clientCompletion);
    void locallogout_inThread(bool removecaches, bool keepSyncsConfigFile, bool reopenStoreAfter);
    void loadSyncConfigsOnFetchnodesComplete_inThread(bool resetSyncConfigStore);
    void resumeSyncsOnStateCurrent_inThread();
    void enableSyncByBackupId_inThread(handle backupId, bool setOriginalPath, std::function<void(error, SyncError, handle)> completion, const string& logname, const string& excludedPath = string());
    void disableSyncByBackupId_inThread(handle backupId, SyncError syncError, bool newEnabledFlag, bool keepSyncDb, std::function<void()> completion);
    void appendNewSync_inThread(const SyncConfig&, bool startSync, std::function<void(error, SyncError, handle)> completion, const string& logname, const string& excludedPath = string());
    void removeSyncAfterDeregistration_inThread(handle backupId, std::function<void(Error)> clientCompletion, std::function<void(MegaClient&, TransferDbCommitter&)> clientRemoveSdsEntryFunction);
    void syncConfigStoreAdd_inThread(const SyncConfig& config, std::function<void(error)> completion);
    void clear_inThread();
    void purgeRunningSyncs_inThread();
    void renameSync_inThread(handle backupId, const string& newname, std::function<void(Error e)> result);
    error backupOpenDrive_inThread(const LocalPath& drivePath);
    error backupCloseDrive_inThread(LocalPath drivePath);
    void getSyncProblems_inThread(SyncProblems& problems);
    bool checkSdsCommandsForDelete(UnifiedSync& us, vector<pair<handle, int>>& sdsBackups, std::function<void(MegaClient&, TransferDbCommitter&)>& clientRemoveSdsEntryFunction);
    bool processRemovingSyncBySds(UnifiedSync& us, bool foundRootNode, vector<pair<handle, int>>& sdsBackups);
    void deregisterThenRemoveSyncBySds(UnifiedSync& us, std::function<void(MegaClient&, TransferDbCommitter&)> clientRemoveSdsEntryFunction);

    void syncLoop();

    enum WhichCloudVersion
    {
        EXACT_VERSION,
        LATEST_VERSION,
        LATEST_VERSION_ONLY,
        FOLDER_ONLY
    };

    bool lookupCloudNode(NodeHandle h, CloudNode& cn,
            string* cloudPath,
            bool* isInTrash,
            bool* nodeIsInActiveSync,
            bool* nodeIsDefinitelyExcluded,
            unsigned* depth,
            WhichCloudVersion,
            handle* owningUser = nullptr,
            vector<pair<handle, int>>* sdsBackups = nullptr);

    bool lookupCloudChildren(NodeHandle h, vector<CloudNode>& cloudChildren);

    // Query whether a specified child is definitely excluded.
    //
    // A node is definitely excluded if:
    // - The node itself is excluded.
    // - One of the node's parents are definitely excluded.
    //
    // It is the caller's responsibility to ensure that:
    // - Necessary locks are acquired such that this function has exclusive
    //   access to both the local and remote node trees.
    // - The node specified by child is below root.
    bool isDefinitelyExcluded(const pair<std::shared_ptr<Node>, Sync*>& root, std::shared_ptr<const Node> child);

    template<typename Predicate>
    Sync* syncMatching(Predicate&& predicate)
    {
        // Sanity.
        assert(onSyncThread());

        lock_guard<mutex> guard(mSyncVecMutex);

        for (auto& i : mSyncVec)
        {
            // Skip inactive syncs.
            if (!i->mSync)
                continue;

            // Have we found our lucky sync?
            if (predicate(*i))
                return i->mSync.get();
        }

        // No syncs match our search criteria.
        return nullptr;
    }

    Sync* syncContainingPath(const LocalPath& path);
    Sync* syncContainingPath(const string& path);

    // Signal that an ignore file failed to load.
    void ignoreFileLoadFailure(const Sync& sync, const LocalPath& path);

    // Records which ignore file failed to load and under which sync.
    struct IgnoreFileFailureContext
    {
        // Clear the context if the associated sync:
        // - Is disabled (or failed.)
        // - No longer exists.
        void reset(Syncs& syncs)
        {
            if (mBackupID == UNDEF)
                return;

            auto predicate = [&](const UnifiedSync& us) {
                return us.mConfig.mBackupId == mBackupID
                       && !!us.mSync;
            };

            if (syncs.syncMatching(std::move(predicate)))
                return;

            reset();
        }

        // Clear the context.
        void reset()
        {
            mBackupID = UNDEF;
            mFilterChain.clear();
            mPath.clear();
        }

        // Report the load failure as a stall.
        void report(SyncStallInfo& stallInfo)
        {
            stallInfo.waitingLocal(mPath, SyncStallEntry(
                SyncWaitReason::FileIssue, true, false,
                {},
                {},
                {mPath, PathProblem::IgnoreFileMalformed},
                {}));
        }

        // Has the ignore file failure been resolved?
        bool resolve(FileSystemAccess& fsAccess)
        {
            // No failures to resolve so we're all good.
            if (mBackupID == UNDEF)
                return true;

            // Try and load the ignore file.
            auto result = mFilterChain.load(fsAccess, mPath);

            // Resolved if the file's been deleted or corrected.
            if (result == FLR_FAILED)
                return false;

            // Clear the failure condition.
            reset();

            // Let the caller know the situation's resolved.
            return true;
        }

        // Has an ignore file failure been signalled?
        bool signalled() const
        {
            return mBackupID != UNDEF;
        }

        // Used to load the ignore file specified below.
        FilterChain mFilterChain;

        // What ignore file failed to load?
        LocalPath mPath;

        // What sync contained the broken ignore file?
        handle mBackupID = UNDEF;
    }; // IgnoreFileFailureContext

    // Check if the sync described by config contains an ignore file.
    bool hasIgnoreFile(const SyncConfig& config);

    void confirmOrCreateDefaultMegaignore(bool transitionToMegaignore, unique_ptr<DefaultFilterChain>& resultIfDfc, unique_ptr<string_vector>& resultIfMegaignoreDefault);

    // ------ private data members

    MegaClient& mClient;

    // Syncs should have a separate fsaccess for thread safety
    unique_ptr<FileSystemAccess> fsaccess;

    // pseudo-random number generator
    PrnGen rng;

    // Track some state during and between recursiveSync runs
    unique_ptr<SyncFlags> mSyncFlags;

    // This user's internal sync configuration store.
    unique_ptr<SyncConfigStore> mSyncConfigStore;

    // Responsible for securely writing config databases to disk.
    unique_ptr<SyncConfigIOContext> mSyncConfigIOContext;

    // Sometimes the Client needs a list of the sync configs, we provide it by copy (mutex for thread safety of course)
    mutable mutex mSyncVecMutex;
    vector<unique_ptr<UnifiedSync>> mSyncVec;

    // unload the Sync (remove from RAM and data structures), its config will be flushed to disk
    bool unloadSyncByBackupID(handle id, bool newEnabledFlag, SyncConfig&);

    // used to asynchronously perform scans.
    unique_ptr<ScanService> mScanService;

    // Separate key to avoid threading issues
    SymmCipher syncKey;

    // data structure with mutex to interchange stall info
    SyncStallInfo stallReport;
    mutable mutex stallReportMutex;

    // When the node tree changes, this structure lets the sync code know which LocalNodes need to be flagged
    map<NodeHandle, bool> triggerHandles;
    map<LocalPath, bool> triggerLocalpaths;
    mutex triggerMutex;

    // Keep track of files that we can't move yet because they are changing
    struct FileChangingState
    {
        // values related to possible files being updated
        m_off_t updatedfilesize = ~0;
        m_time_t updatedfilets = 0;
        m_time_t updatedfileinitialts = 0;
    };
    std::map<LocalPath, FileChangingState> mFileChangingCheckState;

    // Keep track of which LocalNodes are involved in moves
    // Sometimes we can't find the other end by fsid for example, if both Cloud and FSNode moved.
    std::set<LocalNode*> mMoveInvolvedLocalNodes;
    LocalNode* findMoveFromLocalNode(const shared_ptr<LocalNode::RareFields::MoveInProgress>&);

    // shutdown safety
    bool mExecutingLocallogout = false;

    // local record of client's state for thread safety
    std::atomic<bool> mDownloadsPaused {false};
    std::atomic<bool> mUploadsPaused {false};
    std::atomic<bool> mTransferPauseFlagsChanged {false};

    // Responsible for tracking when to send sync/backup heartbeats
    unique_ptr<BackupMonitor> mHeartBeatMonitor;

    // Tracks the last recorded ignore file failure.
    IgnoreFileFailureContext mIgnoreFileFailureContext;

    // for clarifying and confirming which functions run on which threads
    std::thread::id syncThreadId;

    // declared last; would be auto-destructed first.
    std::thread syncThread;


    // structs and classes that are private to the thread, and need access to some internals that should not be generally public
    friend struct LocalNode;
    friend class Sync;
    friend struct SyncPath;
    friend struct UnifiedSync;
    friend class BackupInfoSync;
    friend class BackupMonitor;
    friend struct ProgressingMonitor;

    // Helps guide the engine's activities.
    mutable SyncControllerWeakPtr mSyncController;

    // Serializes access to mSyncController.
    mutable std::mutex mSyncControllerLock;

    // Convenience helper.
    template<typename... Arguments, typename... Parameters>
    bool defer(bool (SyncController::*predicate)(Parameters...) const,
               Arguments&&... arguments) const;

    // How does the engine know when an immediate stall has been reported?
    HasImmediateStallPredicate mHasImmediateStall;

    // How does the engine know that a specific stall is immediate?
    IsImmediateStallPredicate mIsImmediateStall;

    // Serializes access to *ImmediateStall predicates.
    mutable std::mutex mImmediateStallLock;

    // Check whether any immediate stalls have been reported.
    bool hasImmediateStall(const SyncStallInfo& stalls) const;

    // Check whether a specified stall is "immediate."
    bool isImmediateStall(const SyncStallEntry& entry) const;

    // What fingerprints does the engine know about?
    fsfp_tracker_t mFingerprintTracker;

public:
    // Should we defer sending a putnodes for the specified file?
    bool deferPutnode(const LocalPath& path) const;

    // Should we defer the completion of a putnodes sent for the specified file?
    bool deferPutnodeCompletion(const LocalPath& path) const;

    // Should we defer uploading of the specified file?
    bool deferUpload(const LocalPath& path) const;

    // Check if the engine is being controlled by a sync controller.
    //
    // Pretty much the same as the syncController() function below but
    // better expresses intent.
    bool hasSyncController() const;

    // Specify how the engine should determine whether there are any immediate stalls.
    void setHasImmediateStall(HasImmediateStallPredicate predicate);

    // Specify how the engine can determine whether a given stall is "immediate."
    void setIsImmediateStall(IsImmediateStallPredicate predicate);

    // Specify a controller who should guide the engine's activities.
    void setSyncController(SyncControllerPtr controller);

    // Retrieve the engine's current controller.
    SyncControllerPtr syncController() const;

    bool isSyncStalled(handle backupId) const;
};

class OverlayIconCachedPaths
{
    // This class is to help with reporting the status of synced files to the OS filesystem shell app
    // We can't always immediately look up the status instantly, due to mutex lock waits
    // and we don't want to stall the OS shell and make it wait.
    // So,
    // - remember the last (say) 512 paths the shell wanted to know about, and keep those up to date as we notify about them so we can reply instantly for those
    // - remember the last (say) 512 paths that we notified anyway, so that when the OS shell comes back to ask what the status of that notified path is, we can reply instantly
    // 512 should be enough as it would be tough to have that many files on screen at any one time

    typedef map<LocalPath, int> Map;
    Map paths;
    deque<Map::iterator> recentOrder;
    size_t sizeLimit = 512;
    mutex mMutex;
public:
    void addOrUpdate(const LocalPath& lp, int value)
    {
        lock_guard<mutex> g(mMutex);
        auto it_bool = paths.insert(Map::value_type(lp, value));
        if (it_bool.second)
        {
            recentOrder.push_back(it_bool.first);
        }
        else
        {
            it_bool.first->second = value;
        }
        if (recentOrder.size() > sizeLimit)
        {
            paths.erase(recentOrder.front());
            recentOrder.pop_front();
        }
    }
    void overwriteExisting(const LocalPath& lp, int value)
    {
        lock_guard<mutex> g(mMutex);
        auto it = paths.find(lp);
        if (it != paths.end())
        {
            it->second = value;
        }
    }
    bool lookup(const LocalPath& lp, int& value)
    {
        lock_guard<mutex> g(mMutex);
        auto it = paths.find(lp);
        if (it == paths.end()) return false;
        value = it->second;
        return true;
    }
    void clear()
    {
        lock_guard<mutex> g(mMutex);
        recentOrder.clear();
        paths.clear();
    }
};


} // namespace

#endif
#endif
