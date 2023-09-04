/**
 * @file tests/synctests.cpp
 * @brief Mega SDK test file
 *
 * (c) 2018 by Mega Limited, Wellsford, New Zealand
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

// Many of these tests are still being worked on.
// The file uses some C++17 mainly for the very convenient std::filesystem library, though the main SDK must still build with C++11 (and prior)


#include "test.h"
#include "stdfs.h"
#include <mega.h>
#include "gtest/gtest.h"
#include <stdio.h>
#include <map>
#include <future>
//#include <mega/tsthooks.h>
#include <fstream>
#include <atomic>
#include <random>

#include <megaapi_impl.h>

#define DEFAULTWAIT std::chrono::seconds(20)

using namespace ::mega;
using namespace ::std;

std::string getCurrentTimestamp(bool includeDate);

#ifdef ENABLE_SYNC

template<typename T>
shared_promise<T> makeSharedPromise()
{
    return shared_promise<T>(new promise<T>());
}

bool suppressfiles = false;

typedef ::mega::byte byte;

bool adjustLastModificationTime(const fs::path& path, int adjustment)
{
    using std::chrono::seconds;

    std::error_code ec;

    // Retrieve the file's current modification time.
    auto current = fs::last_write_time(path, ec);

    // Bail if we couldn't retrieve the time.
    if (ec) return false;

    // Update the modification time.
    fs::last_write_time(path, current + seconds(adjustment), ec);

    // Let the caller know whether we succeeded.
    return !ec;
}

// Creates a temporary directory in the current path
fs::path makeTmpDir(const int maxTries = 1000)
{
    const auto cwd = fs::current_path();
    std::random_device dev;
    std::mt19937 prng{dev()};
    std::uniform_int_distribution<uint64_t> rand{0};
    fs::path path;
    for (int i = 0;; ++i)
    {
        std::ostringstream os;
        os << std::hex << rand(prng);
        path = cwd / os.str();
        if (fs::create_directory(path))
        {
            break;
        }
        if (i == maxTries)
        {
            throw std::runtime_error{"Couldn't create tmp dir"};
        }
    }
    return path;
}

// Copies a file while maintaining the write time.
void copyFile(const fs::path& source, const fs::path& target)
{
    assert(fs::is_regular_file(source));
    const auto tmpDir = makeTmpDir();
    const auto tmpFile = tmpDir / "copied_file";
    fs::copy_file(source, tmpFile);
    fs::last_write_time(tmpFile, fs::last_write_time(source));
    fs::rename(tmpFile, target);
    fs::remove(tmpDir);
}

string leafname(const string& p)
{
    auto n = p.find_last_of("/");
    return n == string::npos ? p : p.substr(n+1);
}

string parentpath(const string& p)
{
    auto n = p.find_last_of("/");
    return n == string::npos ? "" : p.substr(0, n-1);
}


struct StandardClient;
bool CatchupClients(StandardClient* c1, StandardClient* c2 = nullptr, StandardClient* c3 = nullptr);

bool createFile(const fs::path &path, const void *data, const size_t data_length)
{
#if (__cplusplus >= 201700L)
    ofstream ostream(path, ios::binary);
#else
    ofstream ostream(path.u8string(), ios::binary);
#endif

    LOG_verbose << "Creating local data file at " << path.u8string() << ", length " << data_length;

    ostream.write(reinterpret_cast<const char *>(data), data_length);

    return ostream.good();
}

bool createDataFile(const fs::path &path, const std::string &data)
{
    return createFile(path, data.data(), data.size());
}

bool createDataFile(const fs::path& path, const std::string& data, std::chrono::seconds delta)
{
    if (!createDataFile(path, data)) return false;

    std::error_code result;
    auto current = fs::last_write_time(path, result);

    if (result) return false;

    fs::last_write_time(path, current + delta, result);

    return !result;
}

std::string randomData(const std::size_t length)
{
    std::vector<uint8_t> data(length);

    std::generate_n(data.begin(), data.size(), [](){ return (uint8_t)std::rand(); });

    return std::string((const char*)data.data(), data.size());
}

Model::ModelNode::ModelNode(const ModelNode& other)
    : type(other.type)
    , mCloudName()
    , mFsName()
    , name(other.name)
    , content(other.content)
    , kids()
    , parent()
    , changed(other.changed)
{
    for (auto& child : other.kids)
    {
        addkid(child->clone());
    }
}

Model::ModelNode& Model::ModelNode::fsName(const string& name)
{
    return mFsName = name, *this;
}

const string& Model::ModelNode::fsName() const
{
    return mFsName.empty() ? name : mFsName;
}

Model::ModelNode& Model::ModelNode::cloudName(const string& name)
{
    return mCloudName = name, *this;
}

const string& Model::ModelNode::cloudName() const
{
    return mCloudName.empty() ? name : mCloudName;
}

void Model::ModelNode::generate(const fs::path& path, bool force)
{
    const fs::path ourPath = path / fsName();

    if (type == file)
    {
        if (changed || force)
        {
            ASSERT_TRUE(createDataFile(ourPath, content));
            changed = false;
        }
    }
    else
    {
        fs::create_directory(ourPath);

        for (auto& child : kids)
        {
            child->generate(ourPath, force);
        }
    }
}

string Model::ModelNode::path() const
{
    string s;
    for (auto p = this; p; p = p->parent)
        s = "/" + p->name + s;
    return s;
}

string Model::ModelNode::fsPath() const
{
    string s;
    for (auto p = this; p; p = p->parent)
        s = "/" + p->fsName() + s;
    return s;
}

Model::ModelNode* Model::ModelNode::addkid()
{
    return addkid(::mega::make_unique<ModelNode>());
}

Model::ModelNode* Model::ModelNode::addkid(unique_ptr<ModelNode>&& p)
{
    p->parent = this;
    kids.emplace_back(std::move(p));

    return kids.back().get();
}

bool Model::ModelNode::typematchesnodetype(nodetype_t nodetype) const
{
    switch (type)
    {
    case file: return nodetype == FILENODE;
    case folder: return nodetype == FOLDERNODE;
    }
    return false;
}

void Model::ModelNode::print(string prefix)
{
    out() << prefix << name;
    prefix.append(name).append("/");
    for (const auto &in: kids)
    {
        in->print(prefix);
    }
}

std::unique_ptr<Model::ModelNode> Model::ModelNode::clone()
{
    return ::mega::make_unique<ModelNode>(*this);
}

Model::Model()
    : root(makeModelSubfolder("root"))
{
}

Model::Model(const Model& other)
    : root(other.root->clone())
{
}

Model& Model::operator=(const Model& rhs)
{
    Model temp(rhs);

    swap(temp);

    return *this;
}

Model::ModelNode* Model::addfile(const string& path, const string& content)
{
    auto* node = addnode(path, ModelNode::file);

    node->content = content;
    node->changed = true;

    return node;
}

Model::ModelNode* Model::addfile(const string& path)
{
    return addfile(path, path);
}

Model::ModelNode* Model::addfolder(const string& path)
{
    return addnode(path, ModelNode::folder);
}

Model::ModelNode* Model::addnode(const string& path, ModelNode::nodetype type)
{
    ModelNode* child;
    ModelNode* node = root.get();
    string name;
    size_t current = 0;
    size_t end = path.size();

    while (current < end)
    {
        size_t delimiter = path.find('/', current);

        if (delimiter == path.npos)
        {
            break;
        }

        name = path.substr(current, delimiter - current);

        if (!(child = childnodebyname(node, name)))
        {
            child = node->addkid();

            child->name = name;
            child->type = ModelNode::folder;
        }

        assert(child->type == ModelNode::folder);

        current = delimiter + 1;
        node = child;
    }

    assert(current < end);

    name = path.substr(current);

    if (!(child = childnodebyname(node, name)))
    {
        child = node->addkid();

        child->name = name;
        child->type = type;
    }

    assert(child->type == type);

    return child;
}

Model::ModelNode* Model::copynode(const string& src, const string& dst)
{
    const ModelNode* source = findnode(src);
    ModelNode* destination = addnode(dst, source->type);

    destination->content = source->content;
    destination->kids.clear();

    for (auto& child : source->kids)
    {
        destination->addkid(child->clone());
    }

    return destination;
}

unique_ptr<Model::ModelNode> Model::makeModelSubfolder(const string& utf8Name)
{
    unique_ptr<ModelNode> n(new ModelNode);
    n->name = utf8Name;
    return n;
}

unique_ptr<Model::ModelNode> Model::makeModelSubfile(const string& utf8Name, string content)
{
    unique_ptr<ModelNode> n(new ModelNode);
    n->name = utf8Name;
    n->type = ModelNode::file;
    n->content = content.empty() ? utf8Name : std::move(content);
    return n;
}

unique_ptr<Model::ModelNode> Model::buildModelSubdirs(const string& prefix, int n, int recurselevel, int filesperdir)
{
    if (suppressfiles) filesperdir = 0;

    unique_ptr<ModelNode> nn = makeModelSubfolder(prefix);

    for (int i = 0; i < filesperdir; ++i)
    {
        nn->addkid(makeModelSubfile("file" + to_string(i) + "_" + prefix));
    }

    if (recurselevel > 0)
    {
        for (int i = 0; i < n; ++i)
        {
            unique_ptr<ModelNode> sn = buildModelSubdirs(prefix + "_" + to_string(i), n, recurselevel - 1, filesperdir);
            sn->parent = nn.get();
            nn->addkid(std::move(sn));
        }
    }
    return nn;
}

Model::ModelNode* Model::childnodebyname(ModelNode* n, const std::string& s)
{
    for (auto& m : n->kids)
    {
        if (m->name == s)
        {
            return m.get();
        }
    }
    return nullptr;
}

Model::ModelNode* Model::findnode(string path, ModelNode* startnode)
{
    ModelNode* n = startnode ? startnode : root.get();
    while (n && !path.empty())
    {
        auto pos = path.find("/");
        n = childnodebyname(n, path.substr(0, pos));
        path.erase(0, pos == string::npos ? path.size() : pos + 1);
    }
    return n;
}

unique_ptr<Model::ModelNode> Model::removenode(const string& path)
{
    ModelNode* n = findnode(path);
    if (n && n->parent)
    {
        unique_ptr<ModelNode> extracted;
        ModelNode* parent = n->parent;
        auto newend = std::remove_if(parent->kids.begin(), parent->kids.end(), [&extracted, n](unique_ptr<ModelNode>& v) { if (v.get() == n) return extracted = std::move(v), true; else return false; });
        parent->kids.erase(newend, parent->kids.end());
        return extracted;
    }
    return nullptr;
}

bool Model::movenode(const string& sourcepath, const string& destpath)
{
    ModelNode* source = findnode(sourcepath);
    ModelNode* dest = findnode(destpath);
    if (source && source && source->parent && dest)
    {
        auto replaced_node = removenode(destpath + "/" + source->name);

        unique_ptr<ModelNode> n;
        ModelNode* parent = source->parent;
        auto newend = std::remove_if(parent->kids.begin(), parent->kids.end(), [&n, source](unique_ptr<ModelNode>& v) { if (v.get() == source) return n = std::move(v), true; else return false; });
        parent->kids.erase(newend, parent->kids.end());
        if (n)
        {
            dest->addkid(std::move(n));
            return true;
        }
    }
    return false;
}

bool Model::movetosynctrash(unique_ptr<ModelNode>&& node, const string& syncrootpath)
{
    ModelNode* syncroot;
    if (!(syncroot = findnode(syncrootpath)))
    {
        return false;
    }

    ModelNode* trash;
    if (!(trash = childnodebyname(syncroot, DEBRISFOLDER)))
    {
        auto uniqueptr = makeModelSubfolder(DEBRISFOLDER);
        trash = uniqueptr.get();
        syncroot->addkid(std::move(uniqueptr));
    }

    char today[50];
    auto rawtime = time(NULL);
    strftime(today, sizeof today, "%F", localtime(&rawtime));

    ModelNode* dayfolder;
    if (!(dayfolder = findnode(today, trash)))
    {
        auto uniqueptr = makeModelSubfolder(today);
        dayfolder = uniqueptr.get();
        trash->addkid(std::move(uniqueptr));
    }

    dayfolder->addkid(std::move(node));

    return true;
}

bool Model::movetosynctrash(const string& path, const string& syncrootpath)
{
    if (auto node = removenode(path))
        return movetosynctrash(std::move(node), syncrootpath);

    return false;
}

void Model::ensureLocalDebrisTmpLock(const string& syncrootpath)
{
    // if we've downloaded a file then it's put in debris/tmp initially, and there is a lock file
    if (ModelNode* syncroot = findnode(syncrootpath))
    {
        ModelNode* trash;
        if (!(trash = childnodebyname(syncroot, DEBRISFOLDER)))
        {
            auto uniqueptr = makeModelSubfolder(DEBRISFOLDER);
            trash = uniqueptr.get();
            trash->fsOnly = true;
            syncroot->addkid(std::move(uniqueptr));
        }

        ModelNode* tmpfolder;
        if (!(tmpfolder = findnode("tmp", trash)))
        {
            auto uniqueptr = makeModelSubfolder("tmp");
            tmpfolder = uniqueptr.get();
            trash->addkid(std::move(uniqueptr));
        }

        ModelNode* lockfile;
        if (!(lockfile = findnode("lock", tmpfolder)))
        {
            tmpfolder->addkid(makeModelSubfile("lock"));
        }
    }
}

bool Model::removesynctrash(const string& syncrootpath, const string& subpath)
{
    if (subpath.empty())
    {
        return removenode(syncrootpath + "/" + DEBRISFOLDER).get();
    }
    else
    {
        char today[50];
        auto rawtime = time(NULL);
        strftime(today, sizeof today, "%F", localtime(&rawtime));

        return removenode(syncrootpath + "/" + DEBRISFOLDER + "/" + today + "/" + subpath).get();
    }
}

void Model::emulate_rename(std::string nodepath, std::string newname)
{
    auto node = findnode(nodepath);
    ASSERT_TRUE(!!node);
    if (node) node->name = newname;
}

void Model::emulate_move(std::string nodepath, std::string newparentpath)
{
    auto removed = removenode(newparentpath + "/" + leafname(nodepath));

    ASSERT_TRUE(movenode(nodepath, newparentpath));
}

void Model::emulate_copy(std::string nodepath, std::string newparentpath)
{
    auto node = findnode(nodepath);
    auto newparent = findnode(newparentpath);
    ASSERT_TRUE(!!node);
    ASSERT_TRUE(!!newparent);
    newparent->addkid(node->clone());
}

void Model::emulate_rename_copy(std::string nodepath, std::string newparentpath, std::string newname)
{
    auto node = findnode(nodepath);
    auto newparent = findnode(newparentpath);
    ASSERT_TRUE(!!node);
    ASSERT_TRUE(!!newparent);
    auto newnode = node->clone();
    newnode->name = newname;
    newparent->addkid(std::move(newnode));
}

void Model::emulate_delete(std::string nodepath)
{
    auto removed = removenode(nodepath);
    // ASSERT_TRUE(!!removed);
}

void Model::generate(const fs::path& path, bool force)
{
    fs::create_directories(path);

    for (auto& child : root->kids)
    {
        child->generate(path, force);
    }
}

void Model::swap(Model& other)
{
    using std::swap;

    swap(root, other.root);
}


bool waitonresults(future<bool>* r1 = nullptr, future<bool>* r2 = nullptr, future<bool>* r3 = nullptr, future<bool>* r4 = nullptr)
{
    if (r1) r1->wait();
    if (r2) r2->wait();
    if (r3) r3->wait();
    if (r4) r4->wait();
    return (!r1 || r1->get()) && (!r2 || r2->get()) && (!r3 || r3->get()) && (!r4 || r4->get());
}

atomic<int> next_request_tag{ 1 << 30 };

CloudItem::CloudItem(const Node* node)
  : CloudItem(*node)
{
}

CloudItem::CloudItem(const Node& node)
  : mNodeHandle(node.nodeHandle())
  , mPath()
  , mFromRoot(false)
{
}

CloudItem::CloudItem(const string& path, bool fromRoot)
  : CloudItem(path.c_str(), fromRoot)
{
}

CloudItem::CloudItem(const char* path, bool fromRoot)
  : mNodeHandle()
  , mPath(path)
  , mFromRoot(fromRoot)
{
    if (mFromRoot && path && *path == '/')
        mPath.erase(0, 1);
}

CloudItem::CloudItem(const NodeHandle& nodeHandle)
  : mNodeHandle(nodeHandle)
  , mPath()
  , mFromRoot()
{
}

CloudItem::CloudItem(handle nodeHandle)
  : CloudItem(NodeHandle().set6byte(nodeHandle))
{
}

Node* CloudItem::resolve(StandardClient& client) const
{
    if (!mNodeHandle.isUndef())
        return client.client.nodeByHandle(mNodeHandle);

    auto* root = client.gettestbasenode();

    if (mFromRoot)
        root = client.getcloudrootnode();

    return client.drillchildnodebyname(root, mPath);
}

std::set<string> declaredTestAccounts;

StandardClientInUse ClientManager::getCleanStandardClient(int loginIndex, fs::path workingFolder)
{
    EXPECT_TRUE(loginIndex >= 0) << "ClientManager::getCleanStandardClient(): invalid number of test account to setup " << loginIndex << " is < 0";
    EXPECT_TRUE(loginIndex <= gMaxAccounts) << "ClientManager::getCleanStandardClient(): too many test accounts requested " << loginIndex << " is > " << gMaxAccounts;

    for (auto i = clients[loginIndex].begin(); i != clients[loginIndex].end(); ++i)
    {
        if (!i->inUse)
        {
            i->ptr->cleanupForTestReuse(loginIndex);
            i->ptr->fsBasePath = i->ptr->ensureDir(workingFolder / fs::u8path(i->name));
            return StandardClientInUse(i);
        }
    }

    // otherwise, make a new one
    string clientname = std::to_string(loginIndex) + "_" + std::to_string(clients[loginIndex].size());
    fs::path localAccountRoot = makeReusableClientFolder(clientname);
    shared_ptr<StandardClient> c(
            new StandardClient(localAccountRoot, "client" + clientname, workingFolder));

    string user = getenv(envVarAccount[loginIndex].c_str());
    if (declaredTestAccounts.find(user) == declaredTestAccounts.end())
    {
        // show the email/pass so we can (a) log into the account and see what's happening
        // and (b) add a signal to terminate very long jenkins test runs if they are already failing badly
        string pass = getenv(envVarPass[loginIndex].c_str());

        // modify pass so that it's not obscured in jenkins output... somehow it recognizes it and substitutes [*******] in the console output
        string obfuscatedPass;
        for (auto c : pass)
        {
            obfuscatedPass += "/";
            obfuscatedPass += c;
            obfuscatedPass += "\\";
        }

        cout << "Using test account " << loginIndex << " " << user << " " << obfuscatedPass << endl;
        declaredTestAccounts.insert(user);
    }

    clients[loginIndex].push_back(StandardClientInUseEntry(false, c, clientname, loginIndex));
    c->login_reset(envVarAccount[loginIndex], envVarPass[loginIndex], false, false);

    c->cleanupForTestReuse(loginIndex);

    return StandardClientInUse(--clients[loginIndex].end());
}

ClientManager::~ClientManager()
{
    clear();
}

void ClientManager::clear()
{
    if (clients.empty())
        return;

    while (clients.size())
    {
        LOG_debug << "Shutting down ClientManager, remaining: " << clients.size();
        clients.erase(clients.begin());
    }
    LOG_debug << "ClientManager shutdown complete";
}

void StandardClient::ResultProc::prepresult(resultprocenum rpe, int tag, std::function<void()>&& requestfunc, std::function<bool(error)>&& f, handle h)
{
    if (rpe != COMPLETION)
    {
        lock_guard<recursive_mutex> g(mtx);
        auto& perTypeTags = m[rpe];
        assert(perTypeTags.find(tag) == perTypeTags.end());
        perTypeTags.emplace(tag, id_callback(std::move(f), tag, h));
    }

    std::lock_guard<std::recursive_mutex> lg(client.clientMutex);

    assert(tag > 0);
    int oldtag = client.client.reqtag;
    client.client.reqtag = tag;
    requestfunc();
    client.client.reqtag = oldtag;
    LOG_debug << "tag-result prepared for operation " << rpe << " tag " << tag;

    client.client.waiter->notify();
}

void StandardClient::ResultProc::processresult(resultprocenum rpe, error e, handle h, int tag)
{
    if (tag == 0 && rpe != CATCHUP)
    {
        //out() << "received notification of SDK initiated operation " << rpe << " tag " << tag; // too many of those to output
        return;
    }

    if (tag < (2 << 30))
    {
        out() << "ignoring callback from SDK internal sync operation " << rpe << " tag " << tag;
        return;
    }

    lock_guard<recursive_mutex> g(mtx);
    auto& entry = m[rpe];

    if (rpe == CATCHUP)
    {
        while (!entry.empty())
        {
            entry.begin()->second.f(e);
            entry.erase(entry.begin());
        }
        return;
    }

    if (entry.empty())
    {
        //out() << client.client.clientname
        //      << "received notification of operation type " << rpe << " completion but we don't have a record of it.  tag: " << tag;
        return;
    }

    auto it = entry.find(tag);
    if (it == entry.end())
    {
        out() << client.client.clientname
              << "tag not found for operation completion of " << rpe << " tag " << tag;
        return;
    }

    if (it->second.f(e))
    {
        entry.erase(it);
    }
}

string StandardClient::ensureDir(const fs::path& p)
{
    fs::create_directories(p);

    string result = p.u8string();

    if (result.back() != fs::path::preferred_separator)
    {
        result += fs::path::preferred_separator;
    }

    return result;
}

StandardClient::StandardClient(const fs::path& basepath, const string& name, const fs::path& workingFolder)
    :
      waiter(new WAIT_CLASS),
#ifdef GFX_CLASS
      gfx(::mega::make_unique<GFX_CLASS>()),
#endif
      client_dbaccess_path(ensureDir(basepath / name))
    , httpio(new HTTPIO_CLASS)
    , client(this,
                waiter,
                httpio.get(),
#ifdef DBACCESS_CLASS
                new DBACCESS_CLASS(LocalPath::fromAbsolutePath(client_dbaccess_path)),
#else
                NULL,
#endif
#ifdef GFX_CLASS
                &gfx,
#else
                NULL,
#endif
                "N9tSBJDC",
                USER_AGENT.c_str(),
                THREADS_PER_MEGACLIENT)
    , clientname(name + " ")
    , resultproc(*this)
    , clientthread([this]() { threadloop(); })
{
    client.clientname = clientname + " ";
    client.syncs.mDetailedSyncLogging = true;
    g_netLoggingOn = true;
#ifdef GFX_CLASS
    gfx.startProcessingThread();
#endif

    if (workingFolder.empty())
    {
        fsBasePath = basepath / fs::u8path(name);
    }
    else
    {
        fsBasePath = ensureDir(workingFolder / fs::u8path(name));
    }

    // SyncTests want to skip backup restrictions, so they are not
    // restricted to the path "Vault/My backups/<device>/<backup>"
    client.syncs.mBackupRestrictionsEnabled = false;
}

StandardClient::~StandardClient()
{
    LOG_debug << "StandardClient exiting";

    // Make sure logout completes before we escape.
    logout(false);

    LOG_debug << "~StandardClient final logout complete";

    clientthreadexit = true;
    waiter->notify();
    clientthread.join();
    LOG_debug << "~StandardClient end of function (work thread joined)";
}

void StandardClient::localLogout()
{
    auto result =
        thread_do<bool>([](MegaClient& mc, PromiseBoolSP result)
                        {
                            mc.locallogout(false, true);
                            result->set_value(true);
                        },
                        __FILE__, __LINE__);

    // Make sure logout completes before we escape.
    result.get();
}

bool StandardClient::logout(bool keepSyncsConfigFile)
{
    auto result = thread_do<bool>([=](MegaClient& client, PromiseBoolSP result) {
        client.logout(keepSyncsConfigFile, [=](error e) {
            result->set_value(e == API_OK);
        });
    }, __FILE__, __LINE__);

    if (result.wait_for(DEFAULTWAIT) == future_status::timeout)
        return false;

    return result.get();
}

string StandardClient::lp(LocalNode* ln) { return ln->getLocalPath().toName(*client.fsaccess); }

void StandardClient::onCallback() { lastcb = chrono::steady_clock::now(); };

void StandardClient::sync_added(const SyncConfig& config)
{
    onCallback();

    if (logcb)
    {
        lock_guard<mutex> guard(om);

        out() << clientname
                << "sync_added(): id: "
                << toHandle(config.mBackupId);
    }

    if (onAutoResumeResult)
    {
        onAutoResumeResult(config);
    }
}

void StandardClient::syncs_restored(SyncError syncError)
{
    lock_guard<mutex> g(om);

    out() << clientname
            << "sync restore complete: "
            << SyncConfig::syncErrorToStr(syncError);

    received_syncs_restored = true;
}

void StandardClient::nodes_updated(Node** nodes, int numNodes)
{
    if (!nodes)
    {
        out() << clientname << "nodes_updated: total reset.  total node count now: " << numNodes;
        return;
    }
    if (logcb)
    {
        lock_guard<mutex> g(om);
        if (numNodes > 1) // output root of sync (the second node) for tracing
        {
            out() << clientname << "nodes_updated: received " << numNodes << " including " << nodes[0]->displaypath() << " " << nodes[1]->displaypath();
        }
        else
        {
            out() << clientname << "nodes_updated: received " << numNodes << " including " << nodes[0]->displaypath();
        }
    }
    received_node_actionpackets = true;
    nodes_updated_cv.notify_all();
}

bool StandardClient::waitForNodesUpdated(unsigned numSeconds)
{
    mutex nodes_updated_cv_mutex;
    std::unique_lock<mutex> g(nodes_updated_cv_mutex);
    nodes_updated_cv.wait_for(g, std::chrono::seconds(numSeconds),
                                [&](){ return received_node_actionpackets; });
    return received_node_actionpackets;
}

void StandardClient::syncupdate_stateconfig(const SyncConfig& config)
{
    onCallback();

    if (logcb)
    {
        lock_guard<mutex> g(om);

        out() << clientname << "syncupdate_stateconfig() " << toHandle(config.mBackupId);
    }

    if (mOnSyncStateConfig)
        mOnSyncStateConfig(config);
}

void StandardClient::useralerts_updated(UserAlert::Base** alerts, int numAlerts)
{
    if (logcb)
    {
        lock_guard<mutex> guard(om);

        out() << clientname
              << "useralerts_updated: received "
              << numAlerts;
    }

    received_user_alerts = true;
    user_alerts_updated_cv.notify_all();
}

bool StandardClient::waitForUserAlertsUpdated(unsigned numSeconds)
{
    std::mutex mutex;
    std::unique_lock<std::mutex> guard(mutex);

    user_alerts_updated_cv.wait_for(guard, std::chrono::seconds(numSeconds), [&] {
        return received_user_alerts;
    });

    return received_user_alerts;
}

void StandardClient::syncupdate_scanning(bool b) { if (logcb) { onCallback(); lock_guard<mutex> g(om); out() << clientname << " syncupdate_scanning()" << b; } }

#ifdef DEBUG
void StandardClient::syncdebug_notification(const SyncConfig& config,
                            int queue,
                            const Notification& notification)
{
    if (mOnSyncDebugNotification)
        mOnSyncDebugNotification(config, queue, notification);
}
#endif // DEBUG

bool StandardClient::sync_syncable(Sync* sync, const char* name, LocalPath& path, Node*)
{
    return sync_syncable(sync, name, path);
}

bool StandardClient::sync_syncable(Sync*, const char*, LocalPath&)
{
    onCallback();

    return true;
}

bool StandardClient::istUsertAttributeSet(attr_t attr, unsigned int numSeconds, error& err)
{
    int tag = client.reqtag;
    mutex attr_cv_mutex;
    std::condition_variable user_attribute_updated_cv;
    bool attrIsSet = false;
    bool replyReceived = false;
    mOnGetUA = [&](const attr_t at, error e)
    {
        if (tag != client.restag)
        {
            return;
        }

        std::lock_guard<mutex> g(attr_cv_mutex);
        err = e;
        if (err == API_OK)
        {
            assert(at == attr);
            LOG_debug << "attr: " << attr << " is set";
                attrIsSet = true;
        }

        replyReceived = true;
        user_attribute_updated_cv.notify_all();
    };

    client.getua(client.ownuser(), attr);

    std::unique_lock<mutex> g(attr_cv_mutex);
    user_attribute_updated_cv.wait_for(g, std::chrono::seconds(numSeconds), [&replyReceived](){ return replyReceived; });

    mOnGetUA = nullptr;

    return attrIsSet;
}

bool StandardClient::waitForAttrDeviceIdIsSet(unsigned int numSeconds)
{
    error err;
    bool attrDeviceIsSet = istUsertAttributeSet(attr_t::ATTR_DEVICE_NAMES, numSeconds, err);

    bool deviceIdNoFound = false;
    std::unique_ptr<TLVstore> tlv;
    std::string deviceIdHash = client.getDeviceidHash();
    if (err == API_OK)
    {
        User* ownUser = client.ownuser();
        tlv.reset(TLVstore::containerToTLVrecords(ownUser->getattr(attr_t::ATTR_DEVICE_NAMES), &client.key));
        std::string buffer;
        if (tlv->get(deviceIdHash, buffer))
        {
            deviceIdNoFound = true;
        }
    }
    else
    {
        tlv.reset(new TLVstore);
    }


    if (err == API_ENOENT || !deviceIdNoFound)
    {
        std::string timestamp = getCurrentTimestamp(true);
        std::string deviceName = "Jenkins " + timestamp;

        tlv->set(deviceIdHash, deviceName);
        bool attrDeviceNamePut = false;
        mutex attrDeviceNamePut_mutex;
        std::condition_variable attrDeviceNamePut_cv;
        bool replyReceived = false;
        // serialize and encrypt the TLV container
        std::unique_ptr<string> container(tlv->tlvRecordsToContainer(client.rng, &client.key));
        client.putua(attr_t::ATTR_DEVICE_NAMES, (byte *)container->data(), unsigned(container->size()), -1, UNDEF, 0, 0, [&](Error e)
        {
            std::lock_guard<mutex> g(attrDeviceNamePut_mutex);
            if (e == API_OK)
            {
                attrDeviceNamePut = true;
            }
            else
            {
                LOG_err << "Error setting device id user attribute";
            }

            replyReceived = true;
            attrDeviceNamePut_cv.notify_all();
        });

        std::unique_lock<mutex> g(attrDeviceNamePut_mutex);
        attrDeviceNamePut_cv.wait_for(g, std::chrono::seconds(numSeconds), [&replyReceived](){ return replyReceived; });


        attrDeviceIsSet = istUsertAttributeSet(attr_t::ATTR_DEVICE_NAMES, numSeconds, err);

    }

    return attrDeviceIsSet;
}

bool StandardClient::waitForAttrMyBackupIsSet(unsigned int numSeconds)
{
    error err;
    bool attrMyBackupFolderIsSet = istUsertAttributeSet(attr_t::ATTR_MY_BACKUPS_FOLDER, numSeconds, err);

    if (err == API_ENOENT) // If attribute is not set, it's going to established
    {
        const char* folderName = "My Backups";
        attrMyBackupFolderIsSet = false;
        mutex attrMyBackup_cv_mutex;
        std::condition_variable user_attribute_backup_updated_cv;
        bool replyReceived = false;
        client.setbackupfolder(folderName, client.reqtag, [&](Error e)
        {
            std::lock_guard<mutex> g(attrMyBackup_cv_mutex);
            if (e == API_OK)
            {
                attrMyBackupFolderIsSet = true;
            }
            else
            {
                LOG_err << "Error setting back folder user attribute";
            }

            replyReceived = true;
            user_attribute_backup_updated_cv.notify_all();
        });

        std::unique_lock<mutex> g(attrMyBackup_cv_mutex);
        user_attribute_backup_updated_cv.wait_for(g, std::chrono::seconds(numSeconds), [&replyReceived](){ return replyReceived; });

        // Check if attribute has been established properly
        // Re-initialize variables, mOnGetUA is used as getua_result callback
        attrMyBackupFolderIsSet = istUsertAttributeSet(attr_t::ATTR_MY_BACKUPS_FOLDER, numSeconds, err);
    }

    return attrMyBackupFolderIsSet;
}

void StandardClient::file_added(File* file)
{
    if (mOnFileAdded)
    {
        mOnFileAdded(*file);
    }
}

void StandardClient::file_complete(File* file)
{
    if (mOnFileComplete)
    {
        mOnFileComplete(*file);
    }
}

void StandardClient::notify_retry(dstime t, retryreason_t r)
{
    onCallback();

    if (!logcb) return;

    lock_guard<mutex> guard(om);

    out() << clientname << " notify_retry: " << t << " " << r;
}

void StandardClient::request_error(error e)
{
    onCallback();

    if (!logcb) return;

    lock_guard<mutex> guard(om);

    out() << clientname << " request_error: " << e;
}

void StandardClient::request_response_progress(m_off_t a, m_off_t b)
{
    onCallback();

    if (!logcb) return;

    lock_guard<mutex> guard(om);

    out() << clientname << " request_response_progress: " << a << " " << b;
}

void StandardClient::threadloop()
    try
{
    while (!clientthreadexit)
    {
        int r;

        client.waiter->bumpds();
        dstime t1 = client.waiter->ds;

        {
            std::lock_guard<std::recursive_mutex> lg(clientMutex);

            client.waiter->bumpds();
            dstime t1a = client.waiter->ds;
            if (t1a - t1 > 20) LOG_debug << "lock for preparewait took ds: " << t1a - t1;

            r = client.preparewait();
        }
        assert(r == 0 || r == Waiter::NEEDEXEC);

        client.waiter->bumpds();
        dstime t2 = client.waiter->ds;
        if (t2 - t1 > 20) LOG_debug << "lock and preparewait took ds: " << t2 - t1;


        if (!r)
        {
            r |= client.dowait();
            assert(r == 0 || r == Waiter::NEEDEXEC);
        }

        client.waiter->bumpds();
        dstime t3 = client.waiter->ds;
        if (t3 - t2 > 20) LOG_debug << "dowait took ds: " << t3 - t2;

        std::lock_guard<std::recursive_mutex> lg(clientMutex);

        client.waiter->bumpds();
        dstime t3a = client.waiter->ds;
        if (t3a - t3 > 20) LOG_debug << "lock for exec took ds: " << t3a - t3;

        r |= client.checkevents();
        assert(r == 0 || r == Waiter::NEEDEXEC);

        client.waiter->bumpds();
        dstime t4 = client.waiter->ds;
        if (t4 - t3a > 20) LOG_debug << "checkevents took ds: " << t4 - t3a;

        {
            client.waiter->bumpds();
            auto start = client.waiter->ds;
            std::lock_guard<mutex> g(functionDoneMutex);
            string sourcefile;
            int sourceline = -1;
            if (nextfunctionMC)
            {
                sourcefile = nextfunctionMC_sourcefile;
                sourceline = nextfunctionMC_sourceline;
                nextfunctionMC_sourcefile = "";
                nextfunctionMC_sourceline = -1;
                nextfunctionMC();
                nextfunctionMC = nullptr;
                functionDone.notify_all();
                r |= Waiter::NEEDEXEC;
            }
            if (nextfunctionSC)
            {
                sourcefile = nextfunctionSC_sourcefile;
                sourceline = nextfunctionSC_sourceline;
                nextfunctionSC_sourcefile = "";
                nextfunctionSC_sourceline = -1;
                nextfunctionSC();
                nextfunctionSC = nullptr;
                functionDone.notify_all();
                r |= Waiter::NEEDEXEC;
            }
            client.waiter->bumpds();
            auto end = client.waiter->ds;
            if (end - start > 200)
            {
                // note that in Debug builds (for windows at least), prep for logging in can take 15 seconds in pbkdf2.DeriveKey
                LOG_err << "test functions passed to be executed on the client thread should queue work but not wait for the results themselves. Waited ms: "
                        << end-start << " in " << sourcefile << " line " << sourceline;
                //assert(false);
            }
        }

        client.waiter->bumpds();
        dstime t5 = client.waiter->ds;
        if (t5 - t4 > 20) LOG_debug << "injected functions took ds: " << t5 - t4;

        if ((r & Waiter::NEEDEXEC))
        {
            client.exec();
        }

        client.waiter->bumpds();
        dstime t6 = client.waiter->ds;
        if (t6 - t5 > 20) LOG_debug << "exec took ds: " << t6 - t5;

    }

    // shut down on the same thread, otherwise any ongoing async I/O fails to complete (on windows)
    client.locallogout(false, true);

    out() << clientname << " thread exiting naturally";
}
catch (std::exception& e)
{
    out() << clientname << " thread exception, StandardClient " << clientname << " terminated: " << e.what();
}
catch (...)
{
    out() << clientname << " thread exception, StandardClient " << clientname << " terminated";
}

void StandardClient::preloginFromEnv(const string& userenv, PromiseBoolSP pb)
{
    string user = getenv(userenv.c_str());

    ASSERT_FALSE(user.empty());

    resultproc.prepresult(PRELOGIN, ++next_request_tag,
        [&](){ client.prelogin(user.c_str()); },
        [pb](error e) { pb->set_value(!e); return true; });

}

void StandardClient::loginFromEnv(const string& userenv, const string& pwdenv, PromiseBoolSP pb)
{
    string user = getenv(userenv.c_str());
    string pwd = getenv(pwdenv.c_str());

    ASSERT_FALSE(user.empty());
    ASSERT_FALSE(pwd.empty());

    ::mega::byte pwkey[SymmCipher::KEYLENGTH];

    resultproc.prepresult(LOGIN, ++next_request_tag,
        [&](){
            if (client.accountversion == 1)
            {
                if (error e = client.pw_key(pwd.c_str(), pwkey))
                {
                    ASSERT_TRUE(false) << "login error: " << e;
                }
                else
                {
                    client.login(user.c_str(), pwkey);
                }
            }
            else if (client.accountversion == 2 && !salt.empty())
            {
                client.login2(user.c_str(), pwd.c_str(), &salt);
            }
            else
            {
                ASSERT_TRUE(false) << "Login unexpected error";
            }
        },
        [pb](error e) { pb->set_value(!e); return true; });

}

void StandardClient::loginFromSession(const string& session, PromiseBoolSP pb)
{
    resultproc.prepresult(LOGIN, ++next_request_tag,
        [&](){ client.login(session); },
        [pb](error e) { pb->set_value(!e);  return true; });
}

#if defined(MEGA_MEASURE_CODE) || defined(DEBUG)
void StandardClient::sendDeferredAndReset()
{
    auto futureResult = thread_do<bool>([&](StandardClient& sc, PromiseBoolSP pb) {
        client.reqs.deferRequests = nullptr;
        client.reqs.sendDeferred();
        pb->set_value(true);
    }, __FILE__, __LINE__);
    futureResult.get();
}
#endif

bool StandardClient::copy(const CloudItem& source,
                          const CloudItem& target,
                          const string& name,
                          VersioningOption versioningPolicy)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        client.copy(source, target, name, std::move(result), versioningPolicy);
    }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::minutes(2));

    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

bool StandardClient::copy(const CloudItem& source,
                          const CloudItem& target,
                          VersioningOption versioningPolicy)
{
    return copy(source, target, string(), versioningPolicy);
}

void StandardClient::copy(const CloudItem& source,
                          const CloudItem& target,
                          string name,
                          PromiseBoolSP result,
                          VersioningOption versioningPolicy)
{
    auto* sourceNode = source.resolve(*this);
    EXPECT_TRUE(sourceNode);

    if (!sourceNode)
        return result->set_value(false);

    auto* targetNode = target.resolve(*this);
    EXPECT_TRUE(targetNode);

    if (!targetNode || targetNode->type == FILENODE)
        return result->set_value(false);

    // Make sure name always contains something valid.
    if (name.empty())
        name = sourceNode->displayname();

    // Make sure name is normalized.
    LocalPath::utf8_normalize(&name);

    TreeProcCopy proc;

    // Figure out how many nodes we need to copy.
    client.proctree(sourceNode, &proc, false, true);

    // Allocate and populate nodes.
    proc.allocnodes();

    client.proctree(sourceNode, &proc, false, true);

    // We need the original node's handle if we're using versioning.
    Node* victimNode = nullptr;

    if (versioningPolicy != NoVersioning)
        victimNode = client.childnodebyname(targetNode, name.c_str(), true);

    if (victimNode)
        proc.nn[0].ovhandle = victimNode->nodeHandle();

    proc.nn[0].parenthandle = UNDEF;

    // Populate attributes of copied node.
    {
        SymmCipher key;

        // Load key.
        key.setkey((const ::mega::byte*)proc.nn[0].nodekey.data(), sourceNode->type);

        // Copy existing attributes.
        AttrMap attrs = sourceNode->attrs;

        // Upate the node's name.
        attrs.map['n'] = std::move(name);

        // Generate attribute string.
        string attrstring;

        attrs.getjson(&attrstring);

        // Update node's attribute string.
        client.makeattr(&key, proc.nn[0].attrstring, attrstring.c_str());
    }

    auto completion = [=](const Error& error) {
        LOG_debug << "Putnodes request completed: "
                  << error;

        EXPECT_EQ(error, API_OK);
        result->set_value(error == API_OK);
    };

    LOG_debug << "Scheduling putnodes request now...";

    client.putnodes(targetNode->nodeHandle(),
                    versioningPolicy,
                    std::move(proc.nn),
                    nullptr,
                    0,
                    false,
                    BasicPutNodesCompletion(std::move(completion)));
}

bool StandardClient::putnodes(const CloudItem& parent,
                              VersioningOption versioningPolicy,
                              std::vector<NewNode>&& nodes)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        client.putnodes(parent,
                        versioningPolicy,
                        std::move(nodes),
                        std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(40));

    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

void StandardClient::putnodes(const CloudItem& parent,
                              VersioningOption versioningPolicy,
                              std::vector<NewNode>&& nodes,
                              PromiseBoolSP result)
{
    auto* node = parent.resolve(*this);
    EXPECT_TRUE(node);

    if (!node)
        return result->set_value(false);

    auto completion = BasicPutNodesCompletion([result](const Error& e) {
        LOG_debug << "Putnodes request completed: "
                  << e;

        EXPECT_EQ(e, API_OK);
        result->set_value(e == API_OK);
    });

    LOG_debug << "Scheduling putnodes request now...";

    client.putnodes(node->nodeHandle(),
                    versioningPolicy,
                    std::move(nodes),
                    nullptr,
                    0,
                    false,
                    std::move(completion));
}

void StandardClient::uploadFolderTree_recurse(handle parent, handle& h, const fs::path& p, vector<NewNode>& newnodes)
{
    NewNode n;
    client.putnodes_prepareOneFolder(&n, p.filename().u8string(), false);
    handle thishandle = n.nodehandle = h++;
    n.parenthandle = parent;
    newnodes.emplace_back(std::move(n));

    for (fs::directory_iterator i(p); i != fs::directory_iterator(); ++i)
    {
        if (fs::is_directory(*i))
        {
            uploadFolderTree_recurse(thishandle, h, *i, newnodes);
        }
    }
}

void StandardClient::uploadFolderTree(fs::path p, Node* n2, PromiseBoolSP pb)
{
    auto completion = BasicPutNodesCompletion([pb](const Error& e) {
        pb->set_value(!e);
    });

    resultproc.prepresult(COMPLETION, ++next_request_tag,
        [&](){
            vector<NewNode> newnodes;
            handle h = 1;
            uploadFolderTree_recurse(UNDEF, h, p, newnodes);
            client.putnodes(n2->nodeHandle(), NoVersioning, std::move(newnodes), nullptr, 0, false, std::move(completion));
        },
        nullptr);
}

void StandardClient::downloadFile(const CloudItem& item, const fs::path& destination, PromiseBoolSP result)
{
    auto* node = item.resolve(*this);
    if (!node)
        return result->set_value(false);

    unique_ptr<FileGet> file(new FileGet());

    file->h = node->nodeHandle();
    file->hprivate = true;
    file->setLocalname(LocalPath::fromAbsolutePath(destination.u8string()));
    file->name = node->displayname();
    file->result = std::move(result);

    reinterpret_cast<FileFingerprint&>(*file) = *node;

    TransferDbCommitter committer(client.tctable);

    error r = API_OK;

    client.startxfer(GET, file.get(), committer, false, false, false, NoVersioning, &r, client.nextreqtag());
    EXPECT_EQ(r , API_OK);

    if (r != API_OK)
        return file->result->set_value(false);

    file.release();
}

bool StandardClient::downloadFile(const CloudItem& item, const fs::path& destination)
{
    auto result =
        thread_do<bool>([&](StandardClient& client, PromiseBoolSP result)
                        {
                            client.downloadFile(item, destination, result);
                        }, __FILE__, __LINE__);

    auto status = result.wait_for(DEFAULTWAIT);

    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

bool StandardClient::uploadFolderTree(fs::path p, Node* n2)
{
    auto promise = makeSharedPromise<bool>();
    auto future = promise->get_future();

    uploadFolderTree(p, n2, std::move(promise));

    return future.get();
}

void StandardClient::uploadFile(const fs::path& path, const string& name, const Node* parent, TransferDbCommitter& committer, std::function<void(bool)>&& completion, VersioningOption vo)
{
    unique_ptr<File> file(new FilePut(std::move(completion)));

    file->h = parent->nodeHandle();
    file->setLocalname(LocalPath::fromAbsolutePath(path.u8string()));
    file->name = name;

    error result = API_OK;
    client.startxfer(PUT, file.release(), committer, false, false, false, vo, &result, client.nextreqtag());
    EXPECT_EQ(result, API_OK);
}

void StandardClient::uploadFile(const fs::path& path, const string& name, const Node* parent, std::function<void(bool)>&& completion, VersioningOption vo)
{
    resultproc.prepresult(COMPLETION,
                            ++next_request_tag,
                            [&]()
                            {
                                TransferDbCommitter committer(client.tctable);
                                uploadFile(path, name, parent, committer, std::move(completion), vo);
                            },
                            nullptr);
}

bool StandardClient::uploadFile(const fs::path& path, const string& name, const CloudItem& parent, int timeoutSeconds, VersioningOption vo)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP pb) {
        auto* parentNode = parent.resolve(client);
        if (!parentNode)
            return pb->set_value(false);

        client.uploadFile(path, name, parentNode, [pb](bool b){ pb->set_value(b); }, vo);
    }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(timeoutSeconds));
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

bool StandardClient::uploadFile(const fs::path& path, const CloudItem& parent, int timeoutSeconds, VersioningOption vo)
{
    return uploadFile(path, path.filename().u8string(), parent, timeoutSeconds, vo);
}

void StandardClient::uploadFilesInTree_recurse(const Node* target, const fs::path& p, std::atomic<int>& inprogress, TransferDbCommitter& committer, VersioningOption vo)
{
    if (fs::is_regular_file(p))
    {
        ++inprogress;
        uploadFile(p, p.filename().u8string(), target, committer, [&inprogress](bool){ --inprogress; }, vo);
    }
    else if (fs::is_directory(p))
    {
        if (auto newtarget = client.childnodebyname(target, p.filename().u8string().c_str()))
        {
            for (fs::directory_iterator i(p); i != fs::directory_iterator(); ++i)
            {
                uploadFilesInTree_recurse(newtarget, *i, inprogress, committer, vo);
            }
        }
    }
}

bool StandardClient::uploadFilesInTree(fs::path p, const CloudItem& n2, VersioningOption vo)
{
    std::atomic_int inprogress(0);

    Node* targetNode = nullptr;
    {
        lock_guard<recursive_mutex> guard(clientMutex);
        targetNode = n2.resolve(*this);
    }

    // The target node should always exist.
    EXPECT_TRUE(targetNode);

    if (!targetNode && !inprogress)
        return false;

    auto startedFlag =
        thread_do<bool>([&inprogress, this, targetNode, p, vo](StandardClient& client, PromiseBoolSP result)
                        {
                            TransferDbCommitter committer(client.client.tctable);
                            uploadFilesInTree_recurse(targetNode, p, inprogress, committer, vo);
                            result->set_value(true);
                        }, __FILE__, __LINE__);

    startedFlag.get();

    // 30 seconds, that doesn't immediately time out if you stop in the debugger for a while
    for (int i = 0; i < 300 && inprogress.load(); ++i)
    {
        WaitMillisec(100);
    }

    EXPECT_EQ(inprogress.load(), 0) << "Failed to uploadFilesInTree within 30 seconds";
    return inprogress.load() == 0;
}

void StandardClient::uploadFile(const fs::path& sourcePath,
                                const string& targetName,
                                const CloudItem& parent,
                                std::function<void(error)> completion,
                                const VersioningOption versioningPolicy)
{
    struct Put : public File {
        void completed(Transfer* transfer, putsource_t source)
        {
            // Sanity.
            assert(source == PUTNODES_APP);

            // For purposes of capturing.
            std::function<void(error)> completion = std::move(mCompletion);

            // So we can hook the result of putnodes.
            auto trampoline = [completion](const Error& result,
                                           targettype_t,
                                           vector<NewNode>&,
                                           bool, int tag) {
                EXPECT_EQ(result, API_OK);
                completion(result);
            };

            // Kick off the putnodes request.
            sendPutnodesOfUpload(transfer->client,
                         transfer->uploadhandle,
                         *transfer->ultoken,
                         transfer->filekey,
                         source,
                         NodeHandle(),
                         std::move(trampoline),
                         nullptr,
                         nullptr,
                         false);    // it's a putnodes from app, not from a sync

            // Destroy ourselves.
            delete this;
        }

        void terminated(error result)
        {
            EXPECT_FALSE(true);

            // Let the completion function know we've failed.
            mCompletion(result);

            // Destroy ourselves.
            delete this;
        }

        // Who to call when the upload completes.
        std::function<void(error)> mCompletion;
    }; // Put

    // Make sure we have exclusive access to the client.
    lock_guard<recursive_mutex> guard(clientMutex);

    auto* parentNode = parent.resolve(*this);
    if (!parentNode)
        return completion(API_ENOENT);

    // Create a file to represent and track our upload.
    auto file = ::mega::make_unique<Put>();

    // Populate necessary fields.
    file->h = parentNode->nodeHandle();
    file->mCompletion = std::move(completion);
    file->name = targetName;
    file->setLocalname(LocalPath::fromAbsolutePath(sourcePath.u8string()));

    // Kick off the upload. Client takes ownership of file.
    TransferDbCommitter committer(client.tctable);

    error result = API_OK;

    client.startxfer(PUT,
                     file.get(),
                     committer,
                     false,
                     false,
                     false,
                     versioningPolicy,
                     &result, client.nextreqtag());

    EXPECT_EQ(result, API_OK);

    if (result != API_OK)
        return file->mCompletion(result);

    file.release();
}

void StandardClient::uploadFile(const fs::path& sourcePath,
                                const CloudItem& parent,
                                std::function<void(error)> completion,
                                const VersioningOption versioningPolicy)
{
    uploadFile(sourcePath,
               sourcePath.filename().u8string(),
               parent,
               std::move(completion),
               versioningPolicy);
}

void StandardClient::fetchnodes(bool noCache, PromiseBoolSP pb)
{
    resultproc.prepresult(FETCHNODES, ++next_request_tag,
        [&](){ client.fetchnodes(noCache); },
        [this, pb](error e)
        {
            if (e)
            {
                pb->set_value(false);
            }
            else
            {
                TreeProcPrintTree tppt;
                client.proctree(client.nodeByHandle(client.mNodeManager.getRootNodeFiles()), &tppt);

                if (onFetchNodes)
                {
                    onFetchNodes(*this, pb);
                }
                else
                {
                    pb->set_value(true);
                }
            }
            onFetchNodes = nullptr;
            return true;
        });
}

bool StandardClient::fetchnodes(bool noCache)
{
    auto result =
        thread_do<bool>([=](StandardClient& client, PromiseBoolSP result)
                        {
                            client.fetchnodes(noCache, result);
                        }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(180));
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
    {
        LOG_warn << "Timed out waiting for fetchnodes";
        return false;
    }

    return result.get();
}

NewNode StandardClient::makeSubfolder(const string& utf8Name)
{
    NewNode newnode;
    client.putnodes_prepareOneFolder(&newnode, utf8Name, false);
    return newnode;
}

void StandardClient::catchup(std::function<void(error)> completion)
{
    auto init = std::bind(&MegaClient::catchup, &client);

    auto fini = [completion](error e) {
        LOG_debug << "catchup(...) request completed: "
                  << e;

        EXPECT_EQ(e, API_OK);
        if (e)
            out() << "catchup reports: " << e;

        LOG_debug << "Calling catchup(...) completion function...";

        completion(e);

        return true;
    };

    LOG_debug << "Sending catchup(...) request...";

    resultproc.prepresult(CATCHUP,
                          ++next_request_tag,
                          std::move(init),
                          std::move(fini));
}

void StandardClient::catchup(PromiseBoolSP pb)
{
    catchup([pb](error e) { pb->set_value(!e); });
}

unsigned StandardClient::deleteTestBaseFolder(bool mayNeedDeleting)
{
    auto result = thread_do<unsigned>([=](StandardClient& client, PromiseUnsignedSP result) {
        client.deleteTestBaseFolder(mayNeedDeleting, false, std::move(result));
    }, __FILE__, __LINE__);

    return result.get();
}

void StandardClient::deleteTestBaseFolder(bool mayNeedDeleting, bool deleted, PromiseUnsignedSP result)
{
    if (Node* root = client.nodeByHandle(client.mNodeManager.getRootNodeFiles()))
    {
        if (Node* basenode = client.childnodebyname(root, "mega_test_sync", false))
        {
            if (mayNeedDeleting)
            {
                auto completion = [this, result](NodeHandle, Error e) {
                    EXPECT_EQ(e, API_OK);
                    if (e) out() << "delete of test base folder reply reports: " << e;
                    deleteTestBaseFolder(false, true, result);
                };

                resultproc.prepresult(COMPLETION, ++next_request_tag,
                    [&](){ client.unlink(basenode, false, 0, false, std::move(completion)); },
                    nullptr);
                return;
            }
            out() << "base folder found, but not expected, failing";
            result->set_value(0);
            return;
        }
        else
        {
            //out() << "base folder not found, wasn't present or delete successful";
            result->set_value(deleted ? 2 : 1);
            return;
        }
    }
    out() << "base folder not found, as root was not found!";
    result->set_value(0);
}

void StandardClient::ensureTestBaseFolder(bool mayneedmaking, PromiseBoolSP pb)
{
    if (Node* root = client.nodeByHandle(client.mNodeManager.getRootNodeFiles()))
    {
        if (Node* basenode = client.childnodebyname(root, "mega_test_sync", false))
        {
            out() << clientname << "ensureTestBaseFolder node found";
            if (basenode->type == FOLDERNODE)
            {
                basefolderhandle = basenode->nodehandle;
                //out() << clientname << " Base folder: " << Base64Str<MegaClient::NODEHANDLE>(basefolderhandle);
                //parentofinterest = Base64Str<MegaClient::NODEHANDLE>(basefolderhandle);
                out() << clientname << "ensureTestBaseFolder ok";
                pb->set_value(true);
                return;
            }
        }
        else if (mayneedmaking)
        {
            vector<NewNode> nn(1);
            nn[0] = makeSubfolder("mega_test_sync");

            resultproc.prepresult(PUTNODES, ++next_request_tag,
                [&](){
                    client.putnodes(root->nodeHandle(), NoVersioning, std::move(nn), nullptr, client.reqtag, false, nullptr);
                },
                [pb, this](error e){
                    out() << clientname << "ensureTestBaseFolder putnodes completed with: " << e;
                    ensureTestBaseFolder(false, pb);
                    return true;
                });
            out() << clientname << "ensureTestBaseFolder sending putnodes";
            return;
        }
        out() << clientname << "ensureTestBaseFolder unexpected case"; // but can occur if we look too early because a late actionpacket from prior tests made us think it was time to check
    }
    else {
        out() << clientname << "no file root handle";
    }
    pb->set_value(false);
}

NewNode* StandardClient::buildSubdirs(list<NewNode>& nodes, const string& prefix, int n, int recurselevel)
{
    nodes.emplace_back(makeSubfolder(prefix));
    auto& nn = nodes.back();
    nn.nodehandle = nodes.size();

    if (recurselevel > 0)
    {
        for (int i = 0; i < n; ++i)
        {
            buildSubdirs(nodes, prefix + "_" + to_string(i), n, recurselevel - 1)->parenthandle = nn.nodehandle;
        }
    }

    return &nn;
}

bool StandardClient::makeCloudSubdirs(const string& prefix, int depth, int fanout)
{
    auto result =
        thread_do<bool>([=](StandardClient& client, PromiseBoolSP result)
                        {
                            client.makeCloudSubdirs(prefix, depth, fanout, result);
                        }, __FILE__, __LINE__);

    return result.get();
}

void StandardClient::makeCloudSubdirs(const string& prefix, int depth, int fanout, PromiseBoolSP pb, const string& atpath)
{
    assert(basefolderhandle != UNDEF);

    std::list<NewNode> nodes;
    NewNode* nn = buildSubdirs(nodes, prefix, fanout, depth);
    nn->parenthandle = UNDEF;
    nn->ovhandle = NodeHandle();

    Node* atnode = client.nodebyhandle(basefolderhandle);
    if (atnode && !atpath.empty())
    {
        atnode = drillchildnodebyname(atnode, atpath);
    }
    if (!atnode)
    {
        out() << "path not found: " << atpath;
        pb->set_value(false);
    }
    else
    {
        auto nodearray = vector<NewNode>(nodes.size());
        size_t i = 0;
        for (auto n = nodes.begin(); n != nodes.end(); ++n, ++i)
        {
            nodearray[i] = std::move(*n);
        }

        auto completion = [pb, this](const Error& e, targettype_t, vector<NewNode>& nodes, bool, int tag) {
            lastPutnodesResultFirstHandle = nodes.empty() ? UNDEF : nodes[0].mAddedHandle;
            pb->set_value(!e);
        };

        int tag = ++next_request_tag;
        resultproc.prepresult(COMPLETION, tag,
            [&]() {
                client.putnodes(atnode->nodeHandle(), NoVersioning, std::move(nodearray), nullptr, tag, false, std::move(completion));
            },
            nullptr);
    }
}

SyncConfig StandardClient::syncConfigByBackupID(handle backupID) const
{
    SyncConfig c;
    bool found = client.syncs.syncConfigByBackupId(backupID, c);
    if (!found)
        assert(found);

    return c;
}

bool StandardClient::syncSet(handle backupId, SyncInfo& info) const
{
    SyncConfig c;

    auto found = client.syncs.syncConfigByBackupId(backupId, c);
    EXPECT_TRUE(found)
      << "Unable to find sync with backup ID: "
      << toHandle(backupId);

    if (found)
    {
        info.h = c.mRemoteNode;
        info.localpath = c.getLocalPath().toPath(false);
        info.remotepath = c.mOriginalPathOfRemoteRootNode; // bit of a hack

        return true;
    }

    return false;
}

StandardClient::SyncInfo StandardClient::syncSet(handle backupId)
{
    SyncInfo result;

    out() << "looking up BackupId " << toHandle(backupId);

    bool found = syncSet(backupId, result);
    if (!found)
        assert(found);

    return result;
}

StandardClient::SyncInfo StandardClient::syncSet(handle backupId) const
{
    return const_cast<StandardClient&>(*this).syncSet(backupId);
}

Node* StandardClient::getcloudrootnode()
{
    return client.nodeByHandle(client.mNodeManager.getRootNodeFiles());
}

Node* StandardClient::gettestbasenode()
{
    return client.childnodebyname(getcloudrootnode(), "mega_test_sync", false);
}

Node* StandardClient::getcloudrubbishnode()
{
    return client.nodeByHandle(client.mNodeManager.getRootNodeRubbish());
}

Node* StandardClient::getsyncdebrisnode()
{
    return drillchildnodebyname(getcloudrubbishnode(), "SyncDebris");
}

Node* StandardClient::drillchildnodebyname(Node* n, const string& path)
{
    for (size_t p = 0; n && p < path.size(); )
    {
        auto pos = path.find("/", p);
        if (pos == string::npos) pos = path.size();
        n = client.childnodebyname(n, path.substr(p, pos - p).c_str(), false);
        p = pos == string::npos ? path.size() : pos + 1;
    }
    return n;
}

vector<Node*> StandardClient::drillchildnodesbyname(Node* n, const string& path)
{
    auto pos = path.find("/");
    if (pos == string::npos)
    {
        return client.childnodesbyname(n, path.c_str(), false);
    }
    else
    {
        vector<Node*> results, subnodes = client.childnodesbyname(n, path.c_str(), false);
        for (size_t i = subnodes.size(); i--; )
        {
            if (subnodes[i]->type != FILENODE)
            {
                vector<Node*> v = drillchildnodesbyname(subnodes[i], path.substr(pos + 1));
                results.insert(results.end(), v.begin(), v.end());
            }
        }
        return results;
    }
}

handle StandardClient::setupBackup_mainthread(const string& rootPath)
{
    SyncOptions options;

    options.drivePath = string(1, '\0');
    options.isBackup = true;
    options.uploadIgnoreFile = false;

    return setupBackup_mainthread(rootPath, options);
}

handle StandardClient::setupBackup_mainthread(const string& rootPath,
                                            const SyncOptions& syncOptions)
{
    auto result = thread_do<handle>([&](StandardClient& client, PromiseHandleSP result) {
        client.setupBackup_inThread(rootPath, syncOptions, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(chrono::seconds(45));

    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return UNDEF;

    return result.get();
}

void StandardClient::setupBackup_inThread(const string& rootPath,
                                        const SyncOptions& syncOptions,
                                        PromiseHandleSP result)
{
    auto ec = std::error_code();
    auto rootPath_ = fsBasePath / fs::u8path(rootPath);

    // Try and create the local sync root.
    fs::create_directories(rootPath_, ec);
    EXPECT_FALSE(ec);

    if (ec)
        return result->set_value(UNDEF);

    fs::path excludePath_;

    // Translate exclude path if necessary.
    if (!syncOptions.excludePath.empty())
    {
        excludePath_ = fsBasePath / fs::u8path(syncOptions.excludePath);
    }

    // Create a suitable sync config.
    SyncConfig config(LocalPath::fromAbsolutePath(rootPath_.u8string()),
            rootPath,
            NodeHandle(),
            string(),
            0,
            LocalPath(),
            true,
            SyncConfig::TYPE_BACKUP);


    client.preparebackup(config, [result, this](Error err, SyncConfig sc, MegaClient::UndoFunction revertOnError){

        if (err != API_OK)
        {
            result->set_value(UNDEF);
        }
        else
        {
            client.addsync(std::move(sc), false, [revertOnError, result, this](error e, SyncError se, handle h){
                if (e && revertOnError) revertOnError(nullptr);
                result->set_value(e ? UNDEF : h);

            }, "");
        }
    });
}

handle StandardClient::setupSync_mainthread(const string& rootPath,
                                            const CloudItem& remoteItem,
                                            const bool isBackup,
                                            const bool uploadIgnoreFile,
                                            const string& drivePath)
{
    SyncOptions options;

    options.drivePath = drivePath;
    options.isBackup = isBackup;
    options.uploadIgnoreFile = uploadIgnoreFile;

    return setupSync_mainthread(rootPath, remoteItem, options);
}

handle StandardClient::setupSync_mainthread(const string& rootPath,
                                            const CloudItem& remoteItem,
                                            const SyncOptions& syncOptions)
{
    auto result = thread_do<handle>([&](StandardClient& client, PromiseHandleSP result) {
        client.setupSync_inThread(rootPath, remoteItem, syncOptions, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(chrono::seconds(45));

    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return UNDEF;

    return result.get();
}

void StandardClient::setupSync_inThread(const string& rootPath,
                                        const CloudItem& remoteItem,
                                        const SyncOptions& syncOptions,
                                        PromiseHandleSP result)
{
    // Helpful sentinel.
    static const string internalDrive(1, '\0');

    // Check if node is (or is contained by) an in-share.
    auto isShare = [](const Node* node) {
        for ( ; node; node = node->parent) {
            if (node->type != FOLDERNODE)
                continue;

            if (node->inshare)
                return true;
        }

        return false;
    };

    auto* remoteNode = remoteItem.resolve(*this);
    EXPECT_TRUE(remoteNode);

    if (!remoteNode)
        return result->set_value(UNDEF);

    auto ec = std::error_code();
    auto rootPath_ = fsBasePath / fs::u8path(rootPath);

    // Try and create the local sync root.
    fs::create_directories(rootPath_, ec);
    EXPECT_FALSE(ec);

    if (ec)
        return result->set_value(UNDEF);

    fs::path drivePath_;

    // Populate drive root if necessary.
    if (syncOptions.drivePath != internalDrive)
    {
        // Path should be valid as syncs must be contained by their drive.
        drivePath_ = fsBasePath / fs::u8path(syncOptions.drivePath);

        // Read drive ID if present...
        auto fsAccess = client.fsaccess.get();
        auto id = UNDEF;
        auto path = drivePath_.u8string();
        auto result_ = readDriveId(*fsAccess, path.c_str(), id);

        // Generate one if not...
        if (result_ == API_ENOENT)
        {
            id = generateDriveId(client.rng);
            result_ = writeDriveId(*fsAccess, path.c_str(), id);
        }

        EXPECT_EQ(result_, API_OK);
    }


    // For purposes of capturing.
    auto isBackup = syncOptions.isBackup;
    auto remoteHandle = remoteNode->nodeHandle();
    auto remoteIsShare = isShare(remoteNode);
    auto remotePath = string(remoteNode->displaypath());

    // Called when it's time to actually add the sync.
    auto completion = [=](error e) {
        LOG_debug << "Starting to add sync: "
                  << e;

        // Make sure our caller completed successfully.
        EXPECT_EQ(e, API_OK);

        if (e != API_OK)
            return result->set_value(UNDEF);

        // Convenience.
        constexpr auto BACKUP = SyncConfig::TYPE_BACKUP;
        constexpr auto TWOWAY = SyncConfig::TYPE_TWOWAY;

        // Generate config for the new sync.
        auto config =
          SyncConfig(LocalPath::fromAbsolutePath(rootPath_.u8string()),
                     rootPath_.u8string(),
                     remoteHandle,
                     remotePath,
                     0,
                     LocalPath(),
                     true,
                     isBackup ? BACKUP : TWOWAY);

        // Sanity check.
        EXPECT_TRUE(remoteIsShare || remotePath.substr(0, 1) == "/")
            << "config.mOriginalPathOfRemoteRootNode: "
            << remotePath;

        // Are we dealing with an external backup sync?
        if (!drivePath_.empty())
        {
            // Then make sure we specify where the external drive can be found.
            config.mExternalDrivePath =
              LocalPath::fromAbsolutePath(drivePath_.u8string());
        }

        //if (gScanOnly)
        //{
        //    config.mChangeDetectionMethod = CDM_PERIODIC_SCANNING;
        //    config.mScanIntervalSec = SCAN_INTERVAL_SEC;
        //}

        auto completion = [result](error e, SyncError se, handle id) {
            EXPECT_EQ(e, API_OK);
            EXPECT_NE(id, UNDEF);
            EXPECT_EQ(se, NO_SYNC_ERROR);

            result->set_value(id);
        };

        LOG_debug << "Asking engine to add the sync...";

        LOG_debug << "Local sync root will be: "
                  << config.mLocalPath.toPath(false);

        if (!drivePath_.empty())
        {
            LOG_debug << "External drive will be: "
                      << config.mExternalDrivePath.toPath(false);
        }

        client.addsync(std::move(config),
                       true,
                       std::move(completion),
                       rootPath + " ");
    };

    // Do we need to upload an ignore file?
#ifdef SRW_NEEDED_FOR_THIS_ONE
    if (syncOptions.uploadIgnoreFile)
    {
        auto ignorePath = fsBasePath / ".megaignore";

        // Create the ignore file.
        auto created = createDataFile(ignorePath, "#");
        EXPECT_TRUE(created);

        if (!created)
            return result->set_value(UNDEF);

        LOG_debug << "Uploading initial megaignore file...";

        // Upload the ignore file.
        uploadFile(ignorePath, remoteNode, std::move(completion));

        // Completion function will continue the work.
        return;
    }
#endif

    LOG_debug << "Making sure we've received latest cloud changes...";

    // Make sure the client's received all its action packets.
    //catchup(std::move(completion));
    WaitMillisec(1000);
    completion(API_OK);
    WaitMillisec(1000);
}

void StandardClient::importSyncConfigs(string configs, PromiseBoolSP result)
{
    auto completion = [result](error e) { result->set_value(!e); };
    client.importSyncConfigs(configs.c_str(), std::move(completion));
}

bool StandardClient::importSyncConfigs(string configs)
{
    auto result =
        thread_do<bool>([=](StandardClient& client, PromiseBoolSP result)
                        {
                            client.importSyncConfigs(configs, result);
                        }, __FILE__, __LINE__);

    return result.get();
}

string StandardClient::exportSyncConfigs()
{
    auto result =
        thread_do<string>([](MegaClient& client, PromiseStringSP result)
                        {
                            auto configs = client.syncs.exportSyncConfigs();
                            result->set_value(configs);
                        }, __FILE__, __LINE__);

    return result.get();
}

void StandardClient::delSync_inthread(handle backupId, PromiseBoolSP result)
{
    client.syncs.deregisterThenRemoveSync(backupId,
      [=](Error error) { result->set_value(error == API_OK); }, false);
}

bool StandardClient::recursiveConfirm(Model::ModelNode* mn, Node* n, int& descendants, const string& identifier, int depth, bool& firstreported, bool expectFail, bool skipIgnoreFile)
{
    // top level names can differ so we don't check those
    if (!mn || !n) return false;

    if (depth)
    {
        if (!CloudNameLess().equal(mn->cloudName(), n->displayname()))
        {
            out() << "Node name mismatch: " << mn->path() << " " << n->displaypath();
            return false;
        }
    }

    if (!mn->typematchesnodetype(n->type))
    {
        out() << "Node type mismatch: " << mn->path() << ":" << mn->type << " " << n->displaypath() << ":" << n->type;
        return false;
    }

    if (n->type == FILENODE)
    {
        // not comparing any file versioning (for now)
        return true;
    }

    multimap<string, Model::ModelNode*, CloudNameLess> ms;
    multimap<string, Node*, CloudNameLess> ns;
    for (auto& m : mn->kids)
    {
        if (!m->fsOnly)
        ms.emplace(m->cloudName(), m.get());
    }
    for (auto& n2 : client.getChildren(n))
    {
        ns.emplace(n2->displayname(), n2);
    }

    int matched = 0;
    vector<string> matchedlist;
    for (auto m_iter = ms.begin(); m_iter != ms.end(); )
    {
        if (!depth && m_iter->first == DEBRISFOLDER)
        {
            m_iter = ms.erase(m_iter); // todo: add checks of the remote debris folder later
            continue;
        }

        auto er = ns.equal_range(m_iter->first);
        auto next_m = m_iter;
        ++next_m;
        bool any_equal_matched = false;
        for (auto i = er.first; i != er.second; ++i)
        {
            int rdescendants = 0;
            if (recursiveConfirm(m_iter->second, i->second, rdescendants, identifier, depth+1, firstreported, expectFail, skipIgnoreFile))
            {
                ++matched;
                matchedlist.push_back(m_iter->first);
                ns.erase(i);
                ms.erase(m_iter);
                descendants += rdescendants;
                any_equal_matched = true;
                break;
            }
        }
        if (!any_equal_matched)
        {
            break;
        }
        m_iter = next_m;
    }
    if (ns.empty() && ms.empty())
    {
        descendants += matched;
        return true;
    }
    else if (!firstreported && !expectFail)
    {
        ostringstream ostream;
        firstreported = true;
        ostream << clientname << " " << identifier << " after matching " << matched << " child nodes [";
        for (auto& ml : matchedlist) ostream << ml << " ";
        ostream << "](with " << descendants << " descendants) in " << mn->path() << ", ended up with unmatched model nodes:";
        for (auto& m : ms) ostream << " " << m.first;
        ostream << " and unmatched remote nodes:";
        for (auto& i : ns) ostream << " " << i.first;
        out() << ostream.str();
        EXPECT_TRUE(false) << ostream.str();
    };
    return false;
}

auto StandardClient::equal_range_utf8EscapingCompare(multimap<string, LocalNode*, CloudNameLess>& ns, const string& cmpValue, bool unescapeValue, bool unescapeMap, bool caseInsensitive) -> std::pair<multimap<string, LocalNode*>::iterator, multimap<string, LocalNode*>::iterator>
{
    // first iter not less than cmpValue
    auto iter1 = ns.begin();
    while (iter1 != ns.end() && compareUtf(iter1->first, unescapeMap, cmpValue, unescapeValue, caseInsensitive) < 0) ++iter1;

    // second iter greater then cmpValue
    auto iter2 = iter1;
    while (iter2 != ns.end() && compareUtf(iter2->first, unescapeMap, cmpValue, unescapeValue, caseInsensitive) <= 0) ++iter2;

    return {iter1, iter2};
}

bool StandardClient::recursiveConfirm(Model::ModelNode* mn, LocalNode* n, int& descendants, const string& identifier, int depth, bool& firstreported, bool expectFail, bool skipIgnoreFile)
{
    // top level names can differ so we don't check those
    if (!mn || !n) return false;

    if (depth)
    {
        if (0 != compareUtf(mn->fsName(), true, n->getLocalname(), true, false))
        {
            out() << "LocalNode name mismatch: " << mn->fsPath() << " " << n->getLocalname().toPath(false);
            return false;
        }
    }

    if (!mn->typematchesnodetype(n->type))
    {
        out() << "LocalNode type mismatch: " << mn->fsPath() << ":" << mn->type << " " << n->getLocalname().toPath(false) << ":" << n->type;
        return false;
    }

    auto localpath = n->getLocalPath().toName(*client.fsaccess);
    string n_localname = n->getLocalname().toName(*client.fsaccess);
    if (n_localname.size() && n->parent)  // the sync root node's localname contains an absolute path, not just the leaf name.  Also the filesystem sync root folder and cloud sync root folder don't have to have the same name.
    {
        EXPECT_EQ(n->name, n_localname);
    }
    if (localNodesMustHaveNodes)
    {
        EXPECT_TRUE(n->node != nullptr);
    }
    Node* syncedNode = n->node;
    if (depth && syncedNode)
    {
        EXPECT_EQ(compareUtf(mn->cloudName(), false, syncedNode->displayname(), false, false), 0)
            << "Localnode's associated Node vs model node name mismatch: '"
            << syncedNode->displayname()
            << "', '"
            << mn->cloudName()
            << "'";
    }
    if (depth && mn->parent)
    {
        EXPECT_EQ(mn->parent->type, Model::ModelNode::folder);
        EXPECT_EQ(n->parent->type, FOLDERNODE);

        string parentpath = n->parent->getLocalPath().toName(*client.fsaccess);
        EXPECT_EQ(localpath.substr(0, parentpath.size()), parentpath);
    }
    if (n->node && n->parent && n->parent->node)
    {
        string p = n->node->displaypath();
        string pp = n->parent->node->displaypath();
        EXPECT_EQ(p.substr(0, pp.size()), pp);
        EXPECT_EQ(n->parent->node, n->node->parent);
    }

    multimap<string, Model::ModelNode*, CloudNameLess> ms;
    multimap<string, LocalNode*, CloudNameLess> ns;
    for (auto& m : mn->kids)
    {
        ms.emplace(m->cloudName(), m.get());
    }
    for (auto& n2 : n->children)
    {
        if (!n2.second->deleted) ns.emplace(n2.second->name, n2.second); // todo: should LocalNodes marked as deleted actually have been removed by now?
    }

    int matched = 0;
    vector<string> matchedlist;
    for (auto m_iter = ms.begin(); m_iter != ms.end(); )
    {
        if (!depth && m_iter->first == DEBRISFOLDER)
        {
            m_iter = ms.erase(m_iter); // todo: are there LocalNodes representing the trash?
            continue;
        }

        auto er = ns.equal_range(m_iter->first);
        auto next_m = m_iter;
        ++next_m;
        bool any_equal_matched = false;
        for (auto i = er.first; i != er.second; ++i)
        {
            int rdescendants = 0;
            if (recursiveConfirm(m_iter->second, i->second, rdescendants, identifier, depth+1, firstreported, expectFail, skipIgnoreFile))
            {
                ++matched;
                matchedlist.push_back(m_iter->first);
                ns.erase(i);
                ms.erase(m_iter);
                descendants += rdescendants;
                any_equal_matched = true;
                break;
            }
        }
        if (!any_equal_matched)
        {
            break;
        }
        m_iter = next_m;
    }
    if (ns.empty() && ms.empty())
    {
        return true;
    }
    else if (!firstreported && !expectFail)
    {
        ostringstream ostream;
        firstreported = true;
        ostream << clientname << " " << identifier << " after matching " << matched << " child nodes [";
        for (auto& ml : matchedlist) ostream << ml << " ";
        ostream << "](with " << descendants << " descendants) in " << mn->path() << ", ended up with unmatched model nodes:";
        for (auto& m : ms) ostream << " " << m.first;
        ostream << " and unmatched LocalNodes:";
        for (auto& i : ns) ostream << " " << i.first;
        out() << ostream.str();
        EXPECT_TRUE(false) << ostream.str();
    };
    return false;
}


bool StandardClient::recursiveConfirm(Model::ModelNode* mn, fs::path p, int& descendants, const string& identifier, int depth, bool ignoreDebris, bool& firstreported, bool expectFail, bool skipIgnoreFile)
{
    struct Comparator
    {
        bool operator()(const string& lhs, const string& rhs) const
        {
            return compare(lhs, rhs) < 0;
        }

        int compare(const string& lhs, const string& rhs) const
        {
            return compareUtf(lhs, true, rhs, true, false);
        }
    }; // Comparator

    static Comparator comparator;

    if (!mn) return false;

    if (depth)
    {
        if (comparator.compare(p.filename().u8string(), mn->fsName()))
        {
            out() << "filesystem name mismatch: " << mn->path() << " " << p;
            return false;
        }
    }

    nodetype_t pathtype = fs::is_directory(p) ? FOLDERNODE : fs::is_regular_file(p) ? FILENODE : TYPE_UNKNOWN;
    if (!mn->typematchesnodetype(pathtype))
    {
        out() << "Path type mismatch: " << mn->path() << ":" << mn->type << " " << p.u8string() << ":" << pathtype;
        return false;
    }

    if (pathtype == FILENODE && p.filename().u8string() != "lock")
    {
        if (localFSFilesThatMayDiffer.find(p) == localFSFilesThatMayDiffer.end())
        {
            ifstream fs(p, ios::binary);
            std::vector<char> buffer;
            buffer.resize(mn->content.size() + 1024);
            fs.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
            EXPECT_EQ(size_t(fs.gcount()), mn->content.size()) << " file is not expected size " << p;
            EXPECT_TRUE(!memcmp(buffer.data(), mn->content.data(), mn->content.size())) << " file data mismatch " << p;
        }
    }

    if (pathtype != FOLDERNODE)
    {
        return true;
    }

    multimap<string, Model::ModelNode*, Comparator> ms;
    multimap<string, fs::path, Comparator> ps;

    for (auto& m : mn->kids)
    {
        ms.emplace(m->fsName(), m.get());
    }

    for (fs::directory_iterator pi(p); pi != fs::directory_iterator(); ++pi)
    {
        ps.emplace(pi->path().filename().u8string(), pi->path());
    }

    if (ignoreDebris && depth == 0)
    {
        ms.erase(DEBRISFOLDER);
        ps.erase(DEBRISFOLDER);
    }
    else if (depth == 1 && mn->name == DEBRISFOLDER)
    {
        ms.erase("tmp");
        ps.erase("tmp");
    }
    else if (depth == 0)
    {
        // with ignore files, most tests now involve a download somewhere which means debris/tmp is created.
        // it only matters if the content of these differs, absence or empty is effectively the same
        if (ms.find(DEBRISFOLDER) == ms.end())
        {
            auto d = mn->addkid();
            d->name = DEBRISFOLDER;
            d->type = Model::ModelNode::folder;
            ms.emplace(DEBRISFOLDER, d);
        }
        if (ps.find(DEBRISFOLDER) == ps.end())
        {
            auto pdeb = p / fs::path(DEBRISFOLDER);
            fs::create_directory(pdeb);
            ps.emplace(DEBRISFOLDER, pdeb);
        }
    }

    int matched = 0;
    vector<string> matchedlist;
    for (auto m_iter = ms.begin(); m_iter != ms.end(); )
    {
        auto er = ps.equal_range(m_iter->first);
        auto next_m = m_iter;
        ++next_m;
        bool any_equal_matched = false;
        for (auto i = er.first; i != er.second; ++i)
        {
            int rdescendants = 0;
            if (recursiveConfirm(m_iter->second, i->second, rdescendants, identifier, depth+1, ignoreDebris, firstreported, expectFail, skipIgnoreFile))
            {
                ++matched;
                matchedlist.push_back(m_iter->first);
                ps.erase(i);
                ms.erase(m_iter);
                descendants += rdescendants;
                any_equal_matched = true;
                break;
            }
        }
        if (!any_equal_matched)
        {
            break;
        }
        m_iter = next_m;
    }
    //if (ps.size() == 1 && !mn->parent && ps.begin()->first == DEBRISFOLDER)
    //{
    //    ps.clear();
    //}
    if (ps.empty() && ms.empty())
    {
        return true;
    }
    else if (!firstreported && !expectFail)
    {
        ostringstream ostream;
        firstreported = true;
        ostream << clientname << " " << identifier << " after matching " << matched << " child nodes [";
        for (auto& ml : matchedlist) ostream << ml << " ";
        ostream << "](with " << descendants << " descendants) in " << mn->path() << ", ended up with unmatched model nodes:";
        for (auto& m : ms) ostream << " " << m.first;
        ostream << " and unmatched filesystem paths:";
        for (auto& i : ps) ostream << " " << i.second.filename();
        ostream << " in " << p;
        out() << ostream.str();
        EXPECT_TRUE(false) << ostream.str();
    };
    return false;
}

Sync* StandardClient::syncByBackupId(handle backupId)
{
    return client.syncs.runningSyncByBackupIdForTests(backupId);
}

void StandardClient::enableSyncByBackupId(handle id, PromiseBoolSP result, const string& logname)
{
    client.syncs.enableSyncByBackupId(id, false, false, true, true, [result](Error e, SyncError, handle){
            result->set_value(!e);
        }, true, logname);
}

bool StandardClient::enableSyncByBackupId(handle id, const string& logname)
{
    auto result =
        thread_do<bool>([=](StandardClient& client, PromiseBoolSP result)
                        {
                            client.enableSyncByBackupId(id, result, logname);
                        }, __FILE__, __LINE__);

    return result.get();
}

void StandardClient::backupIdForSyncPath(const fs::path& path, PromiseHandleSP result)
{
    auto localPath = LocalPath::fromAbsolutePath(path.u8string());
    auto id = UNDEF;

    client.syncs.forEachSyncConfig(
        [&](const SyncConfig& config)
        {
            if (config.mLocalPath != localPath) return;
            if (id != UNDEF) return;

            id = config.mBackupId;
        });

    result->set_value(id);
}

handle StandardClient::backupIdForSyncPath(fs::path path)
{
    auto result =
        thread_do<handle>([=](StandardClient& client, PromiseHandleSP result)
                        {
                            client.backupIdForSyncPath(path, result);
                        }, __FILE__, __LINE__);

    return result.get();
}

bool StandardClient::confirmModel_mainthread(handle id, Model::ModelNode* mRoot, Node* rRoot, bool expectFail, bool skipIgnoreFile)
{
    auto result =
        thread_do<bool>(
        [=](StandardClient& client, PromiseBoolSP result)
        {
            result->set_value(client.confirmModel(id, mRoot, rRoot, expectFail, skipIgnoreFile));
        }, __FILE__, __LINE__);

    return result.get();
}

bool StandardClient::confirmModel_mainthread(handle id, Model::ModelNode* mRoot, LocalNode* lRoot, bool expectFail, bool skipIgnoreFile)
{
    auto result =
        thread_do<bool>(
        [=](StandardClient& client, PromiseBoolSP result)
        {
            result->set_value(client.confirmModel(id, mRoot, lRoot, expectFail, skipIgnoreFile));
        }, __FILE__, __LINE__);

    return result.get();
}

bool StandardClient::confirmModel_mainthread(handle id, Model::ModelNode* mRoot, fs::path lRoot, bool ignoreDebris, bool expectFail, bool skipIgnoreFile)
{
    auto result =
        thread_do<bool>(
        [=](StandardClient& client, PromiseBoolSP result)
        {
            result->set_value(client.confirmModel(id, mRoot, lRoot, ignoreDebris, expectFail, skipIgnoreFile));
        }, __FILE__, __LINE__);

    return result.get();
}

bool StandardClient::confirmModel(handle id, Model::ModelNode* mRoot, Node* rRoot, bool expectFail, bool skipIgnoreFile)
{
    string name = "Sync " + toHandle(id);
    int descendents = 0;
    bool reported = false;

    if (!recursiveConfirm(mRoot, rRoot, descendents, name, 0, reported, expectFail, skipIgnoreFile))
    {
        out() << clientname << " syncid " << toHandle(id) << " comparison against remote nodes failed";
        return false;
    }

    return true;
}

bool StandardClient::confirmModel(handle id, Model::ModelNode* mRoot, LocalNode* lRoot, bool expectFail, bool skipIgnoreFile)
{
    string name = "Sync " + toHandle(id);
    int descendents = 0;
    bool reported = false;

    if (!recursiveConfirm(mRoot, lRoot, descendents, name, 0, reported, expectFail, skipIgnoreFile))
    {
        out() << clientname << " syncid " << toHandle(id) << " comparison against LocalNodes failed";
        return false;
    }

    return true;
}

bool StandardClient::confirmModel(handle id, Model::ModelNode* mRoot, fs::path lRoot, bool ignoreDebris, bool expectFail, bool skipIgnoreFile)
{
    string name = "Sync " + toHandle(id);
    int descendents = 0;
    bool reported = false;

    if (!recursiveConfirm(mRoot, lRoot, descendents, name, 0, ignoreDebris, reported, expectFail, skipIgnoreFile))
    {
        out() << clientname << " syncid " << toHandle(id) << " comparison against local filesystem failed";
        return false;
    }

    return true;
}

bool StandardClient::confirmModel(handle backupId, Model::ModelNode* mnode, const int confirm, const bool ignoreDebris, bool expectFail, bool skipIgnoreFile)
{
    SyncInfo si;

    if (!syncSet(backupId, si))
    {
        out() << clientname << " backupId " << toHandle(backupId) << " not found ";
        return false;
    }

    // compare model against nodes representing remote state
    if ((confirm & CONFIRM_REMOTE) && !confirmModel(backupId, mnode, client.nodeByHandle(si.h), expectFail, skipIgnoreFile))
    {
        return false;
    }

    // Get our hands on the sync.
    auto* sync = syncByBackupId(backupId);

    // compare model against LocalNodes
    if (sync)
    {
        if ((confirm & CONFIRM_LOCALNODE) && !confirmModel(backupId, mnode, sync->localroot.get(), expectFail, skipIgnoreFile))
        {
            return false;
        }
    }

    // compare model against local filesystem
    if ((confirm & CONFIRM_LOCALFS) && !confirmModel(backupId, mnode, si.localpath, ignoreDebris, expectFail, skipIgnoreFile))
    {
        return false;
    }

#ifdef SRW_NEEDED_FOR_THIS_ONE

    // Does this sync have a state cache?
    if (!sync)
        return true;

    string statecachename = sync->getConfig().getSyncDbStateCacheName(sync->localroot->fsid_lastSynced, sync->getConfig().mRemoteNode, client.me);

    StateCacheValidator validator;

    // Try and load the state cache.
    EXPECT_TRUE(validator.load(client, statecachename))
        << "Sync "
        << toHandle(backupId)
        << ": Unable to load state cache: "
        << statecachename;

    // Does the state cache accurately reflect the LNT in memory?
    EXPECT_TRUE(validator.compare(*sync->localroot))
        << "Sync "
        << toHandle(backupId)
        << ": State cache mismatch.";
#endif
    return true;
}

void StandardClient::prelogin_result(int, string*, string* salt, error e)
{
    out() << clientname << " Prelogin: " << e;
    if (!e)
    {
        this->salt = *salt;
    }
    resultproc.processresult(PRELOGIN, e, UNDEF, client.restag);
}

void StandardClient::login_result(error e)
{
    out() << clientname << " Login: " << e;
    resultproc.processresult(LOGIN, e, UNDEF, client.restag);
}

void StandardClient::fetchnodes_result(const Error& e)
{
    out() << clientname << " Fetchnodes: " << e;
    resultproc.processresult(FETCHNODES, e, UNDEF, client.restag);
}

bool StandardClient::setattr(const CloudItem& item, attr_map&& updates)
{
    auto result =
        thread_do<bool>([=](StandardClient& client, PromiseBoolSP result) mutable
        {
            client.setattr(std::move(item), std::move(updates), result);
        }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(90));   // allow for up to 60 seconds of unexpected -3s on all channels
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

void StandardClient::setattr(const CloudItem& item, attr_map&& updates, PromiseBoolSP result)
{
    resultproc.prepresult(COMPLETION,
                            ++next_request_tag,
                            [=]()
                            {
                                auto* node = item.resolve(*this);
                                if (!node)
                                    return result->set_value(false);

                                client.setattr(node, attr_map(updates),
                                    [result](NodeHandle, error e) { result->set_value(!e); }, false);
                            }, nullptr);
}

bool StandardClient::rename(const CloudItem& item, const string& newName)
{
    return setattr(item, attr_map('n', newName));
}

void StandardClient::unlink_result(handle h, error e)
{
    resultproc.processresult(UNLINK, e, h, client.restag);
}

void StandardClient::putnodes_result(const Error& e, targettype_t tt, vector<NewNode>& nn, bool targetOverride, int tag)
{
    resultproc.processresult(PUTNODES, e, nn.empty() ? UNDEF : nn[0].mAddedHandle, tag);
}

void StandardClient::catchup_result()
{
    resultproc.processresult(CATCHUP, error(API_OK), UNDEF, client.restag);
}

void StandardClient::disableSync(handle id, SyncError error, bool enabled, PromiseBoolSP result)
{
    client.syncs.disableSyncByBackupId(id,
        false,
        error,
        enabled,
        [result](){
            result->set_value(true);
        });
}

bool StandardClient::disableSync(handle id, SyncError error, bool enabled)
{
    auto result =
        thread_do<bool>([=](StandardClient& client, PromiseBoolSP result)
                        {
                            client.disableSync(id, error, enabled, result);
                        }, __FILE__, __LINE__);

    return result.get();
}

void StandardClient::deleteremote(const CloudItem& item, PromiseBoolSP result)
{
    auto* node = item.resolve(*this);
    if (!node)
        return result->set_value(false);

    client.unlink(node, false, 0, false, [result](NodeHandle, Error e) {
        result->set_value(e == API_OK);
    });
}

bool StandardClient::deleteremote(const CloudItem& item)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        client.deleteremote(item, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(45));
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

bool StandardClient::deleteremotedebris()
{
    return withWait<bool>([&](PromiseBoolSP result) {
        deleteremotedebris(result);
    });
}

void StandardClient::deleteremotedebris(PromiseBoolSP result)
{
    if (auto* debris = getsyncdebrisnode())
    {
        deleteremotenodes({debris}, std::move(result));
    }
    else
    {
        result->set_value(true);
    }
}

void StandardClient::deleteremotenodes(vector<Node*> ns, PromiseBoolSP pb)
{
    if (ns.empty())
    {
        pb->set_value(true);
    }
    else
    {
        for (size_t i = ns.size(); i--; )
        {
            auto completion = [i, pb](NodeHandle, Error e) {
                if (!i) pb->set_value(!e);
            };

            resultproc.prepresult(COMPLETION, ++next_request_tag,
                [&](){ client.unlink(ns[i], false, 0, false, std::move(completion)); },
                nullptr);
        }
    }
}

bool StandardClient::movenode(const CloudItem& source,
                              const CloudItem& target,
                              const string& newName)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        client.movenode(source, target, newName, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(DEFAULTWAIT);
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

void StandardClient::movenode(const CloudItem& source,
                              const CloudItem& target,
                              const string& newName,
                              PromiseBoolSP result)
{
    auto* sourceNode = source.resolve(*this);
    if (!sourceNode)
        return result->set_value(false);

    auto* targetNode = target.resolve(*this);
    if (!targetNode)
        return result->set_value(false);

    auto completion = [result](NodeHandle, Error e) {
        result->set_value(e == API_OK);
    };

    client.rename(sourceNode,
                  targetNode,
                  SYNCDEL_NONE,
                  NodeHandle(),
                  newName.empty() ? nullptr : newName.c_str(),
                  false,
                  std::move(completion));
}

void StandardClient::movenodetotrash(string path, PromiseBoolSP pb)
{
    Node* n = drillchildnodebyname(gettestbasenode(), path);
    Node* p = getcloudrubbishnode();
    if (n && p && n->parent)
    {
        resultproc.prepresult(COMPLETION, ++next_request_tag,
            [pb, n, p, this]()
            {
                client.rename(n, p, SYNCDEL_NONE, NodeHandle(), nullptr, false,
                    [pb](NodeHandle h, Error e) { pb->set_value(!e); });
            },
            nullptr);
        return;
    }
    out() << "node or rubbish or node parent not found";
    pb->set_value(false);
}

void StandardClient::exportnode(Node* n, int del, m_time_t expiry, bool writable, bool megaHosted, promise<Error>& pb)
{
    resultproc.prepresult(COMPLETION, ++next_request_tag,
        [&](){
            error e = client.exportnode(n, del, expiry, writable, megaHosted, client.reqtag, [&](Error e, handle, handle){ pb.set_value(e); });
            if (e)
            {
                pb.set_value(e);
            }
        }, nullptr);  // no need to match callbacks with requests when we use completion functions
}

void StandardClient::getpubliclink(Node* n, int del, m_time_t expiry, bool writable, bool megaHosted, promise<Error>& pb)
{
    resultproc.prepresult(COMPLETION, ++next_request_tag,
        [&](){ client.requestPublicLink(n, del, expiry, writable, megaHosted, client.reqtag, [&](Error e, handle, handle){ pb.set_value(e); }); },
        nullptr);
}


void StandardClient::waitonsyncs(chrono::seconds d)
{
    auto start = chrono::steady_clock::now();
    for (;;)
    {
        bool any_add_del = false;;
        vector<int> syncstates;

        thread_do<bool>([&syncstates, &any_add_del, this](StandardClient& mc, PromiseBoolSP pb)
        {
            mc.client.syncs.forEachRunningSync(
                [&](Sync* s)
                {
                    syncstates.push_back(s->state());
                    any_add_del |= !s->deleteq.empty();
                    any_add_del |= !s->insertq.empty();
                });

            if (!(client.toDebris.empty() && client.toUnlink.empty() /*&& client.synccreate.empty()*/))
            {
                any_add_del = true;
            }
            if (!client.multi_transfers[GET].empty() || !client.multi_transfers[PUT].empty())
            {
                any_add_del = true;
            }
            pb->set_value(true);
        }, __FILE__, __LINE__).get();
        bool allactive = true;
        {
            lock_guard<mutex> g(StandardClient::om);
            //std::out() << "sync state: ";
            //for (auto n : syncstates)
            //{
            //    out() << n;
            //    if (n != SYNC_ACTIVE) allactive = false;
            //}
            //out();
        }

        if (any_add_del || debugging)
        {
            start = chrono::steady_clock::now();
        }

        if (allactive && ((chrono::steady_clock::now() - start) > d) && ((chrono::steady_clock::now() - lastcb) > d))
        {
            break;
        }
