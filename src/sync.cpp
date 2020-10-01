/**
 * @file sync.cpp
 * @brief Class for synchronizing local and remote trees
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
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

#include <type_traits>
#include <unordered_set>

#include "mega.h"

#ifdef ENABLE_SYNC
#include "mega/sync.h"
#include "mega/megaapp.h"
#include "mega/transfer.h"
#include "mega/megaclient.h"
#include "mega/base64.h"

namespace mega {

const int Sync::SCANNING_DELAY_DS = 5;
const int Sync::EXTRA_SCANNING_DELAY_DS = 150;
const int Sync::FILE_UPDATE_DELAY_DS = 30;
const int Sync::FILE_UPDATE_MAX_DELAY_SECS = 60;
const dstime Sync::RECENT_VERSION_INTERVAL_SECS = 10800;
Node * const Sync::NAME_CONFLICT = reinterpret_cast<Node*>(~0ull);

namespace {

// Need this to store `LightFileFingerprint` by-value in `FingerprintSet`
struct LightFileFingerprintComparator
{
    bool operator()(const LightFileFingerprint& lhs, const LightFileFingerprint& rhs) const
    {
        return LightFileFingerprintCmp{}(&lhs, &rhs);
    }
};

// Represents a file/folder for use in assigning fs IDs
struct FsFile
{
    handle fsid;
    LocalPath path;
};

// Caches fingerprints
class FingerprintCache
{
public:
    using FingerprintSet = std::set<LightFileFingerprint, LightFileFingerprintComparator>;

    // Adds a new fingerprint
    template<typename T, typename = typename std::enable_if<std::is_same<LightFileFingerprint, typename std::decay<T>::type>::value>::type>
    const LightFileFingerprint* add(T&& ffp)
    {
         const auto insertPair = mFingerprints.insert(std::forward<T>(ffp));
         return &*insertPair.first;
    }

    // Returns the set of all fingerprints
    const FingerprintSet& all() const
    {
        return mFingerprints;
    }

private:
    FingerprintSet mFingerprints;
};

using FingerprintLocalNodeMap = std::multimap<const LightFileFingerprint*, LocalNode*, LightFileFingerprintCmp>;
using FingerprintFileMap = std::multimap<const LightFileFingerprint*, FsFile, LightFileFingerprintCmp>;

// Collects all syncable filesystem paths in the given folder under `localpath`
set<LocalPath> collectAllPathsInFolder(Sync& sync, MegaApp& app, FileSystemAccess& fsaccess, LocalPath& localpath,
                                    LocalPath& localdebris)
{
    auto fa = fsaccess.newfileaccess(false);
    if (!fa->fopen(localpath, true, false))
    {
        LOG_err << "Unable to open path: " << localpath.toPath(fsaccess);
        return {};
    }
    if (fa->mIsSymLink)
    {
        LOG_debug << "Ignoring symlink: " << localpath.toPath(fsaccess);
        return {};
    }
    assert(fa->type == FOLDERNODE);

    auto da = std::unique_ptr<DirAccess>{fsaccess.newdiraccess()};
    if (!da->dopen(&localpath, fa.get(), false))
    {
        LOG_err << "Unable to open directory: " << localpath.toPath(fsaccess);
        return {};
    }

    set<LocalPath> paths; // has to be a std::set to enforce same sorting as `children` of `LocalNode`

    LocalPath localname;
    while (da->dnext(localpath, localname, false))
    {
        ScopedLengthRestore restoreLength(localpath);
        localpath.appendWithSeparator(localname, false, fsaccess.localseparator);

        // check if this record is to be ignored
        const auto name = localname.toName(fsaccess);
        if (app.sync_syncable(&sync, name.c_str(), localpath))
        {
            // skip the sync's debris folder
            if (!localdebris.isContainingPathOf(localpath, fsaccess.localseparator))
            {
                paths.insert(localpath);
            }
        }
    }

    return paths;
}

// Combines another fingerprint into `ffp`
void hashCombineFingerprint(LightFileFingerprint& ffp, const LightFileFingerprint& other)
{
    hashCombine(ffp.size, other.size);
    hashCombine(ffp.mtime, other.mtime);
}

// Combines the fingerprints of all file nodes in the given map
bool combinedFingerprint(LightFileFingerprint& ffp, const localnode_map& nodeMap)
{
    bool success = false;
    for (const auto& nodePair : nodeMap)
    {
        const LocalNode& l = *nodePair.second;
        if (l.type == FILENODE)
        {
            LightFileFingerprint lFfp;
            lFfp.genfingerprint(l.size, l.mtime);
            hashCombineFingerprint(ffp, lFfp);
            success = true;
        }
    }
    return success;
}

// Combines the fingerprints of all files in the given paths
bool combinedFingerprint(LightFileFingerprint& ffp, FileSystemAccess& fsaccess, const set<LocalPath>& paths)
{
    bool success = false;
    for (auto& path : paths)
    {
        auto fa = fsaccess.newfileaccess(false);
        auto pathArg = path; // todo: sort out const
        if (!fa->fopen(pathArg, true, false))
        {
            LOG_err << "Unable to open path: " << path.toPath(fsaccess);
            success = false;
            break;
        }
        if (fa->mIsSymLink)
        {
            LOG_debug << "Ignoring symlink: " << path.toPath(fsaccess);
            continue;
        }
        if (fa->type == FILENODE)
        {
            LightFileFingerprint faFfp;
            faFfp.genfingerprint(fa->size, fa->mtime);
            hashCombineFingerprint(ffp, faFfp);
            success = true;
        }
    }
    return success;
}

// Computes the fingerprint of the given `l` (file or folder) and stores it in `ffp`
bool computeFingerprint(LightFileFingerprint& ffp, const LocalNode& l)
{
    if (l.type == FILENODE)
    {
        ffp.genfingerprint(l.size, l.mtime);
        return true;
    }
    else if (l.type == FOLDERNODE)
    {
        return combinedFingerprint(ffp, l.children);
    }
    else
    {
        assert(false && "Invalid node type");
        return false;
    }
}

// Computes the fingerprint of the given `fa` (file or folder) and stores it in `ffp`
bool computeFingerprint(LightFileFingerprint& ffp, FileSystemAccess& fsaccess,
                        FileAccess& fa, LocalPath& path, const set<LocalPath>& paths)
{
    if (fa.type == FILENODE)
    {
        assert(paths.empty());
        ffp.genfingerprint(fa.size, fa.mtime);
        return true;
    }
    else if (fa.type == FOLDERNODE)
    {
        return combinedFingerprint(ffp, fsaccess, paths);
    }
    else
    {
        assert(false && "Invalid node type");
        return false;
    }
}

// Collects all `LocalNode`s by storing them in `localnodes`, keyed by LightFileFingerprint.
// Invalidates the fs IDs of all local nodes.
// Stores all fingerprints in `fingerprints` for later reference.
void collectAllLocalNodes(FingerprintCache& fingerprints, FingerprintLocalNodeMap& localnodes,
                          LocalNode& l, fsid_localnode_map& fsidnodes)
{
    // invalidate fsid of `l`
    l.fsid = mega::UNDEF;
    if (l.fsid_it != fsidnodes.end())
    {
        fsidnodes.erase(l.fsid_it);
        l.fsid_it = fsidnodes.end();
    }
    // collect fingerprint
    LightFileFingerprint ffp;
    if (computeFingerprint(ffp, l))
    {
        const auto ffpPtr = fingerprints.add(std::move(ffp));
        localnodes.insert(std::make_pair(ffpPtr, &l));
    }
    if (l.type == FILENODE)
    {
        return;
    }
    for (auto& childPair : l.children)
    {
        collectAllLocalNodes(fingerprints, localnodes, *childPair.second, fsidnodes);
    }
}

// Collects all `File`s by storing them in `files`, keyed by FileFingerprint.
// Stores all fingerprints in `fingerprints` for later reference.
void collectAllFiles(bool& success, FingerprintCache& fingerprints, FingerprintFileMap& files,
                     Sync& sync, MegaApp& app, FileSystemAccess& fsaccess, LocalPath& localpath,
                     LocalPath& localdebris)
{
    auto insertFingerprint = [&files, &fingerprints](FileSystemAccess& fsaccess, FileAccess& fa,
                                                     LocalPath& path, const set<LocalPath>& paths)
    {
        LightFileFingerprint ffp;
        if (computeFingerprint(ffp, fsaccess, fa, path, paths))
        {
            const auto ffpPtr = fingerprints.add(std::move(ffp));
            files.insert(std::make_pair(ffpPtr, FsFile{fa.fsid, path}));
        }
    };

    auto fa = fsaccess.newfileaccess(false);
    if (!fa->fopen(localpath, true, false))
    {
        LOG_err << "Unable to open path: " << localpath.toPath(fsaccess);
        success = false;
        return;
    }
    if (fa->mIsSymLink)
    {
        LOG_debug << "Ignoring symlink: " << localpath.toPath(fsaccess);
        return;
    }
    if (!fa->fsidvalid)
    {
        LOG_err << "Invalid fs id for: " << localpath.toPath(fsaccess);
        success = false;
        return;
    }

    if (fa->type == FILENODE)
    {
        insertFingerprint(fsaccess, *fa, localpath, {});
    }
    else if (fa->type == FOLDERNODE)
    {
        const auto paths = collectAllPathsInFolder(sync, app, fsaccess, localpath, localdebris);
        insertFingerprint(fsaccess, *fa, localpath, paths);
        fa.reset();
        for (const auto& path : paths)
        {
            LocalPath tmpPath = path;
            collectAllFiles(success, fingerprints, files, sync, app, fsaccess, tmpPath, localdebris);
        }
    }
    else
    {
        assert(false && "Invalid file type");
        success = false;
        return;
    }
}

// Assigns fs IDs from `files` to those `localnodes` that match the fingerprints found in `files`.
// If there are multiple matches we apply a best-path heuristic.
size_t assignFilesystemIdsImpl(const FingerprintCache& fingerprints, FingerprintLocalNodeMap& localnodes,
                               FingerprintFileMap& files, fsid_localnode_map& fsidnodes, FileSystemAccess& fsaccess)
{
    LocalPath nodePath;
    size_t assignmentCount = 0;
    for (const auto& fp : fingerprints.all())
    {
        const auto nodeRange = localnodes.equal_range(&fp);
        const auto nodeCount = std::distance(nodeRange.first, nodeRange.second);
        if (nodeCount <= 0)
        {
            continue;
        }

        const auto fileRange = files.equal_range(&fp);
        const auto fileCount = std::distance(fileRange.first, fileRange.second);
        if (fileCount <= 0)
        {
            // without files we cannot assign fs IDs to these localnodes, so no need to keep them
            localnodes.erase(nodeRange.first, nodeRange.second);
            continue;
        }

        struct Element
        {
            int score;
            handle fsid;
            LocalNode* l;
        };
        std::vector<Element> elements;
        elements.reserve(nodeCount * fileCount);

        for (auto nodeIt = nodeRange.first; nodeIt != nodeRange.second; ++nodeIt)
        {
            auto l = nodeIt->second;
            if (l != l->sync->localroot.get()) // never assign fs ID to the root localnode
            {
                nodePath = l->getLocalPath(false);
                for (auto fileIt = fileRange.first; fileIt != fileRange.second; ++fileIt)
                {
                    auto& filePath = fileIt->second.path;
                    const auto score = computeReversePathMatchScore(nodePath, filePath, fsaccess);
                    if (score > 0) // leaf name must match
                    {
                        elements.push_back({score, fileIt->second.fsid, l});
                    }
                }
            }
        }

        // Sort in descending order by score. Elements with highest score come first
        std::sort(elements.begin(), elements.end(), [](const Element& e1, const Element& e2)
                                                    {
                                                        return e1.score > e2.score;
                                                    });

        std::unordered_set<handle> usedFsIds;
        for (const auto& e : elements)
        {
            if (e.l->fsid == mega::UNDEF // node not assigned
                && usedFsIds.find(e.fsid) == usedFsIds.end()) // fsid not used
            {
                e.l->setfsid(e.fsid, fsidnodes);
                usedFsIds.insert(e.fsid);
                ++assignmentCount;
            }
        }

        // the fingerprint that these files and localnodes correspond to has now finished processing
        files.erase(fileRange.first, fileRange.second);
        localnodes.erase(nodeRange.first, nodeRange.second);
    }
    return assignmentCount;
}

} // anonymous

int computeReversePathMatchScore(const LocalPath& path1, const LocalPath& path2, const FileSystemAccess& fsaccess)
{
    if (path1.empty() || path2.empty())
    {
        return 0;
    }

    const auto path1End = path1.localpath.size() - 1;
    const auto path2End = path2.localpath.size() - 1;

    size_t index = 0;
    size_t separatorBias = 0;
    LocalPath accumulated;
    while (index <= path1End && index <= path2End)
    {
        const auto value1 = path1.localpath[path1End - index];
        const auto value2 = path2.localpath[path2End - index];
        if (value1 != value2)
        {
            break;
        }
        accumulated.localpath.push_back(value1);

        ++index;

        if (!accumulated.localpath.empty())
        {
            if (accumulated.localpath.back() == fsaccess.localseparator)
            {
                ++separatorBias;
                accumulated.clear();
            }
        }
    }

    if (index > path1End && index > path2End) // we got to the beginning of both paths (full score)
    {
        return static_cast<int>(index - separatorBias);
    }
    else // the paths only partly match
    {
        return static_cast<int>(index - separatorBias - accumulated.localpath.size());
    }
}

bool assignFilesystemIds(Sync& sync, MegaApp& app, FileSystemAccess& fsaccess, fsid_localnode_map& fsidnodes,
                         LocalPath& localdebris)
{
    auto& rootpath = sync.localroot->localname;
    LOG_info << "Assigning fs IDs at rootpath: " << rootpath.toPath(fsaccess);

    auto fa = fsaccess.newfileaccess(false);
    if (!fa->fopen(rootpath, true, false))
    {
        LOG_err << "Unable to open rootpath";
        return false;
    }
    if (fa->type != FOLDERNODE)
    {
        LOG_err << "rootpath not a folder";
        assert(false);
        return false;
    }
    if (fa->mIsSymLink)
    {
        LOG_err << "rootpath is a symlink";
        assert(false);
        return false;
    }
    fa.reset();

    bool success = true;

    FingerprintCache fingerprints;

    FingerprintLocalNodeMap localnodes;
    collectAllLocalNodes(fingerprints, localnodes, *sync.localroot, fsidnodes);
    LOG_info << "Number of localnodes: " << localnodes.size();

    if (localnodes.empty())
    {
        return success;
    }

    FingerprintFileMap files;
    collectAllFiles(success, fingerprints, files, sync, app, fsaccess, rootpath, localdebris);
    LOG_info << "Number of files: " << files.size();

    LOG_info << "Number of fingerprints: " << fingerprints.all().size();
    const auto assignmentCount = assignFilesystemIdsImpl(fingerprints, localnodes, files, fsidnodes, fsaccess);
    LOG_info << "Number of fsid assignments: " << assignmentCount;

    return success;
}

std::atomic<size_t> ScanService::mNumServices(0);
std::unique_ptr<ScanService::Worker> ScanService::mWorker;
std::mutex ScanService::mWorkerLock;

ScanService::ScanService(Waiter& waiter)
  : mCookie(std::make_shared<Cookie>(waiter))
{
    // Locking here, rather than in the if statement, ensures that the
    // worker is fully constructed when control leaves the constructor.
    std::lock_guard<std::mutex> lock(mWorkerLock);

    if (++mNumServices == 1)
    {
        mWorker.reset(new Worker());
    }
}

ScanService::~ScanService()
{
    if (--mNumServices == 0)
    {
        std::lock_guard<std::mutex> lock(mWorkerLock);
        mWorker.reset();
    }
}

auto ScanService::scan(const LocalNode& target, LocalPath targetPath) -> RequestPtr
{
    // For convenience.
    const auto& debris = target.sync->localdebris;
    const auto& separator = target.sync->client->fsaccess->localseparator;
 
    // Create a request to represent the scan.
    auto request = std::make_shared<ScanRequest>(mCookie, target, targetPath);

    // Have we been asked to scan the debris?
    request->mComplete = debris.isContainingPathOf(targetPath, separator);

    // Don't bother scanning the debris.
    if (!request->mComplete)
    {
        LOG_debug << "Queuing scan for: "
                  << targetPath.toPath(*target.sync->client->fsaccess);

        // Queue request for processing.
        mWorker->queue(request);
    }

    return request;
}

auto ScanService::scan(const LocalNode& target) -> RequestPtr
{
    return scan(target, target.getLocalPath(true));
}

ScanService::ScanRequest::ScanRequest(const std::shared_ptr<Cookie>& cookie,
                                      const LocalNode& target,
                                      LocalPath targetPath)
  : mCookie(cookie)
  , mComplete(false)
  , mDebrisPath(target.sync->localdebris)
  , mFollowSymLinks(target.sync->client->followsymlinks)
  , mResults()
  , mTarget(target)
  , mTargetPath(std::move(targetPath))
{
}

ScanService::Worker::Worker(size_t numThreads)
  : mFsAccess(new FSACCESS_CLASS())
  , mPending()
  , mPendingLock()
  , mPendingNotifier()
  , mThreads()
{
    // Always at least one thread.
    assert(numThreads > 0);

    LOG_debug << "Starting ScanService worker...";

    // Start the threads.
    while (numThreads--)
    {
        try
        {
            mThreads.emplace_back([this]() { loop(); });
        }
        catch (std::system_error& e)
        {
            LOG_err << "Failed to start worker thread: " << e.what();
        }
    }

    LOG_debug << mThreads.size() << " worker thread(s) started.";
    LOG_debug << "ScanService worker started.";
}

ScanService::Worker::~Worker()
{
    LOG_debug << "Stopping ScanService worker...";

    // Queue the 'terminate' sentinel.
    {
        std::unique_lock<std::mutex> lock(mPendingLock);
        mPending.emplace_back();
    }

    // Wake any sleeping threads.
    mPendingNotifier.notify_all();

    LOG_debug << "Waiting for worker thread(s) to terminate...";

    // Wait for the threads to terminate.
    for (auto& thread : mThreads)
    {
        thread.join();
    }

    LOG_debug << "ScanService worker stopped.";
}

void ScanService::Worker::queue(ScanRequestPtr request)
{
    // Queue the request.
    {
        std::unique_lock<std::mutex> lock(mPendingLock);
        mPending.emplace_back(std::move(request));
    }
    
    // Tell the lucky thread it has something to do.
    mPendingNotifier.notify_one();
}

void ScanService::Worker::loop()
{
    // We're ready when we have some work to do.
    auto ready = [this]() { return mPending.size(); };

    for ( ; ; )
    {
        ScanRequestPtr request;

        {
            // Wait for something to do.
            std::unique_lock<std::mutex> lock(mPendingLock);
            mPendingNotifier.wait(lock, ready);

            // Are we being told to terminate?
            if (!mPending.front())
            {
                // Bail, don't deque the sentinel.
                return;
            }
            
            request = std::move(mPending.front());
            mPending.pop_front();
        }

        const auto targetPath =
          request->mTargetPath.toPath(*mFsAccess);

        LOG_debug << "Scanning directory: " << targetPath;

        // Process the request.
        scan(request);

        // Mark the request as complete.
        request->mComplete = true;

        LOG_debug << "Scan complete for: " << targetPath;

        // Do we still have someone to notify?
        auto cookie = request->mCookie.lock();

        if (cookie)
        {
            LOG_debug << "Letting the waiter know it has "
                      << request->mResults.size()
                      << " scan result(s).";

            // Yep, let them know the request is complete.
            cookie->completed();
        }
        else
        {
            LOG_debug << "No waiter, discarding "
                      << request->mResults.size()
                      << " scan result(s).";
        }
    }
}

FSNode ScanService::Worker::interrogate(DirAccess& iterator,
                                        const LocalPath& name,
                                        LocalPath& path)
{
    FSNode result;

    // Always record the name.
    result.localname = name;
    result.name = name.toName(*mFsAccess);

    // Can we open the file?
    auto fileAccess = mFsAccess->newfileaccess(false);

    if (fileAccess->fopen(path, true, false, &iterator))
    {
        // Populate result.
        result.fsid = 0;
        result.isSymlink = fileAccess->mIsSymLink;
        result.mtime = fileAccess->mtime;
        result.size = fileAccess->size;
        result.shortname = mFsAccess->fsShortname(path);
        result.type = fileAccess->type;

        // Record filesystem ID if it's valid.
        if (fileAccess->fsidvalid)
        {
            result.fsid = fileAccess->fsid;
        }

        // Warn about symlinks.
        if (result.isSymlink)
        {
            LOG_debug << "Interrogated path is a symlink: "
                      << path.toPath(*mFsAccess);
        }

        // Fingerprint files.
        if (result.type == FILENODE)
        {
            result.fingerprint.genfingerprint(fileAccess.get());
        }

        return result;
    }

    // Couldn't open the file.
    LOG_warn << "Error opening file: " << path.toPath(*mFsAccess);

    // File's blocked if the error is transient.
    result.isBlocked = fileAccess->retry;

    // Warn about the blocked file.
    if (result.isBlocked)
    {
        LOG_warn << "File blocked: " << path.toPath(*mFsAccess);
    }

    return result;
}

void ScanService::Worker::scan(ScanRequestPtr request)
{
    // For convenience.
    const auto& debris = request->mDebrisPath;
    const auto& separator = mFsAccess->localseparator;

    // Don't bother processing the debris directory.
    if (debris.isContainingPathOf(request->mTargetPath, separator))
    {
        LOG_debug << "Skipping scan of debris directory.";
        return;
    }

    // Have we been passed a valid target path?
    auto fileAccess = mFsAccess->newfileaccess();
    auto path = request->mTargetPath;

    if (!fileAccess->fopen(path, true, false))
    {
        LOG_debug << "Scan target does not exist: "
                  << path.toPath(*mFsAccess);
        return;
    }

    // Does the path denote a directory?
    if (fileAccess->type != FOLDERNODE)
    {
        LOG_debug << "Scan target is not a directory: "
                  << path.toPath(*mFsAccess);
        return;
    }

    std::unique_ptr<DirAccess> dirAccess(mFsAccess->newdiraccess());
    LocalPath name;

    // Can we open the directory?
    if (!dirAccess->dopen(&path, fileAccess.get(), false))
    {
        LOG_debug << "Unable to iterate scan target: "
                  << path.toPath(*mFsAccess);
        return;
    }

    // Process each file in the target.
    std::vector<FSNode> results;

    while (dirAccess->dnext(path, name, request->mFollowSymLinks))
    {
        ScopedLengthRestore restorer(path);
        path.appendWithSeparator(name, false, separator);

        // Except the debris...
        if (debris.isContainingPathOf(path, separator))
        {
            continue;
        }

        // Learn everything we can about the file.
        auto info = interrogate(*dirAccess, name, path);
        results.emplace_back(std::move(info));
    }

    // Publish the results.
    request->mResults = std::move(results);
}

SyncConfigBag::SyncConfigBag(DbAccess& dbaccess, FileSystemAccess& fsaccess, PrnGen& rng, const std::string& id)
{
    std::string dbname = "syncconfigsv2_" + id;
    mTable.reset(dbaccess.open(rng, &fsaccess, &dbname, false, false));
    if (!mTable)
    {
        LOG_err << "Unable to open DB table: " << dbname;
        assert(false);
        return;
    }

    mTable->rewind();

    uint32_t tableId;
    std::string data;
    while (mTable->next(&tableId, &data))
    {
        auto syncConfig = SyncConfig::unserialize(data);
        if (!syncConfig)
        {
            LOG_err << "Unable to unserialize sync config at id: " << tableId;
            assert(false);
            continue;
        }
        syncConfig->dbid = tableId;

        mSyncConfigs.insert(std::make_pair(syncConfig->getTag(), *syncConfig));
        if (tableId > mTable->nextid)
        {
            mTable->nextid = tableId;
        }
    }
    ++mTable->nextid;
}

void SyncConfigBag::insert(const SyncConfig& syncConfig)
{
    auto insertOrUpdate = [this](const uint32_t id, const SyncConfig& syncConfig)
    {
        std::string data;
        const_cast<SyncConfig&>(syncConfig).serialize(&data);
        DBTableTransactionCommitter committer{mTable.get()};
        if (!mTable->put(id, &data)) // put either inserts or updates
        {
            LOG_err << "Incomplete database put at id: " << mTable->nextid;
            assert(false);
            mTable->abort();
            return false;
        }
        return true;
    };

    map<int, SyncConfig>::iterator syncConfigIt = mSyncConfigs.find(syncConfig.getTag());
    if (syncConfigIt == mSyncConfigs.end()) // syncConfig is new
    {
        if (mTable)
        {
            if (!insertOrUpdate(mTable->nextid, syncConfig))
            {
                return;
            }
        }
        auto insertPair = mSyncConfigs.insert(std::make_pair(syncConfig.getTag(), syncConfig));
        if (mTable)
        {
            insertPair.first->second.dbid = mTable->nextid;
            ++mTable->nextid;
        }
    }
    else // syncConfig exists already
    {
        const uint32_t tableId = syncConfigIt->second.dbid;
        if (mTable)
        {
            if (!insertOrUpdate(tableId, syncConfig))
            {
                return;
            }
        }
        syncConfigIt->second = syncConfig;
        syncConfigIt->second.dbid = tableId;
    }
}

bool SyncConfigBag::removeByTag(const int tag)
{
    auto syncConfigPair = mSyncConfigs.find(tag);
    if (syncConfigPair != mSyncConfigs.end())
    {
        if (mTable)
        {
            DBTableTransactionCommitter committer{mTable.get()};
            if (!mTable->del(syncConfigPair->second.dbid))
            {
                LOG_err << "Incomplete database del at id: " << syncConfigPair->second.dbid;
                assert(false);
                mTable->abort();
            }
        }
        mSyncConfigs.erase(syncConfigPair);
        return true;
    }
    return false;
}

const SyncConfig* SyncConfigBag::get(const int tag) const
{
    auto syncConfigPair = mSyncConfigs.find(tag);
    if (syncConfigPair != mSyncConfigs.end())
    {
        return &syncConfigPair->second;
    }
    return nullptr;
}


const SyncConfig* SyncConfigBag::getByNodeHandle(handle nodeHandle) const
{
    for (const auto& syncConfigPair : mSyncConfigs)
    {
        if (syncConfigPair.second.getRemoteNode() == nodeHandle)
            return &syncConfigPair.second;
    }
    return nullptr;
}

void SyncConfigBag::clear()
{
    if (mTable)
    {
        mTable->truncate();
        mTable->nextid = 0;
    }
    mSyncConfigs.clear();
}

std::vector<SyncConfig> SyncConfigBag::all() const
{
    std::vector<SyncConfig> syncConfigs;
    for (const auto& syncConfigPair : mSyncConfigs)
    {
        syncConfigs.push_back(syncConfigPair.second);
    }
    return syncConfigs;
}

// new Syncs are automatically inserted into the session's syncs list
// and a full read of the subtree is initiated
Sync::Sync(MegaClient* cclient, SyncConfig &config, const char* cdebris,
           LocalPath* clocaldebris, Node* remotenode, bool cinshare, int ctag, void *cappdata)
: localroot(new LocalNode)
{
    isnetwork = false;
    client = cclient;
    tag = ctag;
    inshare = cinshare;
    appData = cappdata;
    errorCode = NO_SYNC_ERROR;
    tmpfa = NULL;
    //initializing = true;

    localnodes[FILENODE] = 0;
    localnodes[FOLDERNODE] = 0;

    state = SYNC_INITIALSCAN;
    statecachetable = NULL;

    fullscan = true;
    scanseqno = 0;

    mLocalPath = config.getLocalPath();
    LocalPath crootpath = LocalPath::fromPath(mLocalPath, *client->fsaccess);

    if (cdebris)
    {
        debris = cdebris;
        localdebris = LocalPath::fromPath(debris, *client->fsaccess);

        dirnotify.reset(client->fsaccess->newdirnotify(crootpath, localdebris, client->waiter));

        localdebris.prependWithSeparator(crootpath, client->fsaccess->localseparator);
    }
    else
    {
        localdebris = *clocaldebris;

        // FIXME: pass last segment of localdebris
        dirnotify.reset(client->fsaccess->newdirnotify(crootpath, localdebris, client->waiter));
    }
    dirnotify->sync = this;

    // set specified fsfp or get from fs if none
    const auto cfsfp = config.getLocalFingerprint();
    if (cfsfp)
    {
        fsfp = cfsfp;
    }
    else
    {
        fsfp = dirnotify->fsfingerprint();
        config.setLocalFingerprint(fsfp);
    }

    fsstableids = dirnotify->fsstableids();
    LOG_info << "Filesystem IDs are stable: " << fsstableids;

    mFilesystemType = client->fsaccess->getlocalfstype(crootpath);

    localroot->init(this, FOLDERNODE, NULL, crootpath, nullptr);  // the root node must have the absolute path.  We don't store shortname, to avoid accidentally using relative paths.
    localroot->setnode(remotenode);

#ifdef __APPLE__
    if (macOSmajorVersion() >= 19) //macOS catalina+
    {
        LOG_debug << "macOS 10.15+ filesystem detected. Checking fseventspath.";
        string supercrootpath = "/System/Volumes/Data" + crootpath.platformEncoded();

        int fd = open(supercrootpath.c_str(), O_RDONLY);
        if (fd == -1)
        {
            LOG_debug << "Unable to open path using fseventspath.";
            mFsEventsPath = crootpath.platformEncoded();
        }
        else
        {
            char buf[MAXPATHLEN];
            if (fcntl(fd, F_GETPATH, buf) < 0)
            {
                LOG_debug << "Using standard paths to detect filesystem notifications.";
                mFsEventsPath = crootpath.platformEncoded();
            }
            else
            {
                LOG_debug << "Using fsevents paths to detect filesystem notifications.";
                mFsEventsPath = supercrootpath;
            }
            close(fd);
        }
    }
#endif

    sync_it = client->syncs.insert(client->syncs.end(), this);

    if (client->dbaccess)
    {
        // open state cache table
        handle tableid[3];
        string dbname;

        auto fas = client->fsaccess->newfileaccess(false);

        if (fas->fopen(crootpath, true, false))
        {
            tableid[0] = fas->fsid;
            tableid[1] = remotenode->nodehandle;
            tableid[2] = client->me;

            dbname.resize(sizeof tableid * 4 / 3 + 3);
            dbname.resize(Base64::btoa((byte*)tableid, sizeof tableid, (char*)dbname.c_str()));

            statecachetable = client->dbaccess->open(client->rng, client->fsaccess, &dbname, false, false);

            readstatecache();
        }
    }
}

Sync::~Sync()
{
    // must be set to prevent remote mass deletion while rootlocal destructor runs
    assert(state == SYNC_CANCELED || state == SYNC_FAILED || state == SYNC_DISABLED);
    mDestructorRunning = true;

    // unlock tmp lock
    tmpfa.reset();

    // stop all active and pending downloads
    if (localroot->node)
    {
        TreeProcDelSyncGet tdsg;
        // Create a committer to ensure we update the transfer database in an efficient single commit,
        // if there are transactions in progress.
        DBTableTransactionCommitter committer(client->tctable);
        client->proctree(localroot->node, &tdsg);
    }

    delete statecachetable;

    client->syncs.erase(sync_it);
    client->syncactivity = true;

    {
        // Create a committer and recursively delete all the associated LocalNodes, and their associated transfer and file objects.
        // If any have transactions in progress, the committer will ensure we update the transfer database in an efficient single commit.
        DBTableTransactionCommitter committer(client->tctable);
        localroot.reset();
    }
}

void Sync::addstatecachechildren(uint32_t parent_dbid, idlocalnode_map* tmap, LocalPath& localpath, LocalNode *p, int maxdepth)
{
    auto range = tmap->equal_range(parent_dbid);

    for (auto it = range.first; it != range.second; it++)
    {
        ScopedLengthRestore restoreLen(localpath);

        localpath.appendWithSeparator(it->second->localname, true, client->fsaccess->localseparator);

        LocalNode* l = it->second;
        Node* node = l->node;
        handle fsid = l->fsid;
        m_off_t size = l->size;

        // clear localname to force newnode = true in setnameparent
        l->localname.clear();

        // if we already have the shortname from database, use that, otherwise (db is from old code) look it up
        std::unique_ptr<LocalPath> shortname;
        if (l->slocalname_in_db)
        {
            // null if there is no shortname, or the shortname matches the localname.
            shortname.reset(l->slocalname.release());
        }
        else
        {
            shortname = client->fsaccess->fsShortname(localpath);
        }

        l->init(this, l->type, p, localpath, std::move(shortname));

#ifdef DEBUG
        if (fsid != UNDEF)
        {
            auto fa = client->fsaccess->newfileaccess(false);
            if (fa->fopen(localpath))  // exists, is file
            {
                auto sn = client->fsaccess->fsShortname(localpath);
                if (!(!l->localname.empty() &&
                    ((!l->slocalname && (!sn || l->localname == *sn)) ||
                    (l->slocalname && sn && !l->slocalname->empty() && *l->slocalname != l->localname && *l->slocalname == *sn))))
                {
                    // This can happen if a file was moved elsewhere and moved back before the sync restarts.
                    // We'll refresh slocalname while scanning.
                    LOG_warn << "Shortname mismatch on LocalNode load!" <<
                        " Was: " << (l->slocalname ? l->slocalname->toPath(*client->fsaccess) : "(null") <<
                        " Now: " << (sn ? sn->toPath(*client->fsaccess) : "(null") <<
                        " at " << localpath.toPath(*client->fsaccess);
                }
            }
        }
#endif

        l->parent_dbid = parent_dbid;
        l->size = size;
        l->setfsid(fsid, client->localnodeByFsid);
        l->setnode(node);

        if (!l->slocalname_in_db)
        {
            statecacheadd(l);
            if (insertq.size() > 50000)
            {
                cachenodes();  // periodically output updated nodes with shortname updates, so people who restart megasync still make progress towards a fast startup
            }
        }

        if (maxdepth)
        {
            addstatecachechildren(l->dbid, tmap, localpath, l, maxdepth - 1);
        }
    }
}

bool Sync::readstatecache()
{
    if (statecachetable && state == SYNC_INITIALSCAN)
    {
        string cachedata;
        idlocalnode_map tmap;
        uint32_t cid;
        LocalNode* l;

        statecachetable->rewind();

        // bulk-load cached nodes into tmap
        while (statecachetable->next(&cid, &cachedata, &client->key))
        {
            if ((l = LocalNode::unserialize(this, &cachedata)))
            {
                l->dbid = cid;
                tmap.insert(pair<int32_t,LocalNode*>(l->parent_dbid,l));
            }
        }

        // recursively build LocalNode tree, set scanseqnos to sync's current scanseqno
        addstatecachechildren(0, &tmap, localroot->localname, localroot.get(), 100);
        cachenodes();

        // trigger a single-pass full scan to identify deleted nodes
        fullscan = true;
        scanseqno++;

        return true;
    }

    return false;
}

const SyncConfig& Sync::getConfig() const
{
    assert(client->syncConfigs && "Calling getConfig() requires sync configs");
    const auto config = client->syncConfigs->get(tag);
    assert(config);
    return *config;
}

// remove LocalNode from DB cache
void Sync::statecachedel(LocalNode* l)
{
    if (state == SYNC_CANCELED)
    {
        return;
    }

    insertq.erase(l);

    if (l->dbid)
    {
        deleteq.insert(l->dbid);
    }
}

// insert LocalNode into DB cache
void Sync::statecacheadd(LocalNode* l)
{
    if (state == SYNC_CANCELED)
    {
        return;
    }

    if (l->dbid)
    {
        deleteq.erase(l->dbid);
    }

    insertq.insert(l);
}

void Sync::cachenodes()
{
    if (statecachetable && (state == SYNC_ACTIVE || (state == SYNC_INITIALSCAN /*&& insertq.size() > 100*/)) && (deleteq.size() || insertq.size()))
    {
        LOG_debug << "Saving LocalNode database with " << insertq.size() << " additions and " << deleteq.size() << " deletions";
        statecachetable->begin();

        // deletions
        for (set<uint32_t>::iterator it = deleteq.begin(); it != deleteq.end(); it++)
        {
            statecachetable->del(*it);
        }

        deleteq.clear();

        // additions - we iterate until completion or until we get stuck
        bool added;

        do {
            added = false;

            for (set<LocalNode*>::iterator it = insertq.begin(); it != insertq.end(); )
            {
                if ((*it)->parent->dbid || (*it)->parent == localroot.get())
                {
                    statecachetable->put(MegaClient::CACHEDLOCALNODE, *it, &client->key);
                    insertq.erase(it++);
                    added = true;
                }
                else it++;
            }
        } while (added);

        statecachetable->commit();

        if (insertq.size())
        {
            LOG_err << "LocalNode caching did not complete";
        }
    }
}

