/*******************************************************************************************
 
			Hash(BEGIN(Satoshi[2010]), END(Sunny[2012])) == Videlicet[2014] ++
   
 [Learn and Create] Viz. http://www.opensource.org/licenses/mit-license.php
  
*******************************************************************************************/

#include "db.h"
#include "../util/util.h"
#include "../core/core.h"
#include <boost/version.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#ifndef WIN32
#include "sys/stat.h"
#endif

using namespace std;
using namespace boost;


namespace Wallet
{
	unsigned int nWalletDBUpdated;

	CCriticalSection cs_db;
	static bool fDbEnvInit = false;
	bool fDetachDB = false;
	DbEnv dbenv(0);
	map<string, int> mapFileUseCount;
	static map<string, Db*> mapDb;

	static void EnvShutdown()
	{
		if (!fDbEnvInit)
			return;

		fDbEnvInit = false;
		try
		{
			dbenv.close(0);
		}
		catch (const DbException& e)
		{
			printf("EnvShutdown exception: %s (%d)\n", e.what(), e.get_errno());
		}
		DbEnv(0).remove(GetDataDir().string().c_str(), 0);
	}

	class CDBInit
	{
	public:
		CDBInit()
		{
		}
		~CDBInit()
		{
			EnvShutdown();
		}
	}
	instance_of_cdbinit;


	CDB::CDB(const char *pszFile, const char* pszMode) : pdb(NULL)
	{
		int ret;
		if (pszFile == NULL)
			return;

		fReadOnly = (!strchr(pszMode, '+') && !strchr(pszMode, 'w'));
		bool fCreate = strchr(pszMode, 'c');
		unsigned int nFlags = DB_THREAD;
		if (fCreate)
			nFlags |= DB_CREATE;

		{
			LOCK(cs_db);
			if (!fDbEnvInit)
			{
				if (fShutdown)
					return;
				filesystem::path pathDataDir = GetDataDir();
				filesystem::path pathLogDir = pathDataDir / "database";
				filesystem::create_directory(pathLogDir);
				filesystem::path pathErrorFile = pathDataDir / "db.log";
				printf("dbenv.open LogDir=%s ErrorFile=%s\n", pathLogDir.string().c_str(), pathErrorFile.string().c_str());

				int nDbCache = GetArg("-dbcache", 25);
				dbenv.set_lg_dir(pathLogDir.string().c_str());
				dbenv.set_cachesize(nDbCache / 1024, (nDbCache % 1024)*1048576, 1);
				dbenv.set_lg_bsize(1048576);
				dbenv.set_lg_max(10485760);
				dbenv.set_lk_max_locks(10000);
				dbenv.set_lk_max_objects(10000);
				dbenv.set_errfile(fopen(pathErrorFile.string().c_str(), "a")); /// debug
				dbenv.set_flags(DB_TXN_WRITE_NOSYNC, 1);
				dbenv.set_flags(DB_AUTO_COMMIT, 1);
				dbenv.log_set_config(DB_LOG_AUTO_REMOVE, 1);
				ret = dbenv.open(pathDataDir.string().c_str(),
								 DB_CREATE     |
								 DB_INIT_LOCK  |
								 DB_INIT_LOG   |
								 DB_INIT_MPOOL |
								 DB_INIT_TXN   |
								 DB_THREAD     |
								 DB_RECOVER,
								 S_IRUSR | S_IWUSR);
				if (ret > 0)
					throw runtime_error(strprintf("CDB() : error %d opening database environment", ret));
				fDbEnvInit = true;
			}

			strFile = pszFile;
			++mapFileUseCount[strFile];
			pdb = mapDb[strFile];
			if (pdb == NULL)
			{
				pdb = new Db(&dbenv, 0);

				ret = pdb->open(NULL,      // Txn pointer
								pszFile,   // Filename
								"main",    // Logical db name
								DB_BTREE,  // Database type
								nFlags,    // Flags
								0);

				if (ret > 0)
				{
					delete pdb;
					pdb = NULL;
					{
						 LOCK(cs_db);
						--mapFileUseCount[strFile];
					}
					strFile = "";
					throw runtime_error(strprintf("CDB() : can't open database file %s, error %d", pszFile, ret));
				}

				if (fCreate && !Exists(string("version")))
				{
					bool fTmp = fReadOnly;
					fReadOnly = false;
					WriteVersion(DATABASE_VERSION);
					fReadOnly = fTmp;
				}

				mapDb[strFile] = pdb;
			}
		}
	}