//out() << "waiting 500";
        WaitMillisec(500);
    }

}

bool StandardClient::login_reset(const string& user, const string& pw, bool noCache, bool resetBaseCloudFolder)
{
    received_user_alerts = false;

    future<bool> p1;
    p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.preloginFromEnv(user, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p1))
    {
        out() << "preloginFromEnv failed";
        return false;
    }
    p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromEnv(user, pw, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p1))
    {
        out() << "loginFromEnv failed";
        return false;
    }
    p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.fetchnodes(noCache, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p1)) {
        out() << "fetchnodes failed";
        return false;
    }

    EXPECT_TRUE(waitForUserAlertsUpdated(30));

    p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.upgradeSecurity(pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p1))
    {
        out() << "upgrading security failed";
        return false;
    }

    if (resetBaseCloudFolder)
    {
        if (deleteTestBaseFolder(true) == 0)
        {
            out() << "deleteTestBaseFolder failed";
            return false;
        }

        p1 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(true, pb); }, __FILE__, __LINE__);
        if (!waitonresults(&p1)) {
            out() << "ensureTestBaseFolder failed";
            return false;
        }
    }
    return true;
}

bool StandardClient::resetBaseFolderMulticlient(StandardClient* c2, StandardClient* c3, StandardClient* c4)
{
    auto resetActionPacketFlags = [this, c2, c3, c4]() {
        received_node_actionpackets = false;
        if (c2) c2->received_node_actionpackets = false;
        if (c3) c3->received_node_actionpackets = false;
        if (c4) c4->received_node_actionpackets = false;
    };

    auto waitForActionPackets = [this, c2, c3, c4]() {
        if (!waitForNodesUpdated(45))
            return false;

        if (c2 && !c2->waitForNodesUpdated(45))
            return false;

        if (c3 && !c3->waitForNodesUpdated(45))
            return false;

        if (c4 && !c4->waitForNodesUpdated(45))
            return false;

        return true;
    };

    resetActionPacketFlags();

    switch (deleteTestBaseFolder(true))
    {
    case 0:
        out() << "deleteTestBaseFolder failed";
        return false;
    case 2:
        if (!waitForActionPackets())
        {
            out() << "No actionpacket received in at least one client for base folder deletion.";
            return false;
        }
        break;
    default:
        break;
    }

    resetActionPacketFlags();

    auto p1 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(true, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p1)) {
        out() << "ensureTestBaseFolder failed";
        return false;
    }

    if (!waitForActionPackets())
    {
        out() << "No actionpacket received in at least one client for base folder creation";
        return false;
    }

    auto checkOtherClient = [this](StandardClient* c, bool finalcheck) {
        if (c)
        {
            auto p1 = c->thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(false, pb); }, __FILE__, __LINE__);
            if (!waitonresults(&p1)) {
                if (finalcheck) { out() << "ensureTestBaseFolder c2 failed"; }
                return false;
            }
            if (c->basefolderhandle != basefolderhandle)
            {
                if (finalcheck) { out() << "base folder handle mismatch with c2"; }
                return false;
            }
        }
        return true;
    };

    // although we waited for actionpackets, it's possible that wait was satisfied by some late actionpacket from a prior test.
    // wait a bit longer if it's not there already
    for (int i = 60; i--; )
    {
        if (checkOtherClient(c2, false) &&
            checkOtherClient(c3, false) &&
            checkOtherClient(c4, false))
            return true;
        WaitMillisec(1000);
    }

    if (!checkOtherClient(c2, true)) return false;
    if (!checkOtherClient(c3, true)) return false;
    if (!checkOtherClient(c4, true)) return false;
    return true;
}

