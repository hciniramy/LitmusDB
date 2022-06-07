#include "manager.h"
#include "taurus_log.h"
#include "log.h"																				 
#include <iostream>
#include <fstream>
#include <sys/time.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <queue>
#include "helper.h"

#if LOG_ALGORITHM == LOG_TAURUS

volatile uint32_t TaurusLogManager::num_files_done = 0;
volatile uint64_t ** TaurusLogManager::num_txns_recovered = NULL;

TaurusLogManager::TaurusLogManager()
{
	num_txns_recovered = new uint64_t volatile * [g_thread_cnt];
	for (uint32_t i = 0; i < g_thread_cnt; i++) {
		num_txns_recovered[i] = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		*num_txns_recovered[i] = 0;
	}
	// initiate RLV
#if RECOVER_TAURUS_LOCKFREE
	recoverLV = (volatile uint64_t**) _mm_malloc(sizeof(uint64_t) * g_num_logger, ALIGN_SIZE); //new uint64_t volatile * [g_num_logger];
	for (uint32_t i = 0; i < g_num_logger; i++) {
		recoverLV[i] = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		*recoverLV[i] = 0;
	}
#else
	uint64_t num_worker = g_thread_cnt / g_num_logger;
	recoverLVSPSC = (recoverLV_t**) _mm_malloc(sizeof(recoverLV_t*) * g_num_logger, ALIGN_SIZE);
	recoverLVSPSC_min = (recoverLV_t*) _mm_malloc(sizeof(recoverLV_t) * g_num_logger, ALIGN_SIZE);
	for(uint32_t i=0; i < g_num_logger; i++)
	{
		recoverLVSPSC[i] = (recoverLV_t *) _mm_malloc(sizeof(recoverLV_t) * num_worker, ALIGN_SIZE);
		for(uint32_t j=0; j< num_worker; j++)
		{
			recoverLVSPSC[i][j] = (recoverLV_t) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
			*recoverLVSPSC[i][j] = 0;
		}
		recoverLVSPSC_min[i] = (recoverLV_t) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		*recoverLVSPSC_min[i] = 0;
	}
#endif
	endLV = (uint64_t**) _mm_malloc(sizeof(uint64_t*) * g_num_logger, ALIGN_SIZE);
	for(uint32_t i=0; i<g_num_logger; i++)
	{
		endLV[i] = (uint64_t*) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		//endLV[i][0] = 0;
	}
}

TaurusLogManager::~TaurusLogManager()
{
	for(uint32_t i = 0; i < g_num_logger; i++)
	{
		//delete _logger[i];
		_logger[i]->~LogManager();
		_mm_free(_logger[i]);
	}
	//delete _logger;
	//delete [] _logger;
	_mm_free(_logger);
}

void TaurusLogManager::init()
{
	_logger = (LogManager**) _mm_malloc(sizeof(LogManager*) * g_num_logger, ALIGN_SIZE); //new LogManager * [g_num_logger];
	char hostname[256];
	gethostname(hostname, 256);
	logPLVFlushed = (uint64_t**) _mm_malloc(sizeof(uint64_t*) * g_num_logger, 64);
	for(uint32_t i = 0; i < g_num_logger; i++) {
		_logger[i] = (LogManager *) _mm_malloc(sizeof(LogManager), ALIGN_SIZE); 
		new (_logger[i]) LogManager(i);
		// XXX
		//MALLOC_CONSTRUCTOR(LogManager, _logger[i]);
		string bench = "YCSB";
		if (WORKLOAD == TPCC)
			bench = "TPCC";
		string dir = LOG_DIR;
		if (strncmp(hostname, "istc3", 5) == 0) {
			if (i == 0)
				dir = "/f0/yuxia/";
			else if (i == 1)
				dir = "/f1/yuxia/";
			else if (i == 2)
				dir = "/f2/yuxia/";
			else if (i == 3)
				dir = "/data/yuxia/";
		}
		if (strncmp(hostname, "ip-", 3) == 0) {
			if (i == 0)
				dir = "/data0/";
			else if (i == 1)
				dir = "/data1/";
			else if (i == 2)
				dir = "/data2/";
			else if (i == 3)
				dir = "/data3/";
			else if (i == 4)
				dir = "/data4/";
			else if (i == 5)
				dir = "/data5/";
			else if (i == 6)
				dir = "/data6/";
			else if (i == 7)
				dir = "/data7/";
		} 
		//string dir = ".";
#if LOG_TYPE == LOG_DATA
		_logger[i]->init(dir + "/TD_log" + to_string(i) + "_" + to_string(g_num_logger) + "_" + bench + ".data");
#else
		_logger[i]->init(dir + "/TC_log" + to_string(i) + "_" + to_string(g_num_logger) + "_" + bench + ".data");
#endif
		logPLVFlushed[i] = (uint64_t*) _mm_malloc(sizeof(uint64_t), 64);
		logPLVFlushed[i][0] = 0;
	}
}