void Sync::changestate(syncstate_t newstate, SyncError newSyncError)
{
    if (newstate != state || newSyncError != errorCode)
    {
        LOG_debug << "Sync state/error changing. from " << state << "/" << errorCode << " to "  << newstate << "/" << newSyncError;
        if (newstate != SYNC_CANCELED)
        {
            client->changeSyncState(tag, newstate, newSyncError);
        }

        state = newstate;
        errorCode = newSyncError;
        fullscan = false;
    }
}

// walk path and return corresponding LocalNode and its parent
// path must be relative to l or start with the root prefix if l == NULL
// path must be a full sync path, i.e. start with localroot->localname
// NULL: no match, optionally returns residual path
LocalNode* Sync::localnodebypath(LocalNode* l, const LocalPath& localpath, LocalNode** parent, LocalPath* outpath)
{
    if (outpath)
    {
        assert(outpath->empty());
    }

    size_t subpathIndex = 0;

    if (!l)
    {
        // verify matching localroot prefix - this should always succeed for
        // internal use
        if (!localroot->localname.isContainingPathOf(localpath, client->fsaccess->localseparator, &subpathIndex))
        {
            if (parent)
            {
                *parent = NULL;
            }

            return NULL;
        }

        l = localroot.get();
    }


    LocalPath component;

    while (localpath.nextPathComponent(subpathIndex, component, client->fsaccess->localseparator))
    {
        if (parent)
        {
            *parent = l;
        }

        localnode_map::iterator it;
        if ((it = l->children.find(&component)) == l->children.end()
            && (it = l->schildren.find(&component)) == l->schildren.end())
        {
            // no full match: store residual path, return NULL with the
            // matching component LocalNode in parent
            if (outpath)
            {
                *outpath = std::move(component);
                auto remainder = localpath.subpathFrom(subpathIndex);
                if (!remainder.empty())
                {
                    outpath->appendWithSeparator(remainder, false, client->fsaccess->localseparator);
                }
            }

            return NULL;
        }

        l = it->second;
    }

    // full match: no residual path, return corresponding LocalNode
    if (outpath)
    {
        outpath->clear();
    }
    return l;
}