void StandardClient::cleanupForTestReuse(int loginIndex)
{

    if (client.nodeByPath("/abort_jenkins_test_run"))
    {
        string user = getenv(envVarAccount[loginIndex].c_str());
        cout << "Detected node /abort_jenkins_test_run in account " << user << ", aborting test run" << endl;
        out() << "Detected node /abort_jenkins_test_run in account " << user << ", aborting test run";
        WaitMillisec(100);
        exit(1);
    }

    LOG_debug << clientname << "cleaning syncs for client reuse";
    future<bool> p1;
    p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) {

        sc.client.syncs.prepareForLogout(false, [this, pb](){

            // 3rd param true to "load" (zero) syncs again so store is ready for the test
            client.syncs.locallogout(true, false, true);
            pb->set_value(true);
        });
    }, __FILE__, __LINE__);
    if (!waitonresults(&p1))
    {
        out() << "removeSelectedSyncs failed";
    }
    assert(client.syncs.getConfigs(false).empty());

    // delete everything from Vault
    std::atomic<int> requestcount{0};
    p1 = thread_do<bool>([=, &requestcount](StandardClient& sc, PromiseBoolSP pb) {

        if (auto vault = sc.client.nodeByHandle(sc.client.mNodeManager.getRootNodeVault()))
        {
            for (auto n : sc.client.mNodeManager.getChildren(vault))
            {
                LOG_debug << "vault child: " << n->displaypath();
                for (auto n2 : sc.client.mNodeManager.getChildren(n))
                {
                    LOG_debug << "Unlinking: " << n2->displaypath();
                    ++requestcount;
                    sc.client.unlink(n2, false, 0, true, [&requestcount](NodeHandle, Error){ --requestcount; });
                }
            }
        }
    }, __FILE__, __LINE__);

    // delete everything from //bin
    p1 = thread_do<bool>([=, &requestcount](StandardClient& sc, PromiseBoolSP pb) {

        if (auto bin = sc.client.nodeByHandle(sc.client.mNodeManager.getRootNodeRubbish()))
        {
            for (auto n : sc.client.mNodeManager.getChildren(bin))
            {
                LOG_debug << "Unlinking from bin: " << n->displaypath();
                ++requestcount;
                sc.client.unlink(n, false, 0, false, [&requestcount](NodeHandle, Error){ --requestcount; });
            }
        }
    }, __FILE__, __LINE__);

    int limit = 100;
    while (requestcount.load() > 0 && --limit)
    {
        WaitMillisec(100);
    }

    // Make sure any throttles are reset.
    client.setmaxdownloadspeed(0);
    client.setmaxuploadspeed(0);

    // Make sure any event handlers are released.
    // TODO: May require synchronization?
    mOnConflictsDetected = nullptr;
    mOnFileAdded = nullptr;
    mOnFileComplete = nullptr;
    mOnStall = nullptr;
    mOnSyncStateConfig = nullptr;
    mOnTransferAdded = nullptr;
    onTransferCompleted = nullptr;

#ifdef DEBUG
    mOnMoveBegin = nullptr;
    mOnPutnodesBegin = nullptr;
    mOnSyncDebugNotification = nullptr;
#endif // DEBUG


    LOG_debug << clientname << "cleaning transfers for client reuse";

    future<bool> p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) {

        CancelToken cancelled(true);
        int direction[] = { PUT, GET };
        for (int d = 0; d < 2; ++d)
        {
            for (auto& it : sc.client.multi_transfers[direction[d]])
            {
                for (auto& it2 : it.second->files)
                {
                    if (!it2->syncxfer)
                    {
                        it2->cancelToken = cancelled;
                    }
                }
            }
        }

        pb->set_value(true);
    }, __FILE__, __LINE__);
    if (!waitonresults(&p2))
    {
        out() << "transfer removal failed";
    }

    // wait for completion of ongoing transfers, up to 60s
    for (int i = 30000; i-- && !client.multi_transfers[GET].empty(); ) WaitMillisec(1);
    for (int i = 30000; i-- && !client.multi_transfers[PUT].empty(); ) WaitMillisec(1);
    LOG_debug << clientname << "transfers cleaned";

    // wait further for reqs to finish if any are queued, up to 30s
    for (int i = 30000; i-- && !client.multi_transfers[PUT].empty(); ) WaitMillisec(1);

    // check transfers were canceled successfully
    if (client.multi_transfers[PUT].size() || client.multi_transfers[GET].size())
    {
        LOG_err << clientname << "Failed to clean transfers at cleanupForTestReuse():"
                   << " put: " << client.multi_transfers[PUT].size()
                   << " get: " << client.multi_transfers[GET].size();
    }
    else
    {
        LOG_debug << clientname << "transfers cleaned successfully";
    }

    // todo: make these calls to reqs thread safe. Low priority

    // wait for cmds in flight and queued, up to 120s
    if (client.reqs.cmdsInflight() || client.reqs.readyToSend())
    {
        LOG_debug << clientname << "waiting for requests to finish";
        for (int i = 120000; i-- && (client.reqs.readyToSend() || client.reqs.cmdsInflight()); ) WaitMillisec(1);
    }

    // check any pending command was completed
    if (client.reqs.cmdsInflight() || client.reqs.readyToSend())
    {
        LOG_err << clientname << "Failed to clean pending commands at cleanupForTestReuse():"
                << " pending: " << client.reqs.readyToSend()
                << " inflight: " << client.reqs.cmdsInflight();
    }
    else
    {
        LOG_debug << clientname << "requests cleaned successfully";
    }

}

bool StandardClient::login_reset_makeremotenodes(const string& prefix, int depth, int fanout, bool noCache)
{
    return login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", prefix, depth, fanout, noCache);
}

bool StandardClient::login_reset_makeremotenodes(const string& user, const string& pw, const string& prefix, int depth, int fanout, bool noCache)
{
    if (!login_reset(user, pw, noCache))
    {
        out() << "login_reset failed";
        return false;
    }
    future<bool> p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs(prefix, depth, fanout, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p1))
    {
        out() << "makeCloudSubdirs failed";
        return false;
    }
    return true;
}

void StandardClient::ensureSyncUserAttributes(PromiseBoolSP result)
{
    auto completion = [result](Error e) { result->set_value(!e); };
    client.ensureSyncUserAttributes(std::move(completion));
}

bool StandardClient::ensureSyncUserAttributes()
{
    auto result =
        thread_do<bool>([](StandardClient& client, PromiseBoolSP result)
                        {
                            client.ensureSyncUserAttributes(result);
                        }, __FILE__, __LINE__);

    return result.get();
}

void StandardClient::copySyncConfig(SyncConfig config, PromiseHandleSP result)
{
    auto completion =
        [result](handle id, error e)
        {
            result->set_value(e ? UNDEF : id);
        };

    client.copySyncConfig(config, std::move(completion));
}

handle StandardClient::copySyncConfig(const SyncConfig& config)
{
    auto result =
        thread_do<handle>([=](StandardClient& client, PromiseHandleSP result)
                        {
                            client.copySyncConfig(config, result);
                        }, __FILE__, __LINE__);

    return result.get();
}

bool StandardClient::login(const string& user, const string& pw)
{
    future<bool> p;
    p = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.preloginFromEnv(user, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p)) return false;
    p = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromEnv(user, pw, pb); }, __FILE__, __LINE__);
    return waitonresults(&p);
}

bool StandardClient::login_fetchnodes(const string& user, const string& pw, bool makeBaseFolder, bool noCache)
{
    received_user_alerts = false;

    future<bool> p2;
    p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.preloginFromEnv(user, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p2)) return false;
    p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromEnv(user, pw, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p2)) return false;
    p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.fetchnodes(noCache, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p2)) return false;

    EXPECT_TRUE(waitForUserAlertsUpdated(30));

    p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.upgradeSecurity(pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p2)) return false;

    p2 = thread_do<bool>([makeBaseFolder](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(makeBaseFolder, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p2)) return false;
    return true;
}

bool StandardClient::login_fetchnodes(const string& session)
{
    future<bool> p2;
    p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromSession(session, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p2)) return false;
    p2 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.fetchnodes(false, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p2)) return false;

    p2 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(false, pb); }, __FILE__, __LINE__);
    if (!waitonresults(&p2)) return false;
    return true;
}

bool StandardClient::delSync_mainthread(handle backupId)
{
    future<bool> fb = thread_do<bool>([=](StandardClient& mc, PromiseBoolSP pb) { mc.delSync_inthread(backupId, pb); }, __FILE__, __LINE__);
    return fb.get();
}

bool StandardClient::confirmModel_mainthread(Model::ModelNode* mnode, handle backupId, bool ignoreDebris, int confirm, bool expectFail, bool skipIgnoreFile)
{
    return thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) {
        pb->set_value(sc.confirmModel(backupId, mnode, confirm, ignoreDebris, expectFail, skipIgnoreFile));
    }, __FILE__, __LINE__).get();
}