uint64_t
TaurusLogManager::tryFlush()
{
	uint32_t logger_id = GET_THD_ID % g_num_logger;
	#if COMPRESS_LSN_LOG
	uint64_t lt = _logger[logger_id]->_lsn[0];
	//cout << "called 1 ";
	if(g_psn_flush_freq > 0 && lt / g_psn_flush_freq != logPLVFlushed[logger_id][0])
	{
		//cout << "called 2 ";
		flushPSN();
		logPLVFlushed[logger_id][0] = lt / g_psn_flush_freq;
		INC_INT_STATS(int_psnflush, 1);
	}
	#endif
	return _logger[logger_id]->tryFlush();
	//return _logger[0]->tryFlush();
}

uint64_t TaurusLogManager::flushPSN()
{
	// TODO: flush psn
	// Format
	// checksum:4 | size:4 | PSN_1:8 | PSN_2:8 | ... | PSN_k:8
	uint32_t total_size = sizeof(uint32_t) * 2 + sizeof(uint64_t)*g_num_logger;
	char psn_entry[total_size];
	memcpy(psn_entry + sizeof(uint32_t), &total_size, sizeof(uint32_t));
	psn_entry[0] = 0x7f; // takes the checksum place;
	uint64_t * psn_t = (uint64_t*)(psn_entry + sizeof(uint32_t)*2);
	for(uint32_t i=0; i<g_num_logger; i++)
		psn_t[i] = _logger[i]->get_persistent_lsn();
	uint32_t logger_id = GET_THD_ID % g_num_logger;
	INC_INT_STATS(int_aux_bytes, total_size);
	//for(;;)
	//{
		uint64_t lsn = _logger[logger_id]->logTxn(psn_entry, total_size);
	//	if(lsn != (uint64_t)-1)
	//		break;
	//}
	if(lsn < UINT64_MAX)
	{
		for(uint32_t i=0; i<g_num_logger; i++)
		{
			uint64_t v = *glob_manager->lastPSN[i];
			while(v < psn_t[i] && !ATOM_CAS(*glob_manager->lastPSN[i], v, psn_t[i]))
			{
				PAUSE
				v = glob_manager->lastPSN[i][0];
			}
		}
	}
	//cout << lsn << " flush psn";
	return lsn;
}