	void CDB::Close()
	{
		if (!pdb)
			return;
		if (!vTxn.empty())
			vTxn.front()->abort();
		vTxn.clear();
		pdb = NULL;

		// Flush database activity from memory pool to disk log
		unsigned int nMinutes = 0;
		if (fReadOnly)
			nMinutes = 1;
		if (strFile == "addr.dat")
			nMinutes = 2;
		if (strFile == "blkindex.dat")
			nMinutes = 2;
		if (strFile == "blkindex.dat" && Core::IsInitialBlockDownload())
			nMinutes = 5;

		dbenv.txn_checkpoint(nMinutes ? GetArg("-dblogsize", 100)*1024 : 0, nMinutes, 0);

		{
			LOCK(cs_db);
			--mapFileUseCount[strFile];
		}
	}

	void CloseDb(const string& strFile)
	{
		{
			LOCK(cs_db);
			if (mapDb[strFile] != NULL)
			{
				// Close the database handle
				Db* pdb = mapDb[strFile];
				pdb->close(0);
				delete pdb;
				mapDb[strFile] = NULL;
			}
		}
	}

	bool CDB::Rewrite(const string& strFile, const char* pszSkip)
	{
		while (!fShutdown)
		{
			{
				LOCK(cs_db);
				if (!mapFileUseCount.count(strFile) || mapFileUseCount[strFile] == 0)
				{
					// Flush log data to the dat file
					CloseDb(strFile);
					dbenv.txn_checkpoint(0, 0, 0);
					dbenv.lsn_reset(strFile.c_str(), 0);
					mapFileUseCount.erase(strFile);

					bool fSuccess = true;
					printf("Rewriting %s...\n", strFile.c_str());
					string strFileRes = strFile + ".rewrite";
					{ // surround usage of db with extra {}
						CDB db(strFile.c_str(), "r");
						Db* pdbCopy = new Db(&dbenv, 0);
		
						int ret = pdbCopy->open(NULL,                 // Txn pointer
												strFileRes.c_str(),   // Filename
												"main",    // Logical db name
												DB_BTREE,  // Database type
												DB_CREATE,    // Flags
												0);
						if (ret > 0)
						{
							printf("Cannot create database file %s\n", strFileRes.c_str());
							fSuccess = false;
						}
		
						Dbc* pcursor = db.GetCursor();
						if (pcursor)
							while (fSuccess)
							{
								CDataStream ssKey(SER_DISK, DATABASE_VERSION);
								CDataStream ssValue(SER_DISK, DATABASE_VERSION);
								int ret = db.ReadAtCursor(pcursor, ssKey, ssValue, DB_NEXT);
								if (ret == DB_NOTFOUND)
								{
									pcursor->close();
									break;
								}
								else if (ret != 0)
								{
									pcursor->close();
									fSuccess = false;
									break;
								}
								if (pszSkip &&
									strncmp(&ssKey[0], pszSkip, std::min(ssKey.size(), strlen(pszSkip))) == 0)
									continue;
								if (strncmp(&ssKey[0], "\x07version", 8) == 0)
								{
									// Update version:
									ssValue.clear();
									ssValue << DATABASE_VERSION;
								}
								Dbt datKey(&ssKey[0], ssKey.size());
								Dbt datValue(&ssValue[0], ssValue.size());
								int ret2 = pdbCopy->put(NULL, &datKey, &datValue, DB_NOOVERWRITE);
								if (ret2 > 0)
									fSuccess = false;
							}
						if (fSuccess)
						{
							db.Close();
							CloseDb(strFile);
							if (pdbCopy->close(0))
								fSuccess = false;
							delete pdbCopy;
						}
					}
					if (fSuccess)
					{
						Db dbA(&dbenv, 0);
						if (dbA.remove(strFile.c_str(), NULL, 0))
							fSuccess = false;
						Db dbB(&dbenv, 0);
						if (dbB.rename(strFileRes.c_str(), NULL, strFile.c_str(), 0))
							fSuccess = false;
					}
					if (!fSuccess)
						printf("Rewriting of %s FAILED!\n", strFileRes.c_str());
					return fSuccess;
				}
			}
			Sleep(100);
		}
		return false;
	}