bool StandardClient::match(handle id, const Model::ModelNode* source)
{
    if (!source) return false;

    auto result = thread_do<bool>([=](StandardClient& client, PromiseBoolSP result) {
        client.match(id, source, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(DEFAULTWAIT);

    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

void StandardClient::match(handle id, const Model::ModelNode* source, PromiseBoolSP result)
{
    SyncInfo info;

    auto found = syncSet(id, info);
    EXPECT_TRUE(found);

    if (!found)
        return result->set_value(false);

    const auto* destination = client.nodeByHandle(info.h);
    EXPECT_TRUE(destination);

    result->set_value(destination && match(*destination, *source));
}

bool StandardClient::match(NodeHandle handle, const Model::ModelNode* source)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        client.match(handle, source, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(DEFAULTWAIT);
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

void StandardClient::match(NodeHandle handle, const Model::ModelNode* source, PromiseBoolSP result)
{
    EXPECT_TRUE(source);
    if (!source)
        return result->set_value(false);

    auto node = client.nodeByHandle(handle);
    EXPECT_TRUE(node);

    result->set_value(node && match(*node, *source));
}

bool StandardClient::waitFor(std::function<bool(StandardClient&)> predicate, const std::chrono::seconds &timeout, const std::chrono::milliseconds &sleepIncrement = std::chrono::milliseconds(500))
{
    auto total = std::chrono::milliseconds(0);

    out() << "Waiting for predicate to match...";

    for (;;)
    {
        if (predicate(*this))
        {
            out() << "Predicate has matched!";

            return true;
        }

        if (total >= timeout)
        {
            out() << "Timed out waiting for predicate to match.";
            return false;
        }

        std::this_thread::sleep_for(sleepIncrement);
        total += sleepIncrement;
    }
}

bool StandardClient::match(const Node& destination, const Model::ModelNode& source) const
{
    list<pair<const Node*, decltype(&source)>> pending;
    auto matched = true;

    pending.emplace_back(&destination, &source);

    for ( ; !pending.empty(); pending.pop_front())
    {
        const auto& dn = *pending.front().first;
        const auto& sn = *pending.front().second;

        // Nodes must have matching types.
        if (!sn.typematchesnodetype(dn.type))
        {
            LOG_debug << "Cloud model/type mismatch: "
                      << dn.displaypath()
                      << "("
                      << dn.type
                      << ")"
                      << " vs. "
                      << sn.path()
                      << "("
                      << sn.type
                      << ")";

            matched = false;
            continue;
        }

        // Files require no further processing.
        if (dn.type == FILENODE) continue;

        map<string, decltype(&dn), CloudNameLess> dc;
        map<string, decltype(&sn), CloudNameLess> sc;
        set<string, CloudNameLess> dd;
        set<string, CloudNameLess> sd;

        // Index children for pairing.
        for (const auto* child : dn.client->getChildren(&dn))
        {
            string name = child->displayname();

            // Duplicate already reported?
            if (dd.count(name))
            {
                LOG_debug << "Cloud name conflict: "
                          << child->displaypath();
                continue;
            }

            auto result = dc.emplace(child->displayname(), child);

            // Didn't exist? No duplicate.
            if (result.second)
                continue;

            LOG_debug << "Cloud name conflict: "
                      << child->displaypath();

            // Remmber the duplicate (name conflict.)
            dc.erase(result.first);
            dd.emplace(name);

            matched = false;
        }

        for (const auto& child : sn.kids)
        {
            auto name = child->cloudName();

            // Duplicate already reported?
            if (dd.count(name))
            {
                LOG_debug << "Model node excluded due to cloud duplicates: "
                          << child->path();
                continue;
            }

            if (sd.count(name))
            {
                LOG_debug << "Model name conflict: "
                          << child->path();
                continue;
            }

            auto result = sc.emplace(child->cloudName(), child.get());

            // Didn't exist? No duplicate.
            if (result.second)
                continue;

            LOG_debug << "Model name conflict: "
                      << child->path();

            // Remember the duplicate.
            dc.erase(name);
            sc.erase(result.first);
            sd.emplace(name);

            matched = false;
        }

        // Pair children.
        for (const auto& s : sc)
        {
            // Skip the debris folder if it appears in the root.
            if (&sn == &source)
            {
                if (CloudNameLess::equal(s.first, DEBRISFOLDER))
                {
                    continue;
                }
            }

            // Does this node have a pair in the destination?
            auto d = dc.find(s.first);

            matched &= d != dc.end();

            // If not then there can be no match.
            if (d == dc.end())
            {
                LOG_debug << "Model node has no pair in cloud: "
                          << s.second->path();
                continue;
            }

            // Queue pair for more detailed matching.
            pending.emplace_back(d->second, s.second);

            // Consider the destination node paired.
            dc.erase(d);
        }

        // Can't have a match if we couldn't pair all destination nodes.
        matched &= dc.empty();

        // Log which nodes we couldn't match.
        for (const auto& d : dc)
        {
            LOG_debug << "Cloud node has no pair in the model: "
                      << d.second->displaypath();
        }

    }

    return matched;
}

bool StandardClient::backupOpenDrive(const fs::path& drivePath)
{
    auto result = thread_do<bool>([=](StandardClient& client, PromiseBoolSP result) {
        client.backupOpenDrive(drivePath, std::move(result));
    }, __FILE__, __LINE__);

    return result.get();
}

void StandardClient::backupOpenDrive(const fs::path& drivePath, PromiseBoolSP result)
{
    auto localDrivePath = LocalPath::fromAbsolutePath(drivePath.u8string());
    client.syncs.backupOpenDrive(localDrivePath, [result](Error e){
        result->set_value(e == API_OK);
    });
}

void StandardClient::triggerPeriodicScanEarly(handle backupID)
{
    // This one to be enabled after more of sync rework is merged to develop.
    // We don't have syncs configured for periodic scans yet on develop.
    // But in order to reduce the count of differing lines between the branches,
    // it's advantageous to move the lines calling this function
    //client.syncs.triggerPeriodicScanEarly(backupID).get();
}

handle StandardClient::getNodeHandle(const CloudItem& item)
{
    auto result = thread_do<handle>([&](StandardClient& client, PromiseHandleSP result) {
        client.getNodeHandle(item, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(DEFAULTWAIT);
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

void StandardClient::getNodeHandle(const CloudItem& item, PromiseHandleSP result)
{
    if (auto* node = item.resolve(*this))
        return result->set_value(node->nodehandle);

    result->set_value(UNDEF);
}

FileFingerprint StandardClient::fingerprint(const fs::path& fsPath)
{
    // Convenience.
    auto& fsAccess = *client.fsaccess;

    // Needed so we can access the filesystem.
    auto fileAccess = fsAccess.newfileaccess(false);

    // Translate input path into something useful.
    auto path = LocalPath::fromAbsolutePath(fsPath.u8string());

    FileFingerprint fingerprint;

    // Try and open file for reading.
    if (fileAccess->fopen(path, true, false, FSLogging::logOnError))
    {
        // Generate fingerprint.
        fingerprint.genfingerprint(fileAccess.get());
    }

    return fingerprint;
}

vector<FileFingerprint> StandardClient::fingerprints(const string& path)
{
    vector<FileFingerprint> results;

    // Get our hands on the root node.
    auto* root = gettestbasenode();

    if (!root)
        return results;

    // Locate the specified node.
    auto* node = drillchildnodebyname(root, path);

    if (!node)
        return results;

    // Directories have no fingerprints.
    if (node->type != FILENODE)
        return results;

    // Extract the fingerprints from the version chain.
    results.emplace_back(*node);

    auto nodes = client.mNodeManager.getChildren(node);

    while (!nodes.empty())
    {
        node = nodes.front();
        results.emplace_back(*node);
        nodes = client.mNodeManager.getChildren(node);
    }

    // Pass fingerprints to caller.
    return results;
}

void StandardClient::ipcr(handle id, ipcactions_t action, PromiseBoolSP result)
{
    client.updatepcr(id, action, [=](error e, ipcactions_t) {
        result->set_value(!e);
    });
}

bool StandardClient::ipcr(handle id, ipcactions_t action)
{
    auto result = thread_do<bool>([=](StandardClient& client, PromiseBoolSP result) {
        client.ipcr(id, action, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(45));
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

bool StandardClient::ipcr(handle id)
{
    auto result = thread_do<bool>([=](StandardClient& client, PromiseBoolSP result) {
        auto i = client.client.pcrindex.find(id);
        auto j = client.client.pcrindex.end();

        result->set_value(i != j && !i->second->isoutgoing);
    }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(45));
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

void StandardClient::opcr(const string& email, opcactions_t action, PromiseHandleSP result)
{
    auto completion = [=](handle h, error e, opcactions_t) {
        result->set_value(!e ? h : UNDEF);
    };

    client.setpcr(email.c_str(),
                  action,
                  nullptr,
                  nullptr,
                  UNDEF,
                  std::move(completion));
}

handle StandardClient::opcr(const string& email, opcactions_t action)
{
    auto result = thread_do<handle>([&](StandardClient& client, PromiseHandleSP result) {
        client.opcr(email, action, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(45));
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

bool StandardClient::opcr(const string& email)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        for (auto& i : client.client.pcrindex)
        {
            if (i.second->targetemail == email)
                return result->set_value(i.second->isoutgoing);
        }

        result->set_value(false);
    }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(45));
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

bool StandardClient::iscontact(const string& email)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        for (auto &i : client.client.users)
        {
            if (i.second.email == email)
                return result->set_value(i.second.show == VISIBLE);
        }

        result->set_value(false);
    }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(45));
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

bool StandardClient::isverified(const string& email)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        User* u = client.client.finduser(email.c_str());
        if (u) {
            result->set_value(client.client.areCredentialsVerified(u->userhandle));
        }
        else
        {
            result->set_value(false);
        }
    }, __FILE__, __LINE__);

    auto status = result.wait_for(DEFAULTWAIT);
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

bool StandardClient::verifyCredentials(const string& email)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        User* u = client.client.finduser(email.c_str());
        if (u) {
            result->set_value(client.client.verifyCredentials(u->userhandle) == API_OK);
        }
        else
        {
            result->set_value(false);
        }
    }, __FILE__, __LINE__);

    auto status = result.wait_for(DEFAULTWAIT);
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

bool StandardClient::resetCredentials(const string& email)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        User* u = client.client.finduser(email.c_str());
        if (u) {
            result->set_value(client.client.resetCredentials(u->userhandle) == API_OK);
        }
        else
        {
            result->set_value(false);
        }
    }, __FILE__, __LINE__);

    auto status = result.wait_for(DEFAULTWAIT);
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

void StandardClient::rmcontact(const string& email, PromiseBoolSP result)
{
    client.removecontact(email.c_str(), HIDDEN, [=](error e) {
        result->set_value(!e);
    });
}

bool StandardClient::rmcontact(const string& email)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        client.rmcontact(email, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(std::chrono::seconds(45));
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    return result.get();
}

void StandardClient::share(const CloudItem& item, const string& email, accesslevel_t permissions, PromiseBoolSP result)
{
    auto* node = item.resolve(*this);
    if (!node)
        return result->set_value(false);

    auto completion = [=](Error e, bool) {
        if (e == API_EKEY)
        {
            // create share key and try again
            client.openShareDialog(node, [=](Error osdErr)
                {
                    if (osdErr == API_OK)
                    {
                        client.setshare(node,
                            email.c_str(),
                            permissions,
                            false,
                            nullptr,
                            ++next_request_tag,
                            [result](Error e2, bool) {result->set_value(!e2);});
                    }
                    else
                    {
                        result->set_value(false);
                    }
                }
            );
        }
        else
        {
            result->set_value(!e);
        }
    };

    client.setshare(node,
                    email.c_str(),
                    permissions,
                    false,
                    nullptr,
                    ++next_request_tag,
                    std::move(completion));
}

bool StandardClient::share(const CloudItem& item, const string& email, accesslevel_t permissions)
{
    auto result = thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
        client.share(item, email, permissions, std::move(result));
    }, __FILE__, __LINE__);

    auto status = result.wait_for(DEFAULTWAIT);
    EXPECT_NE(status, future_status::timeout);

    if (status == future_status::timeout)
        return false;

    bool r = result.get();
    return r;
}

void StandardClient::upgradeSecurity(PromiseBoolSP result)
{
    client.upgradeSecurity([=](error e) {
        result->set_value(!e);
    });
}

using SyncWaitPredicate = std::function<bool(StandardClient&)>;

// Useful predicates.
SyncWaitPredicate SyncDisabled(handle id)
{
    return [id](StandardClient& client) {
        return client.syncByBackupId(id) == nullptr;
    };
}

SyncWaitPredicate SyncMonitoring(handle id)
{
    return [id](StandardClient& client) {
        const auto* sync = client.syncByBackupId(id);
        return sync && sync->isBackupMonitoring();
    };
}

SyncWaitPredicate SyncRemoteMatch(const CloudItem& item, const Model::ModelNode* source)
{
    return [=](StandardClient& client) {
        return client.thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
            if (auto* node = item.resolve(client))
                return client.match(node->nodeHandle(), source, std::move(result));
            result->set_value(false);
        }, __FILE__, __LINE__).get();
    };
}

SyncWaitPredicate SyncRemoteMatch(const CloudItem& item, const Model& source)
{
    return SyncRemoteMatch(item, source.root.get());
}

SyncWaitPredicate SyncRemoteNodePresent(const CloudItem& item)
{
    return [item](StandardClient& client) {
        return client.thread_do<bool>([&](StandardClient& client, PromiseBoolSP result) {
            result->set_value(item.resolve(client));
        }, __FILE__, __LINE__).get();
    };
}

void waitonsyncs(chrono::seconds d = std::chrono::seconds(4), StandardClient* c1 = nullptr, StandardClient* c2 = nullptr, StandardClient* c3 = nullptr, StandardClient* c4 = nullptr)
{
    auto totalTimeoutStart = chrono::steady_clock::now();
    auto start = chrono::steady_clock::now();
    std::vector<StandardClient*> v{ c1, c2, c3, c4 };
    bool onelastsyncdown = true;
    for (;;)
    {
        bool any_add_del = false;

        for (auto vn : v)
        {
            if (vn)
            {
                auto result =
                  vn->thread_do<bool>(
                    [&](StandardClient& mc, PromiseBoolSP result)
                    {
                        bool busy = false;

                        mc.client.syncs.forEachRunningSync(
                          [&](Sync* s)
                          {
                              busy |= !s->deleteq.empty();
                              busy |= !s->insertq.empty();
                          });

                        if (!(mc.client.toDebris.empty()
                            && mc.client.localsyncnotseen.empty()
                            && mc.client.toUnlink.empty()
                            && mc.client.synccreateForVault.empty()
                            && mc.client.synccreateGeneral.empty()
                            && mc.client.transferlist.transfers[GET].empty()
                            && mc.client.transferlist.transfers[PUT].empty()))
                        {
                            busy = true;
                        }

                        result->set_value(busy);
                    }, __FILE__, __LINE__);

                any_add_del |= result.get();
            }
        }

        bool allactive = true;
        {
            //lock_guard<mutex> g(StandardClient::om);
            //out() << "sync state: ";
            //for (auto n : syncstates)
            //{
            //    cout << n;
            //    if (n != SYNC_ACTIVE) allactive = false;
            //}
            //out();
        }

        if (any_add_del || StandardClient::debugging)
        {
            start = chrono::steady_clock::now();
        }

        if (onelastsyncdown && (chrono::steady_clock::now() - start + d/2) > d)
        {
            // synced folders that were removed remotely don't have the corresponding local folder removed unless we prompt an extra syncdown.  // todo:  do we need to fix
            for (auto vn : v) if (vn) vn->client.syncdownrequired = true;
            onelastsyncdown = false;
        }

        for (auto vn : v) if (vn)
        {
            if (allactive && ((chrono::steady_clock::now() - start) > d) && ((chrono::steady_clock::now() - vn->lastcb) > d))
            {
                return;
            }
        }

        WaitMillisec(400);

        if ((chrono::steady_clock::now() - totalTimeoutStart) > std::chrono::minutes(5))
        {
            out() << "Waiting for syncing to stop timed out at 5 minutes";
            return;
        }
    }

}


mutex StandardClient::om;
bool StandardClient::debugging = false;



//std::atomic<int> fileSizeCount = 20;

bool createNameFile(const fs::path &p, const string &filename)
{
    return createFile(p / fs::u8path(filename), filename.data(), filename.size());
}

bool createDataFileWithTimestamp(const fs::path &path,
                             const std::string &data,
                             const fs::path& tmpCreationLocation,
                             const fs::file_time_type &timestamp)
{
    // Create the file at a neutral location first so we can set the timestamp without a sync noticing the wrong timestamp first
    bool result = createDataFile(tmpCreationLocation / path.filename(), data);

    if (result)
    {
        fs::last_write_time(tmpCreationLocation / path.filename(), timestamp);

        // Now that it has the proper mtime, move it to the correct location
        error_code rename_error;
        fs::rename(tmpCreationLocation / path.filename(), path, rename_error);
        EXPECT_TRUE(!rename_error) << rename_error;
        result = !rename_error;
    }

    return result;
}

bool buildLocalFolders(fs::path targetfolder, const string& prefix, int n, int recurselevel, int filesperfolder)
{
    if (suppressfiles) filesperfolder = 0;

    fs::path p = targetfolder / fs::u8path(prefix);
    if (!fs::create_directory(p))
        return false;

    for (int i = 0; i < filesperfolder; ++i)
    {
        string filename = "file" + to_string(i) + "_" + prefix;
        createNameFile(p, filename);
        //int thisSize = (++fileSizeCount)/2;
        //for (int j = 0; j < thisSize; ++j) fs << ('0' + j % 10);
    }

    if (recurselevel > 0)
    {
        for (int i = 0; i < n; ++i)
        {
            if (!buildLocalFolders(p, prefix + "_" + to_string(i), n, recurselevel - 1, filesperfolder))
                return false;
        }
    }

    return true;
}

void renameLocalFolders(fs::path targetfolder, const string& newprefix)
{
    std::list<fs::path> toRename;
    for (fs::directory_iterator i(targetfolder); i != fs::directory_iterator(); ++i)
    {
        if (fs::is_directory(i->path()))
        {
            renameLocalFolders(i->path(), newprefix);
        }
        toRename.push_back(i->path());
    }

    for (auto p : toRename)
    {
        auto newpath = p.parent_path() / (newprefix + p.filename().u8string());
        fs::rename(p, newpath);
    }
}


#ifdef __linux__
bool createSpecialFiles(fs::path targetfolder, const string& prefix, int n = 1)
{
    fs::path p = targetfolder;
    for (int i = 0; i < n; ++i)
    {
        string filename = "file" + to_string(i) + "_" + prefix;
        fs::path fp = p / fs::u8path(filename);

        int fdtmp = openat(AT_FDCWD, p.c_str(), O_RDWR|O_CLOEXEC|O_TMPFILE, 0600);
        write(fdtmp, filename.data(), filename.size());

        stringstream fdproc;
        fdproc << "/proc/self/fd/";
        fdproc << fdtmp;

        int r = linkat(AT_FDCWD, fdproc.str().c_str() , AT_FDCWD, fp.c_str(), AT_SYMLINK_FOLLOW);
        if (r)
        {
            cerr << " errno =" << errno;
            return false;
        }
        close(fdtmp);
    }
    return true;
}
#endif

class SyncFingerprintCollisionTest
  : public SdkTestBase
{
public:

    fs::path testRootFolder;

    SyncFingerprintCollisionTest()
      : client0()
      , client1()
      , model0()
      , model1()
      , arbitraryFileLength(16384)
    {
        testRootFolder = makeNewTestRoot();

        client0 = ::mega::make_unique<StandardClient>(testRootFolder, "c0");
        client1 = ::mega::make_unique<StandardClient>(testRootFolder, "c1");

        client0->logcb = true;
        client1->logcb = true;
    }

    ~SyncFingerprintCollisionTest()
    {
    }

    void SetUp() override
    {
        SdkTestBase::SetUp();

        SimpleLogger::setLogLevel(logMax);

        ASSERT_TRUE(client0->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "d", 1, 2));
        ASSERT_TRUE(client1->login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
        ASSERT_EQ(client0->basefolderhandle, client1->basefolderhandle);

        model0.root->addkid(model0.buildModelSubdirs("d", 2, 1, 0));
        model1.root->addkid(model1.buildModelSubdirs("d", 2, 1, 0));

        // Make sure the client's agree on the cloud's state before proceeding.
        {
            auto* root = client0->gettestbasenode();
            ASSERT_NE(root, nullptr);

            auto predicate = SyncRemoteMatch(*root, model0.root.get());
            ASSERT_TRUE(client0->waitFor(predicate, DEFAULTWAIT));
            ASSERT_TRUE(client1->waitFor(predicate, DEFAULTWAIT));
        }

        startSyncs();
        waitOnSyncs();
        confirmModels();
    }

    void addModelFile(Model &model,
                      const std::string &directory,
                      const std::string &file,
                      const std::string &content)
    {
        auto *node = model.findnode(directory);
        ASSERT_NE(node, nullptr);

        node->addkid(model.makeModelSubfile(file, content));
    }

    void confirmModel(StandardClient &client, Model &model, handle backupId)
    {
        ASSERT_TRUE(client.confirmModel_mainthread(model.findnode("d"), backupId));
    }

    void confirmModels()
    {
        confirmModel(*client0, model0, backupId0);
        confirmModel(*client1, model1, backupId1);
    }

    const fs::path localRoot0() const
    {
        return client0->syncSet(backupId0).localpath;
    }

    const fs::path localRoot1() const
    {
        return client1->syncSet(backupId1).localpath;
    }

    void prepareForNodeUpdates()
    {
        client0->received_node_actionpackets = false;
        client1->received_node_actionpackets = false;
    }

    void startSyncs()
    {
        backupId0 = client0->setupSync_mainthread("s0", "d", false, true);
        ASSERT_NE(backupId0, UNDEF);
        backupId1 = client1->setupSync_mainthread("s1", "d", false, false);
        ASSERT_NE(backupId1, UNDEF);
    }

    void waitForNodeUpdates()
    {
        ASSERT_TRUE(client0->waitForNodesUpdated(30));
        ASSERT_TRUE(client1->waitForNodesUpdated(30));
    }

    void waitOnSyncs()
    {
        waitonsyncs(chrono::seconds(4), client0.get(), client1.get());
    }

    handle backupId0 = UNDEF;
    handle backupId1 = UNDEF;

    std::unique_ptr<StandardClient> client0;
    std::unique_ptr<StandardClient> client1;
    Model model0;
    Model model1;
    const std::size_t arbitraryFileLength;
}; /* SyncFingerprintCollision */

TEST_F(SyncFingerprintCollisionTest, DifferentMacSameName)
{
    auto data0 = randomData(arbitraryFileLength);
    auto data1 = data0;
    const auto path0 = localRoot0() / "d_0" / "a";
    const auto path1 = localRoot0() / "d_1" / "a";

    prepareForNodeUpdates();
    ASSERT_TRUE(createDataFile(path0, data0));
    client0->triggerPeriodicScanEarly(backupId0);
    waitForNodeUpdates();

    // Wait for the engine to process any further changes.
    waitOnSyncs();

    // Alter MAC but leave fingerprint untouched.
    data1[0x41] = static_cast<uint8_t>(~data1[0x41]);

    // Prepare the file outside of the sync's view.
    auto stamp = fs::last_write_time(path0);

    ASSERT_TRUE(createDataFileWithTimestamp(client0->fsBasePath / "a", data1, client0->fsBasePath, stamp));

    prepareForNodeUpdates();
    fs::rename(client0->fsBasePath / "a", path1);
    client0->triggerPeriodicScanEarly(backupId0);
    waitForNodeUpdates();

    // Wait for the engine to process changes.
    waitOnSyncs();

    addModelFile(model0, "d/d_0", "a", data0);
    addModelFile(model0, "d/d_1", "a", data1);
    addModelFile(model1, "d/d_0", "a", data0);
#ifdef SRW_NEEDED_FOR_THIS_ONE
    addModelFile(model1, "d/d_1", "a", data1); // SRW gets this one right
#else
    addModelFile(model1, "d/d_1", "a", data1); // with treatAsIfFileDataEqual we can get this one right
#endif
    model1.ensureLocalDebrisTmpLock("d");

    confirmModels();
}

TEST_F(SyncFingerprintCollisionTest, DifferentMacDifferentName)
{
    auto data0 = randomData(arbitraryFileLength);
    auto data1 = data0;
    const auto path0 = localRoot0() / "d_0" / "a";
    const auto path1 = localRoot0() / "d_0" / "b";

    prepareForNodeUpdates();
    ASSERT_TRUE(createDataFile(path0, data0));
    client0->triggerPeriodicScanEarly(backupId0);
    waitForNodeUpdates();

    // Process any further changes.
    waitOnSyncs();

    // Alter MAC but leave fingerprint untouched.
    data1[0x41] = static_cast<uint8_t>(~data1[0x41]);

    // Prepare the file outside of the engine's view.
    auto stamp = fs::last_write_time(path0);

    ASSERT_TRUE(createDataFileWithTimestamp(client0->fsBasePath / "a", data1, client0->fsBasePath, stamp));

    prepareForNodeUpdates();
    fs::rename(client0->fsBasePath / "a", path1);
    client0->triggerPeriodicScanEarly(backupId0);
    waitForNodeUpdates();

    // Wait for the engine to process our change.
    waitOnSyncs();

    addModelFile(model0, "d/d_0", "a", data0);
    addModelFile(model0, "d/d_0", "b", data1);
    addModelFile(model1, "d/d_0", "a", data0);
    addModelFile(model1, "d/d_0", "b", data1);
    model1.ensureLocalDebrisTmpLock("d");

    confirmModels();
}

TEST_F(SyncFingerprintCollisionTest, SameMacDifferentName)
{
    auto data0 = randomData(arbitraryFileLength);
    const auto path0 = localRoot0() / "d_0" / "a";
    const auto path1 = localRoot0() / "d_0" / "b";

    prepareForNodeUpdates();
    ASSERT_TRUE(createDataFile(path0, data0));
    client0->triggerPeriodicScanEarly(backupId0);
    waitForNodeUpdates();

    waitOnSyncs();

    // Build the file somewhere the sync won't notice.
    auto stamp = fs::last_write_time(path0);

    ASSERT_TRUE(createDataFileWithTimestamp(client0->fsBasePath / "b", data0, client0->fsBasePath, stamp));

    // Move file into place.
    prepareForNodeUpdates();
    fs::rename(client0->fsBasePath / "b", path1);
    client0->triggerPeriodicScanEarly(backupId0);
    waitForNodeUpdates();

    // Wait for the engine to process our changes.
    waitOnSyncs();

    addModelFile(model0, "d/d_0", "a", data0);
    addModelFile(model0, "d/d_0", "b", data0);
    addModelFile(model1, "d/d_0", "a", data0);
    addModelFile(model1, "d/d_0", "b", data0);
    model1.ensureLocalDebrisTmpLock("d");

    confirmModels();
}

class SyncTest
    : public SdkTestBase
{
public:

    // Sets up the test case.
    void SetUp() override
    {
        SdkTestBase::SetUp();

        LOG_info << "____TEST SetUp: " << ::testing::UnitTest::GetInstance()->current_test_info()->name();

        SimpleLogger::setLogLevel(logMax);
    }

    // Tears down the test case.
    void TearDown() override
    {
        LOG_info << "____TEST TearDown: " << ::testing::UnitTest::GetInstance()->current_test_info()->name();
    }

}; // SqliteDBTest

TEST_F(SyncTest, BasicSync_DelRemoteFolder)
{
    // delete a remote folder and confirm the client sending the request and another also synced both correctly update the disk
    fs::path localtestroot = makeNewTestRoot();
    auto clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    auto clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2));

    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 3, 3));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2));

    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2->setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);
    clientA1->logcb = clientA2->logcb = true;

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    // delete something remotely and let sync catch up
    clientA1->received_node_actionpackets = false;
    clientA2->received_node_actionpackets = false;

    ASSERT_TRUE(clientA1->deleteremote("f/f_2/f_2_1"));

    ASSERT_TRUE(clientA1->waitForNodesUpdated(60));
    ASSERT_TRUE(clientA2->waitForNodesUpdated(60));

    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);

    // check everything matches in both syncs (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_2/f_2_1", "f"));
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_DelLocalFolder)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    auto clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    auto clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2));

    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 3, 3));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2->setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);
    clientA1->logcb = clientA2->logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    auto checkpath = clientA1->syncSet(backupId1).localpath.u8string();
    out() << "checking paths " << checkpath;
    for(auto& p: fs::recursive_directory_iterator(TestFS::GetTestFolder()))
    {
        out() << "checking path is present: " << p.path().u8string();
    }
    // delete something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code e;
    auto nRemoved = fs::remove_all(clientA1->syncSet(backupId1).localpath / "f_2" / "f_2_1", e);
    ASSERT_TRUE(!e) << "remove failed " << (clientA1->syncSet(backupId1).localpath / "f_2" / "f_2_1").u8string() << " error " << e;
    ASSERT_GT(static_cast<unsigned int>(nRemoved), 0u) << e;

    clientA1->triggerPeriodicScanEarly(backupId1);

    // let them catch up
    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_2/f_2_1", "f"));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));
    ASSERT_TRUE(model.removesynctrash("f"));
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
}

TEST_F(SyncTest, BasicSync_MoveLocalFolderPlain)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    auto clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    auto clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2));

    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 3, 3));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2));

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2->setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(8), clientA1, clientA2);
    clientA1->logcb = clientA2->logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    out() << "----- making sync change to test, now -----";
    clientA1->received_node_actionpackets = false;
    clientA2->received_node_actionpackets = false;

    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code rename_error;
    fs::rename(clientA1->syncSet(backupId1).localpath / "f_2" / "f_2_1", clientA1->syncSet(backupId1).localpath / "f_2_1", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    clientA1->triggerPeriodicScanEarly(backupId1);

    // client1 should send a rename command to the API
    // both client1 and client2 should receive the corresponding actionpacket
    ASSERT_TRUE(clientA1->waitForNodesUpdated(60)) << " no actionpacket received in clientA1 for rename";
    ASSERT_TRUE(clientA2->waitForNodesUpdated(60)) << " no actionpacket received in clientA2 for rename";

    out() << "----- wait for actionpackets ended -----";

    // sync activity should not take much longer after that.
    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movenode("f/f_2/f_2_1", "f"));
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_MoveLocalFolderBetweenSyncs)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();

    auto clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    auto clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    auto clientA3 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2

    ASSERT_TRUE(clientA1->waitForAttrDeviceIdIsSet(60)) << "Error User attr device id isn't establised client1";
    ASSERT_TRUE(clientA2->waitForAttrDeviceIdIsSet(60)) << "Error User attr device id isn't establised client2";
    ASSERT_TRUE(clientA3->waitForAttrDeviceIdIsSet(60)) << "Error User attr device id isn't establised client3";

    ASSERT_TRUE(clientA1->waitForAttrMyBackupIsSet(60)) << "Error User attr My Back Folder isn't establised client1";
    ASSERT_TRUE(clientA2->waitForAttrMyBackupIsSet(60)) << "Error User attr My Back Folder isn't establised client2";
    ASSERT_TRUE(clientA3->waitForAttrMyBackupIsSet(60)) << "Error User attr My Back Folder isn't establised client3";

    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2, clientA3));
    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 3, 3));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2, clientA3));

    // set up sync for A1 and A2, it should build matching local folders
    handle backupId11 = clientA1->setupSync_mainthread("sync1", "f/f_0", false, false);
    ASSERT_NE(backupId11, UNDEF);
    handle backupId12 = clientA1->setupSync_mainthread("sync2", "f/f_2", false, false);
    ASSERT_NE(backupId12, UNDEF);
    handle backupId21 = clientA2->setupSync_mainthread("syncA2_1", "f/f_0", false, false);
    ASSERT_NE(backupId21, UNDEF);
    handle backupId22 = clientA2->setupSync_mainthread("syncA2_2", "f/f_2", false, false);
    ASSERT_NE(backupId22, UNDEF);
    handle backupId31 = clientA3->setupSync_mainthread("syncA3", "f", false, false);
    ASSERT_NE(backupId31, UNDEF);
    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2, clientA3);
    clientA1->logcb = clientA2->logcb = clientA3->logcb = true;

    // also set up backups and move between backup/sync  (to check vw:1 flag is set appropriately both ways)
    handle backupId3b1 = clientA3->setupBackup_mainthread("backup1");
    ASSERT_NE(backupId3b1, UNDEF);
    handle backupId3b2 = clientA3->setupBackup_mainthread("backup2");
    ASSERT_NE(backupId3b2, UNDEF);

    std::error_code fs_error;
    fs::create_directory(clientA3->syncSet(backupId3b1).localpath / "backup1subfolder", fs_error);
    ASSERT_TRUE(!fs_error) << fs_error;
    fs::create_directory(clientA3->syncSet(backupId3b2).localpath / "backup2subfolder", fs_error);
    ASSERT_TRUE(!fs_error) << fs_error;
    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2, clientA3);

    // Create models.
    Model backup1model;
    Model backup2model;
    backup1model.addfolder("backup1subfolder");
    backup2model.addfolder("backup2subfolder");
    ASSERT_TRUE(clientA3->confirmModel_mainthread(backup1model.root.get(), backupId3b1));
    ASSERT_TRUE(clientA3->confirmModel_mainthread(backup2model.root.get(), backupId3b2));

    // Create Sync models.
    Model modelF;
    Model modelF0;
    Model modelF2;

    // f
    modelF.root->addkid(modelF.buildModelSubdirs("f", 3, 3, 0));
    modelF.ensureLocalDebrisTmpLock("f");

    // f_0
    modelF0.root->addkid(modelF.findnode("f/f_0")->clone());
    modelF0.ensureLocalDebrisTmpLock("f_0");

    // f_2
    modelF2.root->addkid(modelF.findnode("f/f_2")->clone());
    modelF2.ensureLocalDebrisTmpLock("f_2");

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(modelF0.findnode("f_0"), backupId11));
    ASSERT_TRUE(clientA1->confirmModel_mainthread(modelF2.findnode("f_2"), backupId12));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(modelF0.findnode("f_0"), backupId21));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(modelF2.findnode("f_2"), backupId22));
    ASSERT_TRUE(clientA3->confirmModel_mainthread(modelF.findnode("f"), backupId31));

    LOG_debug << "----- making sync change to test, now -----";
    clientA1->received_node_actionpackets = false;
    clientA2->received_node_actionpackets = false;
    clientA3->received_node_actionpackets = false;

    // move a folder form one local synced folder to another local synced folder and see if we sync correctly and catch up in A2 and A3 (mover and observer syncs)
    error_code rename_error;
    fs::path path1 = clientA1->syncSet(backupId11).localpath / "f_0_1";
    fs::path path2 = clientA1->syncSet(backupId12).localpath / "f_2_1" / "f_2_1_0" / "f_0_1";
    fs::rename(path1, path2, rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    clientA1->triggerPeriodicScanEarly(backupId11);
    clientA1->triggerPeriodicScanEarly(backupId12);

    // client1 should send a rename command to the API
    // both client1 and client2 should receive the corresponding actionpacket
    ASSERT_TRUE(clientA1->waitForNodesUpdated(30)) << " no actionpacket received in clientA1 for rename";
    ASSERT_TRUE(clientA2->waitForNodesUpdated(30)) << " no actionpacket received in clientA2 for rename";
    ASSERT_TRUE(clientA3->waitForNodesUpdated(30)) << " no actionpacket received in clientA3 for rename";

    // let them catch up
    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2, clientA3);

    // Update models.
    modelF.movenode("f/f_0/f_0_1", "f/f_2/f_2_1/f_2_1_0");
    modelF2.findnode("f_2/f_2_1/f_2_1_0")->addkid(modelF0.removenode("f_0/f_0_1"));

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(modelF0.findnode("f_0"), backupId11));
    ASSERT_TRUE(clientA1->confirmModel_mainthread(modelF2.findnode("f_2"), backupId12));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(modelF0.findnode("f_0"), backupId21));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(modelF2.findnode("f_2"), backupId22));
    ASSERT_TRUE(clientA3->confirmModel_mainthread(modelF.findnode("f"), backupId31));

    // now try moving between syncs and backups

    fs::rename(clientA3->syncSet(backupId3b1).localpath / "backup1subfolder",
               clientA3->syncSet(backupId31).localpath / "backup1subfolder", fs_error);
    ASSERT_TRUE(!fs_error) << fs_error;
    fs::rename(clientA3->syncSet(backupId31).localpath / "f_2",
               clientA3->syncSet(backupId3b1).localpath / "f_2", fs_error);
    ASSERT_TRUE(!fs_error) << fs_error;

    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2, clientA3);

    backup1model.root->addkid(modelF.removenode("f/f_2"));
    modelF.findnode("f")->addkid(backup1model.removenode("backup1subfolder"));

    ASSERT_TRUE(clientA3->confirmModel_mainthread(backup1model.root.get(), backupId3b1));
    ASSERT_TRUE(clientA3->confirmModel_mainthread(modelF.findnode("f"), backupId31));

}

TEST_F(SyncTest, BasicSync_RenameLocalFile)
{
    static auto TIMEOUT = std::chrono::seconds(4);

    const fs::path root = makeNewTestRoot();

    auto client0 = g_clientManager->getCleanStandardClient(0, root); // user 1 client 1
    auto client1 = g_clientManager->getCleanStandardClient(0, root); // user 1 client 2
    ASSERT_TRUE(client0->resetBaseFolderMulticlient(client1));
    ASSERT_TRUE(client0->makeCloudSubdirs("x", 0, 0));
    ASSERT_TRUE(CatchupClients(client0, client1));

    // Log callbacks.
    client0->logcb = true;
    client1->logcb = true;

    // Log clients in.
    ///ASSERT_TRUE(client0.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));
    ///ASSERT_TRUE(client1.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(client0->basefolderhandle, client1->basefolderhandle);

    // Set up syncs.
    handle backupId0 = client0->setupSync_mainthread("s0", "x", false, true);
    ASSERT_NE(backupId0, UNDEF);
    handle backupId1 = client1->setupSync_mainthread("s1", "x", false, false);
    ASSERT_NE(backupId1, UNDEF);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, client0, client1);

    // Add x/f.
    ASSERT_TRUE(createNameFile(client0->syncSet(backupId0).localpath, "f"));

    client0->triggerPeriodicScanEarly(backupId0);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, client0, client1);

    // Confirm model.
    Model model1, model2;
    model1.root->addkid(model1.makeModelSubfolder("x"));
    model1.findnode("x")->addkid(model1.makeModelSubfile("f"));
    model2.root->addkid(model2.makeModelSubfolder("x"));
    model2.findnode("x")->addkid(model2.makeModelSubfile("f"));
    model2.ensureLocalDebrisTmpLock("x"); // since it downloaded f (uploaded by sync 1)

    ASSERT_TRUE(client0->confirmModel_mainthread(model1.findnode("x"), backupId0));
    ASSERT_TRUE(client1->confirmModel_mainthread(model2.findnode("x"), backupId1, true));

    // Rename x/f to x/g.
    fs::rename(client0->syncSet(backupId0).localpath / "f",
               client0->syncSet(backupId0).localpath / "g");

    client0->triggerPeriodicScanEarly(backupId0);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, client0, client1);

    // Update and confirm model.
    model1.findnode("x/f")->name = "g";
    model2.findnode("x/f")->name = "g";

    ASSERT_TRUE(client0->confirmModel_mainthread(model1.findnode("x"), backupId0));
    ASSERT_TRUE(client1->confirmModel_mainthread(model2.findnode("x"), backupId1, true));
}

TEST_F(SyncTest, BasicSync_AddLocalFolder)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClientInUse clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    StandardClientInUse clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2));
    ASSERT_TRUE(clientA2->makeCloudSubdirs("f", 3, 3));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2));

    ASSERT_EQ(clientA1->basefolderhandle, clientA2->basefolderhandle);

    Model model1, model2;
    model1.root->addkid(model1.buildModelSubdirs("f", 3, 3, 0));
    model2.root->addkid(model2.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2->setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);
    clientA1->logcb = clientA2->logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model1.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model2.findnode("f"), backupId2));

    // make new folders (and files) in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    ASSERT_TRUE(buildLocalFolders(clientA1->syncSet(backupId1).localpath / "f_2", "newkid", 2, 2, 2));

    clientA1->triggerPeriodicScanEarly(backupId1);

    // let them catch up
    // two minutes should be long enough to get past API_ETEMPUNAVAIL == -18 for sync2 downloading the files uploaded by sync1
    // 4 seconds was too short sometimes, sync2 not caught up yet, due to a few consecutive -3 for `g`
    waitonsyncs(std::chrono::seconds(10), clientA1, clientA2);

    // check everything matches (model has expected state of remote and local)
    model1.findnode("f/f_2")->addkid(model1.buildModelSubdirs("newkid", 2, 2, 2));
    model2.findnode("f/f_2")->addkid(model2.buildModelSubdirs("newkid", 2, 2, 2));
    model2.ensureLocalDebrisTmpLock("f"); // since we downloaded files

    ASSERT_TRUE(clientA1->confirmModel_mainthread(model1.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model2.findnode("f"), backupId2));
}

// todo: add this test once the sync can keep up with file system notifications - at the moment
// it's too slow because we wait for the cloud before processing the next layer of files+folders.
// So if we add enough changes to exercise the notification queue, we can't check the results because
// it's far too slow at the syncing stage.
TEST_F(SyncTest, BasicSync_MassNotifyFromLocalFolderTree)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClientInUse clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient());
    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 0, 0));
    ASSERT_TRUE(CatchupClients(clientA1));

    //ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 0, 0));
    //ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    //ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    //ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "f", 2));
    waitonsyncs(std::chrono::seconds(4), clientA1/*, &clientA2*/);
    //clientA1.logcb = clientA2.logcb = true;

    // Create a directory tree in one sync, it should be synced to the cloud and back to the other
    // Create enough files and folders that we put a strain on the notification logic: 3k entries
    ASSERT_TRUE(buildLocalFolders(clientA1->syncSet(backupId1).localpath, "initial", 0, 0, 16000));

    clientA1->triggerPeriodicScanEarly(backupId1);

    //waitonsyncs(std::chrono::seconds(10), &clientA1 /*, &clientA2*/);
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // wait until the notify queues subside, it shouldn't take too long.  Limit of 5 minutes
    auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - startTime < std::chrono::seconds(5 * 60))
    {
        size_t remaining = 0;
        auto result0 = clientA1->thread_do<bool>([&](StandardClient &sc, PromiseBoolSP p)
        {
            sc.client.syncs.forEachRunningSync(
              [&](Sync* s)
              {
                  for (int q = DirNotify::NUMQUEUES; q--; )
                  {
                      remaining += s->dirnotify->notifyq[q].size();
                  }
              });

            p->set_value(true);
        }, __FILE__, __LINE__);
        result0.get();
        if (!remaining) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    Model model;
    model.root->addkid(model.buildModelSubdirs("initial", 0, 0, 16000));

    clientA1->waitFor([&](StandardClient&){ return clientA1->transfersAdded.load() > 0; }, std::chrono::seconds(60));  // give it a chance to create all the nodes.

    // check everything matches (just local since it'll still be uploading files)
    clientA1->localNodesMustHaveNodes = false;
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.root.get(), backupId1, false, StandardClient::CONFIRM_LOCAL));
    //ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));

    ASSERT_GT(clientA1->transfersAdded.load(), 0u);
    clientA1->transfersAdded = 0;

    // rename all those files and folders, put a strain on the notify system again.
    // Also, no downloads (or uploads) should occur as a result of this.
 //   renameLocalFolders(clientA1.syncSet(backupId1).localpath, "renamed_");

    // let them catch up
    //waitonsyncs(std::chrono::seconds(10), &clientA1 /*, &clientA2*/);

    // rename is too slow to check, even just in localnodes, for now.

    //ASSERT_EQ(clientA1.transfersAdded.load(), 0u);

    //Model model2;
    //model2.root->addkid(model.buildModelSubdirs("renamed_initial", 0, 0, 100));

    //// check everything matches (model has expected state of remote and local)
    //ASSERT_TRUE(clientA1.confirmModel_mainthread(model2.root.get(), 1));
    ////ASSERT_TRUE(clientA2.confirmModel_mainthread(model2.findnode("f"), 2));
}



/* this one is too slow for regular testing with the current algorithm
TEST_F(SyncTest, BasicSync_MAX_NEWNODES1)
{
    // create more nodes than we can upload in one putnodes.
    // this tree is 5x5 and the algorithm ends up creating nodes one at a time so it's pretty slow (and doesn't hit MAX_NEWNODES as a result)
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "f", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "f", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));

    // make new folders in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    assert(MegaClient::MAX_NEWNODES < 3125);
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "g", 5, 5, 0));  // 5^5=3125 leaf folders, 625 pre-leaf etc

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f")->addkid(model.buildModelSubdirs("g", 5, 5, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));
}
*/

/* this one is too slow for regular testing with the current algorithm
TEST_F(SyncTest, BasicSync_MAX_NEWNODES2)
{
    // create more nodes than we can upload in one putnodes.
    // this tree is 5x5 and the algorithm ends up creating nodes one at a time so it's pretty slow (and doesn't hit MAX_NEWNODES as a result)
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "f", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "f", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));

    // make new folders in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    assert(MegaClient::MAX_NEWNODES < 3000);
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "g", 3000, 1, 0));

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f")->addkid(model.buildModelSubdirs("g", 3000, 1, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));
}
*/