bool Sync::assignfsids()
{
    return assignFilesystemIds(*this, *client->app, *client->fsaccess, client->localnodeByFsid,
                               localdebris);
}

// scan localpath, add or update child nodes, just for this folder.  No recursion.
// localpath must be prefixed with Sync



auto Sync::scanOne(LocalNode& localNodeFolder, LocalPath& localPath) -> vector<FSNode>
{
    if (localdebris.isContainingPathOf(localPath, client->fsaccess->localseparator))
    {
        return {};
    }

    auto fa = client->fsaccess->newfileaccess();

    if (!fa->fopen(localPath, true, false))
    {
        // todo: error handling
        return {};
    }

    if (fa->type != FOLDERNODE)
    {
        // todo: error handling
        return {};
    }

    LOG_debug << "Scanning folder: " << localPath.toPath(*client->fsaccess);

    unique_ptr<DirAccess> da(client->fsaccess->newdiraccess());

    if (!da->dopen(&localPath, fa.get(), false))
    {
        // todo: error handling
        return {};
    }

    // scan the dir, mark all items with a unique identifier

    // todo: skip fingerprinting files if we already know it - name, size, mtime, fsid match

    LocalPath localname;
    vector<FSNode> results;
    while (da->dnext(localPath, localname, client->followsymlinks))
    {
        string name = localname.toName(*client->fsaccess);

        ScopedLengthRestore restoreLen(localPath);
        localPath.appendWithSeparator(localname, false, client->fsaccess->localseparator);

        // skip the sync's debris folder
        if (!localdebris.isContainingPathOf(localPath, client->fsaccess->localseparator))
        {
            results.push_back(checkpathOne(localPath, localname, da.get()));
        }
    }
    return results;
}