	void DBFlush(bool fShutdown)
	{
		// Flush log data to the actual data file
		//  on all files that are not in use
		printf("DBFlush(%s)%s\n", fShutdown ? "true" : "false", fDbEnvInit ? "" : " db not started");
		if (!fDbEnvInit)
			return;
		{
			LOCK(cs_db);
			map<string, int>::iterator mi = mapFileUseCount.begin();
			while (mi != mapFileUseCount.end())
			{
				string strFile = (*mi).first;
				int nRefCount = (*mi).second;
				printf("%s refcount=%d\n", strFile.c_str(), nRefCount);
				if (nRefCount == 0)
				{
					// Move log data to the dat file
					CloseDb(strFile);
					printf("%s checkpoint\n", strFile.c_str());
					dbenv.txn_checkpoint(0, 0, 0);
					if ((strFile != "blkindex.dat" && strFile != "addr.dat") || fDetachDB) {
						printf("%s detach\n", strFile.c_str());
						dbenv.lsn_reset(strFile.c_str(), 0);
					}
					printf("%s closed\n", strFile.c_str());
					mapFileUseCount.erase(mi++);
				}
				else
					mi++;
			}
			if (fShutdown)
			{
				char** listp;
				if (mapFileUseCount.empty())
				{
					dbenv.log_archive(&listp, DB_ARCH_REMOVE);
					EnvShutdown();
				}
			}
		}
	}



	bool CTimeDB::ReadTimeData(int& nOffset)
	{
		if(!Read(0, nOffset))
			return false;
			
		return true;
	}
	
	bool CTimeDB::WriteTimeData(int nOffset)
	{
		if(!Write(0, nOffset))
			return false;
	}
	


	//
	// CTxDB
	//

	bool CTxDB::ReadTxIndex(uint512 hash, Core::CTxIndex& txindex)
	{
		assert(!Net::fClient);
		txindex.SetNull();
		return Read(make_pair(string("tx"), hash), txindex);
	}

	bool CTxDB::UpdateTxIndex(uint512 hash, const Core::CTxIndex& txindex)
	{
		assert(!Net::fClient);
		return Write(make_pair(string("tx"), hash), txindex);
	}

	bool CTxDB::AddTxIndex(const Core::CTransaction& tx, const Core::CDiskTxPos& pos, int nHeight)
	{
		assert(!Net::fClient);

		// Add to tx index
		uint512 hash = tx.GetHash();
		Core::CTxIndex txindex(pos, tx.vout.size());
		return Write(make_pair(string("tx"), hash), txindex);
	}

	bool CTxDB::EraseTxIndex(const Core::CTransaction& tx)
	{
		assert(!Net::fClient);
		uint512 hash = tx.GetHash();

		return Erase(make_pair(string("tx"), hash));
	}

	bool CTxDB::ContainsTx(uint512 hash)
	{
		assert(!Net::fClient);
		return Exists(make_pair(string("tx"), hash));
	}