TEST_F(SyncTest, BasicSync_MoveExistingIntoNewLocalFolder)
{
    // historic case:  in the local filesystem, create a new folder then move an existing file/folder into it
    fs::path localtestroot = makeNewTestRoot();
    StandardClientInUse clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    StandardClientInUse clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2));
    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 3, 3));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2));

    ASSERT_EQ(clientA1->basefolderhandle, clientA2->basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2->setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);
    clientA1->logcb = clientA2->logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folder in the local filesystem
    ASSERT_TRUE(buildLocalFolders(clientA1->syncSet(backupId1).localpath, "new", 1, 0, 0));
    // move an already synced folder into it
    error_code rename_error;
    fs::path path1 = clientA1->syncSet(backupId1).localpath / "f_2"; // / "f_2_0" / "f_2_0_0";
    fs::path path2 = clientA1->syncSet(backupId1).localpath / "new" / "f_2"; // "f_2_0_0";
    fs::rename(path1, path2, rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    clientA1->triggerPeriodicScanEarly(backupId1);

    // let them catch up
    waitonsyncs(std::chrono::seconds(10), clientA1, clientA2);

    // check everything matches (model has expected state of remote and local)
    auto f = model.makeModelSubfolder("new");
    f->addkid(model.removenode("f/f_2")); // / f_2_0 / f_2_0_0"));
    model.findnode("f")->addkid(std::move(f));
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_MoveSeveralExistingIntoDeepNewLocalFolders)
{
    // historic case:  in the local filesystem, create a new folder then move an existing file/folder into it
    fs::path localtestroot = makeNewTestRoot();
    StandardClientInUse clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    StandardClientInUse clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2));
    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 3, 3));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2));

    ASSERT_EQ(clientA1->basefolderhandle, clientA2->basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2->setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);
    clientA1->logcb = clientA2->logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folder tree in the local filesystem
    ASSERT_TRUE(buildLocalFolders(clientA1->syncSet(backupId1).localpath, "new", 3, 3, 3));

    // move already synced folders to serveral parts of it - one under another moved folder too
    error_code rename_error;
    fs::rename(clientA1->syncSet(backupId1).localpath / "f_0", clientA1->syncSet(backupId1).localpath / "new" / "new_0" / "new_0_1" / "new_0_1_2" / "f_0", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;
    fs::rename(clientA1->syncSet(backupId1).localpath / "f_1", clientA1->syncSet(backupId1).localpath / "new" / "new_1" / "new_1_2" / "f_1", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;
    fs::rename(clientA1->syncSet(backupId1).localpath / "f_2", clientA1->syncSet(backupId1).localpath / "new" / "new_1" / "new_1_2" / "f_1" / "f_1_2" / "f_2", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    clientA1->triggerPeriodicScanEarly(backupId1);

    // let them catch up
    waitonsyncs(std::chrono::seconds(20), clientA1, clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f")->addkid(model.buildModelSubdirs("new", 3, 3, 3));
    model.findnode("f/new/new_0/new_0_1/new_0_1_2")->addkid(model.removenode("f/f_0"));
    model.findnode("f/new/new_1/new_1_2")->addkid(model.removenode("f/f_1"));
    model.findnode("f/new/new_1/new_1_2/f_1/f_1_2")->addkid(model.removenode("f/f_2"));
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));
}


#if defined(MEGA_MEASURE_CODE) || defined(DEBUG) // for sendDeferred()
TEST_F(SyncTest, BasicSync_MoveTwiceLocallyButCloudMoveRequestDelayed)
{
    fs::path localtestroot = makeNewTestRoot();
    StandardClientInUse c = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    ASSERT_TRUE(c->resetBaseFolderMulticlient());

    ASSERT_TRUE(c->makeCloudSubdirs("s", 0, 0));

    Model m;
    m.addfolder("a");
    m.addfolder("b");
    m.addfolder("c");
    m.generate(c->fsBasePath / "s");

    handle backupId = c->setupSync_mainthread("s", "s", false, false);
    ASSERT_NE(backupId, UNDEF);

    waitonsyncs(std::chrono::seconds(4), c);
    ASSERT_TRUE(c->confirmModel_mainthread(m.root.get(), backupId));
    c->logcb = true;

    fs::path path1 = c->syncSet(backupId).localpath;

    LOG_info << "Preventing move reqs being sent, then making local move for sync code to upsync";

    c->client.reqs.deferRequests = [](Command* c)
        {
            return true; // nothing can be sent, same as network disconnected
        };

    error_code fs_error;
    fs::rename(path1 / "a", path1 / "b" / "a", fs_error);
    ASSERT_TRUE(!fs_error) << fs_error;

    WaitMillisec(2000);

    fs::create_directory(path1 / "new", fs_error);
    ASSERT_TRUE(!fs_error) << fs_error;

    LOG_info << "Moving folder `a` a second time, into a new folder, while the first move has not yet completed";
    fs::rename(path1 / "b" / "a", path1 / "new" / "a", fs_error);
    ASSERT_TRUE(!fs_error) << fs_error;

    WaitMillisec(2000);

    LOG_info << "Allowing move reqs to continue";

    c->sendDeferredAndReset();

    waitonsyncs(std::chrono::seconds(4), c);

    m.addfolder("new");
    m.addfolder("new/a");
    m.removenode("a");
    ASSERT_TRUE(c->confirmModel_mainthread(m.root.get(), backupId));
}
#endif

/* not expected to work yet
TEST_F(SyncTest, BasicSync_SyncDuplicateNames)
{
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);


    NewNode* nodearray = new NewNode[3];
    nodearray[0] = *clientA1.makeSubfolder("samename");
    nodearray[1] = *clientA1.makeSubfolder("samename");
    nodearray[2] = *clientA1.makeSubfolder("Samename");
    clientA1.resultproc.prepresult(StandardClient::PUTNODES, [this](error e) {
    });
    clientA1.client.putnodes(clientA1.basefolderhandle, nodearray, 3);

    // set up syncs, they should build matching local folders
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.makeModelSubfolder("samename"));
    model.root->addkid(model.makeModelSubfolder("samename"));
    model.root->addkid(model.makeModelSubfolder("Samename"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.root.get(), 2));
}*/

TEST_F(SyncTest, BasicSync_RemoveLocalNodeBeforeSessionResume)
{
    fs::path localtestroot = makeNewTestRoot();
    auto pclientA1 = ::mega::make_unique<StandardClient>(localtestroot, "clientA1");   // user 1 client 1
    // don't use client manager as this client gets replaced
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    pclientA1->received_node_actionpackets = false;
    clientA2.received_node_actionpackets = false;

    // set up sync for A1, it should build matching local folders
    handle backupId1 = pclientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);

    // actually for this one, we don't expect actionpackets because all the cloud nodes are already present before the sync starts
    // ASSERT_TRUE(pclientA1->waitForNodesUpdated(30)) << " no actionpacket received in clientA1";
    // ASSERT_TRUE(clientA2.waitForNodesUpdated(30)) << " no actionpacket received in clientA2";

    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);
    pclientA1->logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // save session
    string session;
    pclientA1->client.dumpsession(session);

    // logout (but keep caches)
    fs::path sync1path = pclientA1->syncSet(backupId1).localpath;
    pclientA1->localLogout();

    pclientA1->received_syncs_restored = false;

    // remove local folders
    error_code e;
    ASSERT_TRUE(fs::remove_all(sync1path / "f_2", e) != static_cast<std::uintmax_t>(-1)) << e;

    // resume session, see if nodes and localnodes get in sync
    pclientA1.reset(new StandardClient(localtestroot, "clientA1"));
    ASSERT_TRUE(pclientA1->login_fetchnodes(session));

    // wait for normal sync resumes to complete
    pclientA1->waitFor([&](StandardClient& sc){ return sc.received_syncs_restored; }, std::chrono::seconds(30));

    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_2", "f"));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
    ASSERT_TRUE(model.removesynctrash("f"));
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
}

/* not expected to work yet
TEST_F(SyncTest, BasicSync_RemoteFolderCreationRaceSamename)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    // SN tagging needed for this one
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for both, it should build matching local folders (empty initially)
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // now have both clients create the same remote folder structure simultaneously.  We should end up with just one copy of it on the server and in both syncs
    future<bool> p1 = clientA1.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("f", 3, 3, pb); });
    future<bool> p2 = clientA2.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("f", 3, 3, pb); });
    ASSERT_TRUE(waitonresults(&p1, &p2));

    // let them catch up
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.root.get(), 2));
}*/

/* not expected to work yet
TEST_F(SyncTest, BasicSync_LocalFolderCreationRaceSamename)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    // SN tagging needed for this one
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for both, it should build matching local folders (empty initially)
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // now have both clients create the same folder structure simultaneously.  We should end up with just one copy of it on the server and in both syncs
    future<bool> p1 = clientA1.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { buildLocalFolders(sc.syncSet(backupId1).localpath, "f", 3, 3, 0); pb->set_value(true); });
    future<bool> p2 = clientA2.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { buildLocalFolders(sc.syncSet(backupId2).localpath, "f", 3, 3, 0); pb->set_value(true); });
    ASSERT_TRUE(waitonresults(&p1, &p2));

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.root.get(), 2));
}*/


TEST_F(SyncTest, BasicSync_ResumeSyncFromSessionAfterNonclashingLocalAndRemoteChanges )
{
    fs::path localtestroot = makeNewTestRoot();
    unique_ptr<StandardClient> pclientA1(new StandardClient(localtestroot, "clientA1"));   // user 1 client 1
    // don't use client manager as this client gets replaced
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    // set up sync for A1, it should build matching local folders
    handle backupId1 = pclientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);
    pclientA1->logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model1, model2;
    model1.root->addkid(model1.buildModelSubdirs("f", 3, 3, 0));
    model2.root->addkid(model2.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model1.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model2.findnode("f"), backupId2));

    out() << "********************* save session A1";
    string session;
    pclientA1->client.dumpsession(session);

    out() << "*********************  logout A1 (but keep caches on disk)";
    fs::path sync1path = pclientA1->syncSet(backupId1).localpath;
    pclientA1->localLogout();

    out() << "*********************  add remote folders via A2";
    future<bool> p1 = clientA2.thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("newremote", 2, 2, pb, "f/f_1/f_1_0"); }, __FILE__, __LINE__);
    model1.findnode("f/f_1/f_1_0")->addkid(model1.buildModelSubdirs("newremote", 2, 2, 0));
    model2.findnode("f/f_1/f_1_0")->addkid(model2.buildModelSubdirs("newremote", 2, 2, 0));
    ASSERT_TRUE(waitonresults(&p1));

    out() << "*********************  remove remote folders via A2";
    ASSERT_TRUE(clientA2.deleteremote("f/f_0"));
    model1.movetosynctrash("f/f_0", "f");
    model2.movetosynctrash("f/f_0", "f");

    out() << "*********************  add local folders in A1";
    ASSERT_TRUE(buildLocalFolders(sync1path / "f_1/f_1_2", "newlocal", 2, 2, 2));
    model1.findnode("f/f_1/f_1_2")->addkid(model1.buildModelSubdirs("newlocal", 2, 2, 2));
    model2.findnode("f/f_1/f_1_2")->addkid(model2.buildModelSubdirs("newlocal", 2, 2, 2));

    out() << "*********************  remove local folders in A1";
    error_code e;
    ASSERT_TRUE(fs::remove_all(sync1path / "f_2", e) != static_cast<std::uintmax_t>(-1)) << e;
    model1.removenode("f/f_2");
    model2.movetosynctrash("f/f_2", "f");

    out() << "*********************  get sync2 activity out of the way";
    waitonsyncs(std::chrono::seconds(4), &clientA2);

    out() << "*********************  resume A1 session (with sync), see if A2 nodes and localnodes get in sync again";
    pclientA1.reset(new StandardClient(localtestroot, "clientA1"));
    ASSERT_TRUE(pclientA1->login_fetchnodes(session));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    // wait for normal sync resumes to complete
    // wait bumped to 40 seconds here because we've seen a case where the 2nd client didn't receive actionpackets for 30 seconds
    pclientA1->waitFor([&](StandardClient& sc){ return sc.received_syncs_restored; }, std::chrono::seconds(40));

    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);

    out() << "*********************  check everything matches (model has expected state of remote and local)";
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model1.findnode("f"), backupId1));
    model2.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model2.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_ResumeSyncFromSessionAfterClashingLocalAddRemoteDelete)
{
    fs::path localtestroot = makeNewTestRoot();
    unique_ptr<StandardClient> pclientA1(new StandardClient(localtestroot, "clientA1"));   // user 1 client 1
    // don't use client manager as this client gets replaced
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = pclientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);
    pclientA1->logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // save session A1
    string session;
    pclientA1->client.dumpsession(session);
    fs::path sync1path = pclientA1->syncSet(backupId1).localpath;

    // logout A1 (but keep caches on disk)
    pclientA1->localLogout();

    // remove remote folder via A2
    ASSERT_TRUE(clientA2.deleteremote("f/f_1"));

    // add local folders in A1 on disk folder
    ASSERT_TRUE(buildLocalFolders(sync1path / "f_1/f_1_2", "newlocal", 2, 2, 2));

    // get sync2 activity out of the way
    waitonsyncs(std::chrono::seconds(4), &clientA2);

    // resume A1 session (with sync), see if A2 nodes and localnodes get in sync again
    pclientA1.reset(new StandardClient(localtestroot, "clientA1"));
    ASSERT_TRUE(pclientA1->login_fetchnodes(session));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    // wait for normal sync resumes to complete
    pclientA1->waitFor([&](StandardClient& sc){ return sc.received_syncs_restored; }, std::chrono::seconds(30));

    waitonsyncs(chrono::seconds(4), pclientA1.get(), &clientA2);

    // Sync rework update:  for now at least, delete wins in this case

    //ASSERT_EQ(waitResult[0].syncStalled, true);
    //ASSERT_EQ(1, waitResult[0].stalledNodePaths.size());
    //ASSERT_EQ(1, waitResult[0].stalledLocalPaths.size());

    Model modelLocal1;
    modelLocal1.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    modelLocal1.findnode("f/f_1/f_1_2")->addkid(model.buildModelSubdirs("newlocal", 2, 2, 2));
    //pclientA1->localNodesMustHaveNodes = false;
    ASSERT_TRUE(modelLocal1.movetosynctrash("f/f_1", "f"));

    Model modelRemote1;
    modelRemote1.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(modelRemote1.movetosynctrash("f/f_1", "f"));

    ASSERT_TRUE(pclientA1->confirmModel_mainthread(modelLocal1.findnode("f"), backupId1, false, StandardClient::CONFIRM_LOCAL));
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(modelRemote1.findnode("f"), backupId1, false, StandardClient::CONFIRM_REMOTE));
    //ASSERT_TRUE(modelRemote1.removesynctrash("f", "f_1/f_1_2/newlocal"));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(modelRemote1.findnode("f"), backupId2));
}

#ifdef SRW_NEEDED_FOR_THIS_ONE
TEST_F(SyncTest, BasicSync_ResumeSyncFromSessionAfterContractoryLocalAndRemoteMoves)
{
    fs::path localtestroot = makeNewTestRoot();
    unique_ptr<StandardClient> pclientA1(new StandardClient(localtestroot, "clientA1"));   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = pclientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);
    pclientA1->logcb = clientA2.logcb = true;

    auto client1LocalSyncRoot = pclientA1->syncSet(backupId1).localpath;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2, true));

    // save session A1
    string session;
    pclientA1->client.dumpsession(session);
    fs::path sync1path = pclientA1->syncSet(backupId1).localpath;

    // logout A1 (but keep caches on disk)
    pclientA1->localLogout();

    // move f_0 into f_1 remote
    ASSERT_TRUE(clientA2.movenode("f/f_0", "f/f_1"));

    // move f_1 into f_0 locally
    error_code rename_error;
    fs::rename(client1LocalSyncRoot / "f_1", client1LocalSyncRoot / "f_0" / "f_1", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    // get sync2 activity out of the way
    waitonsyncs(std::chrono::seconds(4), &clientA2);

    // resume A1 session (with sync), see if A2 nodes and localnodes get in sync again
    pclientA1.reset(new StandardClient(localtestroot, "clientA1"));
    ASSERT_TRUE(pclientA1->login_fetchnodes(session));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);
    vector<SyncWaitResult> waitResult = waitonsyncs(chrono::seconds(4), pclientA1.get(), &clientA2);

    ASSERT_EQ(waitResult[0].syncStalled, true);
    ASSERT_EQ(2u, waitResult[0].stall.cloud.size());  // for now at least, reporting source and destination nodes for each move
    ASSERT_EQ(2u, waitResult[0].stall.local.size());
    ASSERT_EQ(waitResult[0].stall.cloud.begin()->first, "/mega_test_sync/f/f_0");
    ASSERT_EQ(waitResult[0].stall.cloud.rbegin()->first, "/mega_test_sync/f/f_1/f_0");
    ASSERT_EQ(waitResult[0].stall.local.begin()->first.toPath(false), (client1LocalSyncRoot / "f_0" / "f_1").u8string() );
    ASSERT_EQ(waitResult[0].stall.local.rbegin()->first.toPath(false), (client1LocalSyncRoot / "f_1").u8string() );
}
#endif

TEST_F(SyncTest, CmdChecks_RRAttributeAfterMoveNode)
{
    fs::path localtestroot = makeNewTestRoot();
    unique_ptr<StandardClient> pclientA1(new StandardClient(localtestroot, "clientA1"));   // user 1 client 1
    // don't use client manager as this client gets replaced

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));

    Node* f = pclientA1->drillchildnodebyname(pclientA1->gettestbasenode(), "f");
    handle original_f_handle = f->nodehandle;
    handle original_f_parent_handle = f->parent->nodehandle;

    // make sure there are no 'f' in the rubbish
    auto fv = pclientA1->drillchildnodesbyname(pclientA1->getcloudrubbishnode(), "f");
    future<bool> fb = pclientA1->thread_do<bool>([&fv](StandardClient& sc, PromiseBoolSP pb) { sc.deleteremotenodes(fv, pb); }, __FILE__, __LINE__);
    ASSERT_TRUE(waitonresults(&fb));

    f = pclientA1->drillchildnodebyname(pclientA1->getcloudrubbishnode(), "f");
    ASSERT_TRUE(f == nullptr);


    // remove remote folder via A2
    future<bool> p1 = pclientA1->thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb)
        {
            sc.movenodetotrash("f", pb);
        }, __FILE__, __LINE__);
    ASSERT_TRUE(waitonresults(&p1));

    WaitMillisec(3000);  // allow for attribute delivery too

    f = pclientA1->drillchildnodebyname(pclientA1->getcloudrubbishnode(), "f");
    ASSERT_TRUE(f != nullptr);

    // check the restore-from-trash handle got set, and correctly
    nameid rrname = AttrMap::string2nameid("rr");
    ASSERT_EQ(f->nodehandle, original_f_handle);
    ASSERT_EQ(f->attrs.map[rrname], string(Base64Str<MegaClient::NODEHANDLE>(original_f_parent_handle)));
    ASSERT_EQ(f->attrs.map[rrname], string(Base64Str<MegaClient::NODEHANDLE>(pclientA1->gettestbasenode()->nodehandle)));

    // move it back
    ASSERT_TRUE(pclientA1->movenode(f->nodehandle, pclientA1->basefolderhandle));
    WaitMillisec(3000);  // allow for attribute delivery too

    // check it's back and the rr attribute is gone
    f = pclientA1->drillchildnodebyname(pclientA1->gettestbasenode(), "f");
    ASSERT_TRUE(f != nullptr);
    ASSERT_EQ(f->attrs.map[rrname], string());
}


#ifdef __linux__
TEST_F(SyncTest, BasicSync_SpecialCreateFile)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 2, 2));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 2, 2, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folders (and files) in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    ASSERT_TRUE(createSpecialFiles(clientA1.syncSet(backupId1).localpath / "f_0", "newkid", 2));

    for (int i = 0; i < 2; ++i)
    {
        string filename = "file" + to_string(i) + "_" + "newkid";
        model.findnode("f/f_0")->addkid(model.makeModelSubfile(filename));
    }

    clientA1.triggerPeriodicScanEarly(backupId1);

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}
#endif

#ifdef SRW_NEEDED_FOR_THIS_ONE
TEST_F(SyncTest, BasicSync_moveAndDeleteLocalFile)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClientInUse clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    StandardClientInUse clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 1
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2));
    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 1, 1));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2));
    ASSERT_EQ(clientA1->basefolderhandle, clientA2->basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2->setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);
    clientA1->logcb = clientA2->logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code rename_error;
    fs::rename(clientA1->syncSet(backupId1).localpath / "f_0", clientA1->syncSet(backupId1).localpath / "renamed", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;
    fs::remove(clientA1->syncSet(backupId1).localpath / "renamed");

    clientA1->triggerPeriodicScanEarly(backupId1);

    // let them catch up
    waitonsyncs(DEFAULTWAIT, clientA1, clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_0", "f"));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));
    ASSERT_TRUE(model.removesynctrash("f"));
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
}
#endif

namespace {

string makefa(const string& name, int fakecrc, int mtime)
{
    AttrMap attrs;
    attrs.map['n'] = name;

    FileFingerprint ff;
    ff.crc[0] = ff.crc[1] = ff.crc[2] = ff.crc[3] = fakecrc;
    ff.mtime = mtime;
    ff.serializefingerprint(&attrs.map['c']);

    string attrjson;
    attrs.getjson(&attrjson);
    return attrjson;
}

Node* makenode(MegaClient& mc, NodeHandle parent, ::mega::nodetype_t type, m_off_t size, handle owner, const string& attrs, ::mega::byte* key)
{
    static handle handlegenerator = 10;
    auto newnode = new Node(mc, NodeHandle().set6byte(++handlegenerator), parent, type, size, owner, nullptr, 1);

    newnode->setkey(key);
    newnode->attrstring.reset(new string);

    SymmCipher sc;
    sc.setkey(key, type);
    mc.makeattr(&sc, newnode->attrstring, attrs.c_str());

    int attrlen = int(newnode->attrstring->size());
    string base64attrstring;
    base64attrstring.resize(static_cast<size_t>(attrlen * 4 / 3 + 4));
    base64attrstring.resize(static_cast<size_t>(Base64::btoa((::mega::byte *)newnode->attrstring->data(), int(newnode->attrstring->size()), (char *)base64attrstring.data())));

    *newnode->attrstring = base64attrstring;

    return newnode;
}

} // anonymous

TEST_F(SyncTest, NodeSorting_forPhotosAndVideos)
{
    fs::path localtestroot = makeNewTestRoot();

    // Don't use ClientManager or running PutnodesForMultipleFolders after this breaks

    StandardClient standardclient(localtestroot, "sortOrderTests");
    auto& client = standardclient.client;
    handle owner = 99999;

    ::mega::byte key[] = { 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04 };

    // first 3 are root nodes:
    auto cloudroot = makenode(client, NodeHandle(), ROOTNODE, -1, owner, makefa("root", 1, 1), key);
    makenode(client, NodeHandle(), VAULTNODE, -1, owner, makefa("inbox", 1, 1), key);
    makenode(client, NodeHandle(), RUBBISHNODE, -1, owner, makefa("bin", 1, 1), key);

    // now some files to sort
    auto photo1 = makenode(client, cloudroot->nodeHandle(), FILENODE, 9999, owner, makefa("abc.jpg", 1, 1570673890), key);
    auto photo2 = makenode(client, cloudroot->nodeHandle(), FILENODE, 9999, owner, makefa("cba.png", 1, 1570673891), key);
    auto video1 = makenode(client, cloudroot->nodeHandle(), FILENODE, 9999, owner, makefa("xyz.mov", 1, 1570673892), key);
    auto video2 = makenode(client, cloudroot->nodeHandle(), FILENODE, 9999, owner, makefa("zyx.mp4", 1, 1570673893), key);
    auto otherfile = makenode(client, cloudroot->nodeHandle(), FILENODE, 9999, owner, makefa("ASDF.fsda", 1, 1570673894), key);
    auto otherfolder = makenode(client, cloudroot->nodeHandle(), FOLDERNODE, -1, owner, makefa("myfolder", 1, 1570673895), key);

    node_vector v{ photo1, photo2, video1, video2, otherfolder, otherfile };
    for (auto n : v) n->setkey(key);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_PHOTO_ASC, client);
    node_vector v2{ photo1, photo2, video1, video2, otherfolder, otherfile };
    ASSERT_EQ(v, v2);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_PHOTO_DESC, client);
    node_vector v3{ photo2, photo1, video2, video1, otherfolder, otherfile };
    ASSERT_EQ(v, v3);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_VIDEO_ASC, client);
    node_vector v4{ video1, video2, photo1, photo2, otherfolder, otherfile };
    ASSERT_EQ(v, v4);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_VIDEO_DESC, client);
    node_vector v5{ video2, video1, photo2, photo1, otherfolder, otherfile };
    ASSERT_EQ(v, v5);
}


TEST_F(SyncTest, PutnodesForMultipleFolders)
{
    fs::path localtestroot = makeNewTestRoot();

    auto standardclient = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2

    ASSERT_TRUE(standardclient->resetBaseFolderMulticlient());
    ASSERT_TRUE(CatchupClients(standardclient));

    vector<NewNode> newnodes(4);
    standardclient->client.putnodes_prepareOneFolder(&newnodes[0], "folder1", false);
    standardclient->client.putnodes_prepareOneFolder(&newnodes[1], "folder2", false);
    standardclient->client.putnodes_prepareOneFolder(&newnodes[2], "folder2.1", false);
    standardclient->client.putnodes_prepareOneFolder(&newnodes[3], "folder2.2", false);

    newnodes[1].nodehandle = newnodes[2].parenthandle = newnodes[3].parenthandle = 2;

    auto targethandle = standardclient->client.mNodeManager.getRootNodeFiles();

    std::atomic<bool> putnodesDone{false};
    standardclient->resultproc.prepresult(StandardClient::PUTNODES,  ++next_request_tag,
        [&](){ standardclient->client.putnodes(targethandle, NoVersioning, std::move(newnodes), nullptr, standardclient->client.reqtag, false); },
        [&putnodesDone](error e) { putnodesDone = true; return true; });

    while (!putnodesDone)
    {
        WaitMillisec(100);
    }

    Node* cloudRoot = standardclient->client.nodeByHandle(targethandle);

    ASSERT_TRUE(nullptr != standardclient->drillchildnodebyname(cloudRoot, "folder1"));
    ASSERT_TRUE(nullptr != standardclient->drillchildnodebyname(cloudRoot, "folder2"));
    ASSERT_TRUE(nullptr != standardclient->drillchildnodebyname(cloudRoot, "folder2/folder2.1"));
    ASSERT_TRUE(nullptr != standardclient->drillchildnodebyname(cloudRoot, "folder2/folder2.2"));
}

// this test fails frequently on develop due to race conditions with commands vs actionpackets on develop, re-enable after merging sync rework (which has SIC removed)
TEST_F(SyncTest, DISABLED_ExerciseCommands)
{
    fs::path localtestroot = makeNewTestRoot();
    StandardClient standardclient(localtestroot, "ExerciseCommands");
    ASSERT_TRUE(standardclient.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", true));

    // Using this set setup to execute commands direct in the SDK Core
    // so that we can test things that the MegaApi interface would
    // disallow or shortcut.

    // make sure it's a brand new folder
    future<bool> p1 = standardclient.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("testlinkfolder_brandnew3", 1, 1, pb); }, __FILE__, __LINE__);
    ASSERT_TRUE(waitonresults(&p1));

    assert(standardclient.lastPutnodesResultFirstHandle != UNDEF);
    Node* n2 = standardclient.client.nodebyhandle(standardclient.lastPutnodesResultFirstHandle);

    out() << "Testing make public link for node: " << n2->displaypath();

    // try to get a link on an existing unshared folder
    promise<Error> pe1, pe1a, pe2, pe3, pe4;
    standardclient.getpubliclink(n2, 0, 0, false, false, pe1);
    ASSERT_TRUE(debugTolerantWaitOnFuture(pe1.get_future(), 45));
    ASSERT_EQ(API_EACCESS, pe1.get_future().get());

    // create on existing node
    standardclient.exportnode(n2, 0, 0, false, false, pe1a);
    ASSERT_TRUE(debugTolerantWaitOnFuture(pe1a.get_future(), 45));
    ASSERT_EQ(API_OK, pe1a.get_future().get());

    // get link on existing shared folder node, with link already  (different command response)
    standardclient.getpubliclink(n2, 0, 0, false, false, pe2);
    ASSERT_TRUE(debugTolerantWaitOnFuture(pe2.get_future(), 45));
    ASSERT_EQ(API_OK, pe2.get_future().get());

    // delete existing link on node
    standardclient.getpubliclink(n2, 1, 0, false, false, pe3);
    ASSERT_TRUE(debugTolerantWaitOnFuture(pe3.get_future(), 45));
    ASSERT_EQ(API_OK, pe3.get_future().get());

    // create on non existent node
    n2->nodehandle = UNDEF;
    standardclient.getpubliclink(n2, 0, 0, false, false, pe4);
    ASSERT_TRUE(debugTolerantWaitOnFuture(pe4.get_future(), 45));
    ASSERT_EQ(API_EACCESS, pe4.get_future().get());
}

#ifndef _WIN32_SUPPORTS_SYMLINKS_IT_JUST_NEEDS_TURNING_ON
TEST_F(SyncTest, BasicSync_CreateAndDeleteLink)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    fs::remove(clientA1.syncSet(backupId1).localpath / "linked");
    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_CreateRenameAndDeleteLink)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();

    auto clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    auto clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2));
    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 1, 1));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2));

    ASSERT_EQ(clientA1->basefolderhandle, clientA2->basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2->setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);
    clientA1->logcb = clientA2->logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1->syncSet(backupId1).localpath / "f_0", clientA1->syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    // let them catch up
    waitonsyncs(DEFAULTWAIT, clientA1, clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    fs::rename(clientA1->syncSet(backupId1).localpath / "linked", clientA1->syncSet(backupId1).localpath / "linkrenamed", linkage_error);

    // let them catch up
    waitonsyncs(DEFAULTWAIT, clientA1, clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    fs::remove(clientA1->syncSet(backupId1).localpath / "linkrenamed");

    // let them catch up
    waitonsyncs(DEFAULTWAIT, clientA1, clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));
}

#ifndef WIN32

// what is supposed to happen for this one?  It seems that the `linked` symlink is no longer ignored on windows?  client2 is affected!

TEST_F(SyncTest, BasicSync_CreateAndReplaceLinkLocally)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();

    auto clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    auto clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2));
    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 1, 2));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2));

    ASSERT_EQ(clientA1->basefolderhandle, clientA2->basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 2, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2->setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);
    clientA1->logcb = clientA2->logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1->syncSet(backupId1).localpath / "f_0", clientA1->syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    clientA1->triggerPeriodicScanEarly(backupId1);

    // let them catch up
    waitonsyncs(DEFAULTWAIT, clientA1, clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));
    fs::rename(clientA1->syncSet(backupId1).localpath / "f_0", clientA1->syncSet(backupId1).localpath / "linked", linkage_error);

    // let them catch up
    waitonsyncs(DEFAULTWAIT, clientA1, clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    fs::remove(clientA1->syncSet(backupId1).localpath / "linked");

    clientA1->triggerPeriodicScanEarly(backupId1);

    ASSERT_TRUE(createNameFile(clientA1->syncSet(backupId1).localpath, "linked"));

    clientA1->triggerPeriodicScanEarly(backupId1);

    // Wait for the engine to synchronize changes.
    waitonsyncs(DEFAULTWAIT, clientA1, clientA2);

    model.findnode("f")->addkid(model.makeModelSubfile("linked"));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files

    //check client 2 is as expected
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));
}

TEST_F(SyncTest, BasicSync_CreateAndReplaceLinkUponSyncDown)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();

    auto clientA1 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    auto clientA2 = g_clientManager->getCleanStandardClient(0, localtestroot); // user 1 client 2
    ASSERT_TRUE(clientA1->resetBaseFolderMulticlient(clientA2));
    ASSERT_TRUE(clientA1->makeCloudSubdirs("f", 1, 1));
    ASSERT_TRUE(CatchupClients(clientA1, clientA2));
    ASSERT_EQ(clientA1->basefolderhandle, clientA2->basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1->setupSync_mainthread("sync1", "f", false, true);
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2->setupSync_mainthread("sync2", "f", false, false);
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), clientA1, clientA2);
    clientA1->logcb = clientA2->logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1->syncSet(backupId1).localpath / "f_0", clientA1->syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    clientA1->triggerPeriodicScanEarly(backupId1);

    // let them catch up
    waitonsyncs(DEFAULTWAIT, clientA1, clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2->confirmModel_mainthread(model.findnode("f"), backupId2));

    ASSERT_TRUE(createNameFile(clientA2->syncSet(backupId2).localpath, "linked"));

    clientA2->triggerPeriodicScanEarly(backupId2);

    // let them catch up

    clientA1->triggerPeriodicScanEarly(backupId1);

    waitonsyncs(DEFAULTWAIT, clientA1, clientA2);

    model.findnode("f")->addkid(model.makeModelSubfolder("linked")); //notice: the deleted here is folder because what's actually deleted is a symlink that points to a folder
                                                                     //ideally we could add full support for symlinks in this tests suite

    model.movetosynctrash("f/linked","f");
    model.findnode("f")->addkid(model.makeModelSubfile("linked"));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files

    //check client 2 is as expected
    ASSERT_TRUE(clientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
}
#endif

#endif

TEST_F(SyncTest, BasicSync_NewVersionsCreatedWhenFilesModified)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = std::chrono::seconds(4);

    auto c = g_clientManager->getCleanStandardClient(0, TESTROOT);
    CatchupClients(c);

    // Log callbacks.
    c->logcb = true;

    // Fingerprints for each revision.
    vector<FileFingerprint> fingerprints;

    // Log client in.
    //ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));
    ASSERT_TRUE(c->resetBaseFolderMulticlient());
    ASSERT_TRUE(c->makeCloudSubdirs("x", 0, 0));
    ASSERT_TRUE(CatchupClients(c));

    // Add and start sync.
    const auto id = c->setupSync_mainthread("s", "x", false, true);
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c->syncSet(id).localpath;

    // Create and populate model.
    Model model;

    model.addfile("f", "a");
    model.generate(SYNCROOT);

    // Keep track of fingerprint.
    fingerprints.emplace_back(c->fingerprint(SYNCROOT / "f"));
    ASSERT_TRUE(fingerprints.back().isvalid);

    c->triggerPeriodicScanEarly(id);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Check that the file made it to the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Create a new revision of f.
    model.addfile("f", "b");
    model.generate(SYNCROOT);

    // Update fingerprint.
    fingerprints.emplace_back(c->fingerprint(SYNCROOT / "f"));
    ASSERT_TRUE(fingerprints.back().isvalid);

    c->triggerPeriodicScanEarly(id);

    // Wait for change to propagate.
    waitonsyncs(TIMEOUT, c);

    // Validate model.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Create yet anothet revision of f.
    model.addfile("f", "c");
    model.generate(SYNCROOT);

    // Update fingerprint.
    fingerprints.emplace_back(c->fingerprint(SYNCROOT / "f"));
    ASSERT_TRUE(fingerprints.back().isvalid);

    c->triggerPeriodicScanEarly(id);

    // Wait for change to propagate.
    waitonsyncs(TIMEOUT, c);

    // Validate model.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Get our hands on f's node.
    auto *f = c->drillchildnodebyname(c->gettestbasenode(), "x/f");
    ASSERT_TRUE(f);

    // Validate the version chain.
    auto i = fingerprints.crbegin();
    auto matched = true;

    while (f && i != fingerprints.crend())
    {
        matched &= *f == *i++;

        node_list children = c->client.getChildren(f);
        f = children.empty() ? nullptr : children.front();
    }

    matched &= !f && i == fingerprints.crend();
    ASSERT_TRUE(matched);
}

TEST_F(SyncTest, BasicSync_ClientToSDKConfigMigration)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = std::chrono::seconds(4);

    SyncConfig config0;
    SyncConfig config1;
    Model model;

    // Create some syncs for us to migrate.
    {
        StandardClient c0(TESTROOT, "c0");

        // Log callbacks.
        c0.logcb = true;

        // Log in client.
        ASSERT_TRUE(c0.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 1, 2));

        // Add syncs.
        auto id0 = c0.setupSync_mainthread("s0", "s/s_0", false, true);
        ASSERT_NE(id0, UNDEF);

        auto id1 = c0.setupSync_mainthread("s1", "s/s_1", false, false);
        ASSERT_NE(id1, UNDEF);

        // Populate filesystem.
        auto root0 = c0.syncSet(id0).localpath;
        auto root1 = c0.syncSet(id1).localpath;

        model.addfile("d/f");
        model.addfile("f");
        model.generate(root0);
        model.generate(root1, true);

        c0.triggerPeriodicScanEarly(id0);
        c0.triggerPeriodicScanEarly(id1);

        // Wait for sync to complete.
        waitonsyncs(TIMEOUT, &c0);

        // Make sure everything arrived safely.
        ASSERT_TRUE(c0.confirmModel_mainthread(model.root.get(), id0));
        ASSERT_TRUE(c0.confirmModel_mainthread(model.root.get(), id1));

        // Get our hands on the configs.
        config0 = c0.syncConfigByBackupID(id0);
        config1 = c0.syncConfigByBackupID(id1);
    }

    // Migrate the configs.
    StandardClient c1(TESTROOT, "c1");

    // Log callbacks.
    c1.logcb = true;

    // Log in the client.
    ASSERT_TRUE(c1.login("MEGA_EMAIL", "MEGA_PWD"));

    // Make sure sync user attributes are present.
    ASSERT_TRUE(c1.ensureSyncUserAttributes());

    // Update configs so they're useful for this client.
    {
        auto root0 = TESTROOT / "c1" / "s0";
        auto root1 = TESTROOT / "c1" / "s1";

        // Issue new backup IDs.
        config0.mBackupId = UNDEF;
        config1.mBackupId = UNDEF;

        // Update path for c1.
        config0.mLocalPath = LocalPath::fromAbsolutePath(root0.u8string());
        config1.mLocalPath = LocalPath::fromAbsolutePath(root1.u8string());

        // Make sure local sync roots exist.
        fs::create_directories(root0);
        fs::create_directories(root1);
    }

    // Migrate the configs.
    auto id0 = c1.copySyncConfig(config0);
    ASSERT_NE(id0, UNDEF);
    auto id1 = c1.copySyncConfig(config1);
    ASSERT_NE(id1, UNDEF);

    // So we can wait until the syncs are resumed.
    promise<void> notify;

    // Hook OnSyncStateConfig callback.
    c1.mOnSyncStateConfig = ([id0, id1, &notify]() {
        auto waiting = std::make_shared<set<handle>>();

        // Track the syncs we're waiting for.
        waiting->emplace(id0);
        waiting->emplace(id1);

        // Return effective callback.
        return [&notify, waiting](const SyncConfig& config) {
            // Is the sync running?
            if (config.mRunState != SyncRunState::Run)
                return;

            // This sync's been resumed.
            waiting->erase(config.mBackupId);

            // Are we still waiting for any syncs to resume?
            if (!waiting->empty())
                return;

            // Let the waiter know the syncs are up.
            notify.set_value();
        };
    })();

    // Fetch nodes (and resume syncs.)
    ASSERT_TRUE(c1.fetchnodes());

    // Wait for the syncs to be resumed.
    ASSERT_TRUE(debugTolerantWaitOnFuture(notify.get_future(), 45));

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c1);

    // Check that all files from the cloud were downloaded.
    model.ensureLocalDebrisTmpLock("");
    ASSERT_TRUE(c1.confirmModel_mainthread(model.root.get(), id0));
    model.removenode(DEBRISFOLDER);
    ASSERT_TRUE(c1.confirmModel_mainthread(model.root.get(), id1));
}

