/**
 * @file sqlite.h
 * @brief SQLite DB access layer
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

#ifdef USE_SQLITE
#ifndef DBACCESS_CLASS
#define DBACCESS_CLASS SqliteDbAccess

#include <sqlite3.h>

namespace mega {
class MEGA_API SqliteDbAccess : public DbAccess
{
    string dbpath;

public:
    DbTable* open(PrnGen &rng, FileSystemAccess*, string*, bool recycleLegacyDB, bool checkAlwaysTransacted) override;

    SqliteDbAccess(string* = NULL);
    ~SqliteDbAccess();
};

class MEGA_API SqliteDbTable : public DbTable
{
    sqlite3* db = nullptr;
    sqlite3_stmt* pStmt;
    string dbfile;
    FileSystemAccess *fsaccess;

public:
    void rewind();
    bool next(uint32_t*, string*);
    bool get(uint32_t, string*);
    bool getNode(handle nodehandle, NodeSerialized& nodeSerialized) override;
    bool getNodes(std::vector<NodeSerialized>& nodes) override;
    bool getNodesByFingerprint(const FileFingerprint& fingerprint, std::map<mega::handle, NodeSerialized> &nodes) override;
    bool getNodesByOrigFingerprint(const std::string& fingerprint, std::map<mega::handle, NodeSerialized>& nodes) override;
    bool getNodeByFingerprint(const FileFingerprint& fingerprint, NodeSerialized &node) override;
    bool getNodesWithoutParent(std::vector<NodeSerialized>& nodes) override;
    bool getNodesWithShares(std::vector<NodeSerialized>& nodes, shares_t shareType) override;
    bool getChildrenFromNode(handle node, std::map<handle, NodeSerialized>& nodes) override;
    bool getChildrenHandlesFromNode(mega::handle, std::vector<handle>&) override;
    bool getNodesByName(const std::string& name, std::map<mega::handle, NodeSerialized>& nodes) override;
    uint32_t getNumberOfChildrenFromNode(handle node) override;
    NodeCounter getNodeCounter(handle node) override;
    bool isNodesOnDemandDb() override;
    bool isAncestor(handle node, handle ancestor) override;
    handle getFirstAncestor(handle node) override;
    bool isNodeInDB(handle node) override;
    bool put(uint32_t, char*, unsigned);
    bool put(Node* node) override;
    bool del(uint32_t) override;
    bool del(handle nodehandle) override;
    bool removeNodes() override;
    void truncate() override;
    void begin() override;
    void commit() override;
    void abort() override;
    void remove() override;
    std::string getVar(const std::string& name) override;
    bool setVar(const std::string& name, const std::string& value) override;

    SqliteDbTable(PrnGen &rng, sqlite3*, FileSystemAccess *fs, string *filepath, bool checkAlwaysTransacted);
    ~SqliteDbTable();
};
} // namespace

#endif
#endif