	bool CTxDB::ReadOwnerTxes(uint512 hash, int nMinHeight, vector<Core::CTransaction>& vtx)
	{
		assert(!Net::fClient);
		vtx.clear();

		// Get cursor
		Dbc* pcursor = GetCursor();
		if (!pcursor)
			return false;

		unsigned int fFlags = DB_SET_RANGE;
		loop
		{
			// Read next record
			CDataStream ssKey(SER_DISK, DATABASE_VERSION);
			if (fFlags == DB_SET_RANGE)
				ssKey << string("owner") << hash << Core::CDiskTxPos(0, 0, 0);
			CDataStream ssValue(SER_DISK, DATABASE_VERSION);
			int ret = ReadAtCursor(pcursor, ssKey, ssValue, fFlags);
			fFlags = DB_NEXT;
			if (ret == DB_NOTFOUND)
				break;
			else if (ret != 0)
			{
				pcursor->close();
				return false;
			}

			// Unserialize
			string strType;
			uint512 hashItem;
			Core::CDiskTxPos pos;
			int nItemHeight;

			try {
				ssKey >> strType >> hashItem >> pos;
				ssValue >> nItemHeight;
			}
			catch (std::exception &e) {
				return error("%s() : deserialize error", __PRETTY_FUNCTION__);
			}

			// Read transaction
			if (strType != "owner" || hashItem != hash)
				break;
			if (nItemHeight >= nMinHeight)
			{
				vtx.resize(vtx.size()+1);
				if (!vtx.back().ReadFromDisk(pos))
				{
					pcursor->close();
					return false;
				}
			}
		}

		pcursor->close();
		return true;
	}

	bool CTxDB::ReadDiskTx(uint512 hash, Core::CTransaction& tx, Core::CTxIndex& txindex)
	{
		assert(!Net::fClient);
		tx.SetNull();
		if (!ReadTxIndex(hash, txindex))
			return false;
		return (tx.ReadFromDisk(txindex.pos));
	}

	bool CTxDB::ReadDiskTx(uint512 hash, Core::CTransaction& tx)
	{
		Core::CTxIndex txindex;
		return ReadDiskTx(hash, tx, txindex);
	}

	bool CTxDB::ReadDiskTx(Core::COutPoint outpoint, Core::CTransaction& tx, Core::CTxIndex& txindex)
	{
		return ReadDiskTx(outpoint.hash, tx, txindex);
	}

	bool CTxDB::ReadDiskTx(Core::COutPoint outpoint, Core::CTransaction& tx)
	{
		Core::CTxIndex txindex;
		return ReadDiskTx(outpoint.hash, tx, txindex);
	}

	bool CTxDB::WriteBlockIndex(const Core::CDiskBlockIndex& blockindex)
	{
		return Write(make_pair(string("blockindex"), blockindex.GetBlockHash()), blockindex);
	}

	bool CTxDB::EraseBlockIndex(uint1024 hash)
	{
		return Erase(make_pair(string("blockindex"), hash));
	}

	bool CTxDB::ReadHashBestChain(uint1024& hashBestChain)
	{
		return Read(string("hashBestChain"), hashBestChain);
	}

	bool CTxDB::WriteHashBestChain(uint1024 hashBestChain)
	{
		return Write(string("hashBestChain"), hashBestChain);
	}

	bool CTxDB::ReadBestInvalidTrust(CBigNum& bnBestInvalidTrust)
	{
		return Read(string("bnBestInvalidTrust"), bnBestInvalidTrust);
	}

	bool CTxDB::WriteBestInvalidTrust(CBigNum bnBestInvalidTrust)
	{
		return Write(string("bnBestInvalidTrust"), bnBestInvalidTrust);
	}

	bool CTxDB::ReadCheckpointPubKey(string& strPubKey)
	{
		return Read(string("strCheckpointPubKey"), strPubKey);
	}

	bool CTxDB::WriteCheckpointPubKey(const string& strPubKey)
	{
		return Write(string("strCheckpointPubKey"), strPubKey);
	}