#ifdef SRW_NEEDED_FOR_THIS_ONE
TEST_F(SyncTest, DetectsAndReportsNameClashes)
{
    const auto TESTFOLDER = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    StandardClientInUse client = g_clientManager->getCleanStandardClient(0, TESTFOLDER);
    ASSERT_TRUE(client->resetBaseFolderMulticlient());
    ASSERT_TRUE(client->makeCloudSubdirs("x", 0, 0));
    ASSERT_TRUE(CatchupClients(client));

    // Needed so that we can create files with the same name.
    client->client.versions_disabled = true;

    // Populate local filesystem.
    const auto root = client->fsBasePath / "s";

    fs::create_directories(root / "d" / "e");

    createNameFile(root / "d", "f0");
    createNameFile(root / "d", "f%30");
    createNameFile(root / "d" / "e", "g0");
    createNameFile(root / "d" / "e", "g%30");

    // Start the sync.
    handle backupId1 = client->setupSync_mainthread("s", "x", false, true);
    ASSERT_NE(backupId1, UNDEF);

    // Give the client time to synchronize.
    waitonsyncs(TIMEOUT, client);

    // Helpers.
    auto localConflictDetected = [](const NameConflict& nc, const LocalPath& name)
    {
        auto i = nc.clashingLocalNames.begin();
        auto j = nc.clashingLocalNames.end();

        return std::find(i, j, name) != j;
    };

    // Were any conflicts detected?
    // Can we obtain a list of the conflicts?
    list<NameConflict> conflicts;
    ASSERT_TRUE(client->conflictsDetected(conflicts));
    ASSERT_EQ(conflicts.size(), 2u);
    ASSERT_EQ(conflicts.back().localPath, LocalPath::fromRelativePath("d").prependNewWithSeparator(client->syncByBackupId(backupId1)->localroot->localname));
    ASSERT_EQ(conflicts.back().clashingLocalNames.size(), 2u);
    ASSERT_TRUE(localConflictDetected(conflicts.back(), LocalPath::fromRelativePath("f%30")));
    ASSERT_TRUE(localConflictDetected(conflicts.back(), LocalPath::fromRelativePath("f0")));
    ASSERT_EQ(conflicts.back().clashingCloudNames.size(), 0u);

    client->triggerPeriodicScanEarly(backupId1);

    // Resolve the f0 / f%30 conflict.
    ASSERT_TRUE(fs::remove(root / "d" / "f%30"));

    // Give the sync some time to think.
    waitonsyncs(TIMEOUT, client);

    // We should still detect conflicts.
    // Has the list of conflicts changed?
    conflicts.clear();
    ASSERT_TRUE(client->conflictsDetected(conflicts));
    ASSERT_GE(conflicts.size(), 1u);
    ASSERT_EQ(conflicts.front().localPath, LocalPath::fromRelativePath("e")
        .prependNewWithSeparator(LocalPath::fromRelativePath("d"))
        .prependNewWithSeparator(client->syncByBackupId(backupId1)->localroot->localname));
    ASSERT_EQ(conflicts.front().clashingLocalNames.size(), 2u);
    ASSERT_TRUE(localConflictDetected(conflicts.front(), LocalPath::fromRelativePath("g%30")));
    ASSERT_TRUE(localConflictDetected(conflicts.front(), LocalPath::fromRelativePath("g0")));
    ASSERT_EQ(conflicts.front().clashingCloudNames.size(), 0u);

    // Resolve the g / g%30 conflict.
    ASSERT_TRUE(fs::remove(root / "d" / "e" / "g%30"));

    client->triggerPeriodicScanEarly(backupId1);

    // Give the sync some time to think.
    waitonsyncs(TIMEOUT, client);

    // No conflicts should be reported.
    // Is the list of conflicts empty?
    conflicts.clear();
    ASSERT_FALSE(client->conflictsDetected(conflicts));
    ASSERT_EQ(conflicts.size(), 0u);

    // Create a remote name clash.
    auto* node = client->drillchildnodebyname(client->gettestbasenode(), "x/d");
    ASSERT_TRUE(!!node);
    ASSERT_TRUE(client->uploadFile(root / "d" / "f0", "h", node));
    ASSERT_TRUE(client->uploadFile(root / "d" / "f0", "h", node));

    // Let the client attempt to synchronize.
    waitonsyncs(TIMEOUT, client);

    // Have we detected any conflicts?
    conflicts.clear();
    ASSERT_TRUE(client->conflictsDetected(conflicts));

    // Does our list of conflicts include remotes?
    ASSERT_GE(conflicts.size(), 1u);
    ASSERT_EQ(conflicts.front().cloudPath, string("/mega_test_sync/x/d"));
    ASSERT_EQ(conflicts.front().clashingCloudNames.size(), 2u);
    ASSERT_EQ(conflicts.front().clashingCloudNames[0], string("h"));
    ASSERT_EQ(conflicts.front().clashingCloudNames[1], string("h"));
    ASSERT_EQ(conflicts.front().clashingLocalNames.size(), 0u);

    // Resolve the remote conflict.
    ASSERT_TRUE(client->deleteremote("x/d/h"));

    // Wait for the client to process our changes.
    waitonsyncs(TIMEOUT, client);

    conflicts.clear();
    client->conflictsDetected(conflicts);
    ASSERT_EQ(0u, conflicts.size());

    // Conflicts should be resolved.
    conflicts.clear();
    ASSERT_FALSE(client->conflictsDetected(conflicts));
}
#endif

#ifdef SRW_NEEDED_FOR_THIS_ONE
TEST_F(SyncTest, DoesntDownloadFilesWithClashingNames)
{
    const auto TESTFOLDER = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    StandardClientInUse cu = g_clientManager->getCleanStandardClient(0, TESTFOLDER);
    StandardClientInUse cd = g_clientManager->getCleanStandardClient(0, TESTFOLDER);
    // Log callbacks.
    cu->logcb = true;
    cd->logcb = true;
    ASSERT_TRUE(cd->resetBaseFolderMulticlient(cu));
    ASSERT_TRUE(cd->makeCloudSubdirs("x", 0, 0));
    ASSERT_TRUE(CatchupClients(cd, cu));

    // Populate cloud.
    {
        // Needed so that we can create files with the same name.
        cu->client.versions_disabled = true;

        // Create local test hierarchy.
        const auto root = cu->fsBasePath / "x";

        // d will be duplicated and generate a clash.
        fs::create_directories(root / "d");

        // dd will be singular, no clash.
        fs::create_directories(root / "dd");

        // f will be duplicated and generate a clash.
        ASSERT_TRUE(createNameFile(root, "f"));

        // ff will be singular, no clash.
        ASSERT_TRUE(createNameFile(root, "ff"));

        auto* node = cu->drillchildnodebyname(cu->gettestbasenode(), "x");
        ASSERT_TRUE(!!node);

        // Upload d twice, generate clash.
        ASSERT_TRUE(cu->uploadFolderTree(root / "d", node));
        ASSERT_TRUE(cu->uploadFolderTree(root / "d", node));

        // Upload dd once.
        ASSERT_TRUE(cu->uploadFolderTree(root / "dd", node));

        // Upload f twice, generate clash.
        ASSERT_TRUE(cu->uploadFile(root / "f", node));
        ASSERT_TRUE(cu->uploadFile(root / "f", node));

        // Upload ff once.
        ASSERT_TRUE(cu->uploadFile(root / "ff", node));
    }

    // Add and start sync.
    handle backupId1 = cd->setupSync_mainthread("sd", "x", false, false);
    ASSERT_NE(backupId1, UNDEF);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, cd);

    // Populate and confirm model.
    Model model;

    // d and f are missing due to name collisions in the cloud.
    model.root->addkid(model.makeModelSubfolder("x"));
    model.findnode("x")->addkid(model.makeModelSubfolder("dd"));
    model.findnode("x")->addkid(model.makeModelSubfile("ff"));

    // Needed because we've downloaded files.
    model.ensureLocalDebrisTmpLock("x");

    // Confirm the model.
    ASSERT_TRUE(cd->confirmModel_mainthread(
                  model.findnode("x"),
                  backupId1,
                  false,
                  StandardClient::CONFIRM_LOCAL));

    // Resolve the name collisions.
    ASSERT_TRUE(cd->deleteremote("x/d"));
    ASSERT_TRUE(cd->deleteremote("x/f"));

    // Wait for the sync to update.
    waitonsyncs(TIMEOUT, cd);

    // Confirm that d and f have now been downloaded.
    model.findnode("x")->addkid(model.makeModelSubfolder("d"));
    model.findnode("x")->addkid(model.makeModelSubfile("f"));

    // Local FS, Local Tree and Remote Tree should now be consistent.
    ASSERT_TRUE(cd->confirmModel_mainthread(model.findnode("x"), backupId1));
}

// TODO: re-enable after sync rework is merged
TEST_F(SyncTest, DoesntUploadFilesWithClashingNames)
{
    const auto TESTFOLDER = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    StandardClientInUse cd = g_clientManager->getCleanStandardClient(0, TESTFOLDER);
    StandardClientInUse cu = g_clientManager->getCleanStandardClient(0, TESTFOLDER);

    // Log callbacks.
    cd->logcb = true;
    cu->logcb = true;

    // Log in client.
    ASSERT_TRUE(cd->resetBaseFolderMulticlient(cu));
    ASSERT_TRUE(cd->makeCloudSubdirs("x", 0, 0));
    ASSERT_TRUE(CatchupClients(cd, cu));

    ASSERT_EQ(cd->basefolderhandle, cu->basefolderhandle);

    // Populate the local filesystem.
    const auto root = cu->fsBasePath / "su";

    // Make sure clashing directories are skipped.
    fs::create_directories(root / "d0");
    fs::create_directories(root / "d%30");

    // Make sure other directories are uploaded.
    fs::create_directories(root / "d1");

    // Make sure clashing files are skipped.
    createNameFile(root, "f0");
    createNameFile(root, "f%30");

    // Make sure other files are uploaded.
    createNameFile(root, "f1");
    createNameFile(root / "d1", "f0");

    // Start the syncs.
    handle backupId1 = cd->setupSync_mainthread("sd", "x", false, true);
    handle backupId2 = cu->setupSync_mainthread("su", "x", false, false);
    ASSERT_NE(backupId1, UNDEF);
    ASSERT_NE(backupId2, UNDEF);

    // Wait for the initial sync to complete.
    waitonsyncs(TIMEOUT, cu, cd);

    // Populate and confirm model.
    Model model;

    model.root->addkid(model.makeModelSubfolder("root"));
    model.findnode("root")->addkid(model.makeModelSubfolder("d1"));
    model.findnode("root")->addkid(model.makeModelSubfile("f1"));
    model.findnode("root/d1")->addkid(model.makeModelSubfile("f0"));

    model.ensureLocalDebrisTmpLock("root");

    ASSERT_TRUE(cd->confirmModel_mainthread(model.findnode("root"), backupId1));

    // Remove the clashing nodes.
    fs::remove_all(root / "d0");
    fs::remove_all(root / "f0");

    cu->triggerPeriodicScanEarly(backupId2);

    // Wait for the sync to complete.
    waitonsyncs(TIMEOUT, cd, cu);

    // Confirm that d0 and f0 have been downloaded.
    model.findnode("root")->addkid(model.makeModelSubfolder("d0"));
    model.findnode("root")->addkid(model.makeModelSubfile("f0", "f%30"));

    ASSERT_TRUE(cu->confirmModel_mainthread(model.findnode("root"), backupId2, true));
}
#endif

TEST_F(SyncTest, DISABLED_RemotesWithControlCharactersSynchronizeCorrectly)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    // Populate cloud.
    {
        // Upload client.
        StandardClient cu(TESTROOT, "cu");

        // Log callbacks.
        cu.logcb = true;

        // Log in client and clear remote contents.
        ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));

        auto* node = cu.drillchildnodebyname(cu.gettestbasenode(), "x");
        ASSERT_TRUE(!!node);

        // Create some directories containing control characters.
        vector<NewNode> nodes(2);

        // Only some platforms will escape BEL.
        cu.client.putnodes_prepareOneFolder(&nodes[0], "d\7", false);
        cu.client.putnodes_prepareOneFolder(&nodes[1], "d", false);

        ASSERT_TRUE(cu.putnodes(node->nodeHandle(), NoVersioning, std::move(nodes)));

        // Do the same but with some files.
        auto root = TESTROOT / "cu" / "x";
        fs::create_directories(root);

        // Placeholder name.
        ASSERT_TRUE(createNameFile(root, "f"));

        // Upload files.
        ASSERT_TRUE(cu.uploadFile(root / "f", "f\7", node));
        ASSERT_TRUE(cu.uploadFile(root / "f", node));
    }

    // Download client.
    StandardClient cd(TESTROOT, "cd");

    // Log callbacks.
    cd.logcb = true;

    // Log in client.
    ASSERT_TRUE(cd.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Add and start sync.
    handle backupId1 = cd.setupSync_mainthread("sd", "x", false, false);
    ASSERT_NE(backupId1, UNDEF);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Populate and confirm model.
    Model model;

    model.addfolder("x/d\7");
    model.addfolder("x/d");
    model.addfile("x/f\7", "f");
    model.addfile("x/f", "f");

    // Needed because we've downloaded files.
    model.ensureLocalDebrisTmpLock("x");

    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Remotely remove d\7.
    ASSERT_TRUE(cd.deleteremote("x/d\7"));
    ASSERT_TRUE(model.movetosynctrash("x/d\7", "x"));

    // Locally remove f\7.
    auto syncRoot = TESTROOT / "cd" / "sd";
#ifdef _WIN32
    ASSERT_TRUE(fs::remove(syncRoot / "f%07"));
#else /* _WIN32 */
    ASSERT_TRUE(fs::remove(syncRoot / "f\7"));
#endif /* ! _WIN32 */
    ASSERT_TRUE(!!model.removenode("x/f\7"));

    cd.triggerPeriodicScanEarly(backupId1);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Confirm models.
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Locally create some files with escapes in their names.
#ifdef _WIN32
    ASSERT_TRUE(fs::create_directories(syncRoot / "dd%07"));
    ASSERT_TRUE(createDataFile(syncRoot / "ff%07", "ff"));
#else
    ASSERT_TRUE(fs::create_directories(syncRoot / "dd\7"));
    ASSERT_TRUE(createDataFile(syncRoot / "ff\7", "ff"));
#endif /* ! _WIN32 */
    cd.triggerPeriodicScanEarly(backupId1);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Update and confirm models.
    model.addfolder("x/dd\7");
    model.addfile("x/ff\7", "ff");

    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));
}

// TODO: re-enable after sync rework is merged
TEST_F(SyncTest, DISABLED_RemotesWithEscapesSynchronizeCorrectly)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    // Populate cloud.
    {
        // Upload client.
        StandardClient cu(TESTROOT, "cu");

        // Log callbacks.
        cu.logcb = true;

        // Log in client and clear remote contents.
        ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));

        // Build test hierarchy.
        const auto root = TESTROOT / "cu" / "x";

        // Escapes will not be decoded as we're uploading directly.
        fs::create_directories(root / "d0");
        fs::create_directories(root / "d%30");

        ASSERT_TRUE(createNameFile(root, "f0"));
        ASSERT_TRUE(createNameFile(root, "f%30"));

        auto* node = cu.drillchildnodebyname(cu.gettestbasenode(), "x");
        ASSERT_TRUE(!!node);

        // Upload directories.
        ASSERT_TRUE(cu.uploadFolderTree(root / "d0", node));
        ASSERT_TRUE(cu.uploadFolderTree(root / "d%30", node));

        // Upload files.
        ASSERT_TRUE(cu.uploadFile(root / "f0", node));
        ASSERT_TRUE(cu.uploadFile(root / "f%30", node));
    }

    // Download client.
    StandardClient cd(TESTROOT, "cd");

    // Log callbacks.
    cd.logcb = true;

    // Log in client.
    ASSERT_TRUE(cd.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Add and start sync.
    handle backupId1 = cd.setupSync_mainthread("sd", "x", false, false);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Populate and confirm local fs.
    Model model;

    model.addfolder("x/d0");
    model.addfolder("x/d%30")->fsName("d%2530");
    model.addfile("x/f0", "f0");
    model.addfile("x/f%30", "f%30")->fsName("f%2530");

    // Needed as we've downloaded files.
    model.ensureLocalDebrisTmpLock("x");

    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Locally remove an escaped node.
    const auto syncRoot = cd.syncSet(backupId1).localpath;

    fs::remove_all(syncRoot / "d%2530");
    ASSERT_TRUE(!!model.removenode("x/d%30"));

    // Remotely remove an escaped file.
    ASSERT_TRUE(cd.deleteremote("x/f%30"));
    ASSERT_TRUE(model.movetosynctrash("x/f%30", "x"));

    // Wait for sync up to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Confirm models.
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Locally create some files with escapes in their names.
    {
        // Bogus escapes.
        ASSERT_TRUE(fs::create_directories(syncRoot / "dd%"));
        model.addfolder("x/dd%");

        ASSERT_TRUE(createNameFile(syncRoot, "ff%"));
        model.addfile("x/ff%", "ff%");

        // Sane character escapes.
        ASSERT_TRUE(fs::create_directories(syncRoot / "dd%31"));
        model.addfolder("x/dd1")->fsName("dd%31");

        ASSERT_TRUE(createNameFile(syncRoot, "ff%31"));
        model.addfile("x/ff1", "ff%31")->fsName("ff%31");

    }

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, &cd);

    // Confirm model.
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Let's try with escaped control sequences.
    ASSERT_TRUE(fs::create_directories(syncRoot / "dd%250a"));
    model.addfolder("x/dd%0a")->fsName("dd%250a");

    ASSERT_TRUE(createNameFile(syncRoot, "ff%250a"));
    model.addfile("x/ff%0a", "ff%250a")->fsName("ff%250a");

    // Wait for sync and confirm model.
    waitonsyncs(TIMEOUT, &cd);
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));

    // Remotely delete the nodes with control sequences.
    ASSERT_TRUE(cd.deleteremote("x/dd%0a"));
    model.movetosynctrash("x/dd%0a", "x");

    ASSERT_TRUE(cd.deleteremote("x/ff%0a"));
    model.movetosynctrash("x/ff%0a", "x");

    // Wait for sync and confirm model.
    waitonsyncs(TIMEOUT, &cd);
    ASSERT_TRUE(cd.confirmModel_mainthread(model.findnode("x"), backupId1));
}

TEST_F(SyncTest, BasicSyncExportImport)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT  = chrono::seconds(4);

    // Sync client.
    unique_ptr<StandardClient> cx(new StandardClient(TESTROOT, "cx"));

    // Log callbacks.
    cx->logcb = true;

    // Log in client.
    ASSERT_TRUE(cx->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 1, 3));

    // Create and start syncs.
    auto id0 = cx->setupSync_mainthread("s0", "s/s_0", false, false);
    ASSERT_NE(id0, UNDEF);

    auto id1 = cx->setupSync_mainthread("s1", "s/s_1", false, false);
    ASSERT_NE(id1, UNDEF);

    auto id2 = cx->setupSync_mainthread("s2", "s/s_2", false, false);
    ASSERT_NE(id2, UNDEF);

    // Get our hands on the sync's local root.
    auto root0 = cx->syncSet(id0).localpath;
    auto root1 = cx->syncSet(id1).localpath;
    auto root2 = cx->syncSet(id2).localpath;

    // Give the syncs something to synchronize.
    Model model0;
    Model model1;
    Model model2;

    model0.addfile("d0/f0");
    model0.addfile("f0");
    model0.generate(root0);

    model1.addfile("d0/f0");
    model1.addfile("d0/f1");
    model1.addfile("d1/f0");
    model1.addfile("d1/f1");
    model1.generate(root1);

    model2.addfile("f0");
    model2.addfile("f1");
    model2.generate(root2);

    cx->triggerPeriodicScanEarly(id0);
    cx->triggerPeriodicScanEarly(id1);
    cx->triggerPeriodicScanEarly(id2);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, cx.get());

    // Make sure everything was uploaded okay.
    ASSERT_TRUE(cx->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(cx->confirmModel_mainthread(model1.root.get(), id1));
    ASSERT_TRUE(cx->confirmModel_mainthread(model2.root.get(), id2));

    // Export the syncs.
    auto configs = cx->exportSyncConfigs();
    ASSERT_FALSE(configs.empty());

    // Log out client, don't keep caches.
    cx.reset();

    // Recreate client.
    cx.reset(new StandardClient(TESTROOT, "cx"));

    // Log client back in.
    ASSERT_TRUE(cx->login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

    // Import the syncs.
    ASSERT_TRUE(cx->importSyncConfigs(std::move(configs)));

    // Determine the imported sync's backup IDs.
    id0 = cx->backupIdForSyncPath(root0);
    ASSERT_NE(id0, UNDEF);

    id1 = cx->backupIdForSyncPath(root1);
    ASSERT_NE(id1, UNDEF);

    id2 = cx->backupIdForSyncPath(root2);
    ASSERT_NE(id2, UNDEF);

    // Make sure nothing's changed since we exported the syncs.
    ASSERT_TRUE(cx->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(cx->confirmModel_mainthread(model1.root.get(), id1));
    ASSERT_TRUE(cx->confirmModel_mainthread(model2.root.get(), id2));

    // Make some changes.
    model0.addfile("d0/f1");
    model0.generate(root0);

    model1.addfile("f0");
    model1.generate(root1);

    model2.addfile("d0/d0f0");
    model2.generate(root2);

    // Imported syncs should be disabled.
    // So, we're waiting for the syncs to do precisely nothing.
    waitonsyncs(TIMEOUT, cx.get());

    // Confirm should fail.
    ASSERT_FALSE(cx->confirmModel_mainthread(model0.root.get(), id0, false, StandardClient::Confirm::CONFIRM_ALL, true));
    ASSERT_FALSE(cx->confirmModel_mainthread(model1.root.get(), id1, false, StandardClient::Confirm::CONFIRM_ALL, true));
    ASSERT_FALSE(cx->confirmModel_mainthread(model2.root.get(), id2, false, StandardClient::Confirm::CONFIRM_ALL, true));

    // Enable the imported syncs.
    ASSERT_TRUE(cx->enableSyncByBackupId(id0, "sync0 "));
    ASSERT_TRUE(cx->enableSyncByBackupId(id1, "sync1 "));
    ASSERT_TRUE(cx->enableSyncByBackupId(id2, "sync2 "));

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, cx.get());

    // Changes should now be in the cloud.
    ASSERT_TRUE(cx->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(cx->confirmModel_mainthread(model1.root.get(), id1));
    ASSERT_TRUE(cx->confirmModel_mainthread(model2.root.get(), id2));
}

TEST_F(SyncTest, RenameReplaceFileBetweenSyncs)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClientInUse c0 = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    c0->logcb = true;

    // Log in client.
    ASSERT_TRUE(c0->resetBaseFolderMulticlient());
    ASSERT_TRUE(c0->makeCloudSubdirs("s0", 0, 0));
    ASSERT_TRUE(c0->makeCloudSubdirs("s1", 0, 0));
    ASSERT_TRUE(CatchupClients(c0));
    //ASSERT_TRUE(c0->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s0", 0, 0));

    // Set up syncs.
    const auto id0 = c0->setupSync_mainthread("s0", "s0", false, false);
    ASSERT_NE(id0, UNDEF);

    const auto id1 = c0->setupSync_mainthread("s1", "s1", false, false);
    ASSERT_NE(id1, UNDEF);

    // Convenience.
    const auto SYNCROOT0 = c0->fsBasePath / "s0";
    const auto SYNCROOT1 = c0->fsBasePath / "s1";

    // Set up models.
    Model model0;
    Model model1;

    model0.addfile("f0", "x");
    model0.generate(SYNCROOT0);

    c0->triggerPeriodicScanEarly(id0);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c0);

    // Confirm models.
    ASSERT_TRUE(c0->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(c0->confirmModel_mainthread(model1.root.get(), id1));

    // Move s0/f0 to s1/f0.
    model1 = model0;

    fs::rename(SYNCROOT0 / "f0", SYNCROOT1 / "f0");

    // Replace s0/f0.
    model0.removenode("f0");
    model0.addfile("f0", "y");

    ASSERT_TRUE(createDataFile(SYNCROOT0 / "f0", "y"));

    c0->triggerPeriodicScanEarly(id0);
    c0->triggerPeriodicScanEarly(id1);
    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c0);

    // Confirm models.
    ASSERT_TRUE(c0->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(c0->confirmModel_mainthread(model1.root.get(), id1));

    // Disable s0.
    ASSERT_TRUE(c0->disableSync(id0, NO_SYNC_ERROR, false));

    // Make sure s0 is disabled.
    ASSERT_TRUE(createDataFile(SYNCROOT0 / "f1", "z"));

    c0->triggerPeriodicScanEarly(id0);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c0);

    // Confirm models.
    ASSERT_TRUE(c0->confirmModel_mainthread(
                  model0.root.get(),
                  id0,
                  false,
                  StandardClient::CONFIRM_REMOTE));

    // Move s1/f0 to s0/f2.
    model1.removenode("f0");

    fs::rename(SYNCROOT1 / "f0", SYNCROOT0 / "f2");

    // Replace s1/f0.
    model1.addfile("f0", "q");

    ASSERT_TRUE(createDataFile(SYNCROOT1 / "f0", "q"));

    c0->triggerPeriodicScanEarly(id0);
    c0->triggerPeriodicScanEarly(id1);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c0);

    // Confirm models.
    ASSERT_TRUE(c0->confirmModel_mainthread(
                  model0.root.get(),
                  id0,
                  false,
                  StandardClient::CONFIRM_REMOTE));

    ASSERT_TRUE(c0->confirmModel_mainthread(model1.root.get(), id1));
}

TEST_F(SyncTest, RenameReplaceFileWithinSync)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT = std::chrono::seconds(8);

    StandardClientInUse c = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    c->logcb = true;

    // Log in client.
    ASSERT_TRUE(c->resetBaseFolderMulticlient());
    ASSERT_TRUE(c->makeCloudSubdirs("s", 0, 0));
    ASSERT_TRUE(CatchupClients(c));

    // Populate model.
    Model m;

    // Will be rename-replaced to /ft.
    m.addfile("fs");

    // Will be rename-replaced down to /dd/dt/ft.
    m.addfile("dd/fs");
    m.addfolder("dd/dt");

    // Will be rename-replaced up to /du/ft.
    m.addfile("du/ds/fs");

    // Populate local filesystem.
    m.generate(c->fsBasePath / "s");

    // Add and start sync.
    auto id = c->setupSync_mainthread("s", "s", false, false);
    ASSERT_NE(id, UNDEF);

    // Wait for the initial sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Make sure the initial sync was successful.
    ASSERT_TRUE(c->confirmModel_mainthread(m.root.get(), id));

    // Rename/replace across siblings.
    //
    // Models this case:
    //   echo fs > fs
    //   <synchronize>
    //   mv fs ft && echo x > fs
    //   <synchronize>
    {
        // Locally move fs to ft.
        fs::rename(c->fsBasePath / "s" / "fs",
                   c->fsBasePath / "s" / "ft");

        m.findnode("fs")->name = "ft";

        // Replace fs.
        ASSERT_TRUE(createDataFile(c->fsBasePath / "s" / "fs", "x"));

        m.addfile("fs", "x");

        // For periodic scanning.
        //c->triggerFullScan(id);

        // Wait for the change to be synchronized.
        waitonsyncs(TIMEOUT, c);

        // Was the change correctly synchronized?
        ASSERT_TRUE(c->confirmModel_mainthread(m.root.get(), id));
    }

    // Rename/replace down the hierarchy.
    //
    // Models this case:
    //   mkdir -p dd/dt
    //   echo dd/fs > dd/fs
    //   <synchronize>
    //   mv dd/fs dd/dt/ft && echo x > dd/fs
    //   <synchronize>
    {
        // Move /dd/fs to /dd/dt/ft.
        fs::rename(c->fsBasePath / "s" / "dd" / "fs",
                   c->fsBasePath / "s" / "dd" / "dt" / "ft");

        m.movenode("dd/fs", "dd/dt");
        m.findnode("dd/dt/fs")->name = "ft";

        // Replace /dd/fs.
        ASSERT_TRUE(createDataFile(c->fsBasePath / "s" / "dd" / "fs", "x"));

        m.addfile("dd/fs", "x");

        // For periodic scanning.
        //c->triggerFullScan(id);

        // Wait for the change to be synchronized.
        waitonsyncs(TIMEOUT, c);

        // Was the change correctly synchronized?
        ASSERT_TRUE(c->confirmModel_mainthread(m.root.get(), id));
    }

    // Rename/replace up the hierarchy.
    //
    // Models this case:
    //   mkdir -p du/ds
    //   echo du/ds/fs > du/ds/fs
    //   <synchronize>
    //   mv du/ds/fs du/fs && echo x > du/ds/fs
    //   <synchronize>
    {
        // Move du/ds/fs to du/ft.
        fs::rename(c->fsBasePath / "s" / "du" / "ds" / "fs",
                   c->fsBasePath / "s" / "du" / "ft");

        m.movenode("du/ds/fs", "du");
        m.findnode("du/fs")->name = "ft";

        // Replace du/ds/fs.
        ASSERT_TRUE(createDataFile(c->fsBasePath / "s" / "du" / "ds" / "fs", "x"));

        m.addfile("du/ds/fs", "x");

        // For periodic scanning.
        //c->triggerFullScan(id);

        // Wait for the change to be synchronized.
        waitonsyncs(TIMEOUT, c);

        // Was the change correctly synchronized?
        ASSERT_TRUE(c->confirmModel_mainthread(m.root.get(), id));
    }
}

// TODO: re-enable after sync rework is merged
TEST_F(SyncTest, DISABLED_RenameReplaceFolderBetweenSyncs)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClientInUse c0 = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    c0->logcb = true;

    // Log in client.
    ASSERT_TRUE(c0->resetBaseFolderMulticlient());
    ASSERT_TRUE(c0->makeCloudSubdirs("s0", 0, 0));
    ASSERT_TRUE(c0->makeCloudSubdirs("s1", 0, 0));
    ASSERT_TRUE(CatchupClients(c0));

    // Set up syncs.
    const auto id0 = c0->setupSync_mainthread("s0", "s0", false, false);
    ASSERT_NE(id0, UNDEF);

    const auto id1 = c0->setupSync_mainthread("s1", "s1", false, false);
    ASSERT_NE(id1, UNDEF);

    // Convenience.
    const auto SYNCROOT0 = c0->fsBasePath / "s0";
    const auto SYNCROOT1 = c0->fsBasePath / "s1";

    // Set up models.
    Model model0;
    Model model1;

    model0.addfile("d0/f0");
    model0.generate(SYNCROOT0);

    c0->triggerPeriodicScanEarly(id0);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c0);

    // Confirm models.
    ASSERT_TRUE(c0->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(c0->confirmModel_mainthread(model1.root.get(), id1));

    // Move s0/d0 to s1/d0. (and replace)
    model1 = model0;

    fs::rename(SYNCROOT0 / "d0", SYNCROOT1 / "d0");

    // Replace s0/d0.
    model0.removenode("d0/f0");

    fs::create_directories(SYNCROOT0 / "d0");

    c0->triggerPeriodicScanEarly(id0);
    c0->triggerPeriodicScanEarly(id1);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c0);

    // Confirm models.
    ASSERT_TRUE(c0->confirmModel_mainthread(model0.root.get(), id0));
    ASSERT_TRUE(c0->confirmModel_mainthread(model1.root.get(), id1));

    // Disable s0.
    ASSERT_TRUE(c0->disableSync(id0, NO_SYNC_ERROR, false));

    // Make sure s0 is disabled.
    fs::create_directories(SYNCROOT0 / "d1");

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c0);

    // Confirm models.
    ASSERT_TRUE(c0->confirmModel_mainthread(
                  model0.root.get(),
                  id0,
                  false,
                  StandardClient::CONFIRM_REMOTE));

    // Move s1/d0 to s0/d2.
    model1.removenode("d0/f0");

    fs::rename(SYNCROOT1 / "d0", SYNCROOT0 / "d2");

    // Replace s1/d0.
    fs::create_directories(SYNCROOT1 / "d0");

    c0->triggerPeriodicScanEarly(id0);
    c0->triggerPeriodicScanEarly(id1);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c0);

    // Confirm models.
    ASSERT_TRUE(c0->confirmModel_mainthread(
                  model0.root.get(),
                  id0,
                  false,
                  StandardClient::CONFIRM_REMOTE));

    ASSERT_TRUE(c0->confirmModel_mainthread(model1.root.get(), id1));
}

TEST_F(SyncTest, RenameReplaceFolderWithinSync)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClientInUse c0 = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    c0->logcb = true;

    // Log in client.
    ASSERT_TRUE(c0->resetBaseFolderMulticlient());
    ASSERT_TRUE(c0->makeCloudSubdirs("s0", 0, 0));
    ASSERT_TRUE(CatchupClients(c0));

    // Set up sync.
    const auto id = c0->setupSync_mainthread("s0", "s0", false, false);
    ASSERT_NE(id, UNDEF);

    // Populate local FS.
    const auto SYNCROOT = c0->fsBasePath / "s0";

    Model model;

    model.addfile("d1/f0");
    model.generate(SYNCROOT);

    c0->triggerPeriodicScanEarly(id);

    // Wait for synchronization to complete.
    waitonsyncs(chrono::seconds(15), c0);

    // Confirm model.
    ASSERT_TRUE(c0->confirmModel_mainthread(model.root.get(), id));

    // Rename /d1 to /d2.
    // This tests the case where the target is processed after the source.
    model.addfolder("d2");
    model.movenode("d1/f0", "d2");

    fs::rename(SYNCROOT / "d1", SYNCROOT / "d2");

    // Replace /d1.
    fs::create_directories(SYNCROOT / "d1");

    c0->triggerPeriodicScanEarly(id);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c0);

    // Confirm model.
    ASSERT_TRUE(c0->confirmModel_mainthread(model.root.get(), id));

    // Rename /d2 to /d0.
    // This tests the case where the target is processed before the source.
    model.addfolder("d0");
    model.movenode("d2/f0", "d0");

    fs::rename(SYNCROOT / "d2", SYNCROOT / "d0");

    // Replace /d2.
    fs::create_directories(SYNCROOT / "d2");

    c0->triggerPeriodicScanEarly(id);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c0);

    // Confirm model.
    ASSERT_TRUE(c0->confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, DownloadedDirectoriesHaveFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClientInUse c = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    c->logcb = true;

    // Log in client.
    ASSERT_TRUE(c->resetBaseFolderMulticlient());
    ASSERT_TRUE(c->makeCloudSubdirs("s", 0, 0));
    ASSERT_TRUE(CatchupClients(c));

    // Create /d in the cloud.
    {
        vector<NewNode> nodes(1);

        // Initialize new node.
        c->client.putnodes_prepareOneFolder(&nodes[0], "d", false);

        // Get our hands on the sync root.
        auto* root = c->drillchildnodebyname(c->gettestbasenode(), "s");
        ASSERT_TRUE(root);

        // Create new node in the cloud.
        ASSERT_TRUE(c->putnodes(root->nodeHandle(), NoVersioning, std::move(nodes)));
    }

    // Add and start sync.
    const auto id = c->setupSync_mainthread("s", "s", false, false);
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c->syncSet(id).localpath;

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c);

    // Confirm /d has made it to disk.
    Model model;

    model.addfolder("d");

    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Trigger a filesystem notification.
    model.addfile("d/f", "x");

    ASSERT_TRUE(createDataFile(SYNCROOT / "d" / "f", "x"));

    c->triggerPeriodicScanEarly(id);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c);

    // Confirm /d/f made it to the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, FilesystemWatchesPresentAfterResume)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    auto c = ::mega::make_unique<StandardClient>(TESTROOT, "c");

    // Log callbacks.
    c->logcb = true;

    // Log in client.
    ASSERT_TRUE(c->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Add and start sync.
    const auto id = c->setupSync_mainthread("s", "s", false, false);
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c->syncSet(id).localpath;

    // Build model and populate filesystem.
    Model model;

    model.addfolder("d0/d0d0");
    model.generate(SYNCROOT);

    c->triggerPeriodicScanEarly(id);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, c.get());

    // Make sure directories made it to the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Logout / Resume.
    {
        string session;

        // Save session.
        c->client.dumpsession(session);

        // Logout (taking care to preserve the caches.)
        c->localLogout();

        // Recreate client.
        c.reset(new StandardClient(TESTROOT, "c"));

        // Hook onAutoResumeResult callback.
        promise<void> notify;

        c->onAutoResumeResult = [&](const SyncConfig&) {
            notify.set_value();
        };

        // Resume session.
        ASSERT_TRUE(c->login_fetchnodes(session));

        // Wait for the sync to be resumed.
        ASSERT_TRUE(debugTolerantWaitOnFuture(notify.get_future(), 45));

        // Wait for sync to complete.
        waitonsyncs(TIMEOUT, c.get());

        // Make sure everything's as we left it.
        ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));
    }

    c->received_node_actionpackets = false;

    // Trigger some filesystem notifications.
    {
        model.addfile("f", "f");
        ASSERT_TRUE(createDataFile(SYNCROOT / "f", "f"));

        model.addfile("d0/d0f", "d0f");
        ASSERT_TRUE(createDataFile(SYNCROOT / "d0" / "d0f", "d0f"));

        model.addfile("d0/d0d0/d0d0f", "d0d0f");
        ASSERT_TRUE(createDataFile(SYNCROOT / "d0" / "d0d0" / "d0d0f", "d0d0f"));
    }

    ASSERT_TRUE(c->waitForNodesUpdated(30)) << " no actionpacket received";

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c.get());

    // Did the new files make it to the cloud?
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, MoveTargetHasFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    auto c = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    c->logcb = true;

    // Log in client.
    ASSERT_TRUE(c->resetBaseFolderMulticlient());
    ASSERT_TRUE(c->makeCloudSubdirs("s", 0, 0));
    ASSERT_TRUE(CatchupClients(c));

    // Set up sync.
    const auto id = c->setupSync_mainthread("s", "s", false, false);
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c->syncSet(id).localpath;

    // Build model and populate filesystem.
    Model model;

    model.addfolder("d0/dq");
    model.addfolder("d1");
    model.addfolder("d2/dx");
    model.generate(SYNCROOT);

    c->triggerPeriodicScanEarly(id);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Wait for everything to reach the cloud.
    {
        auto *root = c->gettestbasenode();
        ASSERT_NE(root, nullptr);

        root = c->drillchildnodebyname(root, "s");
        ASSERT_NE(root, nullptr);

        auto predicate = SyncRemoteMatch(*root, model.root.get());
        ASSERT_TRUE(c->waitFor(std::move(predicate), DEFAULTWAIT));
    }

    // Confirm directories have hit the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Local move.
    {
        // d0/dq -> d1/dq (ascending.)
        model.movenode("d0/dq", "d1");

        fs::rename(SYNCROOT / "d0" / "dq",
                   SYNCROOT / "d1" / "dq");

        // d2/dx -> d1/dx (descending.)
        model.movenode("d2/dx", "d1");

        fs::rename(SYNCROOT / "d2" / "dx",
                   SYNCROOT / "d1" / "dx");
    }

    c->triggerPeriodicScanEarly(id);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Make sure movement has propagated to the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Trigger some filesystem notifications.
    model.addfile("d1/dq/fq", "q");
    model.addfile("d1/dx/fx", "x");

    ASSERT_TRUE(createDataFile(SYNCROOT / "d1" / "dq" / "fq", "q"));
    ASSERT_TRUE(createDataFile(SYNCROOT / "d1" / "dx" / "fx", "x"));

    c->triggerPeriodicScanEarly(id);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Have the files made it up to the cloud?
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // So we can detect whether we've received packets for the below.
    c->received_node_actionpackets = false;

    // Remotely move.
    {
        StandardClient cr(TESTROOT, "cr");

        // Log in client.
        ASSERT_TRUE(cr.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

        // d1/dq -> d2/dq (ascending.)
        model.movenode("d1/dq", "d2");

        ASSERT_TRUE(cr.movenode("s/d1/dq", "s/d2"));

        // d1/dx -> d0/dx (descending.)
        model.movenode("d1/dx", "d0");

        ASSERT_TRUE(cr.movenode("s/d1/dx", "s/d0"));
    }

    // Wait for the client to receive action packets for the above change.
    ASSERT_TRUE(c->waitForNodesUpdated(30));

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Make sure movements occured on disk.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Trigger some filesystem notifications.
    model.removenode("d2/dq/fq");
    model.removenode("d0/dx/fx");

    fs::remove(SYNCROOT / "d2" / "dq" / "fq");
    fs::remove(SYNCROOT / "d0" / "dx" / "fx");

    c->triggerPeriodicScanEarly(id);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Make sure removes propagated to the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));
}