// new algorithm:  just make a LocalNode for this entry.  Caller will decide to keep it or not. No recursion

// todo: be more efficient later, use existing localnode from parent if they match, and don't re-fingerprint if name, mtime, fsid match.  Mange lifetimes - maybe shared_ptr


auto Sync::checkpathOne(LocalPath& localPath, const LocalPath& leafname, DirAccess* iteratingDir) -> FSNode
{
    // todo: skip fingerprinting files if we already know it - name, size, mtime, fsid match

    FSNode result;

    result.localname = leafname;
    result.name = leafname.toName(*client->fsaccess);

    // attempt to open/type this file
    auto fa = client->fsaccess->newfileaccess(false);

    if (fa->fopen(localPath, true, false, iteratingDir))
    {
        if (fa->mIsSymLink)
        {
            // todo: make nodes for symlinks, but never sync them (until we do that as a future project)
            LOG_debug << "checked path is a symlink: " << localPath.toPath(*client->fsaccess);
            result.isSymlink = true;
        }
        result.type = fa->type;
        result.shortname = client->fsaccess->fsShortname(localPath);
        result.fsid = fa->fsidvalid ? fa->fsid : 0;  // todo: do we need logic for the non-valid case?
        result.size = fa->size;
        result.mtime = fa->mtime;
        if (fa->type == FILENODE)
        {
            result.fingerprint.genfingerprint(fa.get());
        }
    }
    else
    {
        LOG_warn << "Error opening file: ";
        if (fa->retry)
        {
            // fopen() signals that the failure is potentially transient - do
            // nothing and request a recheck
            LOG_warn << "File blocked. Adding notification to the retry queue: " << localPath.toPath(*client->fsaccess);
            //dirnotify->notify(DirNotify::RETRY, ll, LocalPath(*localpathNew));

            result.isBlocked = true;

        }
    }

    return result;
}


/// todo:   things to figure out where to put them in new system:
///
// no fsid change detected or overwrite with unknown file:
//if (fa->mtime != l->mtime || fa->size != l->size)
//{
//    if (fa->fsidvalid && l->fsid != fa->fsid)
//    {
//        l->setfsid(fa->fsid, client->fsidnode);
//    }
//
//    m_off_t dsize = l->size > 0 ? l->size : 0;
//
//    l->genfingerprint(fa.get());
//
//    client->app->syncupdate_local_file_change(this, l, path.c_str());
//
//    DBTableTransactionCommitter committer(client->tctable);
//    client->stopxfer(l, &committer); // TODO:  can we use one committer for all the files in the folder?  Or for the whole recursion?
//    l->bumpnagleds();
//    l->deleted = false;
//
//    client->syncactivity = true;
//
//    statecacheadd(l);
//
//    fa.reset();
//
//    if (isnetwork && l->type == FILENODE)
//    {
//        LOG_debug << "Queueing extra fs notification for modified file";
//        dirnotify->notify(DirNotify::EXTRA, NULL, LocalPath(*localpathNew));
//    }
//    return l;
//}
//    }
//    else
//    {
//    // (we tolerate overwritten folders, because we do a
//    // content scan anyway)
//    if (fa->fsidvalid && fa->fsid != l->fsid)
//    {
//        l->setfsid(fa->fsid, client->fsidnode);
//        newnode = true;
//    }
//    }

//client->app->syncupdate_local_folder_addition(this, l, path.c_str());
//                else
//                {
//                if (fa->fsidvalid && l->fsid != fa->fsid)
//                {
//                    l->setfsid(fa->fsid, client->fsidnode);
//                }
//
//                if (l->genfingerprint(fa.get()))
//                {
//                    changed = true;
//                    l->bumpnagleds();
//                    l->deleted = false;
//                }
//
//                if (newnode)
//                {
//                    client->app->syncupdate_local_file_addition(this, l, path.c_str());
//                }
//                else if (changed)
//                {
//                    client->app->syncupdate_local_file_change(this, l, path.c_str());
//                    DBTableTransactionCommitter committer(client->tctable); // TODO:  can we use one committer for all the files in the folder?  Or for the whole recursion?
//                    client->stopxfer(l, &committer);
//                }
//
//                if (newnode || changed)
//                {
//                    statecacheadd(l);
//                }
//                }
//            }
//        }
//
//        if (changed || newnode)
//        {
//            if (isnetwork && l->type == FILENODE)
//            {
//                LOG_debug << "Queueing extra fs notification for new file";
//                dirnotify->notify(DirNotify::EXTRA, NULL, LocalPath(*localpathNew));
//            }
//
//            client->syncactivity = true;
//        }
//    }
//    else
//    {
//    LOG_warn << "Error opening file";
//    if (fa->retry)
//    {
//        // fopen() signals that the failure is potentially transient - do
//        // nothing and request a recheck
//        LOG_warn << "File blocked. Adding notification to the retry queue: " << path;
//        dirnotify->notify(DirNotify::RETRY, ll, LocalPath(*localpathNew));
//        client->syncfslockretry = true;
//        client->syncfslockretrybt.backoff(SCANNING_DELAY_DS);
//        client->blockedfile = *localpathNew;
//    }
//    else if (l)
//    {
//        // immediately stop outgoing transfer, if any
//        if (l->transfer)
//        {
//            DBTableTransactionCommitter committer(client->tctable); // TODO:  can we use one committer for all the files in the folder?  Or for the whole recursion?
//            client->stopxfer(l, &committer);
//        }
//
//        client->syncactivity = true;
//
//        // in fullscan mode, missing files are handled in bulk in deletemissing()
//        // rather than through setnotseen()
//        if (!fullscan)
//        {
//            l->setnotseen(1);
//        }
//    }