	Core::CBlockIndex static * InsertBlockIndex(uint1024 hash)
	{
		if (hash == 0)
			return NULL;

		// Return existing
		map<uint1024, Core::CBlockIndex*>::iterator mi = Core::mapBlockIndex.find(hash);
		if (mi != Core::mapBlockIndex.end())
			return (*mi).second;

		// Create new
		Core::CBlockIndex* pindexNew = new Core::CBlockIndex();
		if (!pindexNew)
			throw runtime_error("LoadBlockIndex() : new Core::CBlockIndex failed");
		mi = Core::mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
		pindexNew->phashBlock = &((*mi).first);

		return pindexNew;
	}

	bool CTxDB::LoadBlockIndex()
	{

		Dbc* pcursor = GetCursor();
		if (!pcursor)
			return false;


		unsigned int fFlags = DB_SET_RANGE;
		loop
		{
			// Read next record
			CDataStream ssKey(SER_DISK, DATABASE_VERSION);
			if (fFlags == DB_SET_RANGE)
				ssKey << make_pair(string("blockindex"), uint1024(0));
			CDataStream ssValue(SER_DISK, DATABASE_VERSION);
			int ret = ReadAtCursor(pcursor, ssKey, ssValue, fFlags);
			fFlags = DB_NEXT;
			if (ret == DB_NOTFOUND)
				break;
			else if (ret != 0)
				return false;

			// Unserialize

			try {
				string strType;
				ssKey >> strType;
				if (strType == "blockindex" && !fRequestShutdown)
				{
					Core::CDiskBlockIndex diskindex;
					ssValue >> diskindex;

					// Construct block index object
					Core::CBlockIndex* pindexNew = InsertBlockIndex(diskindex.GetBlockHash());
					pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
					pindexNew->pnext          = InsertBlockIndex(diskindex.hashNext);
					pindexNew->nFile          = diskindex.nFile;
					pindexNew->nBlockPos      = diskindex.nBlockPos;
					pindexNew->nMint          = diskindex.nMint;
					pindexNew->nMoneySupply   = diskindex.nMoneySupply;
					pindexNew->nFlags         = diskindex.nFlags;
					pindexNew->nVersion       = diskindex.nVersion;
					pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
					pindexNew->nChannel       = diskindex.nChannel;
					pindexNew->nHeight        = diskindex.nHeight;
					pindexNew->nBits          = diskindex.nBits;
					pindexNew->nNonce         = diskindex.nNonce;
					pindexNew->nTime          = diskindex.nTime;

					// Watch for genesis block
					if (Core::pindexGenesisBlock == NULL && diskindex.GetBlockHash() == Core::hashGenesisBlock)
						Core::pindexGenesisBlock = pindexNew;

					if (!pindexNew->CheckIndex())
						return error("LoadBlockIndex() : CheckIndex failed at %d", pindexNew->nHeight);

				}
				else
				{
					break; // if shutdown requested or finished loading block index
				}
			}    // try
			catch (std::exception &e) {
				return error("%s() : deserialize error", __PRETTY_FUNCTION__);
			}
		}
		pcursor->close();

		if (fRequestShutdown)
			return true;

		/** Calculate the Chain Trust. **/
		if (!ReadHashBestChain(Core::hashBestChain))
		{
			if (Core::pindexGenesisBlock == NULL)
				return true;
			return error("CTxDB::LoadBlockIndex() : hashBestChain not loaded");
		}
		
		if (!Core::mapBlockIndex.count(Core::hashBestChain))
			return error("CTxDB::LoadBlockIndex() : hashBestChain not found in the block index");
		Core::pindexBest = Core::mapBlockIndex[Core::hashBestChain];
		Core::nBestHeight = Core::pindexBest->nHeight;
		Core::bnBestChainTrust = Core::pindexBest->bnChainTrust;
		
		Core::CBlockIndex* pindex = Core::pindexGenesisBlock;
		
		loop
		{
		
			/** Get the Coinbase Transaction Rewards. **/
			if(pindex->pprev)
			{
				Core::CBlock block;
				if (!block.ReadFromDisk(pindex))
					break;
					
				if(pindex->IsProofOfWork())
				{
					unsigned int nSize = block.vtx[0].vout.size();
					pindex->nCoinbaseRewards[0] = 0;
					for(int nIndex = 0; nIndex < nSize - 2; nIndex++)
						pindex->nCoinbaseRewards[0] += block.vtx[0].vout[nIndex].nValue;
							
					pindex->nCoinbaseRewards[1] = block.vtx[0].vout[nSize - 2].nValue;
					pindex->nCoinbaseRewards[2] = block.vtx[0].vout[nSize - 1].nValue;
				}
				
				/** Add Transaction to Current Trust Keys **/
				else if(pindex->IsProofOfStake() && !Core::cTrustPool.Accept(block, true))
				{
					pindex->nCoinbaseRewards[0] = 0;
					pindex->nCoinbaseRewards[1] = 0;
					pindex->nCoinbaseRewards[2] = 0;
				
					return error("CTxDB::LoadBlockIndex() : Failed To Accept Trust Key Block.");
				}
				
				/** Grab the transactions for the block and set the address balances. **/
				for(int nTx = 0; nTx < block.vtx.size(); nTx++)
				{
					for(int nOut = 0; nOut < block.vtx[nTx].vout.size(); nOut++)
					{	
						NexusAddress cAddress;
						if(!ExtractAddress(block.vtx[nTx].vout[nOut].scriptPubKey, cAddress))
							continue;
							
						Core::mapAddressTransactions[cAddress.GetHash256()] += block.vtx[nTx].vout[nOut].nValue;
							
						//printf("%s Credited %f Nexus | Balance : %f Nexus\n", cAddress.ToString().c_str(), (double)block.vtx[nTx].vout[nOut].nValue / COIN, (double)Core::mapAddressTransactions[cAddress.GetHash256()] / COIN);
					}
					
					if(!block.vtx[nTx].IsCoinBase())
					{
						BOOST_FOREACH(const Core::CTxIn& txin, block.vtx[nTx].vin)
						{
							Core::CTransaction tx;
							Core::CTxIndex txind;
							
							if(!ReadTxIndex(txin.prevout.hash, txind))
								continue;
								
							if(!tx.ReadFromDisk(txind.pos))
								continue;
							
							NexusAddress cAddress;
							if(!ExtractAddress(tx.vout[txin.prevout.n].scriptPubKey, cAddress))
								continue;
							
							Core::mapAddressTransactions[cAddress.GetHash256()] -= tx.vout[txin.prevout.n].nValue;
							
							//printf("%s Debited %f Nexus | Balance : %f Nexus\n", cAddress.ToString().c_str(), (double)tx.vout[txin.prevout.n].nValue / COIN, (double)Core::mapAddressTransactions[cAddress.GetHash256()] / COIN);
						}
					}
				}
				
			}
			else
			{
				
				pindex->nCoinbaseRewards[0] = 0;
				pindex->nCoinbaseRewards[1] = 0;
				pindex->nCoinbaseRewards[2] = 0;
			}
				
				
			/** Calculate the Chain Trust. **/
			pindex->bnChainTrust = (pindex->pprev ? pindex->pprev->bnChainTrust : 0) + pindex->GetBlockTrust();
			
			
			/** Release the Nexus Rewards into the Blockchain. **/
			const Core::CBlockIndex* pindexPrev = GetLastChannelIndex(pindex->pprev, pindex->GetChannel());
			pindex->nChannelHeight = (pindexPrev ? pindexPrev->nChannelHeight : 0) + 1;
			
			
			/** Compute the Released Reserves. **/
			for(int nType = 0; nType < 3; nType++)
			{
				if(pindex->IsProofOfWork() && pindexPrev)
				{
					int64 nReserve = GetReleasedReserve(pindex, pindex->GetChannel(), nType);
					pindex->nReleasedReserve[nType] = pindexPrev->nReleasedReserve[nType] + nReserve - pindex->nCoinbaseRewards[nType];
				}
				else
					pindex->nReleasedReserve[nType] = 0;

			}
				
				
			/** Add the Pending Checkpoint into the Blockchain. **/
			if(!pindex->pprev || Core::HardenCheckpoint(pindex, true))
				pindex->PendingCheckpoint = make_pair(pindex->nHeight, pindex->GetBlockHash());
			else
				pindex->PendingCheckpoint = pindex->pprev->PendingCheckpoint;
	
			/** Exit the Loop on the Best Block. **/
			if(pindex->GetBlockHash() == Core::hashBestChain)
			{
				printf("LoadBlockIndex(): hashBestChain=%s  height=%d  trust=%s\n", Core::hashBestChain.ToString().substr(0,20).c_str(), Core::nBestHeight, Core::bnBestChainTrust.ToString().c_str());
				break;
			}
			
			
			pindex = pindex->pnext;
		}

		// Load bnBestInvalidTrust, OK if it doesn't exist
		ReadBestInvalidTrust(Core::bnBestInvalidTrust);
		
		

		/** Verify the Blocks in the Best Chain To Last Checkpoint. **/
		int nCheckLevel = GetArg("-checklevel", 1);
		
		int nCheckDepth = GetArg( "-checkblocks", 100);
		//if (nCheckDepth == 0)
		//	nCheckDepth = 1000000000;
			
		if (nCheckDepth > Core::nBestHeight)
			nCheckDepth = Core::nBestHeight;
		printf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
		Core::CBlockIndex* pindexFork = NULL;
		
		
		map<pair<unsigned int, unsigned int>, Core::CBlockIndex*> mapBlockPos;
		for (Core::CBlockIndex* pindex = Core::pindexBest; pindex && pindex->pprev && nCheckDepth > 0; pindex = pindex->pprev)
		{
			if (pindex->nHeight < Core::nBestHeight - nCheckDepth)
				break;
				
			Core::CBlock block;
			if (!block.ReadFromDisk(pindex))
				return error("LoadBlockIndex() : block.ReadFromDisk failed");
				
			if (nCheckLevel > 0 && !block.CheckBlock())
			{
				printf("LoadBlockIndex() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
				pindexFork = pindex->pprev;
			}
			
			// check level 2: verify transaction index validity
			if (nCheckLevel>1)
			{
				pair<unsigned int, unsigned int> pos = make_pair(pindex->nFile, pindex->nBlockPos);
				mapBlockPos[pos] = pindex;
				BOOST_FOREACH(const Core::CTransaction &tx, block.vtx)
				{
					uint512 hashTx = tx.GetHash();
					Core::CTxIndex txindex;
					if (ReadTxIndex(hashTx, txindex))
					{
						// check level 3: checker transaction hashes
						if (nCheckLevel>2 || pindex->nFile != txindex.pos.nFile || pindex->nBlockPos != txindex.pos.nBlockPos)
						{
							// either an error or a duplicate transaction
							Core::CTransaction txFound;
							if (!txFound.ReadFromDisk(txindex.pos))
							{
								printf("LoadBlockIndex() : *** cannot read mislocated transaction %s\n", hashTx.ToString().c_str());
								pindexFork = pindex->pprev;
							}
							else
								if (txFound.GetHash() != hashTx) // not a duplicate tx
								{
									printf("LoadBlockIndex(): *** invalid tx position for %s\n", hashTx.ToString().c_str());
									pindexFork = pindex->pprev;
								}
						}
						// check level 4: check whether spent txouts were spent within the main chain
						unsigned int nOutput = 0;
						if (nCheckLevel>3)
						{
							BOOST_FOREACH(const Core::CDiskTxPos &txpos, txindex.vSpent)
							{
								if (!txpos.IsNull())
								{
									pair<unsigned int, unsigned int> posFind = make_pair(txpos.nFile, txpos.nBlockPos);
									if (!mapBlockPos.count(posFind))
									{
										printf("LoadBlockIndex(): *** found bad spend at %d, hashBlock=%s, hashTx=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str(), hashTx.ToString().c_str());
										pindexFork = pindex->pprev;
									}
									// check level 6: check whether spent txouts were spent by a valid transaction that consume them
									if (nCheckLevel>5)
									{
										Core::CTransaction txSpend;
										if (!txSpend.ReadFromDisk(txpos))
										{
											printf("LoadBlockIndex(): *** cannot read spending transaction of %s:%i from disk\n", hashTx.ToString().c_str(), nOutput);
											pindexFork = pindex->pprev;
										}
										else if (!txSpend.CheckTransaction())
										{
											printf("LoadBlockIndex(): *** spending transaction of %s:%i is invalid\n", hashTx.ToString().c_str(), nOutput);
											pindexFork = pindex->pprev;
										}
										else
										{
											bool fFound = false;
											BOOST_FOREACH(const Core::CTxIn &txin, txSpend.vin)
												if (txin.prevout.hash == hashTx && txin.prevout.n == nOutput)
													fFound = true;
											if (!fFound)
											{
												printf("LoadBlockIndex(): *** spending transaction of %s:%i does not spend it\n", hashTx.ToString().c_str(), nOutput);
												pindexFork = pindex->pprev;
											}
										}
									}
								}
								nOutput++;
							}
						}
					}
					// check level 5: check whether all prevouts are marked spent
					if (nCheckLevel>4)
					{
						 BOOST_FOREACH(const Core::CTxIn &txin, tx.vin)
						 {
							  Core::CTxIndex txindex;
							  if (ReadTxIndex(txin.prevout.hash, txindex))
								  if (txindex.vSpent.size()-1 < txin.prevout.n || txindex.vSpent[txin.prevout.n].IsNull())
								  {
									  printf("LoadBlockIndex(): *** found unspent prevout %s:%i in %s\n", txin.prevout.hash.ToString().c_str(), txin.prevout.n, hashTx.ToString().c_str());
									  pindexFork = pindex->pprev;
								  }
						 }
					}
				}
			}
		}
		if (pindexFork)
		{
			// Reorg back to the fork
			printf("LoadBlockIndex() : *** moving best chain pointer back to block %d\n", pindexFork->nHeight);
			Core::CBlock block;
			if (!block.ReadFromDisk(pindexFork))
				return error("LoadBlockIndex() : block.ReadFromDisk failed");
			CTxDB txdb;
			block.SetBestChain(txdb, pindexFork);
		}

		return true;
	}





	//
	// CAddrDB
	//

	bool CAddrDB::WriteAddrman(const Net::CAddrMan& addrman)
	{
		return Write(string("addrman"), addrman);
	}

	bool CAddrDB::LoadAddresses()
	{
		if (Read(string("addrman"), Net::addrman))
		{
			printf("Loaded %i addresses\n", Net::addrman.size());
			return true;
		}
		
		// Read pre-0.6 addr records

		vector<Net::CAddress> vAddr;
		vector<vector<unsigned char> > vDelete;

		// Get cursor
		Dbc* pcursor = GetCursor();
		if (!pcursor)
			return false;

		loop
		{
			// Read next record
			CDataStream ssKey(SER_DISK, DATABASE_VERSION);
			CDataStream ssValue(SER_DISK, DATABASE_VERSION);
			int ret = ReadAtCursor(pcursor, ssKey, ssValue);
			if (ret == DB_NOTFOUND)
				break;
			else if (ret != 0)
				return false;

			// Unserialize
			string strType;
			ssKey >> strType;
			if (strType == "addr")
			{
				Net::CAddress addr;
				ssValue >> addr;
				vAddr.push_back(addr);
			}
		}
		pcursor->close();

		Net::addrman.Add(vAddr, Net::CNetAddr("0.0.0.0"));
		printf("Loaded %i addresses\n", Net::addrman.size());

		// Note: old records left; we ran into hangs-on-startup
		// bugs for some users who (we think) were running after
		// an unclean shutdown.

		return true;
	}

	bool LoadAddresses()
	{
		return CAddrDB("cr+").LoadAddresses();
	}
}