// TODO: re-enable after sync rework is merged
TEST_F(SyncTest, DISABLED_DeleteReplaceReplacementHasFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClient c(TESTROOT, "c");

    // Log callbacks.
    c.logcb = true;

    // Log in client.
    ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Add and start sync.
    const auto id = c.setupSync_mainthread("s", "s", false, false);
    ASSERT_NE(id, UNDEF);

    const auto ROOT = c.syncSet(id).localpath;

    // Populate filesystem.
    Model model;

    model.addfolder("dx/f");
    model.generate(ROOT);

    c.triggerPeriodicScanEarly(id);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Make sure the directory's been uploaded to the cloud.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Remove/replace the directory.
    fs::remove_all(ROOT / "dx");
    fs::create_directory(ROOT / "dx");

    c.triggerPeriodicScanEarly(id);

    // Wait for all notifications to be processed.
    waitonsyncs(TIMEOUT, &c);

    // Make sure the new directory is in the cloud.
    model.removenode("dx/f");

    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

    // Add a file in the new directory so we trigger a notification.
    out() << "creating file dx/g";
    model.addfile("dx/g", "g");

    ASSERT_TRUE(createDataFile(ROOT / "dx" / "g", "g"));

    c.triggerPeriodicScanEarly(id);

    // Wait for notifications to be processed.
    waitonsyncs(TIMEOUT, &c);

    // Check if g has been uploaded.
    // If it hasn't, we probably didn't receive a notification from the filesystem.
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, RenameReplaceSourceAndTargetHaveFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(8);

    auto c = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    c->logcb = true;

    // Log in client.
    ASSERT_TRUE(c->resetBaseFolderMulticlient());
    ASSERT_TRUE(c->makeCloudSubdirs("s", 0, 0));
    ASSERT_TRUE(CatchupClients(c));

    // Add and start sync.
    const auto id = c->setupSync_mainthread("s", "s", false, false);
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c->syncSet(id).localpath;

    // Build model and populate filesystem.
    Model model;

    model.addfolder("dq");
    model.addfolder("dz");
    model.generate(SYNCROOT);

    c->triggerPeriodicScanEarly(id);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Make sure directories have made it to the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Rename /dq -> /dr (ascending), replace /dq.
    model.addfolder("dr");

    fs::rename(SYNCROOT / "dq", SYNCROOT / "dr");
    fs::create_directories(SYNCROOT / "dq");

    // Rename /dz -> /dy (descending), replace /dz.
    model.addfolder("dy");

    fs::rename(SYNCROOT / "dz", SYNCROOT / "dy");
    fs::create_directories(SYNCROOT / "dz");

    c->triggerPeriodicScanEarly(id);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Make sure moves made it to the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Make sure rename targets still receive notifications.
    model.addfile("dr/fr", "r");
    model.addfile("dy/fy", "y");

    ASSERT_TRUE(createDataFile(SYNCROOT / "dr" / "fr", "r"));
    ASSERT_TRUE(createDataFile(SYNCROOT / "dy" / "fy", "y"));

    c->triggerPeriodicScanEarly(id);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Did the files make it to the cloud?
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Make sure (now replaced) rename sources still receive notifications.
    model.addfile("dq/fq", "q");
    model.addfile("dz/fz", "z");

    LOG_debug << " --- Creating files fq and fz now ----";

    ASSERT_TRUE(createDataFile(SYNCROOT / "dq" / "fq", "q"));
    ASSERT_TRUE(createDataFile(SYNCROOT / "dz" / "fz", "z"));

    c->triggerPeriodicScanEarly(id);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Did the files make it to the cloud?
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, RenameTargetHasFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    StandardClientInUse c = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    c->logcb = true;

    // Log in client.
    ASSERT_TRUE(c->resetBaseFolderMulticlient());
    ASSERT_TRUE(c->makeCloudSubdirs("s", 0, 0));
    ASSERT_TRUE(CatchupClients(c));

    // Add and start sync.
    const auto id = c->setupSync_mainthread("s", "s", false, false);
    ASSERT_NE(id, UNDEF);

    const auto SYNCROOT = c->syncSet(id).localpath;

    // Build model and populate filesystem.
    Model model;

    model.addfolder("dq");
    model.addfolder("dz");
    model.generate(SYNCROOT);

    c->triggerPeriodicScanEarly(id);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c);

    // Confirm model.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Locally rename.
    {
        // - dq -> dr (ascending)
        model.removenode("dq");
        model.addfolder("dr");

        fs::rename(SYNCROOT / "dq", SYNCROOT / "dr");

        // - dz -> dy (descending)
        model.removenode("dz");
        model.addfolder("dy");

        fs::rename(SYNCROOT / "dz", SYNCROOT / "dy");
    }

    c->triggerPeriodicScanEarly(id);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c);

    // Make sure rename has hit the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Make sure rename targets receive notifications.
    model.addfile("dr/f", "x");
    model.addfile("dy/f", "y");

    ASSERT_TRUE(createDataFile(SYNCROOT / "dr" / "f", "x"));
    ASSERT_TRUE(createDataFile(SYNCROOT / "dy" / "f", "y"));

    c->triggerPeriodicScanEarly(id);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c);

    // Check file has made it to the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    // Remotely rename.
    {
        StandardClientInUse cr = g_clientManager->getCleanStandardClient(0, TESTROOT);

        auto* root = cr->gettestbasenode();
        ASSERT_TRUE(root);

        // dr -> ds (ascending.)
        model.removenode("dr");
        model.addfile("ds/f", "x");

        auto* dr = cr->drillchildnodebyname(root, "s/dr");
        ASSERT_TRUE(dr);

        ASSERT_TRUE(cr->setattr(dr, attr_map('n', "ds")));

        // dy -> dx (descending.)
        model.removenode("dy");
        model.addfile("dx/f", "y");

        auto* dy = cr->drillchildnodebyname(root, "s/dy");
        ASSERT_TRUE(dy);

        ASSERT_TRUE(cr->setattr(dy, attr_map('n', "dx")));
    }

    // it can take a while for APs to arrive (or to be sent)
    WaitMillisec(4000);

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c);

    // Confirm move has occured locally.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));

    c->received_node_actionpackets = false;

    // Check that /ds and /dx receive notifications.
    model.removenode("ds/f");
    model.removenode("dx/f");
    fs::remove(SYNCROOT / "ds" / "f");
    fs::remove(SYNCROOT / "dx" / "f");

    c->triggerPeriodicScanEarly(id);

    ASSERT_TRUE(c->waitForNodesUpdated(30)) << " no actionpacket received in c";

    // Wait for synchronization to complete.
    waitonsyncs(TIMEOUT, c);

    // Confirm remove has hit the cloud.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, DISABLED_ReplaceParentWithEmptyChild)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(4);

    handle id;
    Model model;
    string session;

    // Populate initial filesystem.
    {
        StandardClient c(TESTROOT, "c");

        // Log callbacks.
        c.logcb = true;

        // Log in client.
        ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

        // Add and start sync.
        id = c.setupSync_mainthread("s", "s", false, false);
        ASSERT_NE(id, UNDEF);

        // Build model and populate filesystem.
        model.addfolder("0/1/2/3");
        model.addfolder("4/5/6/7");
        model.generate(c.syncSet(id).localpath);

        c.triggerPeriodicScanEarly(id);

        // Wait for the sync to complete.
        waitonsyncs(TIMEOUT, &c);

        // Make sure the tree made it to the cloud.
        ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

        // Save the session.
        c.client.dumpsession(session);

        // Locally log out the client.
        c.localLogout();
    }

    StandardClient c(TESTROOT, "c");

    // Log callbacks.
    c.logcb = true;

    // Locally replace 0 with 0/1/2/3.
    {
        model.removenode("0");
        model.addfolder("0");

        fs::rename(c.fsBasePath / "s" / "0" / "1" / "2" / "3",
                   c.fsBasePath / "s" / "3");

        fs::remove_all(c.fsBasePath / "s" / "0");

        fs::rename(c.fsBasePath / "s" / "3",
                   c.fsBasePath / "s" / "0");
    }

    // Remotely replace 4 with 4/5/6/7.
    {
        model.movetosynctrash("4", "");
        model.addfolder("4");

        // New client so we can alter the cloud without resuming syncs.
        StandardClient cr(TESTROOT, "cr");

        // Log callbacks.
        cr.logcb = true;

        // Log in client.
        ASSERT_TRUE(cr.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

        // Replace 4 with 4/5/6/7.
        ASSERT_TRUE(cr.movenode("s/4/5/6/7", "s"));
        ASSERT_TRUE(cr.deleteremote("s/4"));
        ASSERT_TRUE(cr.rename("s/7", "4"));
    }

    // Hook resume callbacks.
    promise<void> notify;

    c.mOnSyncStateConfig = [&notify](const SyncConfig& config) {
        if (config.mRunState == SyncRunState::Run)
            notify.set_value();
    };

    // Resume client.
    ASSERT_TRUE(c.login_fetchnodes(session));

    // Wait for sync to resume.
    ASSERT_TRUE(debugTolerantWaitOnFuture(notify.get_future(), 45));

    // Wait for the sync to complete.
    waitonsyncs(TIMEOUT, &c);

    // Is the cloud as we expect?
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, RootHasFilesystemWatch)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClientInUse c = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    c->logcb = true;

    // Log in client.
    ASSERT_TRUE(c->resetBaseFolderMulticlient());
    ASSERT_TRUE(c->makeCloudSubdirs("s", 0, 0));
    ASSERT_TRUE(CatchupClients(c));

    // Set up sync
    const auto id = c->setupSync_mainthread("s", "s", false, false);
    ASSERT_NE(id, UNDEF);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Trigger some filesystem notifications.
    Model model;

    model.addfolder("d0");
    model.addfile("f0");
    model.generate(c->syncSet(id).localpath);

    c->triggerPeriodicScanEarly(id);

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Confirm models.
    ASSERT_TRUE(c->confirmModel_mainthread(model.root.get(), id));
}

struct TwoWaySyncSymmetryCase
{
    enum SyncType { type_twoWay, type_backupSync, type_numTypes };

    enum Action { action_rename, action_moveWithinSync, action_moveOutOfSync, action_moveIntoSync, action_delete, action_numactions };

    enum MatchState { match_exact,      // the sync destination has the exact same file/folder at the same relative path
                      match_older,      // the sync destination has an older file/folder at the same relative path
                      match_newer,      // the sync destination has a newer file/folder at the same relative path
                      match_absent };   // the sync destination has no node at the same relative path

    SyncType syncType = type_twoWay;
    Action action = action_rename;
    bool selfChange = false; // changed by our own client or another
    bool up = false;  // or down - sync direction
    bool file = false;  // or folder.  Which one this test changes
    bool isExternal = false;
    bool pauseDuringAction = false;
    Model localModel;
    Model remoteModel;
    handle backupId = UNDEF;

    bool printTreesBeforeAndAfter = false;

    struct State
    {
        StandardClient& steadyClient;
        StandardClient& resumeClient;
        StandardClient& nonsyncClient;
        fs::path localBaseFolderSteady;
        fs::path localBaseFolderResume;
        std::string remoteBaseFolder = "twoway";   // leave out initial / so we can drill down from root node
        std::string first_test_name;
        fs::path first_test_initiallocalfolders;

        State(StandardClient& ssc, StandardClient& rsc, StandardClient& sc2) : steadyClient(ssc), resumeClient(rsc), nonsyncClient(sc2) {}
    };

    State& state;
    TwoWaySyncSymmetryCase(State& wholestate) : state(wholestate) {}

    std::string typeName()
    {
        switch (syncType)
        {
        case type_twoWay:
            return "twoWay_";
        case type_backupSync:
            return isExternal ? "external_backup_"
                              : "internal_backup_";
        default:
            assert(false);
            return "";
        }
    }

    std::string actionName()
    {
        switch (action)
        {
        case action_rename: return "rename";
        case action_moveWithinSync: return "move";
        case action_moveOutOfSync: return "moveOut";
        case action_moveIntoSync: return "moveIn";
        case action_delete: return "delete";
        default: assert(false); return "";
        }
    }

    std::string matchName(MatchState m)
    {
        switch (m)
        {
            case match_exact: return "exact";
            case match_older: return "older";
            case match_newer: return "newer";
            case match_absent: return "absent";
        }
        return "bad enum";
    }

    std::string name()
    {
        return  typeName() + actionName() +
                (up?"_up" : "_down") +
                (selfChange?"_self":"_other") +
                (file?"_file":"_folder") +
                (pauseDuringAction?"_resumed":"_steady");
    }

    fs::path localTestBasePathSteady;
    fs::path localTestBasePathResume;
    std::string remoteTestBasePath;

    Model& sourceModel() { return up ? localModel : remoteModel; }
    Model& destinationModel() { return up ? remoteModel : localModel; }

    StandardClient& client1() { return pauseDuringAction ? state.resumeClient : state.steadyClient; }
    StandardClient& changeClient() { return selfChange ? client1() : state.nonsyncClient; }

    fs::path localTestBasePath() { return pauseDuringAction ? localTestBasePathResume : localTestBasePathSteady; }

    bool CopyLocalTree(const fs::path& destination, const fs::path& source) try
    {
        using PathPair = std::pair<fs::path, fs::path>;

        // Assume we've already copied if the destination exists.
        if (fs::exists(destination)) return true;

        std::list<PathPair> pending;

        pending.emplace_back(destination, source);

        for (; !pending.empty(); pending.pop_front())
        {
            const auto& dst = pending.front().first;
            const auto& src = pending.front().second;

            // Assume target directory doesn't exist.
            fs::create_directories(dst);

            // Iterate over source directory's children.
            auto i = fs::directory_iterator(src);
            auto j = fs::directory_iterator();

            for ( ; i != j; ++i)
            {
                auto from = i->path();
                auto to = dst / from.filename();

                // If it's a file, copy it and preserve its modification time.
                if (fs::is_regular_file(from))
                {
                    // Copy the file.
                    fs::copy_file(from, to);

                    // Preserve modification time.
                    fs::last_write_time(to, fs::last_write_time(from));

                    // Process next child.
                    continue;
                }

                // If it's not a file, it must be a directory.
                assert(fs::is_directory(from));

                // So, create it!
                fs::create_directories(to);

                // And copy its content.
                pending.emplace_back(to, from);
            }
        }

        return true;
    }
    catch (...)
    {
        return false;
    }

    void makeMtimeFile(std::string name, int mtime_delta, Model& m1, Model& m2)
    {
        createNameFile(state.first_test_initiallocalfolders, name);
        auto initial_mtime = fs::last_write_time(state.first_test_initiallocalfolders / name);
        fs::last_write_time(localTestBasePath() / name, initial_mtime + std::chrono::seconds(mtime_delta));
        fs::rename(state.first_test_initiallocalfolders / name, state.first_test_initiallocalfolders / "f" / name); // move it after setting the time to be 100% sure the sync sees it with the adjusted mtime only
        m1.findnode("f")->addkid(m1.makeModelSubfile(name));
        m2.findnode("f")->addkid(m2.makeModelSubfile(name));
    }

    PromiseBoolSP cloudCopySetupPromise = makeSharedPromise<bool>();

    // prepares a local folder for testing, which will be two-way synced before the test
    void SetupForSync()
    {
        // Prepare Cloud
        {
            remoteTestBasePath = state.remoteBaseFolder + "/" + name();

            auto& client = changeClient();

            auto* root = client.gettestbasenode();
            ASSERT_NE(root, nullptr);

            root = client.drillchildnodebyname(root, state.remoteBaseFolder);
            ASSERT_NE(root, nullptr);

            auto* from = client.drillchildnodebyname(root, "initial");
            ASSERT_NE(from, nullptr);

            ASSERT_TRUE(client.copy(from, root, name()));
        }

        // Prepare Local Filesystem
        {
            localTestBasePathSteady = state.localBaseFolderSteady / name();
            localTestBasePathResume = state.localBaseFolderResume / name();

            auto from = state.nonsyncClient.fsBasePath / "twoway" / "initial";
            ASSERT_TRUE(CopyLocalTree(localTestBasePathResume, from));
            ASSERT_TRUE(CopyLocalTree(localTestBasePathSteady, from));

            ASSERT_TRUE(CopyLocalTree(state.localBaseFolderResume / "initial", from));
            ASSERT_TRUE(CopyLocalTree(state.localBaseFolderSteady / "initial", from));
        }

        // Prepare models.
        {
            localModel.root->addkid(localModel.buildModelSubdirs("f", 2, 2, 2));
            localModel.root->addkid(localModel.buildModelSubdirs("outside", 2, 1, 1));
            localModel.addfile("f/file_older_1", "file_older_1");
            localModel.addfile("f/file_older_2", "file_older_2");
            localModel.addfile("f/file_newer_1", "file_newer_1");
            localModel.addfile("f/file_newer_2", "file_newer_2");
            remoteModel = localModel;
        }
    }

    bool isBackup() const
    {
        return syncType == type_backupSync;
    }

    bool isExternalBackup() const
    {
        return isExternal && isBackup();
    }

    bool isInternalBackup() const
    {
        return !isExternal && isBackup();
    }

    bool shouldRecreateOnResume() const
    {
        if (pauseDuringAction)
        {
            return isExternalBackup();
        }

        return false;
    }

    bool shouldDisableSync() const
    {
        if (up)
        {
            return false;
        }

        if (pauseDuringAction)
        {
            return isInternalBackup();
        }

        return isBackup();
    }

    bool shouldUpdateDestination() const
    {
        return up || !isBackup();
    }

    bool shouldUpdateModel() const
    {
        return up
               || !pauseDuringAction
               || !isExternalBackup();
    }

    fs::path localSyncRootPath()
    {
        return localTestBasePath() / "f";
    }

    string remoteSyncRootPath()
    {
        return remoteTestBasePath + "/f";
    }

    Node* remoteSyncRoot()
    {
        Node* root = client1().client.nodebyhandle(client1().basefolderhandle);
        std::string remoteRootPath = remoteSyncRootPath();
        if (!root)
        {
            LOG_err << name()
                    << " root is NULL, local sync root:" 
                    << localSyncRootPath().u8string() 
                    << " remote sync root:" 
                    << remoteRootPath;
            return nullptr;
        }

        Node* n = client1().drillchildnodebyname(root, remoteRootPath);
        if (!n)
        {
            LOG_err << "remote sync root is NULL, local sync root:" 
                    << root->displaypath()
                    << " remote sync root:" 
                    << remoteRootPath;
        }
        return n;
    }

    void SetupTwoWaySync()
    {
        ASSERT_NE(remoteSyncRoot(), nullptr);

        string basePath   = client1().fsBasePath.u8string();
        string drivePath  = string(1, '\0');
        string sourcePath = localSyncRootPath().u8string();
        string targetPath = remoteSyncRootPath();

        if (isExternalBackup())
        {
            drivePath = localTestBasePath().u8string();
            drivePath.erase(0, basePath.size() + 1);
        }

        sourcePath.erase(0, basePath.size() + 1);

        backupId = client1().setupSync_mainthread(sourcePath, targetPath, isBackup(), false, drivePath);
        ASSERT_NE(backupId, UNDEF);

        if (Sync* sync = client1().syncByBackupId(backupId))
        {
            sync->syncname += "/" + name() + " ";
        }
    }

    void PauseTwoWaySync()
    {
        if (shouldRecreateOnResume())
        {
            client1().delSync_mainthread(backupId);
        }
    }

    void ResumeTwoWaySync()
    {
        if (shouldRecreateOnResume())
        {
            SetupTwoWaySync();
        }
    }

    void remote_rename(std::string nodepath, std::string newname, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        if (deleteTargetFirst) remote_delete(parentpath(nodepath) + "/" + newname, updatemodel, reportaction, true); // in case the target already exists

        if (updatemodel) remoteModel.emulate_rename(nodepath, newname);

        Node* testRoot = changeClient().client.nodebyhandle(client1().basefolderhandle);
        Node* n = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        ASSERT_TRUE(!!n);

        if (reportaction) out() << name() << " action: remote rename " << n->displaypath() << " to " << newname;

        attr_map updates('n', newname);
        auto e = changeClient().client.setattr(n, std::move(updates), nullptr, false);

        ASSERT_EQ(API_OK, error(e));
    }

    void remote_move(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        if (deleteTargetFirst) remote_delete(newparentpath + "/" + leafname(nodepath), updatemodel, reportaction, true); // in case the target already exists

        if (updatemodel) remoteModel.emulate_move(nodepath, newparentpath);

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote move " << n1->displaypath() << " to " << n2->displaypath();

        auto e = changeClient().client.rename(n1, n2, SYNCDEL_NONE, NodeHandle(), nullptr, false, nullptr);
        ASSERT_EQ(API_OK, e);
    }

    void remote_copy(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        if (updatemodel) remoteModel.emulate_copy(nodepath, newparentpath);

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote copy " << n1->displaypath() << " to " << n2->displaypath();

        TreeProcCopy tc;
        changeClient().client.proctree(n1, &tc, false, true);
        tc.allocnodes();
        changeClient().client.proctree(n1, &tc, false, true);
        tc.nn[0].parenthandle = UNDEF;

        SymmCipher key;
        AttrMap attrs;
        string attrstring;
        key.setkey((const ::mega::byte*)tc.nn[0].nodekey.data(), n1->type);
        attrs = n1->attrs;
        attrs.getjson(&attrstring);
        client1().client.makeattr(&key, tc.nn[0].attrstring, attrstring.c_str());
        changeClient().client.putnodes(n2->nodeHandle(), NoVersioning, std::move(tc.nn), nullptr, ++next_request_tag, false);
    }

    void remote_renamed_copy(std::string nodepath, std::string newparentpath, string newname, bool updatemodel, bool reportaction)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        if (updatemodel)
        {
            remoteModel.emulate_rename_copy(nodepath, newparentpath, newname);
        }

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote rename + copy " << n1->displaypath() << " to " << n2->displaypath() << " as " << newname;

        TreeProcCopy tc;
        changeClient().client.proctree(n1, &tc, false, true);
        tc.allocnodes();
        changeClient().client.proctree(n1, &tc, false, true);
        tc.nn[0].parenthandle = UNDEF;

        SymmCipher key;
        AttrMap attrs;
        string attrstring;
        key.setkey((const ::mega::byte*)tc.nn[0].nodekey.data(), n1->type);
        attrs = n1->attrs;
        LocalPath::utf8_normalize(&newname);
        attrs.map['n'] = newname;
        attrs.getjson(&attrstring);
        client1().client.makeattr(&key, tc.nn[0].attrstring, attrstring.c_str());
        changeClient().client.putnodes(n2->nodeHandle(), NoVersioning, std::move(tc.nn), nullptr, ++next_request_tag, false);
    }

    void remote_renamed_move(std::string nodepath, std::string newparentpath, string newname, bool updatemodel, bool reportaction)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        if (updatemodel)
        {
            remoteModel.emulate_rename_copy(nodepath, newparentpath, newname);
        }

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote rename + move " << n1->displaypath() << " to " << n2->displaypath() << " as " << newname;

        error e = changeClient().client.rename(n1, n2, SYNCDEL_NONE, NodeHandle(), newname.c_str(), false, nullptr);
        EXPECT_EQ(e, API_OK);
    }

    void remote_delete(std::string nodepath, bool updatemodel, bool reportaction, bool mightNotExist)
    {
        std::lock_guard<std::recursive_mutex> g(changeClient().clientMutex);

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        if (mightNotExist && !n) return;  // eg when checking to remove an item that is a move target but there isn't one

        ASSERT_TRUE(!!n);

        if (reportaction) out() << name() << " action: remote delete " << n->displaypath();

        if (updatemodel) remoteModel.emulate_delete(nodepath);

        auto e = changeClient().client.unlink(n, false, ++next_request_tag, false);
        ASSERT_TRUE(!e);
    }

    fs::path fixSeparators(std::string p)
    {
        for (auto& c : p)
            if (c == '/')
                c = fs::path::preferred_separator;
        return fs::u8path(p);
    }

    void local_rename(std::string path, std::string newname, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (deleteTargetFirst) local_delete(parentpath(path) + "/" + newname, updatemodel, reportaction, true); // in case the target already exists

        if (updatemodel) localModel.emulate_rename(path, newname);

        fs::path p1(localTestBasePath());
        p1 /= fixSeparators(path);
        fs::path p2 = p1.parent_path() / newname;

        if (reportaction) out() << name() << " action: local rename " << p1 << " to " << p2;

        std::error_code ec;
        for (int i = 0; i < 5; ++i)
        {
            fs::rename(p1, p2, ec);
            if (!ec) break;
            WaitMillisec(100);
        }

        if (!pauseDuringAction)
            client1().triggerPeriodicScanEarly(backupId);

        ASSERT_TRUE(!ec) << "local_rename " << p1 << " to " << p2 << " failed: " << ec.message();
    }

    void local_move(std::string from, std::string to, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (deleteTargetFirst) local_delete(to + "/" + leafname(from), updatemodel, reportaction, true);

        if (updatemodel) localModel.emulate_move(from, to);

        fs::path p1(localTestBasePath());
        fs::path p2(localTestBasePath());
        p1 /= fixSeparators(from);
        p2 /= fixSeparators(to);
        p2 /= p1.filename();  // non-existing file in existing directory case

        if (reportaction) out() << name() << " action: local move " << p1 << " to " << p2;

        std::error_code ec;
        fs::rename(p1, p2, ec);
        if (ec)
        {
            fs::remove_all(p2, ec);
            fs::rename(p1, p2, ec);
        }

        if (!pauseDuringAction)
            client1().triggerPeriodicScanEarly(backupId);

        ASSERT_TRUE(!ec) << "local_move " << p1 << " to " << p2 << " failed: " << ec.message();
    }

    void local_copy(std::string from, std::string to, bool updatemodel, bool reportaction)
    {
        if (updatemodel) localModel.emulate_copy(from, to);

        fs::path p1(localTestBasePath());
        fs::path p2(localTestBasePath());
        p1 /= fixSeparators(from);
        p2 /= fixSeparators(to);

        if (reportaction) out() << name() << " action: local copy " << p1 << " to " << p2;

        std::error_code ec;
        fs::copy(p1, p2, ec);

        if (!pauseDuringAction)
            client1().triggerPeriodicScanEarly(backupId);

        ASSERT_TRUE(!ec) << "local_copy " << p1 << " to " << p2 << " failed: " << ec.message();
    }

    void local_delete(std::string path, bool updatemodel, bool reportaction, bool mightNotExist)
    {
        fs::path p(localTestBasePath());
        p /= fixSeparators(path);

        if (mightNotExist && !fs::exists(p)) return;

        if (reportaction) out() << name() << " action: local_delete " << p;

        std::error_code ec;
        fs::remove_all(p, ec);

        if (!pauseDuringAction)
            client1().triggerPeriodicScanEarly(backupId);

        ASSERT_TRUE(!ec) << "local_delete " << p << " failed: " << ec.message();
        if (updatemodel) localModel.emulate_delete(path);
    }

    void source_rename(std::string nodepath, std::string newname, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (up) local_rename(nodepath, newname, updatemodel, reportaction, deleteTargetFirst);
        else remote_rename(nodepath, newname, updatemodel, reportaction, deleteTargetFirst);
    }

    void source_move(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (up) local_move(nodepath, newparentpath, updatemodel, reportaction, deleteTargetFirst);
        else remote_move(nodepath, newparentpath, updatemodel, reportaction, deleteTargetFirst);
    }

    void source_copy(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction)
    {
        if (up) local_copy(nodepath, newparentpath, updatemodel, reportaction);
        else remote_copy(nodepath, newparentpath, updatemodel, reportaction);
    }

    void source_delete(std::string nodepath, bool updatemodel, bool reportaction = false)
    {
        if (up) local_delete(nodepath, updatemodel, reportaction, false);
        else remote_delete(nodepath, updatemodel, reportaction, false);
    }

    void fileMayDiffer(std::string filepath)
    {
        fs::path p(localTestBasePath());
        p /= fixSeparators(filepath);

        client1().localFSFilesThatMayDiffer.insert(p);
        out() << "File may differ: " << p;
    }

    // Two-way sync has been started and is stable.  Now perform the test action

    enum ModifyStage { Prepare, MainAction };

    void PrintLocalTree(fs::path p)
    {
        out() << p;
        if (fs::is_directory(p))
        {
            for (auto i = fs::directory_iterator(p); i != fs::directory_iterator(); ++i)
            {
                PrintLocalTree(*i);
            }
        }
    }

    void PrintLocalTree(const LocalNode& node)
    {
        out() << node.getLocalPath().toPath(false);

        if (node.type == FILENODE) return;

        for (const auto& childIt : node.children)
        {
            PrintLocalTree(*childIt.second);
        }
    }

    void PrintRemoteTree(Node* n, string prefix = "")
    {
        prefix += string("/") + n->displayname();
        out() << prefix;
        if (n->type == FILENODE) return;
        for (auto& c : client1().client.getChildren(n))
        {
            PrintRemoteTree(c, prefix);
        }
    }

    void PrintModelTree(Model::ModelNode* n, string prefix = "")
    {
        prefix += string("/") + n->name;
        out() << prefix;
        if (n->type == Model::ModelNode::file) return;
        for (auto& c : n->kids)
        {
            PrintModelTree(c.get(), prefix);
        }
    }

    void Modify(ModifyStage stage)
    {
        bool prep = stage == Prepare;
        bool act = stage == MainAction;

        if (prep) out() << "Preparing action ";
        if (act) out() << "Executing action ";

        if (prep && printTreesBeforeAndAfter)
        {
            out() << " ---- local filesystem initial state ----";
            PrintLocalTree(fs::path(localTestBasePath()));

            if (auto* sync = client1().syncByBackupId(backupId))
            {
                out() << " ---- local node tree initial state ----";
                PrintLocalTree(*sync->localroot);
            }

            out() << " ---- remote node tree initial state ----";
            Node* testRoot = client1().client.nodebyhandle(changeClient().basefolderhandle);
            if (Node* n = client1().drillchildnodebyname(testRoot, remoteTestBasePath))
            {
                PrintRemoteTree(n);
            }
        }

        switch (action)
        {
        case action_rename:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_rename("f/f_0/file0_f_0", "file0_f_0_renamed", shouldUpdateModel(), true, true);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_rename("f/f_0/file0_f_0", "file0_f_0_renamed");
                    }
                }
                else
                {
                    source_rename("f/f_0", "f_0_renamed", shouldUpdateModel(), true, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_rename("f/f_0", "f_0_renamed");
                    }
                }
            }
            break;

        case action_moveWithinSync:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_move("f/f_1/file0_f_1", "f/f_0", shouldUpdateModel(), true, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_move("f/f_1/file0_f_1", "f/f_0");
                    }
                }
                else
                {
                    source_move("f/f_1", "f/f_0", shouldUpdateModel(), true, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_move("f/f_1", "f/f_0");
                    }
                }
            }
            break;

        case action_moveOutOfSync:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_move("f/f_0/file0_f_0", "outside", shouldUpdateModel(), false, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_delete("f/f_0/file0_f_0");
                    }
                }
                else
                {
                    source_move("f/f_0", "outside", shouldUpdateModel(), false, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_delete("f/f_0");
                    }
                }
            }
            break;

        case action_moveIntoSync:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_move("outside/file0_outside", "f/f_0", shouldUpdateModel(), false, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_copy("outside/file0_outside", "f/f_0");
                    }
                }
                else
                {
                    source_move("outside", "f/f_0", shouldUpdateModel(), false, false);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_delete("f/f_0/outside");
                        destinationModel().emulate_copy("outside", "f/f_0");
                    }
                }
            }
            break;

        case action_delete:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_delete("f/f_0/file0_f_0", shouldUpdateModel(), true);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_delete("f/f_0/file0_f_0");
                    }
                }
                else
                {
                    source_delete("f/f_0", shouldUpdateModel(), true);
                    if (shouldUpdateDestination())
                    {
                        destinationModel().emulate_delete("f/f_0");
                    }
                }
            }
            break;

        default: ASSERT_TRUE(false);
        }
    }

    void CheckSetup(State&, bool initial)
    {
        if (!initial && printTreesBeforeAndAfter)
        {
            out() << " ---- local filesystem before change ----";
            PrintLocalTree(fs::path(localTestBasePath()));

            if (auto* sync = client1().syncByBackupId(backupId))
            {
                out() << " ---- local node tree before change ----";
                PrintLocalTree(*sync->localroot);
            }

            out() << " ---- remote node tree before change ----";
            Node* testRoot = client1().client.nodebyhandle(changeClient().basefolderhandle);
            if (Node* n = client1().drillchildnodebyname(testRoot, remoteTestBasePath))
            {
                PrintRemoteTree(n);
            }
        }

        if (!initial) out() << "Checking setup state (should be no changes in twoway sync source): "<< name();

        // confirm source is unchanged after setup  (Two-way is not sending changes to the wrong side)
        bool localfs = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALFS, true, false, false); // todo: later enable debris checks
        bool localnode = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALNODE, true, false, false); // todo: later enable debris checks
        bool remote = client1().confirmModel(backupId, remoteModel.findnode("f"), StandardClient::CONFIRM_REMOTE, true, false, false); // todo: later enable debris checks
        EXPECT_EQ(localfs, localnode);
        EXPECT_EQ(localnode, remote);
        EXPECT_TRUE(localfs && localnode && remote) << " failed in " << name();
    }

    void WaitSetup()
    {
        const std::chrono::seconds maxWaitSeconds(120);
        const std::chrono::milliseconds checkInterval(5000);

        auto remoteIsReady = [this](StandardClient& client){ 
                int descendents1 = 0, descendents2 = 0;
                bool reported1 = false, reported2 = false;
                return client.recursiveConfirm(remoteModel.findnode("f"), client.drillchildnodebyname(client.gettestbasenode(), remoteTestBasePath + "/f"), descendents1, name(), 0, reported1, true, false) 
                    && client.recursiveConfirm(remoteModel.findnode("outside"), client.drillchildnodebyname(client.gettestbasenode(), remoteTestBasePath + "/outside"), descendents2, name(), 0, reported2, true, false);
        };

        state.resumeClient.waitFor(remoteIsReady, maxWaitSeconds, checkInterval);
        state.steadyClient.waitFor(remoteIsReady, maxWaitSeconds, checkInterval);
        state.nonsyncClient.waitFor(remoteIsReady, maxWaitSeconds, checkInterval);
    }

    // Two-way sync is stable again after the change.  Check the results.
    bool finalResult = false;
    void CheckResult(State&)
    {
        Sync* sync = client1().syncByBackupId(backupId);

        std::function<void()> printTrees = [&]() {
            out() << " ---- local filesystem after sync of change ----";
            PrintLocalTree(fs::path(localTestBasePath()));

            if (sync && sync->localroot)
            {
                out() << " ---- local node tree after sync of change ----";
                PrintLocalTree(*sync->localroot);
            }

            out() << " ---- remote node tree after sync of change ----";
            Node* testRoot = client1().client.nodebyhandle(changeClient().basefolderhandle);
            if (Node* n = client1().drillchildnodebyname(testRoot, remoteTestBasePath))
            {
                PrintRemoteTree(n);
            }
            out() << " ---- expected sync destination (model) ----";
            auto n = destinationModel().findnode("f");
            if (n) PrintModelTree(n);
        };

        if (printTreesBeforeAndAfter)
        {
            printTrees();

            // Don't saturate the logs if we're already displaying the trees.
            printTrees = nullptr;
        }

        out() << "Checking twoway sync "<< name();

        auto confirmedOk = true;

        if (shouldDisableSync())
        {
            bool lfs = client1().confirmModel(backupId, localModel.findnode("f"), localSyncRootPath(), true, false, false);
            bool rnt = client1().confirmModel(backupId, remoteModel.findnode("f"), remoteSyncRoot(), false, false);

            EXPECT_EQ(sync, nullptr) << "Sync isn't disabled: " << name();
            EXPECT_TRUE(lfs) << "Couldn't confirm LFS: " << name();
            EXPECT_TRUE(rnt) << "Couldn't confirm RNT: " << name();

            confirmedOk &= lfs && rnt;

            finalResult = sync == nullptr;
            finalResult &= confirmedOk;
        }
        else
        {
            EXPECT_NE(sync, (Sync*)nullptr);
            EXPECT_TRUE(sync && sync->state() == SYNC_ACTIVE);

            bool localfs = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALFS, true, false, false); // todo: later enable debris checks
            bool localnode = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALNODE, true, false, false); // todo: later enable debris checks
            bool remote = client1().confirmModel(backupId, remoteModel.findnode("f"), StandardClient::CONFIRM_REMOTE, true, false, false); // todo: later enable debris checks
            EXPECT_EQ(localfs, localnode);
            EXPECT_EQ(localnode, remote);
            EXPECT_TRUE(localfs && localnode && remote) << " failed in " << name();

            confirmedOk &= localfs && localnode && remote;

            finalResult = sync && sync->state() == SYNC_ACTIVE;
            finalResult &= confirmedOk;
        }

        // Show the trees if there's been a mismatch.
        if (printTrees && !confirmedOk)
        {
            printTrees();
        }
    }
};

bool CatchupClients(StandardClient* c1, StandardClient* c2, StandardClient* c3)
{
    out() << "Catching up";
    auto pb1 = makeSharedPromise<bool>();
    auto pb2 = makeSharedPromise<bool>();
    auto pb3 = makeSharedPromise<bool>();
    if (c1) c1->catchup(pb1);
    if (c2) c2->catchup(pb2);
    if (c3) c3->catchup(pb3);

    auto f1 = pb1->get_future();
    auto f2 = pb2->get_future();
    auto f3 = pb3->get_future();

    if (c1 && f1.wait_for(chrono::seconds(10)) == future_status::timeout) return false;
    if (c2 && f2.wait_for(chrono::seconds(10)) == future_status::timeout) return false;
    if (c3 && f3.wait_for(chrono::seconds(10)) == future_status::timeout) return false;

    EXPECT_TRUE((!c1 || f1.get()) &&
                (!c2 || f2.get()) &&
                (!c3 || f3.get()));
    out() << "Caught up";
    return true;
}

void PrepareForSync(StandardClient& client)
{
    auto local = client.fsBasePath / "twoway" / "initial";

    fs::create_directories(local);

    ASSERT_TRUE(buildLocalFolders(local, "f", 2, 2, 2));
    ASSERT_TRUE(buildLocalFolders(local, "outside", 2, 1, 1));

    constexpr auto delta = std::chrono::seconds(3600);

    ASSERT_TRUE(createDataFile(local / "f" / "file_older_1", "file_older_1", -delta));
    ASSERT_TRUE(createDataFile(local / "f" / "file_older_2", "file_older_2", -delta));
    ASSERT_TRUE(createDataFile(local / "f" / "file_newer_1", "file_newer_1", delta));
    ASSERT_TRUE(createDataFile(local / "f" / "file_newer_2", "file_newer_2", delta));

    auto* remote = client.drillchildnodebyname(client.gettestbasenode(), "twoway");
    ASSERT_NE(remote, nullptr);


    // Upload initial sync contents.
    ASSERT_TRUE(client.uploadFolderTree(local, remote));
    ASSERT_TRUE(client.uploadFilesInTree(local, remote));
}

bool WaitForRemoteMatch(map<string, TwoWaySyncSymmetryCase>& testcases,
                        chrono::seconds timeout)
{
    auto check = [&]() {
        auto i = testcases.begin();
        auto j = testcases.end();

        for ( ; i != j; ++i)
        {
            auto& testcase = i->second;

            if (testcase.pauseDuringAction) continue;

            auto& client = testcase.client1();
            auto& id = testcase.backupId;
            auto& model = testcase.remoteModel;

            if (!client.match(id, model.findnode("f")))
            {
                out() << "Cloud/model misatch: "
                      << client.client.clientname
                      << ": "
                      << testcase.name();

                return false;
            }
        }

        return true;
    };

    auto total = std::chrono::milliseconds(0);
    constexpr auto sleepIncrement = std::chrono::milliseconds(500);

    do
    {
        if (check())
        {
            out() << "Cloud/model matched.";
            return true;
        }

        out() << "Waiting for cloud/model match...";

        std::this_thread::sleep_for(sleepIncrement);
        total += sleepIncrement;
    }
    while (total < timeout);

    if (!check())
    {
        out() << "Timed out waiting for cloud/model match.";
        return false;
    }

    return true;
}