bool Sync::checkLocalPathForMovesRenames(syncRow& row, syncRow& parentRow, LocalPath& fullPath, bool& rowResult)
{
    if (row.syncNode)
    {
        // (FIXME: handle type changes)
        if (row.syncNode->type == row.fsNode->type)
        {
            // mark as present
            row.syncNode->setnotseen(0); // todo: do we need this - prob not always right now

            if (row.syncNode->type == FILENODE)
            {
                // we already checked fsid differs before calling

                // was the file overwritten by moving an existing file over it?
                if (LocalNode* sourceLocalNode = client->findLocalNodeByFsid(*row.fsNode, *this))
                {
                    // catch the not so unlikely case of a false fsid match due to
                    // e.g. a file deletion/creation cycle that reuses the same inode
                    if (sourceLocalNode->mtime != row.fsNode->mtime || sourceLocalNode->size != row.fsNode->size)
                    {
                        // This location's file can't be useing that fsid then.
                        // Clear our fsid, and let normal comparison run
                        row.fsNode->fsid = UNDEF;

                        return false;
                    }
                    else
                    {
                        LOG_debug << "File move/overwrite detected";

                        // delete existing LocalNode...
                        delete row.syncNode;      // todo:  CAUTION:  this will queue commands to remove the cloud node
                        row.syncNode = nullptr;

                        // ...move remote node out of the way...
                        client->execsyncdeletions();   // todo:  CAUTION:  this will send commands to remove the cloud node

                        // ...and atomically replace with moved one
                        client->app->syncupdate_local_move(this, sourceLocalNode, fullPath.toPath(*client->fsaccess).c_str());

                        // (in case of a move, this synchronously updates l->parent and l->node->parent)
                        sourceLocalNode->setnameparent(parentRow.syncNode, &fullPath, client->fsaccess->fsShortname(fullPath), true);

                        // mark as seen / undo possible deletion
                        sourceLocalNode->setnotseen(0);  // todo: do we still need this?

                        statecacheadd(sourceLocalNode);

                        rowResult = false;
                        return true;
                    }
                }
            }
        }
    }
    else
    {
        // !row.syncNode

        // rename or move of existing node?
        if (row.fsNode->isSymlink)
        {
            LOG_debug << "checked path is a symlink, blocked: " << fullPath.toPath(*client->fsaccess);
            row.syncNode->setUseBlocked();    // todo:   move earlier?  no syncnode here
            rowResult = false;
            return true;
        }
        else if (LocalNode* sourceLocalNode = client->findLocalNodeByFsid(*row.fsNode, *this))
        {
            LOG_debug << client->clientname << "Move detected by fsid. Type: " << sourceLocalNode->type << " new path: " << fullPath.toPath(*client->fsaccess) << " old localnode: " << sourceLocalNode->localnodedisplaypath(*client->fsaccess);

            // logic to detect files being updated in the local computer moving the original file
            // to another location as a temporary backup
            if (sourceLocalNode->type == FILENODE &&
                client->checkIfFileIsChanging(*row.fsNode, sourceLocalNode->getLocalPath(true)))
            {
                // if we revist here and the file is still the same after enough time, we'll move it
                rowResult = false;
                return true;
            }

            client->app->syncupdate_local_move(this, sourceLocalNode, fullPath.toPath(*client->fsaccess).c_str());

            // (in case of a move, this synchronously updates l->parent
            // and l->node->parent)
            sourceLocalNode->setnameparent(parentRow.syncNode, &fullPath, client->fsaccess->fsShortname(fullPath), false);

            // make sure that active PUTs receive their updated filenames
            client->updateputs();

            statecacheadd(sourceLocalNode);

            // unmark possible deletion
            sourceLocalNode->setnotseen(0);    // todo: do we still need this?

            // immediately scan folder to detect deviations from cached state
            if (fullscan && sourceLocalNode->type == FOLDERNODE)
            {
                sourceLocalNode->setFutureScan(true, true);
            }
        }
    }
    return false;
 }

bool Sync::checkCloudPathForMovesRenames(syncRow& row, syncRow& parentRow, LocalPath& fullPath, bool& rowResult)
{

    // First, figure out if this cloud node is different now because it was moved or renamed to this location

    // look up LocalNode by cloud nodehandle eventually, probably.  But ptrs are useful for now

    if (row.cloudNode->localnode && row.cloudNode->localnode->parent)
    {
        // It's a move or rename

        row.cloudNode->localnode->treestate(TREESTATE_SYNCING);

        LocalPath sourcePath = row.cloudNode->localnode->getLocalPath(true);
        LOG_verbose << "Renaming/moving from the previous location: " << sourcePath.toPath(*client->fsaccess);

        if (client->fsaccess->renamelocal(sourcePath, fullPath))
        {
            // todo: move anything at this path to sync debris first?  Old algo didn't though

            client->app->syncupdate_local_move(this, row.cloudNode->localnode, fullPath.toPath(*client->fsaccess).c_str());

            // update LocalNode tree to reflect the move/rename
            row.cloudNode->localnode->setnameparent(parentRow.syncNode, &fullPath, client->fsaccess->fsShortname(fullPath), false);
            statecacheadd(row.cloudNode->localnode);

            // update filenames so that PUT transfers can continue seamlessly
            client->updateputs();
            client->syncactivity = true;  // todo: prob don't need this?

            row.cloudNode->localnode->treestate(TREESTATE_SYNCED);
        }
        else if (client->fsaccess->transient_error)
        {
            row.syncNode->setUseBlocked();
        }

    }
    return false;
}



//bool Sync::checkValidNotification(int q, Notification& notification)
//{
//    // This code moved from filtering before going on notifyq, to filtering after when it's thread-safe to do so
//
//    if (q == DirNotify::DIREVENTS || q == DirNotify::EXTRA)
//    {
//        Notification next;
//        while (dirnotify->notifyq[q].peekFront(next)
//            && next.localnode == notification.localnode && next.path == notification.path)
//        {
//            dirnotify->notifyq[q].popFront(next);  // this is the only thread removing from the queue so it will be the same item
//            if (!notification.timestamp || !next.timestamp)
//            {
//                notification.timestamp = 0;  // immediate
//            }
//            else
//            {
//                notification.timestamp = std::max(notification.timestamp, next.timestamp);
//            }
//            LOG_debug << "Next notification repeats, skipping duplicate";
//        }
//    }
//
//    if (notification.timestamp && /*!initializing &&*/ q == DirNotify::DIREVENTS)
//    {
//        LocalPath tmppath;
//        if (notification.localnode)
//        {
//            tmppath = notification.localnode->getLocalPath(true);
//        }
//
//        if (!notification.path.empty())
//        {
//            tmppath.appendWithSeparator(notification.path, false, client->fsaccess->localseparator);
//        }
//
//        attr_map::iterator ait;
//        auto fa = client->fsaccess->newfileaccess(false);
//        bool success = fa->fopen(tmppath, false, false);
//        LocalNode *ll = localnodebypath(notification.localnode, notification.path);
//        if ((!ll && !success && !fa->retry) // deleted file
//            || (ll && success && ll->node && ll->node->localnode == ll
//                && (ll->type != FILENODE || (*(FileFingerprint *)ll) == (*(FileFingerprint *)ll->node))
//                && (ait = ll->node->attrs.map.find('n')) != ll->node->attrs.map.end()
//                && ait->second == ll->name
//                && fa->fsidvalid && fa->fsid == ll->fsid && fa->type == ll->type
//                && (ll->type != FILENODE || (ll->mtime == fa->mtime && ll->size == fa->size))))
//        {
//            LOG_debug << "Self filesystem notification skipped";
//            return false;
//        }
//    }
//    return true;
//}


//  Just mark the relative LocalNodes as needing to be rescanned.
void Sync::procscanq(int q)
{
    if (dirnotify->notifyq[q].empty()) return;

    LOG_verbose << "Marking sync tree with filesystem notifications: " << dirnotify->notifyq[q].size();

    Notification notification;
    while (dirnotify->notifyq[q].popFront(notification))
    {
        LocalNode* l;
        if ((l = notification.localnode) != (LocalNode*)~0)
        {
            LocalPath remainder;
            LocalNode *deepestParent;
            LocalNode *matching = localnodebypath(l, notification.path, &deepestParent, &remainder);
            if (LocalNode *deepest = matching && matching->parent ? matching->parent : deepestParent)
            {
                deepest->setFutureScan(true, !remainder.empty());
                client->filesystemNotificationsQuietTime = Waiter::ds + (isnetwork && l->type == FILENODE ? Sync::EXTRA_SCANNING_DELAY_DS : SCANNING_DELAY_DS);
            }
        }
        else
        {
            string utf8path = notification.path.toPath(*client->fsaccess);
            LOG_debug << "Notification skipped: " << utf8path;
        }
    }
}

// delete all child LocalNodes that have been missing for two consecutive scans (*l must still exist)
void Sync::deletemissing(LocalNode* l)
{
    LocalPath path;
    std::unique_ptr<FileAccess> fa;
    for (localnode_map::iterator it = l->children.begin(); it != l->children.end(); )
    {
        if (scanseqno-it->second->scanseqno > 1)
        {
            if (!fa)
            {
                fa = client->fsaccess->newfileaccess();
            }
            client->unlinkifexists(it->second, fa.get(), path);
            delete it++->second;
        }
        else
        {
            deletemissing(it->second);
            it++;
        }
    }
}

