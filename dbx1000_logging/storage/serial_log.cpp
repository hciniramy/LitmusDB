#include "manager.h"
#include "serial_log.h"
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

#if LOG_ALGORITHM == LOG_SERIAL

volatile uint32_t SerialLogManager::num_files_done = 0;
volatile uint64_t ** SerialLogManager::num_txns_recovered = NULL;

SerialLogManager::SerialLogManager()
{
	num_txns_recovered = new uint64_t volatile * [g_thread_cnt];
	for (uint32_t i = 0; i < g_thread_cnt; i++) {
		num_txns_recovered[i] = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		*num_txns_recovered[i] = 0;
	}
} 

SerialLogManager::~SerialLogManager()
{
        //_mm_free(lastLoggedTID);
        //_mm_free(logLatch);
	for(uint32_t i = 0; i < g_num_logger; i++)
		_mm_free(_logger[i]);
	delete[] _logger;
}

void SerialLogManager::init()
{
	lastLoggedTID = (volatile uint64_t *) _mm_malloc(sizeof(uint64_t), 64);
	logLatch =  (volatile uint64_t *) _mm_malloc(sizeof(uint64_t), 64);
        *lastLoggedTID = 0;
        *logLatch = 0;
	_logger = new LogManager * [g_num_logger];
	assert(g_num_logger==1); // serial!
	char hostname[256];
	gethostname(hostname, 256);
	for(uint32_t i = 0; i < g_num_logger; i++) { 
		// XXX
		//MALLOC_CONSTRUCTOR(LogManager, _logger[i]);
		_logger[i] = (LogManager*) _mm_malloc(sizeof(LogManager), 64); //new LogManager(i);
		new (_logger[i]) LogManager(i);
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
		_logger[i]->init(dir + "/SD_log" + to_string(i) + "_" + bench + "_S.data");
#else
		_logger[i]->init(dir + "/SC_log" + to_string(i) + "_" + bench + "_S.data");
#endif
	}
}

bool 
SerialLogManager::tryFlush()
{
	return _logger[0]->tryFlush();
}

uint64_t 
SerialLogManager::serialLogTxn(char * log_entry, uint32_t entry_size, lsnType tid)
{
#if CC_ALG == SILO
	assert(tid!=UINT64_MAX);
	//cout << tid << ", " << lastLoggedTID << endl;
	/*
	uint64_t lltid = *lastLoggedTID;
	if(tid <= lltid)
		return (uint64_t) -1;
	//if(!ATOM_CAS(logLatch, 0, 1))
	//	return (uint64_t) -1; 
	
	while(!ATOM_CAS(*lastLoggedTID, lltid, tid)) 
	{
        lltid = *lastLoggedTID;
		if(tid <= lltid)
			return (uint64_t) -1;
	};
	*/
	/*	
	if(tid <= lastLoggedTID)
	{
		logLatch = 0;
		return (uint64_t) -1;
	}*/
	//cout << "Swapped from " << lastLoggedTID << " to " << tid << endl;
	//lastLoggedTID = tid;
#endif

	// Format
	// total_size | log_entry (format seen in txn_man::create_log_entry)
	//uint32_t total_size = sizeof(uint32_t) + entry_size; // + sizeof(uint64_t);
	//char new_log_entry[total_size];
	//assert(total_size > 0);	
	assert(entry_size == *((uint32_t *)log_entry + 1));
	//assert(entry_size > 300);
	INC_INT_STATS(log_data, entry_size);
	
	//uint32_t offset = 0;
	// Total Size
	//memcpy(new_log_entry, &total_size, sizeof(uint32_t));
	//offset += sizeof(uint32_t);
	// Log Entry
	//memcpy(new_log_entry + offset, log_entry, entry_size);
	//offset += entry_size;
	//assert(offset == total_size);
	uint64_t newlsn;
	//do {
#if CC_ALG == SILO
	//newlsn = _logger[0]->logTxn(log_entry, entry_size, 0, false);
	//newlsn = _logger[0]->logTxn(log_entry, entry_size, 0, true);
	for(;;)
	{
		newlsn = _logger[0]->logTxn(log_entry, entry_size, 0, true); // need to sync
		if(newlsn < UINT64_MAX)
			break;
		//assert(false);
		PAUSE // usleep(10);
	}
	//logLatch = 0;
#else
	for(;;)
	{
		#if CC_ALG == NO_WAIT || CC_ALG == DETRESERVE
		newlsn = _logger[0]->logTxn(log_entry, entry_size, 0, true);
		#else
		newlsn = _logger[0]->logTxn(log_entry, entry_size, 0, false);
		#endif
		if(newlsn < UINT64_MAX)
			break;
		//assert(false);
		usleep(100);
	}

#endif
	return newlsn;
}

void 
SerialLogManager::readFromLog(char * &entry)
{
	assert(false); // this function should not be called.
	/*
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
	*/
}

uint64_t 
SerialLogManager::get_persistent_lsn()
{
	return _logger[0]->get_persistent_lsn();
}


#endif