TEST_F(SyncTest, TwoWay_Highlevel_Symmetries)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();

    StandardClient clientA2(localtestroot, "clientA2");

    ASSERT_TRUE(clientA2.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "twoway", 0, 0, true));

    PrepareForSync(clientA2);

    StandardClient clientA1Steady(localtestroot, "clientA1S");
    StandardClient clientA1Resume(localtestroot, "clientA1R");
    ASSERT_TRUE(clientA1Steady.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", false, true));
    ASSERT_TRUE(clientA1Resume.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", false, true));
    fs::create_directory(clientA1Steady.fsBasePath / fs::u8path("twoway"));
    fs::create_directory(clientA1Resume.fsBasePath / fs::u8path("twoway"));
    fs::create_directory(clientA2.fsBasePath / fs::u8path("twoway"));

    TwoWaySyncSymmetryCase::State allstate(clientA1Steady, clientA1Resume, clientA2);
    allstate.localBaseFolderSteady = clientA1Steady.fsBasePath / fs::u8path("twoway");
    allstate.localBaseFolderResume = clientA1Resume.fsBasePath / fs::u8path("twoway");

    std::map<std::string, TwoWaySyncSymmetryCase> cases;

    static set<string> tests = {
    }; // tests

    for (int syncType = TwoWaySyncSymmetryCase::type_numTypes; syncType--; )
    {
        //if (syncType != TwoWaySyncSymmetryCase::type_backupSync) continue;

        for (int selfChange = 0; selfChange < 2; ++selfChange)
        {
            //if (!selfChange) continue;

            for (int up = 0; up < 2; ++up)
            {
                //if (!up) continue;

                for (int action = 0; action < (int)TwoWaySyncSymmetryCase::action_numactions; ++action)
                {
                    //if (action != TwoWaySyncSymmetryCase::action_rename) continue;

                    for (int file = 0; file < 2; ++file)
                    {
                        //if (!file) continue;

                        for (int isExternal = 0; isExternal < 2; ++isExternal)
                        {
                            if (isExternal && syncType != TwoWaySyncSymmetryCase::type_backupSync)
                            {
                                continue;
                            }

                            for (int pauseDuringAction = 0; pauseDuringAction < 2; ++pauseDuringAction)
                            {
                                //if (pauseDuringAction) continue;

                                // we can't make changes if the client is not running
                                if (pauseDuringAction && selfChange) continue;

                                TwoWaySyncSymmetryCase testcase(allstate);
                                testcase.syncType = TwoWaySyncSymmetryCase::SyncType(syncType);
                                testcase.selfChange = selfChange != 0;
                                testcase.up = up;
                                testcase.action = TwoWaySyncSymmetryCase::Action(action);
                                testcase.file = file;
                                testcase.isExternal = isExternal;
                                testcase.pauseDuringAction = pauseDuringAction;
                                testcase.printTreesBeforeAndAfter = !tests.empty();

                                if (tests.empty() || tests.count(testcase.name()) > 0)
                                {
                                    auto name = testcase.name();
                                    cases.emplace(name, std::move(testcase));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    out() << "Creating initial local files/folders for " << cases.size() << " sync test cases";
    for (auto& testcase : cases)
    {
        testcase.second.SetupForSync();
    }

    out() << "Waiting intial state to be ready and clients are updated with actionpackets";
    for (auto& testcase : cases)
    {
        testcase.second.WaitSetup();
    }
    
    out() << "Setting up each sub-test's Two-way sync of 'f'";
    for (auto& testcase : cases)
    {
        testcase.second.SetupTwoWaySync();
    }

    out() << "Letting all " << cases.size() << " Two-way syncs run";
    waitonsyncs(std::chrono::seconds(10), &clientA1Steady, &clientA1Resume);

    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);
    waitonsyncs(std::chrono::seconds(10), &clientA1Steady, &clientA1Resume);

    out() << "Checking intial state";
    for (auto& testcase : cases)
    {
        testcase.second.CheckSetup(allstate, true);
    }

    // make changes in destination to set up test
    for (auto& testcase : cases)
    {
        testcase.second.Modify(TwoWaySyncSymmetryCase::Prepare);
    }

    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);

    out() << "Letting all " << cases.size() << " Two-way syncs run";
    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA1Resume, &clientA2);

    out() << "Checking Two-way source is unchanged";
    for (auto& testcase : cases)
    {
        testcase.second.CheckSetup(allstate, false);
    }

    auto backupsAreMonitoring = [&cases]() {
        for (auto& i : cases)
        {
            // Convenience.
            auto& testcase = i.second;

            // Only check backup syncs.
            if (!testcase.isBackup()) continue;

            // Only check active syncs.
            if (!testcase.client1().syncByBackupId(testcase.backupId)) continue;

            // Get the sync's config so we can determine if it's monitoring.
            auto config = testcase.client1().syncConfigByBackupID(testcase.backupId);

            // Is the sync monitoring as it should be?
            if (config.getBackupState() != SYNC_BACKUP_MONITOR)
            {
                LOG_warn << "Backup state is not SYNC_BACKUP_MONITOR, for test " << testcase.name() << ". actual state: " << config.getBackupState();
                return false;
            }
        }

        // Everyone's monitoring as they should be.
        return true;
    };

    out() << "Checking Backups are Monitoring";
    ASSERT_TRUE(backupsAreMonitoring());

    int paused = 0;
    for (auto& testcase : cases)
    {
        if (testcase.second.pauseDuringAction)
        {
            testcase.second.PauseTwoWaySync();
            ++paused;
        }
    }

    // save session and local log out A1R to set up for resume
    string session;
    clientA1Resume.client.dumpsession(session);
    clientA1Resume.localLogout();

    auto remainingResumeSyncs = clientA1Resume.client.syncs.getConfigs(false);
    ASSERT_EQ(0u, remainingResumeSyncs.size());

    if (paused)
    {
        out() << "Paused " << paused << " Two-way syncs";
        WaitMillisec(1000);
    }

    clientA1Steady.logcb = clientA1Resume.logcb = clientA2.logcb = true;

    out() << "Performing action ";
    for (auto& testcase : cases)
    {
        testcase.second.Modify(TwoWaySyncSymmetryCase::MainAction);
    }
    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA2);   // leave out clientA1Resume as it's 'paused' (locallogout'd) for now
    CatchupClients(&clientA1Steady, &clientA2);
    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA2);   // leave out clientA1Resume as it's 'paused' (locallogout'd) for now

    // resume A1R session (with sync), see if A2 nodes and localnodes get in sync again
    clientA1Resume.received_syncs_restored = false;
    ASSERT_TRUE(clientA1Resume.login_fetchnodes(session));
    ASSERT_EQ(clientA1Resume.basefolderhandle, clientA2.basefolderhandle);

    // wait for normal sync resumes to complete
    clientA1Resume.waitFor([&](StandardClient& sc) { return sc.received_syncs_restored; }, std::chrono::seconds(30));

    // now resume remainder - some are external syncs
    int resumed = 0;
    for (auto& testcase : cases)
    {
        if (testcase.second.pauseDuringAction)
        {
            testcase.second.ResumeTwoWaySync();
            ++resumed;
        }
    }
    if (resumed)
    {
        out() << "Resumed " << resumed << " Two-way syncs";
        WaitMillisec(3000);
    }

    out() << "Waiting for remote changes to make it to clients...";
    EXPECT_TRUE(WaitForRemoteMatch(cases, chrono::seconds(64)));  // 64 because the jenkins machines can be slow

    out() << "Letting all " << cases.size() << " Two-way syncs run";

    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA1Resume, &clientA2);

    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);
    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA1Resume, &clientA2);

    out() << "Checking Backups are Monitoring";
    ASSERT_TRUE(backupsAreMonitoring());

    out() << "Checking local and remote state in each sub-test";

    for (auto& testcase : cases)
    {
        testcase.second.CheckResult(allstate);
    }
    int succeeded = 0, failed = 0;
    for (auto& testcase : cases)
    {
        if (testcase.second.finalResult) ++succeeded;
        else
        {
            out() << "failed: " << testcase.second.name();
            ++failed;
        }
    }
    out() << "Succeeded: " << succeeded << " Failed: " << failed;

    // Clear tree-state cache.
    {
        StandardClient cC(localtestroot, "cC");
        ASSERT_TRUE(cC.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", false, true));
    }
}

TEST_F(SyncTest, MoveExistingIntoNewDirectoryWhilePaused)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT  = chrono::seconds(4);

    Model model;
    fs::path root;
    string session;
    handle id;

    // Initial setup.
    {
        StandardClient c(TESTROOT, "c");
        // don't use clientManager as we re-use the client "c" for its directory

        // Log in client.
        ASSERT_TRUE(c.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

        // Add and start sync.
        id = c.setupSync_mainthread("s", "s", false, false);
        ASSERT_NE(id, UNDEF);

        // Squirrel away for later use.
        root = c.syncSet(id).localpath.u8string();

        // Populate filesystem.
        model.addfolder("a");
        model.addfolder("c");
        model.generate(root);

        c.triggerPeriodicScanEarly(id);

        // Wait for initial sync to complete.
        waitonsyncs(TIMEOUT, &c);

        // Make sure everything arrived safely.
        ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));

        // Save the session so we can resume later.
        c.client.dumpsession(session);

        // Log out client, taking care to keep caches.
        c.localLogout();
    }

    StandardClient c(TESTROOT, "c");

    // Add a new hierarchy to be scanned.
    model.addfolder("b");
    model.generate(root);

    // Move c under b.
    fs::rename(root / "c", root / "b" / "c");

    // Update the model.
    model.movenode("c", "b");

    // Hook onAutoResumeResult callback.
    promise<void> notify;

    c.mOnSyncStateConfig = [&notify](const SyncConfig& config) {
        if (config.mRunState == SyncRunState::Run)
            notify.set_value();
    };

    // Log in client resuming prior session.
    ASSERT_TRUE(c.login_fetchnodes(session));

    // Wait for the sync to be resumed.
    ASSERT_TRUE(debugTolerantWaitOnFuture(notify.get_future(), 45));

    // Wait for the sync to catch up.
    waitonsyncs(TIMEOUT, &c);

    // Were the changes propagated?
    ASSERT_TRUE(c.confirmModel_mainthread(model.root.get(), id));
}

TEST_F(SyncTest, ForeignChangesInTheCloudDisablesMonitoringBackup)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClientInUse c = g_clientManager->getCleanStandardClient(0, TESTROOT);
    StandardClientInUse cu = g_clientManager->getCleanStandardClient(0, TESTROOT);
    // Log callbacks.
    c->logcb = true;
    cu->logcb = true;
    ASSERT_TRUE(cu->resetBaseFolderMulticlient(c));
    ASSERT_TRUE(c->makeCloudSubdirs("s", 0, 0));
    ASSERT_TRUE(CatchupClients(cu, c));

    // Add and start sync.
    const auto id = c->setupSync_mainthread("s", "s", true, false);
    ASSERT_NE(id, UNDEF);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, c);

    // Make sure we're in monitoring mode.
    ASSERT_TRUE(c->waitFor(SyncMonitoring(id), TIMEOUT));

    // Make a (foreign) change to the cloud.
    {
        // Create a directory.
        vector<NewNode> node(1);

        cu->client.putnodes_prepareOneFolder(&node[0], "d", false);

        ASSERT_TRUE(cu->putnodes(c->syncSet(id).h, NoVersioning, std::move(node)));
    }

    // Give our sync some time to process remote changes.
    waitonsyncs(TIMEOUT, c);

    // Wait for the sync to be disabled.
    ASSERT_TRUE(c->waitFor(SyncDisabled(id), TIMEOUT));

    // Has the sync failed?
    {
        SyncConfig config = c->syncConfigByBackupID(id);

        ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MONITOR);
        ASSERT_EQ(config.mEnabled, false);
        ASSERT_EQ(config.mError, BACKUP_MODIFIED);
    }
}

class BackupClient
  : public StandardClient
{
public:
    BackupClient(const fs::path& basePath, const string& name)
      : StandardClient(basePath, name)
      , mOnFileAdded()
    {
    }

    void file_added(File* file) override
    {
        StandardClient::file_added(file);

        if (mOnFileAdded) mOnFileAdded(*file);
    }

    using FileAddedCallback = std::function<void(File&)>;

    FileAddedCallback mOnFileAdded;
}; // Client

TEST_F(SyncTest, MonitoringExternalBackupRestoresInMirroringMode)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(8);

    // Model.
    Model m;

    // Sync Root Handle.
    NodeHandle rootHandle;

    // Session ID.
    string sessionID;

    // Sync Backup ID.
    handle id;

    {
        StandardClient cb(TESTROOT, "cb");
        // can not use ClientManager as both these clients must refer to their filesystem as "cb"
        // even though there are two of them

        // Log callbacks.
        cb.logcb = true;

        // Log in client.
        ASSERT_TRUE(cb.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

        // Create some files to synchronize.
        m.addfile("d/f");
        m.addfile("f");
        m.generate(cb.fsBasePath / "s");

        // Add and start sync.
        id = cb.setupSync_mainthread("s", "s", true, true, "");
        ASSERT_NE(id, UNDEF);

        // Wait for sync to complete.
        waitonsyncs(TIMEOUT, &cb);

        // Give the engine some time to actually upload the files.
        ASSERT_TRUE(cb.waitFor(SyncRemoteMatch("s", m.root.get()), TIMEOUT));

        // Make sure everything made it to the cloud.
        ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));

        // Wait for sync to transition to monitoring mode.
        ASSERT_TRUE(cb.waitFor(SyncMonitoring(id), TIMEOUT));

        // Get our hands on the sync's root handle.
        rootHandle = cb.syncSet(id).h;

        // Record this client's session.
        cb.client.dumpsession(sessionID);

        // Log out the client.
        cb.localLogout();
    }

    StandardClient cb(TESTROOT, "cb");

    cb.logcb = true;

    // Log in client.
    ASSERT_TRUE(cb.login_fetchnodes(sessionID));

    // Make a change in the cloud.
    {
        vector<NewNode> node(1);

        cb.client.putnodes_prepareOneFolder(&node[0], "g", false);

        ASSERT_TRUE(cb.putnodes(rootHandle, NoVersioning, std::move(node)));
    }

    // Restore the backup sync.
    ASSERT_TRUE(cb.backupOpenDrive(cb.fsBasePath));

    // Re-enable the sync.
    ASSERT_TRUE(cb.enableSyncByBackupId(id, "cb"));

    // Wait for the mirror to complete.
    waitonsyncs(TIMEOUT, &cb);

    // Cloud should mirror the local disk. (ie, g should be gone in the cloud)
    ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));
}

TEST_F(SyncTest, MonitoringExternalBackupResumesInMirroringMode)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    StandardClientInUse cb = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    cb->logcb = true;

    // Log in client.
    ASSERT_TRUE(cb->resetBaseFolderMulticlient());
    ASSERT_TRUE(cb->makeCloudSubdirs("s", 0, 0));
    ASSERT_TRUE(CatchupClients(cb));

    // Create some files to be synchronized.
    Model m;

    m.addfile("d/f");
    m.addfile("f");
    m.generate(cb->fsBasePath / "s");

    // Add and start sync.
    auto id = cb->setupSync_mainthread("s", "s", true, true, "");
    ASSERT_NE(id, UNDEF);

    // Wait for the mirror to complete.
    waitonsyncs(TIMEOUT, cb);

    // Make sure everything arrived safe and sound.
    ASSERT_TRUE(cb->confirmModel_mainthread(m.root.get(), id));

    // Wait for transition to monitoring mode.
    ASSERT_TRUE(cb->waitFor(SyncMonitoring(id), TIMEOUT));

    // Disable the sync.
    ASSERT_TRUE(cb->disableSync(id, NO_SYNC_ERROR, true));

    // Make sure the sync's config is as we expect.
    {
        auto config = cb->syncConfigByBackupID(id);

        // Backup should remain in monitoring mode.
        ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MONITOR);

        // Disabled external backups are always considered "user-disabled."
        // That is, the user must consciously decide to resume these syncs.
        ASSERT_EQ(config.mEnabled, false);
    }

    // Make a change in the cloud.
    {
        vector<NewNode> node(1);

        cb->client.putnodes_prepareOneFolder(&node[0], "g", false);

        auto rootHandle = cb->syncSet(id).h;
        ASSERT_TRUE(cb->putnodes(rootHandle, NoVersioning, std::move(node)));
    }

    // Re-enable the sync.
    ASSERT_TRUE(cb->enableSyncByBackupId(id, ""));

    // Wait for the mirror to complete.
    waitonsyncs(TIMEOUT, cb);

    // Cloud should mirror the disk.
    ASSERT_TRUE(cb->confirmModel_mainthread(m.root.get(), id));
}

TEST_F(SyncTest, MirroringInternalBackupResumesInMirroringMode)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT  = chrono::seconds(4);

    // Session ID.
    string sessionID;

    // Sync Backup ID.
    handle id;

    // Sync Root Handle.
    NodeHandle rootHandle;

    // Model.
    Model m;

    StandardClientInUse cf = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    cf->logcb = true;

    // Log in client.
    ASSERT_TRUE(cf->resetBaseFolderMulticlient());
    ASSERT_TRUE(cf->makeCloudSubdirs("s", 0, 0));
    ASSERT_TRUE(CatchupClients(cf));

    // Check manual resume.
    {
        BackupClient cb(TESTROOT, "cb");
        // can not use ClientManager as is a BackupClient and
        // "cb" is re-used later

        // Log callbacks.
        cb.logcb = true;

        // Log client in.
        ASSERT_TRUE(cb.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

        // Set upload throttle.
        //
        // This is so that we can disable the sync before it transitions
        // to the monitoring state.
        cb.client.setmaxuploadspeed(1);

        // Give the sync something to backup.
        m.addfile("d/f", randomData(16384));
        m.addfile("f", randomData(16384));
        m.generate(cb.fsBasePath / "s");

        // Disable the sync when it starts uploading a file.
        cb.mOnFileAdded = [&cb, &id](File& file) {

            // the upload has been set super slow so there's loads of time.

            // get the single sync
            SyncConfig config;
            ASSERT_TRUE(cb.client.syncs.syncConfigByBackupId(id, config));

            // Make sure the sync's in mirroring mode.
            ASSERT_EQ(config.mBackupId, id);
            ASSERT_EQ(config.mSyncType, SyncConfig::TYPE_BACKUP);
            ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MIRROR);

            // Disable the sync.
            cb.client.syncs.disableSyncs(false, NO_SYNC_ERROR, true, nullptr);

            // Callback's done its job.
            cb.mOnFileAdded = nullptr;
        };

        // Add and start sync.
        id = cb.setupSync_mainthread("s", "s", true, false);
        ASSERT_NE(id, UNDEF);

        // Let the sync mirror.
        waitonsyncs(TIMEOUT, &cb);

        // Make sure the sync's been disabled.
        ASSERT_FALSE(cb.syncByBackupId(id));

        // Make sure it's still in mirror mode.
        {
            auto config = cb.syncConfigByBackupID(id);

            ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MIRROR);
            ASSERT_EQ(config.mEnabled, true);
            ASSERT_EQ(config.mError, NO_SYNC_ERROR);
        }

        // Get our hands on sync root's cloud handle.
        rootHandle = cb.syncSet(id).h;
        ASSERT_TRUE(!rootHandle.isUndef());

        // Make some changes to the cloud.
        vector<NewNode> node(1);

        cf->client.putnodes_prepareOneFolder(&node[0], "g", false);

        ASSERT_TRUE(cf->putnodes(rootHandle, NoVersioning, std::move(node)));

        // Log out the client when we try and upload a file.
        std::promise<void> waiter;

        cb.mOnFileAdded = [&cb, &waiter, &id](File& file) {

            // get the single sync
            SyncConfig config;
            ASSERT_TRUE(cb.client.syncs.syncConfigByBackupId(id, config));

            // Make sure we're mirroring.
            ASSERT_EQ(config.mBackupId, id);
            ASSERT_EQ(config.mSyncType, SyncConfig::TYPE_BACKUP);
            ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MIRROR);

            // Notify the waiter.
            waiter.set_value();

            // Callback's done its job.
            cb.mOnFileAdded = nullptr;
        };

        // Resume the backup.
        ASSERT_TRUE(cb.enableSyncByBackupId(id, ""));

        // Wait for the sync to try and upload a file.
        ASSERT_TRUE(debugTolerantWaitOnFuture(waiter.get_future(), 45));

        // Save the session ID.
        cb.client.dumpsession(sessionID);

        // Log out the client.
        cb.localLogout();
    }

    // Create a couple new nodes.
    {
        vector<NewNode> nodes(2);

        cf->client.putnodes_prepareOneFolder(&nodes[0], "h0", false);
        cf->client.putnodes_prepareOneFolder(&nodes[1], "h1", false);

        ASSERT_TRUE(cf->putnodes(rootHandle, NoVersioning, std::move(nodes)));
    }

    // Check automatic resume.
    StandardClient cb(TESTROOT, "cb");

    // Log callbacks.
    cb.logcb = true;

    // So we can pause execution until al the syncs are restored.
    promise<void> notifier;

    // Hook the OnSyncStateConfig callback.
    cb.mOnSyncStateConfig = [&](const SyncConfig& config) {
        // Let waiters know we've restored the sync.
        if (config.mRunState == SyncRunState::Run)
            notifier.set_value();
    };

    // Log in client, resuming prior session.
    ASSERT_TRUE(cb.login_fetchnodes(sessionID));

    // Wait for the sync to be restored.
    ASSERT_NE(notifier.get_future().wait_for(std::chrono::seconds(8)),
              future_status::timeout);

    // Check config has been resumed.
    ASSERT_TRUE(cb.syncByBackupId(id));

    // Just let the sync mirror, Marge!
    waitonsyncs(TIMEOUT, &cb);

    // The cloud should match the local disk precisely.
    ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));
}

TEST_F(SyncTest, MonitoringInternalBackupResumesInMonitoringMode)
{
    const auto TESTROOT = makeNewTestRoot();
    const auto TIMEOUT = chrono::seconds(8);

    // Sync Backup ID.
    handle id;

    // Sync Root Handle.
    NodeHandle rootHandle;

    // Session ID.
    string sessionID;

    // Model.
    Model m;

    StandardClientInUse cf = g_clientManager->getCleanStandardClient(0, TESTROOT);

    // Log callbacks.
    cf->logcb = true;

    // Log in client.
    ASSERT_TRUE(cf->resetBaseFolderMulticlient());
    ASSERT_TRUE(cf->makeCloudSubdirs("s", 0, 0));
    ASSERT_TRUE(CatchupClients(cf));

    // Manual resume.
    {
        StandardClient cb(TESTROOT, "cb");
        // can not use ClientManager as we re-use "cb" (in filesystem) later

        // Log callbacks.
        cb.logcb = true;

        // Log in client.
        ASSERT_TRUE(cb.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

        // Give the sync something to mirror.
        m.addfile("d/f");
        m.addfile("f");
        m.generate(cb.fsBasePath / "s");

        // Add and start backup.
        id = cb.setupSync_mainthread("s", "s", true, false);
        ASSERT_NE(id, UNDEF);

        // Wait for the backup to complete.
        waitonsyncs(TIMEOUT, &cb);

        // Wait for transition to monitoring mode.
        ASSERT_TRUE(cb.waitFor(SyncMonitoring(id), TIMEOUT));

        // Disable the sync.
        ASSERT_TRUE(cb.disableSync(id, NO_SYNC_ERROR, true));

        // Make sure the sync was monitoring.
        {
            auto config = cb.syncConfigByBackupID(id);

            ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MONITOR);
            ASSERT_EQ(config.mEnabled, true);
            ASSERT_EQ(config.mError, NO_SYNC_ERROR);
        }

        // Get our hands on the sync's root handle.
        rootHandle = cb.syncSet(id).h;

        // Make a remote change.
        //
        // This is so the backup will fail upon resume.
        {
            vector<NewNode> node(1);

            cf->client.putnodes_prepareOneFolder(&node[0], "g", false);

            ASSERT_TRUE(cf->putnodes(rootHandle, NoVersioning, std::move(node)));
        }

        // Enable the backup.
        ASSERT_TRUE(cb.enableSyncByBackupId(id, ""));

        // Give the sync some time to think.
        waitonsyncs(TIMEOUT, &cb);

        // Wait for the sync to be disabled.
        ASSERT_TRUE(cb.waitFor(SyncDisabled(id), TIMEOUT));

        // Make sure it's been disabled for the right reasons.
        {
            auto config = cb.syncConfigByBackupID(id);

            ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MONITOR);
            ASSERT_EQ(config.mEnabled, false);
            ASSERT_EQ(config.mError, BACKUP_MODIFIED);
        }

        // Manually enable the sync.
        // It should come up in mirror mode.
        ASSERT_TRUE(cb.enableSyncByBackupId(id, ""));

        // Let it bring the cloud in line.
        waitonsyncs(TIMEOUT, &cb);

        // Cloud should match the local disk precisely.
        ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));

        // Save the session ID.
        cb.client.dumpsession(sessionID);

        // Log out the client.
        cb.localLogout();
    }

    // Make a remote change.
    {
        vector<NewNode> node(1);

        cf->client.putnodes_prepareOneFolder(&node[0], "h", false);

        ASSERT_TRUE(cf->putnodes(rootHandle, NoVersioning, std::move(node)));
    }

    // Automatic resume.
    StandardClient cb(TESTROOT, "cb");

    // Log callbacks.
    cb.logcb = true;

    // Hook onAutoResumeResult callback.
    promise<void> notify;

    cb.mOnSyncStateConfig = [&cb, &notify](const SyncConfig& config) {
        // Is the sync up and running?
        if (config.mRunState != SyncRunState::Run)
            return;

        // Then let our waiter know it can proceed.
        notify.set_value();

        // We're not interested in any further callbacks.
        cb.mOnSyncStateConfig = nullptr;
    };

    // Log in the client.
    ASSERT_TRUE(cb.login_fetchnodes(sessionID));

    // Wait for the sync to be resumed.
    ASSERT_TRUE(debugTolerantWaitOnFuture(notify.get_future(), 45));

    // Give the sync some time to think.
    waitonsyncs(TIMEOUT, &cb);

    // Wait for the sync to be disabled.
    ASSERT_TRUE(cb.waitFor(SyncDisabled(id), TIMEOUT));

    // Make sure it's been disabled for the right reasons.
    {
        auto config = cb.syncConfigByBackupID(id);

        ASSERT_EQ(config.mBackupState, SYNC_BACKUP_MONITOR);
        ASSERT_EQ(config.mEnabled, false);
        ASSERT_EQ(config.mError, BACKUP_MODIFIED);
    }

    // Re-enable the sync.
    ASSERT_TRUE(cb.enableSyncByBackupId(id, ""));

    // Wait for the sync to complete mirroring.
    waitonsyncs(TIMEOUT, &cb);

    // Cloud should mirror the disk.
    ASSERT_TRUE(cb.confirmModel_mainthread(m.root.get(), id));
}

#ifdef DEBUG

class BackupBehavior
  : public ::testing::Test
{
public:
    void doTest(const string& initialContent, const string& updatedContent);
}; // BackupBehavior

void BackupBehavior::doTest(const string& initialContent,
                            const string& updatedContent)
{
    auto TESTROOT = makeNewTestRoot();
    auto TIMEOUT  = std::chrono::seconds(8);

    StandardClient cu(TESTROOT, "cu");

    // Log callbacks.
    cu.logcb = true;

    // Log in uploader client.
    ASSERT_TRUE(cu.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "s", 0, 0));

    // Add and start a backup sync.
    const auto idU = cu.setupSync_mainthread("su", "s", true, true);
    ASSERT_NE(idU, UNDEF);

    // Add a file for the engine to synchronize.
    Model m;

    m.addfile("f", initialContent);
    m.generate(cu.fsBasePath / "su");

    cu.triggerPeriodicScanEarly(idU);

    // Wait for the engine to process and upload the file.
    waitonsyncs(TIMEOUT, &cu);

    // Make sure the file made it to the cloud.
    ASSERT_TRUE(cu.confirmModel_mainthread(m.root.get(), idU));

    // Update file.
    {
        // Capture file's current mtime.
        auto mtime = fs::last_write_time(cu.fsBasePath / "su" / "f");

        // Update the file's content.
        m.addfile("f", updatedContent);

        // Hook callback so we can tweak the mtime.
        cu.mOnSyncDebugNotification = [&](const SyncConfig&, int, const Notification& notification) {
            // Roll back the mtime now that we know it will be processed.
            fs::last_write_time(cu.fsBasePath / "su" / "f", mtime);

            // No need for the engine to call us again.
            cu.mOnSyncDebugNotification = nullptr;
        };

        // Write the file.
        m.generate(cu.fsBasePath / "su", true);

        // do not Rewind the file's mtime here. Let the callback just above do it.
        // otherwise, on checking the fs notification we will conclude "Self filesystem notification skipped"
        // possibly we could do it this way after sync rework is merged.
        // fs::last_write_time(cu.fsBasePath / "su" / "f", mtime);

        cu.triggerPeriodicScanEarly(idU);
    }

    // Wait for the engine to process the change.
    waitonsyncs(TIMEOUT, &cu);

    // Make sure the sync hasn't been disabled.
    {
        auto config = cu.syncConfigByBackupID(idU);

        ASSERT_EQ(config.mEnabled, true);
        ASSERT_EQ(config.mError, NO_SYNC_ERROR);
    }

    // Check that the file's been uploaded to the cloud.
    {
        StandardClient cd(TESTROOT, "cd");

        // Log in client.
        ASSERT_TRUE(cd.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));

        // Add and start a new sync.
        auto idD = cd.setupSync_mainthread("sd", "s", false, true);
        ASSERT_NE(idD, UNDEF);

        // Wait for the sync to complete.
        waitonsyncs(TIMEOUT, &cd);

        // Make sure we haven't uploaded anything.
        ASSERT_TRUE(cu.confirmModel_mainthread(m.root.get(), idU));

        // Necessary since we've downloaded a file.
        m.ensureLocalDebrisTmpLock("");

        // Check that we've downloaded what we should've.
        ASSERT_TRUE(cd.confirmModel_mainthread(m.root.get(), idD));
    }
}

TEST_F(BackupBehavior, SameMTimeSmallerCRC)
{
    // File's small enough that the content is the CRC.
    auto initialContent = string("f");
    auto updatedContent = string("e");

    doTest(initialContent, updatedContent);
}

TEST_F(BackupBehavior, SameMTimeSmallerSize)
{
    auto initialContent = string("ff");
    auto updatedContent = string("f");

    doTest(initialContent, updatedContent);
}
#endif // DEBUG

TEST_F(SyncTest, UndecryptableSharesBehavior)
{
    const auto TESTROOT = makeNewTestRoot();

    StandardClient client0(TESTROOT, "client0");
    // can not use ClientManager as we re-login later
    StandardClient client1(TESTROOT, "client1");
    StandardClient client2(TESTROOT, "client2");

    // Log in the clients.
    ASSERT_TRUE(client0.login_reset("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(client1.login_reset("MEGA_EMAIL_AUX", "MEGA_PWD_AUX"));
    ASSERT_TRUE(client2.login_reset("MEGA_EMAIL_AUX2", "MEGA_PWD_AUX2"));

    // Make sure our "contacts" know about each other.
    {
        // Convenience predicates.
        auto contactRequestReceived = [](handle id) {
            return [id](StandardClient& client) {
                return client.ipcr(id);
            };
        };
        auto contactRequestFnished = [](string& email) {
            return [&email](StandardClient& client) {
                return !client.opcr(email);
            };
        };
        auto contactVerificationFinished = [](string& email) {
            return [&email](StandardClient& client) {
                return client.isverified(email);
            };
        };

        // Convenience helper.
        auto contactAdd = [&](StandardClient& client, const string& name) {
            // Get our hands on the contact's email.
            string email = getenv(name.c_str());
            // Get main client email.
            string email0 = getenv("MEGA_EMAIL");

            // Are we already associated with this contact?
            if (client0.iscontact(email))
            {
                // Then remove them.
                ASSERT_TRUE(client0.rmcontact(email));
            }

            // Remove pending contact request, if any.
            if (client0.opcr(email))
            {
                ASSERT_TRUE(client0.opcr(email, OPCA_DELETE));
            }

            // Add the contact.
            auto id = client0.opcr(email, OPCA_ADD);
            ASSERT_NE(id, UNDEF);

            // Wait for the contact to receive the request.
            ASSERT_TRUE(client.waitFor(contactRequestReceived(id), DEFAULTWAIT));

            // Accept the contact request.
            ASSERT_TRUE(client.ipcr(id, IPCA_ACCEPT));

            // Wait for the response to reach first client
            ASSERT_TRUE(client0.waitFor(contactRequestFnished(email), DEFAULTWAIT));

            // Verify contact credentials if they are not
            if (gManualVerification)
            {
                if (!client0.isverified(email))
                {
                    ASSERT_TRUE(client0.verifyCredentials(email));
                }
                if (!client.isverified(email0))
                {
                    ASSERT_TRUE(client.verifyCredentials(email0));
                }

                // Wait for contact verification
                ASSERT_TRUE(client0.waitFor(contactVerificationFinished(email), DEFAULTWAIT));
                ASSERT_TRUE(client.waitFor(contactVerificationFinished(email0), DEFAULTWAIT));
            }
        };

        // Introduce the contacts to each other.
        ASSERT_NO_FATAL_FAILURE(contactAdd(client1, "MEGA_EMAIL_AUX"));
        ASSERT_NO_FATAL_FAILURE(contactAdd(client2, "MEGA_EMAIL_AUX2"));
    }

    Model model;

    // Populate the local filesystem.
    model.addfile("t/f");
    model.addfile("u/f");
    model.addfile("v/f");
    model.addfile("f");
    model.addfile(".megaignore", "#");
    model.generate(client1.fsBasePath / "s");

    // Get our hands on the remote test root.
    Node* r = client0.gettestbasenode();
    ASSERT_NE(r, nullptr);

    // Populate the remote test root.
    {
        auto sPath = client1.fsBasePath / "s";

        ASSERT_TRUE(client0.uploadFolderTree(sPath, r));
        ASSERT_TRUE(client0.uploadFilesInTree(sPath, r));
    }

    NodeHandle sh;

    // Get our hands on the remote sync root.
    {
        Node* s = client0.drillchildnodebyname(r, "s");
        ASSERT_NE(s, nullptr);

        sh = s->nodeHandle();
    }

    // Share the test root with client 1.
    ASSERT_TRUE(client0.share(*r, getenv("MEGA_EMAIL_AUX"), FULL));
    ASSERT_TRUE(client1.waitFor(SyncRemoteNodePresent(*r), std::chrono::seconds(90)));

    // Share the sync root with client 2.
    ASSERT_TRUE(client0.share(sh, getenv("MEGA_EMAIL_AUX2"), FULL));
    ASSERT_TRUE(client2.waitFor(SyncRemoteNodePresent(sh), std::chrono::seconds(90)));

    // Add and start a new sync.
    auto id = UNDEF;

    // Add the sync.
    id = client1.setupSync_mainthread("s", sh, false, false);
    ASSERT_NE(id, UNDEF);

    // Wait for the initial sync to complete.
    waitonsyncs(DEFAULTWAIT, &client1);

    // Make sure the clients all agree with what's in the cloud.
    ASSERT_TRUE(client1.confirmModel_mainthread(model.root.get(), id));
    ASSERT_TRUE(client2.waitFor(SyncRemoteMatch(sh, model.root.get()), DEFAULTWAIT));

    // Log out the sharing client so that it doesn't maintain keys.
    ASSERT_TRUE(client0.logout(false));

    // Make a couple changes to client1's sync via client2.
    {
        // Nodes from client2's perspective.
        auto* xs = client2.client.nodeByHandle(sh);
        ASSERT_NE(xs, nullptr);

        auto* xt = client2.client.childnodebyname(xs, "t");
        ASSERT_NE(xt, nullptr);

        auto* xu = client2.client.childnodebyname(xs, "u");
        ASSERT_NE(xu, nullptr);

        auto* xv = client2.client.childnodebyname(xs, "v");
        ASSERT_NE(xv, nullptr);

        // Create a new directory w under s.
        {
            vector<NewNode> node(1);

            client1.received_node_actionpackets = false;

            model.addfolder("w");

            client2.client.putnodes_prepareOneFolder(&node[0], "w", false);
            ASSERT_TRUE(client2.putnodes(xs->nodeHandle(), NoVersioning, std::move(node)));
            ASSERT_TRUE(client1.waitForNodesUpdated(30));
        }

        // Get our hands on w from client 2's perspective.
        auto* xw = client2.client.childnodebyname(xs, "w");
        ASSERT_NE(xw, nullptr);

        // Be certain that client 1 can see w.
        ASSERT_TRUE(client1.waitFor(SyncRemoteNodePresent(*xw), DEFAULTWAIT));

        // Let the engine try and process the change.
        waitonsyncs(DEFAULTWAIT, &client1);

        // Move t, u and v under w.
        client1.received_node_actionpackets = false;

        model.movenode("t", "w");

        ASSERT_TRUE(client2.movenode(xt->nodehandle, xw->nodehandle));
        ASSERT_TRUE(client1.waitForNodesUpdated(50));

        client1.received_node_actionpackets = false;

        model.movenode("u", "w");

        ASSERT_TRUE(client2.movenode(xu->nodehandle, xw->nodehandle));
        ASSERT_TRUE(client1.waitForNodesUpdated(50));

        client1.received_node_actionpackets = false;

        model.movenode("v", "w");

        ASSERT_TRUE(client2.movenode(xv->nodehandle, xw->nodehandle));
        ASSERT_TRUE(client1.waitForNodesUpdated(50));
    }

    // Wait for client 1 to stall (due to undecryptable nodes.)
    //ASSERT_TRUE(client1.waitFor(SyncStallState(true), DEFAULTWAIT));

    // Temporarily log out client 1.
    //
    // Undecrpytable nodes won't be serialized.
    string session;

    client1.client.dumpsession(session);
    client1.localLogout();

    // Hook resume callback.
    promise<void> notify;

    //client1.mOnSyncStateConfig = [&](const SyncConfig& config) {
    //    if (config.mRunState != SyncRunState::Run)
    //        return;

    //    notify.set_value();
    //    client1.mOnSyncStateConfig = nullptr;
    //};

    client1.onAutoResumeResult = [&](const SyncConfig&) {
        notify.set_value();
        client1.onAutoResumeResult = nullptr;
    };

    // Log the client back in.
    ASSERT_TRUE(client1.login_fetchnodes(session));

    // Wait for the sync to resume.
    ASSERT_NE(notify.get_future().wait_for(DEFAULTWAIT), future_status::timeout);

    // Give the sync some time to process changes.
    waitonsyncs(DEFAULTWAIT, &client1);

    // Make sure the client hasn't stalled.
    //ASSERT_FALSE(client1.client.syncs.syncStallDetected());

    // client 1 should've wipedd everything.
    //
    // This is the behavior we're going to want to fix.
    model.movetosynctrash("w", "");
    model.ensureLocalDebrisTmpLock("");

    ASSERT_TRUE(client1.confirmModel_mainthread(model.root.get(), id, true, StandardClient::CONFIRM_LOCALFS));
}

#endif