uint64_t 
TaurusLogManager::serialLogTxn(char * log_entry, uint32_t entry_size, lsnType * lsn_vec)
{
        //uint64_t SLT_start = get_sys_clock();
	// Format
	// log_entry (format seen in txn_man::create_log_entry) | Taurus_metadata
	uint64_t starttime = get_sys_clock();
	INC_INT_STATS(int_num_log, 1);
	
#if COMPRESS_LSN_LOG
	// Taurus_metadata = psnCompressCounter | psn * (psnCompressCounter - 1)
	//uint64_t * psn_t;
	uint64_t psnToWrite[g_num_logger + 1];
		uint32_t psnCompressCounter = 0;
		for(uint32_t i=0; i<g_num_logger; i++)
			if(lsn_vec[i] > glob_manager->lastPSN[i][0])
				psnToWrite[psnCompressCounter++] = (lsn_vec[i] << 5) | i;
				// ASSUMPTION: g_num_logger <= 2**5
		//cout << psnCompressCounter << " psn ";
		psnToWrite[psnCompressCounter] = psnCompressCounter;
		//INC_INT_STATS(int_aux_bytes, (psnCompressCounter + 1) * sizeof(uint64_t));
		INC_INT_STATS(int_aux_bytes, psnCompressCounter * sizeof(uint64_t) + 1);
		// put the size info at the end because otherwise we cannot distinguish the encoding of txn and the metadata while recovering
		// Note: here we let last item = len(psnToWrite), instead of the number of compressed items
		uint32_t total_size = entry_size + sizeof(uint64_t) * psnCompressCounter + 1; //sizeof(uint64_t); // + sizeof(uint64_t);
		assert(total_size < g_max_log_entry_size);
		//char new_log_entry[total_size];
		//assert(total_size > 0);	
		assert(*log_entry!=(char)0xef || entry_size == *(uint32_t *)(log_entry + sizeof(uint32_t)));
		
		INC_STATS(GET_THD_ID, log_data, total_size);
		
		uint32_t offset = entry_size;
		// Total Size
		//memcpy(new_log_entry, &total_size, sizeof(uint32_t));
		//offset += sizeof(uint32_t);
		memcpy(log_entry + offset, psnToWrite, 1 + sizeof(uint64_t) * psnCompressCounter);
		offset += 1 + sizeof(uint64_t) * psnCompressCounter;
		//memcpy(log_entry + offset, psnToWrite, sizeof(uint64_t) + sizeof(uint64_t) * psnCompressCounter);
		//offset += sizeof(uint64_t) + sizeof(uint64_t) * psnCompressCounter;
		// update the size in the entry
		
/*
	if(g_psn_flush_freq!=0 &&
		(stats->_stats[GET_THD_ID]->_int_stats[STAT_int_num_log] / g_psn_flush_freq) % (g_thread_cnt / g_num_logger)== GET_THD_ID / g_num_logger)
	{
		psn_t = (uint64_t*)(log_entry + offset);
		for(uint32_t i=0; i<g_num_logger; i++)
			psn_t[i] = _logger[i]->get_persistent_lsn();
		log_entry[0] = 0x7f;
		offset += g_num_logger * sizeof(uint64_t);
	}
*/
	memcpy(log_entry + sizeof(uint32_t), &offset, sizeof(uint32_t));
	//cout << offset << " size ";
#else
	uint32_t total_size = entry_size + sizeof(uint64_t) * g_num_logger; // + sizeof(uint64_t);
	assert(total_size < g_max_log_entry_size);
	//char new_log_entry[total_size];
	//assert(total_size > 0);	
	//assert(entry_size == *(uint32_t *)(log_entry + sizeof(uint32_t)));
	INC_INT_STATS(int_aux_bytes, g_num_logger * sizeof(uint64_t));
	INC_STATS(GET_THD_ID, log_data, total_size);
	/*
	uint32_t offset = 0;
	// Total Size
	memcpy(new_log_entry, &total_size, sizeof(uint32_t));
	offset += sizeof(uint32_t);
	memcpy(new_log_entry, lsn_vec, sizeof(uint64_t) * g_num_logger);
	offset += sizeof(uint64_t) * g_num_logger;
	*/
	uint32_t offset = entry_size;
	memcpy(log_entry + offset, lsn_vec, sizeof(uint64_t) * g_num_logger);
	offset += sizeof(uint64_t) * g_num_logger;
	// update the size in the entry
	memcpy(log_entry + sizeof(uint32_t), &offset, sizeof(uint32_t));
#endif
	// Log Entry
	//memcpy(new_log_entry + offset, log_entry, entry_size);
	//offset += entry_size;
	//INC_INT_STATS(time_insideSLT1, get_sys_clock() - SLT_start);
	//assert(offset == total_size);
	uint32_t logger_id = GET_THD_ID % g_num_logger;
	INC_INT_STATS(time_STLother, get_sys_clock() - starttime);

	uint64_t newlsn;
	//do {
	#if CC_ALG == SILO
	// we only make a single attempt
	newlsn = _logger[logger_id]->logTxn(log_entry, total_size);
	if(newlsn < UINT64_MAX)
	{
		lsn_vec[logger_id] = newlsn;
/* #if COMPRESS_LSN_LOG
		if(log_entry[0] == 0x7f)
		{
			//uint64_t v = *glob_manager->lastPSN[logger_id]
		}
#endif*/
	}
	return newlsn;
	#else
	for(;;)
	{
		newlsn = _logger[logger_id]->logTxn(log_entry, total_size, 0, true);
		if(newlsn < UINT64_MAX)
			break;
		//assert(false);
		usleep(100);
	}
	#endif
	//} while(newlsn < 0);
	//uint64_t newlsn = _logger[logger_id]->logTxn(new_log_entry, total_size);
	// 'lsn_vec[logger_id] is always smaller than newlsn' is not true.
	//assert(lsn_vec[logger_id] < newlsn);
	lsn_vec[logger_id] = newlsn;  // update lsn
	//INC_INT_STATS(time_debug1, get_sys_clock() - starttime);
	return newlsn;
}

void 
TaurusLogManager::readFromLog(char * &entry)
{
	// Decode the log entry.
	// This process is the reverse of parallelLogTxn() 
	// XXX
	//char * raw_entry = _logger[0]->readFromLog();
	char * raw_entry = NULL; 
	if (raw_entry == NULL) {
		entry = NULL;
		num_files_done ++;
		return;
	}
	// Total Size
	uint32_t total_size = *(uint32_t *)raw_entry;
	M_ASSERT(total_size > 0 && total_size < 4096, "total_size=%d\n", total_size);
	// Log Entry
	entry = raw_entry + sizeof(uint32_t); 
}

uint64_t 
TaurusLogManager::get_persistent_lsn()
{
	assert(false); // this function should not be called;
	return _logger[0]->get_persistent_lsn();
}

#endif