bool Sync::movetolocaldebris(LocalPath& localpath)
{
    char buf[32];
    struct tm tms;
    string day, localday;
    bool havedir = false;
    struct tm* ptm = m_localtime(m_time(), &tms);

    for (int i = -3; i < 100; i++)
    {
        ScopedLengthRestore restoreLen(localdebris);

        if (i == -2 || i > 95)
        {
            LOG_verbose << "Creating local debris folder";
            client->fsaccess->mkdirlocal(localdebris, true);
        }

        sprintf(buf, "%04d-%02d-%02d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);

        if (i >= 0)
        {
            sprintf(strchr(buf, 0), " %02d.%02d.%02d.%02d", ptm->tm_hour,  ptm->tm_min, ptm->tm_sec, i);
        }

        day = buf;
        localdebris.appendWithSeparator(LocalPath::fromPath(day, *client->fsaccess), true, client->fsaccess->localseparator);

        if (i > -3)
        {
            LOG_verbose << "Creating daily local debris folder";
            havedir = client->fsaccess->mkdirlocal(localdebris, false) || client->fsaccess->target_exists;
        }

        localdebris.appendWithSeparator(localpath.subpathFrom(localpath.getLeafnameByteIndex(*client->fsaccess)), true, client->fsaccess->localseparator);

        client->fsaccess->skip_errorreport = i == -3;  // we expect a problem on the first one when the debris folders or debris day folders don't exist yet
        if (client->fsaccess->renamelocal(localpath, localdebris, false))
        {
            client->fsaccess->skip_errorreport = false;
            return true;
        }
        client->fsaccess->skip_errorreport = false;

        if (client->fsaccess->transient_error)
        {
            return false;
        }

        if (havedir && !client->fsaccess->target_exists)
        {
            return false;
        }
    }

    return false;
}

auto Sync::computeSyncTriplets(const LocalNode& root, vector<FSNode>& fsNodes) const -> vector<syncRow>
{
    // One comparator to sort them all.
    class Comparator
    {
    public:
        explicit Comparator(const Sync& sync)
          : mFsAccess(*sync.client->fsaccess)
          , mFsType(sync.mFilesystemType)
        {
        }

        int compare(const FSNode& lhs, const LocalNode& rhs) const
        {
            // Cloud name, case sensitive.
            return lhs.localname.compare(rhs.name);
        }

        int compare(const Node& lhs, const syncRow& rhs) const
        {
            // Local name, filesystem-dependent sensitivity.
            auto a = LocalPath::fromName(lhs.displayname(), mFsAccess, mFsType);

            return a.fsCompare(name(rhs), mFsType);
        }

        bool operator()(const FSNode& lhs, const FSNode& rhs) const
        {
            // Cloud name, case sensitive.
            return lhs.localname.compare(rhs.localname) < 0;
        }

        bool operator()(const LocalNode* lhs, const LocalNode* rhs) const
        {
            assert(lhs && rhs);

            // Cloud name, case sensitive.
            return lhs->name < rhs->name;
        }

        bool operator()(const Node* lhs, const Node* rhs) const
        {
            assert(lhs && rhs);

            // Local name, filesystem-dependent sensitivity.
            auto a = LocalPath::fromName(lhs->displayname(), mFsAccess, mFsType);

            return a.fsCompare(rhs->displayname(), mFsType) < 0;
        }

        bool operator()(const syncRow& lhs, const syncRow& rhs) const
        {
            // Local name, filesystem-dependent sensitivity.
            return name(lhs).fsCompare(name(rhs), mFsType) < 0;
        }

    private:
        const LocalPath& name(const syncRow& row) const
        {
            assert(row.fsNode || row.syncNode);

            if (row.syncNode)
            {
                return row.syncNode->localname;
            }

            return row.fsNode->localname;
        }

        FileSystemAccess& mFsAccess;
        FileSystemType mFsType;
    }; // Comparator

    Comparator comparator(*this);
    vector<LocalNode*> localNodes;
    vector<Node*> remoteNodes;
    vector<syncRow> triplets;

    localNodes.reserve(root.children.size());
    remoteNodes.reserve(root.node->children.size());

    for (auto& child : root.children)
    {
        localNodes.emplace_back(child.second);
    }

    for (auto* child : root.node->children)
    {
        if (child->syncable(root))
        {
            remoteNodes.emplace_back(child);
        }
    }

    std::sort(fsNodes.begin(), fsNodes.end(), comparator);
    std::sort(localNodes.begin(), localNodes.end(), comparator);

    // Pair filesystem nodes with local nodes.
    {
        auto fCurr = fsNodes.begin();
        auto fEnd  = fsNodes.end();
        auto lCurr = localNodes.begin();
        auto lEnd  = localNodes.end();

        for ( ; ; )
        {
            // Determine the next filesystem node.
            auto fNext = std::upper_bound(fCurr, fEnd, *fCurr, comparator);

            // Determine the next local node.
            auto lNext = std::upper_bound(lCurr, lEnd, *lCurr, comparator);

            // By design, we should never have any conflicting local nodes.
            assert(std::distance(lCurr, lNext) < 2);

            auto *fsNode = fCurr != fEnd ? &*fCurr : nullptr;
            auto *localNode = lCurr != lEnd ? *lCurr : nullptr;

            // Bail, there's nothing left to pair.
            if (!(fsNode || localNode)) break;

            if (fsNode && localNode)
            {
                const auto relationship =
                  comparator.compare(*fsNode, *localNode);

                // Non-null entries are considered equivalent.
                if (relationship < 0)
                {
                    // Process the filesystem node first.
                    localNode = nullptr;
                }
                else if (relationship > 0)
                {
                    // Process the local node first.
                    fsNode = nullptr;
                }
            }

            // Add the pair.
            triplets.emplace_back(nullptr, localNode, fsNode);

            // Mark conflicts.
            if (fsNode && std::distance(fCurr, fNext) > 1)
            {
                triplets.back().cloudNode = NAME_CONFLICT;
            }

            fCurr = fsNode ? fNext : fCurr;
            lCurr = localNode ? lNext : lCurr;
        }
    }

    std::sort(remoteNodes.begin(), remoteNodes.end(), comparator);
    std::sort(triplets.begin(), triplets.end(), comparator);

    // Link cloud nodes with triplets.
    {
        auto rCurr = remoteNodes.begin();
        auto rEnd = remoteNodes.end();
        size_t tCurr = 0;
        size_t tEnd = triplets.size();

        for ( ; ; )
        {
            auto rNext = std::upper_bound(rCurr, rEnd, *rCurr, comparator);
            auto tNext = tCurr;

            // Compute upper bound manually.
            for ( ; tNext != tEnd; ++tNext)
            {
                if (comparator(triplets[tCurr], triplets[tNext]))
                {
                    break;
                }
            }

            // There should never be any conflicting triplets.
            assert(tNext - tCurr < 2);

            auto* remoteNode = rCurr != rEnd ? *rCurr : nullptr;
            auto* triplet = tCurr != tEnd ? &triplets[tCurr] : nullptr;

            // Bail as there's nothing to pair.
            if (!(remoteNode || triplet)) break;

            if (remoteNode && triplet)
            {
                const auto relationship =
                  comparator.compare(*remoteNode, *triplet);

                // Non-null entries are considered equivalent.
                if (relationship < 0)
                {
                    // Process remote node first.
                    triplet = nullptr;
                }
                else if (relationship > 0)
                {
                    // Process triplet first.
                    remoteNode = nullptr;
                }
            }

            // Have we detected a remote name conflict?
            if (remoteNode && std::distance(rCurr, rNext) > 1)
            {
                remoteNode = NAME_CONFLICT;
            }

            if (triplet)
            {
                // Only match the remote if we didn't detect a conflict earlier.
                if (triplet->cloudNode != NAME_CONFLICT)
                {
                    triplet->cloudNode = remoteNode;
                }
            }
            else
            {
                triplets.emplace_back(remoteNode, nullptr, nullptr);
            }

            if (triplet)    tCurr = tNext;
            if (remoteNode) rCurr = rNext;
        }
    }

    return triplets;
}

auto Sync::computeSyncTriplets(const LocalNode& root) const -> vector<syncRow>
{
    vector<FSNode> fsNodes;

    fsNodes.reserve(root.children.size());

    for (auto& child : root.children)
    {
        if (child.second->fsid != UNDEF)
        {
            fsNodes.emplace_back(child.second->getKnownFSDetails());
        }
    }

    return computeSyncTriplets(root, fsNodes);
}

bool Sync::recursiveSync(syncRow& row, LocalPath& localPath)
{
    LOG_verbose << "Entering folder with syncagain=" << row.syncNode->syncAgain << " scanagain=" << row.syncNode->scanAgain << " at " << localPath.toPath(*client->fsaccess);

    assert(row.syncNode);

    // Sentinel value used to signal that we've detected a name conflict.
    Node* const NAME_CONFLICT = reinterpret_cast<Node*>(~0ull);

    if (!row.syncNode)
    {
        // visit this node again later when we have a LocalNode at this level
        LOG_verbose << "No syncnode";
        return false;
    }

    // nothing to do for this subtree? Skip traversal
    if (!(row.syncNode->scanRequired() || row.syncNode->syncRequired()))
    {
        LOG_verbose << "No syncing or scanning needed";
        // Make sure our parent knows about any conflicts we may have detected.
        row.syncNode->conflictRefresh();
        return true;
    }

    if (row.cloudNode && !row.cloudNode->mPendingChanges.empty())
    {
        // visit this node again later when commands are complete
        LOG_verbose << "Waitig for pending changes at this level to complete";
        row.syncNode->conflictRefresh();
        return false;
    }

    // propagate full-scan flags to children
    if (row.syncNode->scanAgain == LocalNode::SYNCTREE_ACTION_HERE_AND_BELOW)
    {
        for (auto& c : row.syncNode->children)
        {
            c.second->scanAgain = LocalNode::SYNCTREE_ACTION_HERE_AND_BELOW;
        }

        row.syncNode->scanAgain = LocalNode::SYNCTREE_ACTION_HERE_ONLY;
    }

    // propagate full-sync flags to children
    if (row.syncNode->syncAgain == LocalNode::SYNCTREE_ACTION_HERE_AND_BELOW)
    {
        for (auto& c : row.syncNode->children)
        {
            c.second->syncAgain = LocalNode::SYNCTREE_ACTION_HERE_AND_BELOW;
        }

        row.syncNode->syncAgain = LocalNode::SYNCTREE_ACTION_HERE_ONLY;
    }

    // Get the filesystem items list
    if (row.syncNode->scanAgain == LocalNode::SYNCTREE_ACTION_HERE_ONLY)
    {
        if (Waiter::ds - row.syncNode->lastScanTime < 20)
        {
            // Make sure our parent knows about any conflicts we may have detected.
            row.syncNode->conflictRefresh();

            // Don't scan a particular folder more frequently than every 2 seconds.  Just revisit later
            LOG_verbose << "Can't scan again yet, too early";
            return false;
        }

        // If we need to scan at this level, do it now - just scan one folder then return from the stack to release the mutex.
        // Sync actions can occur on the next run

        row.syncNode->lastFolderScan.reset(new vector<FSNode>);
        *row.syncNode->lastFolderScan = scanOne(*row.syncNode, localPath);
        row.syncNode->lastScanTime = Waiter::ds;
        row.syncNode->syncAgain = LocalNode::SYNCTREE_ACTION_HERE_ONLY;
    }

    vector<FSNode> fsChildrenReconstructed;
    if (!row.syncNode->lastFolderScan)
    {
        // LocalNodes are in sync with the filesystem so we can reconstruct lastFolderScan
        fsChildrenReconstructed.reserve(row.syncNode->children.size());
        for (auto& c: row.syncNode->children)
        {
            if (c.second->fsid != UNDEF)
            {
                fsChildrenReconstructed.push_back(c.second->getKnownFSDetails());
            }
        }
    }
    auto& fsChildren = row.syncNode->lastFolderScan? *row.syncNode->lastFolderScan : fsChildrenReconstructed;

    // clear scan flag, especially check-descendants.
    row.syncNode->scanAgain = LocalNode::SYNCTREE_RESOLVED;

    bool syncHere = row.syncNode->syncAgain == LocalNode::SYNCTREE_ACTION_HERE_ONLY;

    // Get sync triplets.
    auto childRows = computeSyncTriplets(*row.syncNode, fsChildren);

    row.syncNode->syncAgain = LocalNode::SYNCTREE_RESOLVED;

    // Clear our conflict state.
    // It'll be recomputed as we traverse this subtree.
    row.syncNode->conflictsResolved();

    bool folderSynced = true;
    bool subfoldersSynced = true;

    for (auto& childRow : childRows)
    {
        // Skip rows that signal name conflicts.
        if (childRow.cloudNode == NAME_CONFLICT)
        {
            row.syncNode->conflictDetected();
            continue;
        }

        ScopedLengthRestore restoreLen(localPath);
        if (childRow.fsNode)
        {
            localPath.appendWithSeparator(childRow.fsNode->localname, true, client->fsaccess->localseparator);
        }
        else if (childRow.syncNode)
        {
            localPath.appendWithSeparator(childRow.syncNode->localname, true, client->fsaccess->localseparator);
        }
        else if (childRow.cloudNode)
        {
            localPath.appendWithSeparator(LocalPath::fromName(childRow.cloudNode->displayname(), *client->fsaccess, mFilesystemType), true, client->fsaccess->localseparator);
        }

        assert(!childRow.syncNode || childRow.syncNode->getLocalPath(true) == localPath);

        if (syncHere)
        {
            if (!syncItem(childRow, row, localPath))
            {
                folderSynced = false;
            }
        }

        if (childRow.syncNode && childRow.syncNode->type == FOLDERNODE)
        {
            if (childRow.cloudNode && childRow.fsNode)
            {
                if (!recursiveSync(childRow, localPath))
                {
                    subfoldersSynced = false;
                }
            }
        }
    }

    row.syncNode->setFutureSync(!folderSynced, !subfoldersSynced);

    if (folderSynced)
    {
        row.syncNode->lastFolderScan.reset();
    }

    LOG_verbose << "Exiting folder with synced=" << folderSynced << " subsync= " << subfoldersSynced << " syncagain=" << row.syncNode->syncAgain << " scanagain=" << row.syncNode->scanAgain << " at " << localPath.toPath(*client->fsaccess);
    return folderSynced && subfoldersSynced;
}

bool Sync::syncItem(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{

//todo: this used to be in scan().  But now we create LocalNodes for all - shall we check it in this function
    //// check if this record is to be ignored
    //if (client->app->sync_syncable(this, name.c_str(), localPath))
    //{
    //}
    //else
    //{
    //    LOG_debug << "Excluded: " << name;
    //}


    LOG_verbose << "Considering sync triplet:" <<
        " " << (row.cloudNode ? row.cloudNode->displaypath() : "(null)") <<
        " " << (row.syncNode ? row.syncNode->getLocalPath(true).toPath(*client->fsaccess) : "(null)") <<
        " " << (row.fsNode ? fullPath.toPath(*client->fsaccess) : "(null)");

    // Under some circumstances on sync startup, our shortname records can be out of date.
    // If so, we adjust for that here, as the diretories are scanned
    if (row.syncNode && row.fsNode && row.fsNode->shortname)
    {
        if (!row.syncNode->slocalname || *row.syncNode->slocalname != *row.fsNode->shortname)
        {
            LOG_warn << "Updating slocalname: " << row.fsNode->shortname->toPath(*client->fsaccess)
                     << " at " << fullPath.toPath(*client->fsaccess)
                     << " was " << (row.syncNode->slocalname ? row.syncNode->slocalname->toPath(*client->fsaccess) : "(null)");
            row.syncNode->setnameparent(row.syncNode->parent, nullptr, move(row.fsNode->shortname), false);
        }
    }

    if (row.syncNode && row.syncNode->useBlocked >= LocalNode::SYNCTREE_ACTION_HERE_ONLY)
    {
        if (!row.syncNode->rare().blockedTimer->armed())
        {
            LOG_verbose << "Waiting on use blocked timer, retry in ds: " << row.syncNode->rare().blockedTimer->retryin();
            return false;
        }
    }

    if (row.syncNode && row.syncNode->scanBlocked >= LocalNode::SYNCTREE_ACTION_HERE_ONLY)
    {
        if (row.syncNode->rare().blockedTimer->armed())
        {
            LOG_verbose << "Scan blocked timer elapsed, trigger parent rescan.";
            parentRow.syncNode->setFutureScan(true, false);
        }
        else
        {
            LOG_verbose << "Waiting on scan blocked timer, retry in ds: " << row.syncNode->rare().blockedTimer->retryin();
        }
        return false;
    }

    if (row.syncNode && (
        row.syncNode->useBlocked >= LocalNode::SYNCTREE_DESCENDANT_FLAGGED ||
        row.syncNode->scanBlocked >= LocalNode::SYNCTREE_DESCENDANT_FLAGGED))
    {
        // reset the flag for this node. Anything still blocked here or in the tree below will set it again.
        row.syncNode->scanBlocked = LocalNode::SYNCTREE_RESOLVED;
        row.syncNode->rare().blockedTimer.reset();
    }

    if (row.fsNode && (row.fsNode->type == TYPE_UNKNOWN || row.fsNode->isBlocked))
    {
        // We were not able to get details of the filesystem item when scanning the directory.
        // Consider it a blocked file, and we'll rescan the folder from time to time.
        LOG_verbose << "File/folder was blocked when reading directory, retry later: " << fullPath.toPath(*client->fsaccess);
        if (!row.syncNode) resolve_makeSyncNode_fromFS(row, parentRow, fullPath);
        row.syncNode->setScanBlocked();
        return false;
    }

    bool rowSynced = false;

    // First deal with detecting local moves/renames and propagating correspondingly
    // Independent of the 8 combos below so we don't have duplicate checks in those.

    if (row.fsNode && (!row.syncNode || (row.syncNode->fsid != UNDEF &&
                                         row.syncNode->fsid != row.fsNode->fsid)))
    {
        bool rowResult;
        if (checkLocalPathForMovesRenames(row, parentRow, fullPath, rowResult))
        {
            return rowResult;
        }
    }

    if (row.cloudNode && (!row.syncNode || (!row.syncNode->syncedCloudNodeHandle.isUndef() &&
        row.syncNode->syncedCloudNodeHandle.as8byte() != row.cloudNode->nodehandle)))
    {
        bool rowResult;
        if (checkCloudPathForMovesRenames(row, parentRow, fullPath, rowResult))
        {
            return rowResult;
        }
    }



    // each of the 8 possible cases of present/absent for this row
    if (row.syncNode)
    {
        if (row.fsNode)
        {
            if (row.cloudNode)
            {
                // all three exist; compare
                bool cloudEqual = syncEqual(*row.cloudNode, *row.syncNode);
                bool fsEqual = syncEqual(*row.fsNode, *row.syncNode);
                if (cloudEqual && fsEqual)
                {
                    // success! this row is synced
                    if (row.syncNode->fsid != row.fsNode->fsid ||
                        row.syncNode->syncedCloudNodeHandle != row.cloudNode->nodehandle)
                    {
                        LOG_verbose << "Row is synced, setting fsid and nodehandle";

                        row.syncNode->fsid = row.fsNode->fsid;
                        row.syncNode->syncedCloudNodeHandle.set6byte(row.cloudNode->nodehandle);

                        // todo: eventually remove pointers
                        row.syncNode->node = client->nodebyhandle(row.syncNode->syncedCloudNodeHandle.as8byte());
                        if (row.syncNode->node) row.syncNode->node->localnode = row.syncNode;

                        statecacheadd(row.syncNode);
                    }
                    else
                    {
                        LOG_verbose << "Row was already synced";
                    }
                    rowSynced = true;
                }
                else if (cloudEqual)
                {
                    // filesystem changed, put the change
                    rowSynced = resolve_upsync(row, parentRow, fullPath);
                }
                else if (fsEqual)
                {
                    // cloud has changed, get the change
                    rowSynced = resolve_downsync(row, parentRow, fullPath, true);
                }
                else
                {
                    // both changed, so we can't decide without the user's help
                    rowSynced = resolve_userIntervention(row, parentRow, fullPath);
                }
            }
            else
            {
                // cloud item absent
                if (row.syncNode->syncedCloudNodeHandle.isUndef())
                {
                    // cloud item did not exist before; upsync
                    rowSynced = resolve_upsync(row, parentRow, fullPath);
                }
                else
                {
                    // cloud item disappeared - remove locally (or figure out if it was a move, etc)
                    rowSynced = resolve_cloudNodeGone(row, parentRow, fullPath);
                }
            }
        }
        else
        {
            if (row.cloudNode)
            {
                // local item not present
                if (row.syncNode->fsid != UNDEF)
                {
                    // used to be synced - remove in the cloud (or detect move)
                    rowSynced = resolve_fsNodeGone(row, parentRow, fullPath);
                }
                else
                {
                    // fs item did not exist before; downsync
                    rowSynced = resolve_downsync(row, parentRow, fullPath, false);
                }
            }
            else
            {
                // local and cloud disappeared; remove sync item also
                rowSynced = resolve_delSyncNode(row, parentRow, fullPath);
            }
        }
    }

    else
    {
        if (row.fsNode)
        {
            if (row.cloudNode)
            {
                // Item exists locally and remotely but we haven't synced them previously
                // If they are equal then join them with a Localnode. Othewise report or choose greater mtime.
                if (row.fsNode->type != row.cloudNode->type)
                {
                    rowSynced = resolve_userIntervention(row, parentRow, fullPath);
                }
                else if (row.fsNode->type != FILENODE ||
                         row.fsNode->fingerprint == *static_cast<FileFingerprint*>(row.cloudNode))
                {
                    rowSynced = resolve_makeSyncNode_fromFS(row, parentRow, fullPath);
                }
                else
                {
                    rowSynced = resolve_pickWinner(row, parentRow, fullPath);
                }
            }
            else
            {
                // Item exists locally only. Check if it was moved/renamed here, or Create
                // If creating, next run through will upload it
                rowSynced = resolve_makeSyncNode_fromFS(row, parentRow, fullPath);
            }
        }
        else
        {
            if (row.cloudNode)
            {
                // item exists remotely only
                rowSynced = resolve_makeSyncNode_fromCloud(row, parentRow, fullPath);
            }
            else
            {
                // no entries
                assert(false);
            }
        }
    }
    return rowSynced;
}


bool Sync::resolve_makeSyncNode_fromFS(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{

    if (!fsStateCurrent)
    {
        // We can't be sure the file hasn't been moved to some not-yet-scanned folder, so wait
        LOG_verbose << "We must wait until scanning finishes to know if this is a move or not.";
        return false;
    }

    // this really is a new node: add
    LOG_debug << "Creating LocalNode from FS at: " << fullPath.toPath(*client->fsaccess);
    auto l = new LocalNode;

    if (row.fsNode->type == FILENODE)
    {
        assert(row.fsNode->fingerprint.isvalid);
        *static_cast<FileFingerprint*>(l) = row.fsNode->fingerprint;
    }
    l->init(this, row.fsNode->type, parentRow.syncNode, fullPath, std::move(row.fsNode->shortname));
    assert(row.fsNode->fsid != UNDEF);
    l->setfsid(row.fsNode->fsid, client->localnodeByFsid);
    l->treestate(TREESTATE_PENDING);
    if (l->type != FILENODE)
    {
        l->setFutureScan(true, true);
    }
    parentRow.syncNode->setFutureScan(true, false);
    statecacheadd(l);

    return false;
}

bool Sync::resolve_makeSyncNode_fromCloud(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    LOG_debug << "Creating LocalNode from Cloud at: " << fullPath.toPath(*client->fsaccess);
    auto l = new LocalNode;

    if (row.cloudNode->type == FILENODE)
    {
        assert(row.cloudNode->fingerprint().isvalid);
        *static_cast<FileFingerprint*>(l) = row.cloudNode->fingerprint();
    }
    l->init(this, row.cloudNode->type, parentRow.syncNode, fullPath, nullptr);
    l->syncedCloudNodeHandle.set6byte(row.cloudNode->nodehandle);
    l->treestate(TREESTATE_PENDING);
    if (l->type != FILENODE)
    {
        l->setFutureScan(true, true);
    }
    parentRow.syncNode->setFutureScan(true, false);
    statecacheadd(l);
    return false;
}

bool Sync::resolve_delSyncNode(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    // local and cloud disappeared; remove sync item also
    LOG_verbose << "Marking Localnode for deletion";

    // deletes itself and subtree, queues db record removal
    delete row.syncNode;
    row.syncNode = nullptr;

    return false;
}

bool Sync::resolve_upsync(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    if (row.fsNode->type == FILENODE)
    {
        // upload the file if we're not already uploading
        if (!row.syncNode->transfer)
        {
            if (parentRow.cloudNode)
            {
                LOG_debug << "Uploading file " << fullPath.toPath(*client->fsaccess);
                assert(row.syncNode->isvalid); // LocalNodes for files always have a valid fingerprint
                DBTableTransactionCommitter committer(client->tctable); // todo: move higher

                row.syncNode->h = parentRow.cloudNode->nodehandle;
                client->nextreqtag();
                client->startxfer(PUT, row.syncNode, committer);  // full path will be calculated in the prepare() callback
                client->app->syncupdate_put(this, row.syncNode, fullPath.toPath(*client->fsaccess).c_str());
            }
            else
            {
                LOG_verbose << "Parent cloud folder to upload to doesn't exist yet";
            }
        }
        else
        {
            LOG_verbose << "Upload already in progress";
        }
    }
    else
    {
        LOG_verbose << "Creating cloud node for: " << fullPath.toPath(*client->fsaccess);
        // while the operation is in progress sync() will skip over the parent folder
        vector<NewNode> nn(1);
        client->putnodes_prepareOneFolder(&nn[0], row.syncNode->name);
        client->putnodes(parentRow.cloudNode->nodehandle, move(nn), nullptr, 0);
    }
    return false;
}

bool Sync::resolve_downsync(syncRow& row, syncRow& parentRow, LocalPath& fullPath, bool alreadyExists)
{
    if (row.cloudNode->type == FILENODE)
    {
        // download the file if we're not already downloading
        // if (alreadyExists), we will move the target to the trash when/if download completes //todo: check
        if (!row.cloudNode->syncget)
        {
            // FIXME: to cover renames that occur during the
            // download, reconstruct localname in complete()
            LOG_debug << "Start fetching file node";
            client->app->syncupdate_get(this, row.cloudNode, fullPath.toPath(*client->fsaccess).c_str());

            row.cloudNode->syncget = new SyncFileGet(this, row.cloudNode, fullPath);
            DBTableTransactionCommitter committer(client->tctable); // TODO: use one committer for all files in the loop, without calling syncdown() recursively
            client->nextreqtag();
            client->startxfer(GET, row.cloudNode->syncget, committer);
        }
        else
        {
            LOG_verbose << "Download already in progress";
        }
    }
    else
    {
        assert(!alreadyExists); // if it did we would have matched it

        LOG_verbose << "Creating local folder at: " << fullPath.toPath(*client->fsaccess);

        if (client->fsaccess->mkdirlocal(fullPath))
        {
            assert(row.syncNode);
            parentRow.syncNode->setFutureScan(true, false);
        }
        else if (client->fsaccess->transient_error)
        {
            LOG_debug << "Transient error creating folder, marking as blocked " << fullPath.toPath(*client->fsaccess);
            assert(row.syncNode);
            row.syncNode->setUseBlocked();
        }
        else // !transient_error
        {
            // let's consider this case as blocked too, alert the user
            LOG_debug << "Non transient error creating folder, marking as blocked " << fullPath.toPath(*client->fsaccess);
            assert(row.syncNode);
            row.syncNode->setUseBlocked();
        }
    }
    return false;
}


bool Sync::resolve_userIntervention(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    LOG_debug << "write me";
    return false;
}

bool Sync::resolve_pickWinner(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    LOG_debug << "write me";
    return false;
}

bool Sync::resolve_cloudNodeGone(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    // First, figure out if this is a rename or move or if we can even tell that yet.

    if (!client->statecurrent)
    {
        // We could still be catching up on actionpackets during startup.
        // Wait until that settles down, this node may reappear or be moved again
        LOG_verbose << "Cloud node missing but we are not up to date with the cloud yet.";
        return false;
    }

    if (Node* n = client->nodebyhandle(row.syncNode->syncedCloudNodeHandle.as8byte()))
    {
        if (n->parent && n->parent == parentRow.cloudNode)
        {
            // File has been renamed away from here (but is still in the same folder)
            // Process renames in the receiving row - so just make sure we revisit.
            LOG_debug << "Cloud node was renamed away from this row.";
            return false;
        }
        else
        {
            // file has been moved (and possibly also renamed) to another folder
            // Process moves in the receiving node and row - so just make sure we revisit both folders
            LOG_debug << "Cloud node was moved away from this row.";
            return false;
        }
    }

    // node no longer exists anywhere  // todo:  what about debris & day folder creation - will we revisit here while that is going on?
    LOG_debug << "Moving local item to local sync debris: " << fullPath.toPath(*client->fsaccess);
    if (movetolocaldebris(fullPath))
    {
        // Let's still have another run of sync() on this folder afterward to ensure we are up to date
        row.syncNode->setfsid(UNDEF, client->localnodeByFsid);
    }

    return false;
}

LocalNode* MegaClient::findLocalNodeByFsid(FSNode& fsNode, Sync& filesystemSync)
{
    if (fsNode.fsid == UNDEF) return nullptr;

    auto range = localnodeByFsid.equal_range(fsNode.fsid);

    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second->type != fsNode.type) continue;

        // make sure we are in the same filesystem (fsid comparison is not valid in other filesystems)
        if (it->second->sync != &filesystemSync)
        {
            auto fp1 = it->second->sync->dirnotify->fsfingerprint();
            auto fp2 = filesystemSync.dirnotify->fsfingerprint();
            if (!fp1 || !fp2 || fp1 != fp2)
            {
                continue;
            }

#ifdef _WIN32
            // (from original sync code) Additionally for windows, check drive letter
            // only consider fsid matches between different syncs for local drives with the
            // same drive letter, to prevent problems with cloned Volume IDs
            if (it->second->sync->localroot->localname.driveLetter() !=
                filesystemSync.localroot->localname.driveLetter())
            {
                continue;
            }
#endif
            if (fsNode.type == FILENODE &&
                (fsNode.mtime != it->second->mtime ||
                    fsNode.size != it->second->size))
            {
                // fsid match, but size or mtime mismatch
                // treat as different
                continue;
            }

            // If we got this far, it's a good enough match to use
            // todo: come back for other matches?
            return it->second;
        }
    }
    return nullptr;
}


bool MegaClient::checkIfFileIsChanging(FSNode& fsNode, const LocalPath& fullPath)
{
    // logic to prevent moving files that may still be being updated

    // (original sync code comment:)
    // detect files being updated in the local computer moving the original file
    // to another location as a temporary backup

    assert(fsNode.type == FILENODE);

    bool waitforupdate = false;
    FileChangingState& state = mFileChangingCheckState[fullPath];

    m_time_t currentsecs = m_time();
    if (!state.updatedfileinitialts)
    {
        state.updatedfileinitialts = currentsecs;
    }

    if (currentsecs >= state.updatedfileinitialts)
    {
        if (currentsecs - state.updatedfileinitialts <= Sync::FILE_UPDATE_MAX_DELAY_SECS)
        {
            auto prevfa = fsaccess->newfileaccess(false);

            bool exists = prevfa->fopen(fullPath);
            if (exists)
            {
                LOG_debug << "File detected in the origin of a move";

                if (currentsecs >= state.updatedfilets)
                {
                    if ((currentsecs - state.updatedfilets) < (Sync::FILE_UPDATE_DELAY_DS / 10))
                    {
                        LOG_verbose << "currentsecs = " << currentsecs << "  lastcheck = " << state.updatedfilets
                            << "  currentsize = " << prevfa->size << "  lastsize = " << state.updatedfilesize;
                        LOG_debug << "The file was checked too recently. Waiting...";
                        waitforupdate = true;
                    }
                    else if (state.updatedfilesize != prevfa->size)
                    {
                        LOG_verbose << "currentsecs = " << currentsecs << "  lastcheck = " << state.updatedfilets
                            << "  currentsize = " << prevfa->size << "  lastsize = " << state.updatedfilesize;
                        LOG_debug << "The file size has changed since the last check. Waiting...";
                        state.updatedfilesize = prevfa->size;
                        state.updatedfilets = currentsecs;
                        waitforupdate = true;
                    }
                    else
                    {
                        LOG_debug << "The file size seems stable";
                    }
                }
                else
                {
                    LOG_warn << "File checked in the future";
                }

                if (!waitforupdate)
                {
                    if (currentsecs >= prevfa->mtime)
                    {
                        if (currentsecs - prevfa->mtime < (Sync::FILE_UPDATE_DELAY_DS / 10))
                        {
                            LOG_verbose << "currentsecs = " << currentsecs << "  mtime = " << prevfa->mtime;
                            LOG_debug << "File modified too recently. Waiting...";
                            waitforupdate = true;
                        }
                        else
                        {
                            LOG_debug << "The modification time seems stable.";
                        }
                    }
                    else
                    {
                        LOG_warn << "File modified in the future";
                    }
                }
            }
            else
            {
                if (prevfa->retry)
                {
                    LOG_debug << "The file in the origin is temporarily blocked. Waiting...";
                    waitforupdate = true;
                }
                else
                {
                    LOG_debug << "There isn't anything in the origin path";
                }
            }

            if (waitforupdate)
            {
                LOG_debug << "Possible file update detected.";
                return NULL;
            }
        }
        else
        {
            sendevent(99438, "Timeout waiting for file update", 0);
        }
    }
    else
    {
        LOG_warn << "File check started in the future";
    }

    if (!waitforupdate)
    {
        mFileChangingCheckState.erase(fullPath);
    }
    return waitforupdate;
}

bool Sync::resolve_fsNodeGone(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    if (!fsStateCurrent)
    {
        // We can't be sure the file hasn't been moved to some not-yet-scanned folder, so wait
        LOG_verbose << "Local file/folder missing but we have not finished scanning yet.";
        return false;
    }



    // todo: what about moves, renames, etc.
    LOG_debug << "Moving cloud item to cloud sync debris: " << row.cloudNode->displaypath();

    // clear fsid so we don't assume it is present anymore in future passes
    row.syncNode->setfsid(UNDEF, client->localnodeByFsid);

    // remove the cloud node (we won't be called back here again while there is a pending command on the node)
    // todo: double check that is the case - what if the debris folder has to be created first, is there a gap?
    client->movetosyncdebris(row.cloudNode, inshare);
    return false;
}

bool Sync::syncEqual(const Node& n, const LocalNode& ln)
{
    // Assuming names already match
    // Not comparing nodehandle here.  If they all match we set syncedCloudNodeHandle
    if (n.type != ln.type) return false;
    if (n.type != FILENODE) return true;
    assert(n.fingerprint().isvalid && ln.fingerprint().isvalid);
    return n.fingerprint() == ln.fingerprint();  // size, mtime, crc
}

bool Sync::syncEqual(const FSNode& fsn, const LocalNode& ln)
{
    // Assuming names already match
    // Not comparing fsid here. If they all match then we set LocalNode's fsid
    if (fsn.type != ln.type) return false;
    if (fsn.type != FILENODE) return true;
    assert(fsn.fingerprint.isvalid && ln.fingerprint().isvalid);
    return fsn.fingerprint == ln.fingerprint();  // size, mtime, crc
}


} // namespace
#endif
