#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include "log.h"
#include "parallel_log.h"
#include "taurus_log.h"
#include <iostream>
#include <fstream>
#include <sys/time.h>
#include <stdio.h>
#include <sys/types.h>
#include <aio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <atomic>
#include <helper.h>
#include "manager.h"
#include <inttypes.h>

//int volatile count_busy = g_buffer_size;

LogManager::LogManager(uint32_t logger_id)
	: _logger_id (logger_id)
{
	_log_buffer_size = g_log_buffer_size;
	if (g_log_recover) {
		
#if LOG_ALGORITHM == LOG_TAURUS && !PER_WORKER_RECOVERY
		next_lsn = 0;
		disk_lsn = 0;
#else		
		_disk_lsn = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		*_disk_lsn = 0;
		_next_lsn = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		*_next_lsn = 0;
#endif
#if LOG_ALGORITHM != LOG_TAURUS
		_gc_lsn = new uint64_t volatile * [g_thread_cnt];
		for (uint32_t i = 0; i < g_thread_cnt; i++) {
			_gc_lsn[i] = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
			*_gc_lsn[i] = 0;
		}
#else
		uint64_t num_worker = g_thread_cnt / g_num_logger;
		rasterizedLSN = (volatile uint64_t**) _mm_malloc(sizeof(uint64_t*) * num_worker, 64);
		for(uint i=0; i<num_worker; i++)
		{
			rasterizedLSN[i] = (volatile uint64_t*) _mm_malloc(sizeof(uint64_t), 64);
			rasterizedLSN[i][0] = 0;
		}
		
		#if PER_WORKER_RECOVERY
		reserveLSN = (volatile uint64_t**) _mm_malloc(sizeof(uint64_t*) * num_worker, 64);
		for(uint i=0; i<num_worker; i++)
		{
			reserveLSN[i] = (volatile uint64_t*) _mm_malloc(sizeof(uint64_t), 64);
			reserveLSN[i][0] = 0;
		}
		#endif
		
		
#endif
		_eof = false;
		
		if(logger_id == 3)
		// if this is the RAID 5
		{
			_log_buffer_size *= 2;
		}
		
	} else {
		assert(g_thread_cnt <= 64); // see log.h
		//_lsn = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		//_persistent_lsn = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		*_lsn = 0;
		*_persistent_lsn = 0;
		//assert(g_num_logger + g_thread_cnt <= 64); // for the filled_lsn and allocate_lsn
		//_filled_lsn = (volatile uint64_t**) _mm_malloc(sizeof(uint64_t*) * g_thread_cnt, ALIGN_SIZE); //new uint64_t volatile * [g_thread_cnt];
		for (uint32_t i = 0; i < g_thread_cnt; i++) {
			_filled_lsn[i] = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
			*_filled_lsn[i] = 0; //(uint64_t) -1;  // initial value to make sure that allocate_lsn >= filled_lsn is not true
		}
#if SOLVE_LIVELOCK
		for (uint32_t i = 0; i < g_thread_cnt; i++) {
			_allocate_lsn[i] = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
			*_allocate_lsn[i] = (uint64_t) -1;
		}
#endif
		#if LOG_ALGORITHM == LOG_TAURUS && COMPRESS_LSN_LOG
		log_filled_lsn = -1;
		#endif
	}
	_flush_interval = g_flush_interval; // in ns  
	_last_flush_time = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
	*_last_flush_time = get_sys_clock();
	
	// log buffer
/*#if LOG_ALGORITHM == LOG_BATCH
	// TODO. the number should be dynamic. 
	// Right now, assuming buffer[i] will be flushed when buffer[i + _num_buffer] starts to fill. 
	_num_buffers = 4;
	_buffers = new char * [_num_buffers];
	for (uint32_t i = 0; i < _num_buffers; i ++)
		_buffers[i] = (char *) _mm_malloc(_log_buffer_size, ALIGN_SIZE);
	if (!g_log_recover) {
		//_max_epochs = new volatile uint64_t * [g_thread_cnt];
		//for (uint32_t i = 0; i < g_thread_cnt; i ++) {
		//	_max_epochs[i] = (volatile uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		//	*_max_epochs[i] = 0;
		//}
		_max_flushed_epoch = (uint64_t *) _mm_malloc(sizeof(uint64_t), ALIGN_SIZE);
		*_max_flushed_epoch = 0;
		//_buffer_state = new BufferState * [_num_buffers];
	}
#endif*/

	
	// TODO: need to adapt different settings for the 4-th logger on a RAID 5 disk.

	_buffer = (char *) _mm_malloc(_log_buffer_size + g_max_log_entry_size, 512); // logical block size of the file system
}

void LogManager::flushRestNClose()
{
    if (!g_log_recover && !g_no_flush) {
		//return; // TODO: potential future TODOs.
		// we just ignore the following epoch.
		
		// What if someone is still copying data?
		//cout << "!!!" << endl;
		printf("Destructor. flush size=%" PRIu64 " (%" PRIu64 " to %" PRIu64 ")\n", (*_lsn) / 512 * 512 - *_persistent_lsn, 
			*_persistent_lsn, (*_lsn) / 512 * 512);
		uint64_t end_lsn = (*_lsn) / 512 * 512;
		uint64_t start_lsn = *_persistent_lsn;
		flush(start_lsn, end_lsn);
		INC_FLOAT_STATS(log_bytes, end_lsn - start_lsn);
			
		uint32_t bytes = write(_fd, &end_lsn, sizeof(uint64_t));
		//uint32_t bytes = write(_fd, _lsn, sizeof(uint64_t));
		assert(bytes == sizeof(uint64_t));
		fsync(_fd);
		fsync(_fd_data);

		//fseek(_fd_data, 0, SEEK_SET); // put the position indicator back to the beginning.

		close(_fd);
		close(_fd_data);
	}
}

void LogManager::reopen(int flag=O_RDONLY)
{
	init(_file_name);
}

LogManager::~LogManager()
{
#if LOG_RAM_DISK
	if (!g_log_recover && !g_no_flush)
		_disk->flush(*_lsn);
	delete _disk;
#else 
	flushRestNClose();
	_mm_free(_buffer); // because this could be very big.
#endif
}

void LogManager::init(string log_file_name)
{
	if (g_no_flush)
		return;
	_file_name = log_file_name;
	printf("log file:\t\t%s\n", log_file_name.c_str());
	if (g_log_recover) {
//#if LOG_ALGORITHM != LOG_BATCH
			string path = log_file_name + ".0";
            if(g_ramdisk)
                _fd_data = open(path.c_str(), O_RDONLY);
            else
			    _fd_data = open(path.c_str(), O_DIRECT | O_RDONLY);
			uint64_t fdatasize = lseek(_fd_data, 0, SEEK_END);
			cout << "data size " << fdatasize << endl;
			#if LOG_ALGORITHM == LOG_TAURUS
			log_manager->endLV[_logger_id][0] = fdatasize;
			#endif
			lseek(_fd_data, 0, SEEK_SET);
			assert(_fd != -1);
		//if (g_log_recover) {
			int _fd = open(log_file_name.c_str(), O_RDONLY);
			assert(_fd != -1);
			uint32_t fsize = lseek(_fd, 0, SEEK_END);
			lseek(_fd, 0, SEEK_SET);
			_num_chunks = fsize / sizeof(uint64_t);
			//_starting_lsns = new uint64_t [_num_chunks];
			_starting_lsns = (uint64_t*) _mm_malloc(_num_chunks * sizeof(uint64_t), ALIGN_SIZE);
			uint32_t size = read(_fd, _starting_lsns, fsize);
			assert(size == fsize);
			_chunk_size = 0;
			for (uint32_t i = 0; i < _num_chunks; i ++) {
				if (_starting_lsns[i] >= fdatasize)
				{
					_num_chunks = i;
					break;
				}
				uint64_t fstart = _starting_lsns[i] / 512 * 512; 
				uint64_t fend = _starting_lsns[i+1];
				if (fend % 512 != 0)
					fend = fend / 512 * 512 + 512; 
				if (fend - fstart > _chunk_size)
					_chunk_size = fend - fstart;
				printf("_starting_lsns[%d] = %" PRIu64 "\n", i, _starting_lsns[i]);
			}
			// let the profiling tool only measures the all-thread-busy-working part.
			uint32_t worker_per_logger = g_thread_cnt / g_num_logger;
			_num_chunks = (_num_chunks - 1) / worker_per_logger * worker_per_logger + 1;
			printf("Chunk Number adjusted to %d\n", _num_chunks);

			_chunk_size = _chunk_size / 512 * 512 + 1024;
			close(_fd);
			_next_chunk = 0;
			_file_size = fsize;

#if ASYNC_IO
			memset(&cb, 0, sizeof(aiocb64));
			cb.aio_fildes = _fd_data;
			fileOffset = 0;
			lastStartLSN = 0;
			lastSize = 0;
			lastTime = 0;
			AIOworking = false;
#endif
		//}
		//_mutex = new pthread_mutex_t;
		//pthread_mutex_init(_mutex, NULL);
//#endif
	} else {
		//cout << log_file_name << endl;
		string name = _file_name;
	    // for parallel logging. The metafile format.
		//  | file_id | start_lsn | * num_of_log_files
		_fd = open(name.c_str(), O_TRUNC | O_WRONLY | O_CREAT, 0664);
	 // #if LOG_ALGORITHM == LOG_PARALLEL
		//_fd = open(name.c_str(), O_TRUNC | O_WRONLY | O_CREAT, 0664);
		//uint32_t bytes = write(_fd, &_curr_file_id, sizeof(_curr_file_id));
		// the start of the first segment. _lsn = 0 
		assert(*_lsn  == 0);
		uint32_t bytes = write(_fd, (uint64_t*)_lsn, sizeof(uint64_t));
		assert(bytes == sizeof(uint64_t));
		fsync(_fd);
	 // #endif
		assert(_fd != -1);
//	  #if LOG_ALGORITHM == LOG_PARALLEL
		name = _file_name + ".0"; // + to_string(_curr_file_id);
        if(g_ramdisk)
            _fd_data = open(name.c_str(), O_TRUNC | O_WRONLY | O_CREAT, 0664);
        else
		    _fd_data = open(name.c_str(), O_DIRECT | O_TRUNC | O_WRONLY | O_CREAT, 0664);
		assert(_fd_data != -1);
//	  #endif
	}
}

//#if LOG_ALGORITHM == LOG_BATCH
/*uint64_t 
LogManager::logTxn(char * log_entry, uint32_t size, uint64_t epoch)
{
	uint32_t buffer_id = epoch % _num_buffers;
	return logTxn(log_entry, size);
	// waiting for the logging threads to flush the buffer. 
	//printf("epoch = %ld\n", epoch);
//	if (epoch >= *_max_flushed_epoch + _num_buffers) {
//		return -1;
//	}
//	uint64_t lsn = ATOM_FETCH_ADD(*_lsns[buffer_id], size);
//	M_ASSERT(lsn + size < _log_buffer_size, "Log buffers are too small\n");
//	memcpy(_buffers[buffer_id] + lsn, log_entry, size);
//  	return lsn;
}
*/
//#else 
uint64_t
LogManager::logTxn(char * log_entry, uint32_t size, uint64_t epoch, bool sync)
{
	// called by serial logging AND parallel command logging 
	// The log is stored to the in memory cirular buffer
    // if the log buffer is full, wait for it to flush.  
		
	//printf("TPCC log size: %d\n", size);
	//COMPILER_BARRIER;

	//assert(log_entry[0]==0x7f || uint8_t(log_entry[0]) == 0xef);

	uint64_t starttime = get_sys_clock();
	if (*_lsn + size >= 
		*_persistent_lsn + _log_buffer_size - g_max_log_entry_size * g_thread_cnt / g_num_logger) 
	{
	#if LOG_ALGORITHM == LOG_TAURUS && COMPRESS_LSN_LOG
		if(log_entry[0] == 0x7f)
			log_filled_lsn = *_lsn; //TODO: is this a hack?
		else
	#endif
	#if !SOLVE_LIVELOCK
		*_filled_lsn[GET_THD_ID] = *_lsn;
	#endif
		//printf("[%" PRIu64 "] txn aborted, persistent_lsn=%" PRIu64 "\n", GET_THD_ID, *_persistent_lsn);
		//assert(false);
		return -1; 
	}

//INC_INT_STATS(time_debug3, get_sys_clock() - starttime);	
//INC_INT_STATS(time_debug10, get_sys_clock() - starttime);	
	uint32_t size_aligned = size % 64 == 0 ? size: size + 64 - size % 64;
	//INC_INT_STATS(time_debug6, get_sys_clock() - starttime);
	#if SOLVE_LIVELOCK
	if(log_entry[0] != 0x7f)
		*_allocate_lsn[GET_THD_ID] = *_lsn; 
	// assumingly this will be no less than the _filled_lsn
	#endif
	COMPILER_BARRIER
	uint64_t lsn;
	if(sync)
  		lsn = ATOM_FETCH_ADD(*_lsn, size_aligned);	
	else
	{
		assert(LOG_ALGORITHM == LOG_SERIAL); // only serial logging puts this code in a critical section.

		lsn = *_lsn;
		*_lsn += size_aligned;
	}
	
	//COMPILER_BARRIER
	//INC_INT_STATS(time_debug7, get_sys_clock() - starttime);
//COMPILER_BARRIER;
	//uint64_t lsn = ATOM_FETCH_ADD(*_lsn, size);

//INC_INT_STATS(time_debug8, get_sys_clock() - starttime);

	
	if ((lsn / _log_buffer_size < (lsn + size) / _log_buffer_size))	{
		// reaching the end of the circular buffer, write in two steps 
		uint32_t tail_size = _log_buffer_size - lsn % _log_buffer_size; 
		memcpy(_buffer + lsn % _log_buffer_size, log_entry, tail_size);
		memcpy(_buffer, log_entry + tail_size, size - tail_size);
	} else {
		memcpy(_buffer + lsn % _log_buffer_size, log_entry, size);
	}
	COMPILER_BARRIER
INC_INT_STATS(time_insideSLT1, get_sys_clock() - starttime);
	
//INC_INT_STATS(time_insideSLT2, get_sys_clock() - starttime);
	//////////// DEBUG
	//uint32_t size_entry = *(uint32_t*)(log_entry + sizeof(uint32_t));
	//assert(size == size_entry && size > 0 && size <= g_max_log_entry_size);
	/////////////////////
  #if LOG_ALGORITHM == LOG_BATCH
	glob_manager->update_epoch_lsn_mapping(epoch, lsn);
  #endif
	//OMPILER_BARRIER
	#if LOG_ALGORITHM == LOG_TAURUS && COMPRESS_LSN_LOG
		if(log_entry[0] == 0x7f)
			log_filled_lsn = -1;  // set as -1 to inactivate
		else
	#endif
//INC_INT_STATS(time_debug4, get_sys_clock() - starttime);
		/*
		stringstream ss;
		ss<<GET_THD_ID << " " << (uint64_t)(_filled_lsn[GET_THD_ID]) << endl;
		cout << ss.str();
		*/
		*(_filled_lsn[GET_THD_ID]) = lsn + size_aligned; 
//INC_INT_STATS(time_debug5, get_sys_clock() - starttime);
INC_INT_STATS(time_insideSLT2, get_sys_clock() - starttime);
  	return lsn + size; // or it could be lsn+size_aligned-1
}
//#endif

uint64_t
LogManager::tryFlush() 
{
	// entries before ready_lsn can be flushed. 
	if (g_no_flush) {
		uint32_t size = *_lsn - *_persistent_lsn; //*_persistent_lsn - *_lsn;
		*_persistent_lsn = *_lsn;
		return size;
	}
	//assert(false);
	//#if SOLVE_LIVELOCK
	uint64_t ready_lsn = *_lsn;
#if LOG_ALGORITHM == LOG_TAURUS && COMPRESS_LSN_LOG
	if(log_filled_lsn != (uint64_t)-1 && ready_lsn > log_filled_lsn)
		ready_lsn = log_filled_lsn;
#endif
	//#else
	//uint64_t ready_lsn = *_filled_lsn[_logger_id];  // note that logger_id could be larger than g_thread_cnt!
	//#endif
	//printf("_lsn = %ld\n", *_lsn);
	COMPILER_BARRIER
	#if SOLVE_LIVELOCK
	for (uint32_t i = _logger_id; i < g_thread_cnt; i+= g_num_logger)
		// ready_lsn is the smallest lsn
		//if (i % g_num_logger == _logger_id && ready_lsn > *_filled_lsn[i]) {
	{
		uint64_t filledLSN = *_filled_lsn[i]; // the reading order matters
		COMPILER_BARRIER
		uint64_t allocateLSN = *_allocate_lsn[i];
		if (allocateLSN >= filledLSN && ready_lsn > allocateLSN) {
			ready_lsn = allocateLSN;
		}
	}
	#else
	for (uint32_t i = _logger_id; i < g_thread_cnt; i+= g_num_logger)
		if (ready_lsn > *_filled_lsn[i]) {
			ready_lsn = *_filled_lsn[i];
		}
	#endif
	
	#if LOG_ALGORITHM == LOG_TAURUS && COMPRESS_LSN_LOG
		// log_filled_lsn is greater than 0 only when active
		// we do not need to worry racing since log_filled_lsn is only modified in the current log thread.
		if(log_filled_lsn > 0 && ready_lsn > log_filled_lsn)
			ready_lsn = log_filled_lsn;
	#endif

	// Flush to disk if 1) it's been long enough since last flush. OR 2) the buffer is full enough. 
	if (get_sys_clock() - *_last_flush_time < _flush_interval &&
	    ready_lsn - *_persistent_lsn < _log_buffer_size / 2) 
	{	
		return 0;
	}
	if(ready_lsn - *_persistent_lsn < _log_buffer_size / 2)
	{
		INC_INT_STATS(int_flush_time_interval, 1); // caused by time larger than _flush_interval
	}
	else
	{
		INC_INT_STATS(int_flush_half_full, 1); // caused by half full buffer
	}
	assert(ready_lsn >= *_persistent_lsn);
	// timeout or buffer full enough.
	*_last_flush_time = get_sys_clock();

	//ready_lsn -= ready_lsn % 512; 
	//assert(*_persistent_lsn % 512 == 0);
	
	uint64_t start_lsn = *_persistent_lsn;
	uint64_t end_lsn = ready_lsn - ready_lsn % 512; // round to smaller 512x
	//if(end_lsn == *_persistent_lsn) end_lsn = ready_lsn; // small progress could happen when TPCC high contention.
	/*******************************/

#if WITHOLD_LOG
	//nanosleep((const struct timespec[]){{0, 32000L}}, NULL);  // do nothing
#else
	flush(start_lsn, end_lsn);
#endif
	/*******************************/
	COMPILER_BARRIER
	//printf("[%" PRIu64 "] update persistent lsn from %" PRIu64 " to %" PRIu64 ", ready=%" PRIu64 "\n", GET_THD_ID, *_persistent_lsn, end_lsn, ready_lsn);
	*_persistent_lsn = end_lsn;
	
	//assert(*_persistent_lsn <655360000);

#if LOG_ALGORITHM == LOG_BATCH
	glob_manager->update_persistent_epoch(_logger_id, end_lsn);
#endif
#if LOG_ALGORITHM != LOG_TAURUS // taurus does not need this.
	uint32_t chunk_size = g_log_chunk_size;// _log_buffer_size 
	if (end_lsn / chunk_size  > start_lsn / chunk_size ) {
		// write ready_lsn into the file.

		uint32_t bytes = write(_fd, &ready_lsn, sizeof(ready_lsn)); // TODO: end_lsn??
		assert(bytes == sizeof(ready_lsn));
		fsync(_fd);
	}
	#endif
	return end_lsn - start_lsn;
}

void 
LogManager::flush(uint64_t start_lsn, uint64_t end_lsn)
{
	uint64_t starttime = get_sys_clock();
	//assert(false);
//#if LOG_ALGORITHM != LOG_BATCH
	if (start_lsn == end_lsn) return;
	assert(end_lsn - start_lsn < _log_buffer_size);
	
	assert(_fd_data != 1 && _fd_data != 0);
	uint32_t bytes;
	if (start_lsn / _log_buffer_size < end_lsn / _log_buffer_size) {
		// flush in two steps.
		uint32_t tail_size = _log_buffer_size - start_lsn % _log_buffer_size;
		bytes = write(_fd_data, _buffer + start_lsn % _log_buffer_size, tail_size); 
		assert(bytes == tail_size);
		bytes = write(_fd_data, _buffer, end_lsn % _log_buffer_size); 
		assert(bytes == end_lsn % _log_buffer_size);
	} else { 
		// here an error might occur that, the serial port (SATA) might be being used by another one.
		// where the error number would be 
		bytes = write(_fd_data, (void *)(_buffer + start_lsn % _log_buffer_size), end_lsn - start_lsn);
		M_ASSERT(bytes == end_lsn - start_lsn, "bytes=%d, planned=%" PRIu64 ", errno=%d, _fd=%d, end_lsn=%" PRIu64 ", start_lsn=%" PRIu64 ", data=%" PRIu64 "\n", 
			bytes, end_lsn - start_lsn, errno, _fd_data, end_lsn, start_lsn, (uint64_t)(_buffer));
		//printf("start_lsn = %ld, end_lsn = %ld\n", start_lsn, end_lsn);
		//assert(*(uint32_t*)(_buffer + start_lsn % _log_buffer_size) == 0xbeef);
	}
	INC_INT_STATS(int_debug2, bytes);
	INC_INT_STATS(int_debug3, 1);
	fsync(_fd_data); // sync the data
	INC_INT_STATS(time_io, get_sys_clock() - starttime); // actual time in flush.
//#endif
}


uint32_t 
LogManager::tryReadLog()
{
	uint64_t bytes, start_lsn_moded, end_lsn_moded;
	bytes = 0;
	#if ASYNC_IO
	if(AIOworking){
		// short-cut tryReadLog function if the previous is till working
		if(aio_error64(&cb) == EINPROGRESS)
			return 0;
		bytes = aio_return64(&cb);
		//cout << GET_THD_ID << " Read bytes:" << bytes << endl;
		if(bytes < lastSize)
			_eof = true;
		fileOffset += bytes;
		start_lsn_moded = lastStartLSN % _log_buffer_size;
		end_lsn_moded = (lastStartLSN + bytes) % _log_buffer_size;
		assert(end_lsn_moded == 0 || end_lsn_moded >= start_lsn_moded); // bytes could be 0
		if(start_lsn_moded < g_max_log_entry_size)
		{
			if(end_lsn_moded > 0 && end_lsn_moded < g_max_log_entry_size)
				memcpy(_buffer + start_lsn_moded + _log_buffer_size, _buffer + start_lsn_moded, end_lsn_moded - start_lsn_moded);
			else
				memcpy(_buffer + start_lsn_moded + _log_buffer_size, _buffer + start_lsn_moded, g_max_log_entry_size - start_lsn_moded);
		}
		AIOworking = false;
#if LOG_ALGORITHM == LOG_TAURUS && !PER_WORKER_RECOVERY
  //cout << start_lsn << " " << end_lsn << " " << bytes << " " << _log_buffer_size << " " << gc_lsn << endl;
	//cout << "changed disk_lsn from " << disk_lsn << endl;
		disk_lsn = lastStartLSN + bytes;
	//cout << "changed disk_lsn to " << disk_lsn << endl;
#else
		*_disk_lsn = lastStartLSN + bytes;
#endif
		INC_INT_STATS(int_debug3, 1);
		INC_INT_STATS(int_debug2, bytes);
		INC_INT_STATS(time_debug9, get_sys_clock() - lastTime);
	}
	
	
	#endif
	uint64_t starttime = get_sys_clock();
//#if LOG_ALGORITHM != LOG_BATCH
#if LOG_ALGORITHM != LOG_TAURUS
	uint64_t gc_lsn = *_next_lsn;
	COMPILER_BARRIER
	for (uint32_t i = 0; i < g_thread_cnt; i++)
		if (i % g_num_logger == _logger_id && gc_lsn > *_gc_lsn[i] && *_gc_lsn[i] != 0) {
			gc_lsn = *_gc_lsn[i];
		}
	assert(gc_lsn <= *_disk_lsn);
	if (*_disk_lsn - gc_lsn > _log_buffer_size / 2) 
		return bytes;
	assert(*_disk_lsn >= gc_lsn);
	assert(*_disk_lsn % 512 == 0);
	uint64_t start_lsn = *_disk_lsn;
#else
	
#if PER_WORKER_RECOVERY
	uint64_t num_worker = g_thread_cnt / g_num_logger;
	uint64_t gc_lsn = *_next_lsn;
	for(uint i=0; i<num_worker; i++)
	{
		//if(reserveLSN[i][0] && gc_lsn > rasterizedLSN[i][0])
		//if(gc_lsn > rasterizedLSN[i][0])
		//	gc_lsn = rasterizedLSN[i][0];
		uint64_t rlsn = reserveLSN[i][0];
		if(rlsn > 0 && gc_lsn > rlsn)
			gc_lsn = rlsn;
	}
	assert(gc_lsn <= *_disk_lsn);
	//uint64_t gc_lsn = rasterizedLSN[0]; // *log_manager->recoverLVSPSC_min[GET_THD_ID % g_num_logger]; // *_next_lsn;
	if (*_disk_lsn - gc_lsn > _log_buffer_size / 2) 
	{
		//AIOworking = false;
		return bytes;
	}
	assert(*_disk_lsn >= gc_lsn);
	assert(*_disk_lsn % 512 == 0);
	uint64_t start_lsn = *_disk_lsn;
	
#else
	#if DECODE_AT_WORKER
	uint64_t num_worker = g_thread_cnt / g_num_logger;
	uint64_t gc_lsn = rasterizedLSN[0][0];
	for(uint i=1; i<num_worker; i++)
	{
		if(gc_lsn > rasterizedLSN[i][0])
			gc_lsn = rasterizedLSN[i][0];
	}
	assert(gc_lsn <= disk_lsn);
	//uint64_t gc_lsn = rasterizedLSN[0]; // *log_manager->recoverLVSPSC_min[GET_THD_ID % g_num_logger]; // *_next_lsn;
	if (disk_lsn - gc_lsn > _log_buffer_size / 2)
	{
		//AIOworking = false;
		return bytes;
	}
	#else
	uint64_t gc_lsn = rasterizedLSN[0][0];
	#endif
	//assert(disk_lsn >= gc_lsn);
	assert(disk_lsn % 512 == 0);
	uint64_t start_lsn = disk_lsn;
#endif
	
#endif
		
	gc_lsn -= gc_lsn % 512; 

	
	
	uint64_t end_lsn = gc_lsn + (uint64_t)(_log_buffer_size * RECOVER_BUFFER_PERC);
	if (start_lsn == end_lsn) return bytes;
	assert(end_lsn - start_lsn <= _log_buffer_size);
	//uint32_t bytes;
	start_lsn_moded = start_lsn % _log_buffer_size;
	end_lsn_moded = end_lsn % _log_buffer_size;
	/*
	if(end_lsn_moded < g_max_log_entry_size)
	{
		end_lsn_moded = 0;
		end_lsn -= end_lsn_moded; // read less so that the wrapping copy always makes sense
	}
	*/
	#if ASYNC_IO
	lastStartLSN = start_lsn;
	//memset(&cb, 0, sizeof(aiocb64));
	//cb.aio_fildes = _fd_data;
	cb.aio_buf = _buffer + start_lsn_moded;
	cb.aio_offset = fileOffset;
	if (end_lsn_moded > 0 && start_lsn_moded >= end_lsn_moded) {
		// flush in two steps.
		uint32_t tail_size = _log_buffer_size - start_lsn_moded;
		cb.aio_nbytes = tail_size;
		lastSize = tail_size;
		//bytes = read(_fd_data, _buffer + start_lsn_moded, tail_size);
	} else { 
		cb.aio_nbytes = end_lsn - start_lsn;
		lastSize = end_lsn - start_lsn;
	}
	//cout << GET_THD_ID << " aio read from " << start_lsn << " to " << end_lsn << "|" << lastSize << endl;
	lastTime = get_sys_clock();
	if(aio_read64(&cb) == -1)
	{
		assert(false); // Async request failure
	}
	AIOworking = true;
	#else
	if (end_lsn_moded > 0 && start_lsn_moded >= end_lsn_moded) {
		// flush in two steps.
		uint32_t tail_size = _log_buffer_size - start_lsn_moded;

		bytes = read(_fd_data, _buffer + start_lsn_moded, tail_size);
		if(start_lsn_moded < g_max_log_entry_size)
			memcpy(_buffer + start_lsn_moded + _log_buffer_size, _buffer + start_lsn_moded, g_max_log_entry_size - start_lsn_moded);
		if (bytes < tail_size) 
			_eof = true; 
		else {
			bytes += read(_fd_data, _buffer, end_lsn_moded);
			if (bytes < end_lsn  - start_lsn)
				_eof = true; 
			// fill in the dummy tail
			//cout << GET_THD_ID % g_num_logger << " Wrapping " << start_lsn << " " << end_lsn << " " << _log_buffer_size << endl;
			memcpy(_buffer+_log_buffer_size, _buffer, end_lsn_moded < g_max_log_entry_size ? end_lsn_moded : g_max_log_entry_size);
		}

	} else { 
		bytes = read(_fd_data, _buffer + start_lsn_moded, end_lsn - start_lsn);
		//M_ASSERT(bytes == end_lsn - start_lsn, "bytes=%d, planned=%" PRIu64 ", errno=%d, _fd=%d, end_lsn=%" PRIu64 ", start_lsn=%" PRIu64 ", data=%" PRIu64 "\n", 
		//	bytes, end_lsn - start_lsn, errno, _fd_data, end_lsn, start_lsn, (uint64_t)(_buffer));
		if (bytes < end_lsn - start_lsn) 
			_eof = true;
		if(start_lsn_moded < g_max_log_entry_size)
		{
			if(end_lsn_moded > 0)
				memcpy(_buffer + start_lsn_moded + _log_buffer_size, _buffer + start_lsn_moded, (end_lsn_moded < g_max_log_entry_size ? end_lsn_moded : g_max_log_entry_size) - start_lsn_moded);
			else
			{
				memcpy(_buffer + start_lsn_moded + _log_buffer_size, _buffer + start_lsn_moded, g_max_log_entry_size - start_lsn_moded);
			}
		}
	}
	end_lsn = start_lsn + bytes;
	
	
	COMPILER_BARRIER
#if LOG_ALGORITHM == LOG_TAURUS && !PER_WORKER_RECOVERY
  //cout << start_lsn << " " << end_lsn << " " << bytes << " " << _log_buffer_size << " " << gc_lsn << endl;
	//cout << "changed disk_lsn from " << disk_lsn << endl;
	disk_lsn = end_lsn;
	//cout << "changed disk_lsn to " << disk_lsn << endl;
#else
	*_disk_lsn = end_lsn;
#endif
	INC_INT_STATS(int_debug3, 1);
	INC_INT_STATS(int_debug2, bytes);
#endif
	INC_INT_STATS(time_debug10, get_sys_clock() - starttime); // actual time in flush.
	return bytes;
/*#else 
	assert(false);
	return 0;
#endif*/
}

#if LOG_ALGORITHM == LOG_TAURUS || LOG_ALGORITHM == LOG_SERIAL

uint64_t LogManager::get_next_log_entry_non_atom(char * &entry, bool copybuffer) //, uint32_t &mysize)
{
	//uint64_t next_lsn;
	uint32_t size;
	uint64_t t1 = get_sys_clock();
	
		//next_lsn = *_next_lsn;
    
		// TODO. 
		// There is a werid bug: for the last block (512-bit) of the file, the data 
		// is corrupted? the assertion in txn.cpp : 449 would fail.
		// Right now, the hack to solve this bug:
		//  	do not read the last few blocks.
		uint32_t dead_tail = _eof? 2048 : 0;
#if LOG_ALGORITHM == LOG_SERIAL
		if (UNLIKELY(*_next_lsn + sizeof(uint32_t) * 2 >= *_disk_lsn - dead_tail)) {
#else // it could be taurus if per_worker_recover is turned off
		if (UNLIKELY(next_lsn + sizeof(uint32_t) * 2 >= disk_lsn - dead_tail)) {
#endif
			entry = NULL;
			INC_INT_STATS(time_debug11, get_sys_clock() - t1);
			return -1;
		}
		// Assumption. 
		// Each log record has the following format
		//  | checksum (32 bit) | size (32 bit) | ...

		//uint64_t t2 = get_sys_clock();
		// handle the boundary case.
		//uint32_t size_offset = (next_lsn + sizeof(uint32_t)) % _log_buffer_size;
#if LOG_ALGORITHM == LOG_SERIAL
		uint32_t size_offset = *_next_lsn % _log_buffer_size + sizeof(uint32_t);
#else
		uint32_t size_offset = next_lsn % _log_buffer_size + sizeof(uint32_t);
#endif
		/*
		uint32_t tail = _log_buffer_size - size_offset;
		if (UNLIKELY(tail < sizeof(uint32_t))) {
			memcpy(&size, _buffer + size_offset, tail);
			memcpy((char *)&size + tail, _buffer, sizeof(uint32_t) - tail);
		} else 
			//memcpy(&size, _buffer + size_offset, sizeof(uint32_t));
		*/
			size = *(uint32_t*) (_buffer + size_offset);
			//mysize = size;
		//assert(size > 0);

		if(size == 0)
		{
			entry = NULL;
			_eof = true;
			// hack
		}
		
		// round to a cacheline size
		size = size % 64 == 0 ? size : size + 64 - size % 64;
		//INC_INT_STATS(time_debug6, get_sys_clock() - t2);
#if LOG_ALGORITHM == LOG_SERIAL
		if (UNLIKELY(*_next_lsn + size >= *_disk_lsn - dead_tail)) {
#else
		if (UNLIKELY(next_lsn + size >= disk_lsn - dead_tail)) {
#endif
			entry = NULL;
			INC_INT_STATS(time_debug12, get_sys_clock() - t1);
			return -1;
		}
		//INC_INT_STATS(int_debug5, 1);
	uint64_t tt2 = get_sys_clock();
	INC_INT_STATS(time_debug_get_next, tt2 - t1);
	/*
	if (next_lsn / _log_buffer_size != (next_lsn + size) / _log_buffer_size) {
		// copy to entry in 2 steps
		uint32_t tail = _log_buffer_size - next_lsn % _log_buffer_size;
		memcpy(entry, _buffer + next_lsn % _log_buffer_size, tail);
		memcpy(entry + tail, _buffer, size - tail);
	} else 
	*/
#if LOG_ALGORITHM == LOG_SERIAL
	if(copybuffer)
	{
		// use copybuffer when the processing speed could be lower than reading speed
		memcpy(entry, _buffer + (*_next_lsn % _log_buffer_size), size);
		
		*_next_lsn = *_next_lsn + size;
	}
	else
	{
		entry = _buffer + (*_next_lsn % _log_buffer_size);
		// TODO: assume file read is slower than processing txn,
		// a.k.a., the circular buffer will not be freshed.
	
		*_next_lsn = *_next_lsn + size;
	}
	
	

	INC_INT_STATS(int_debug_get_next, 1);	
	INC_INT_STATS(time_recover5, get_sys_clock() - t1);
	return *_next_lsn - 1; // for maxLSN
#else
	entry = _buffer + (next_lsn % _log_buffer_size);
		// TODO: assume file read is slower than processing txn,
		// a.k.a., the circular buffer will not be freshed.
	next_lsn = next_lsn + size;

	INC_INT_STATS(int_debug_get_next, 1);	
	INC_INT_STATS(time_recover5, get_sys_clock() - t1);
	return next_lsn - 1; // for maxLSN
#endif
}
#endif

uint64_t 
LogManager::get_next_log_entry(char * &entry, uint32_t & callback_size)
{
#if LOG_ALGORITHM == LOG_TAURUS && PER_WORKER_RECOVERY
	uint64_t next_lsn;
	uint32_t size;
	uint32_t size_aligned;
	//char * mentry = entry;
	uint64_t t1 = get_sys_clock();
	uint64_t workerId = GET_THD_ID / g_num_logger;
	for(;;) {
		//mentry = entry;
		reserveLSN[workerId][0] = 0;
		next_lsn = *_next_lsn;
		//rasterizedLSN[workerId][0] = next_lsn;
    
		// TODO. 
		// There is a werid bug: for the last block (512-bit) of the file, the data 
		// is corrupted? the assertion in txn.cpp : 449 would fail.
		// Right now, the hack to solve this bug:
		//  	do not read the last few blocks.
		uint32_t dead_tail = _eof? 2048 : 0;
		if (UNLIKELY(next_lsn + sizeof(uint32_t) * 2 >= *_disk_lsn - dead_tail)) {
			//cout << "next " << next_lsn << " " << *_disk_lsn << endl;
			entry = NULL;
			INC_INT_STATS(time_debug11, get_sys_clock() - t1);
			return -1;
		}
		// Assumption. 
		// Each log record has the following format
		//  | checksum (32 bit) | size (32 bit) | ...

		//uint64_t t2 = get_sys_clock();
		// handle the boundary case.
		// uint32_t size_offset = (next_lsn + sizeof(uint32_t)) % _log_buffer_size;
		//if(*_next_lsn != next_lsn) continue;
		uint32_t size_offset = next_lsn % _log_buffer_size + sizeof(uint32_t);
		/*
		uint32_t tail = _log_buffer_size - size_offset;
		if (UNLIKELY(tail < sizeof(uint32_t))) {
			memcpy(&size, _buffer + size_offset, tail);
			memcpy((char *)&size + tail, _buffer, sizeof(uint32_t) - tail);
		} else  
		*/
			//memcpy(&size, _buffer + size_offset, sizeof(uint32_t));
			size = *(uint32_t*) (_buffer + size_offset);
		//if(*_next_lsn != next_lsn) continue;
			// round to a cacheline size
		size_aligned = size % 64 == 0 ? size : size + 64 - size % 64;
		//INC_INT_STATS(time_debug6, get_sys_clock() - t2);
		if (UNLIKELY(next_lsn + size_aligned >= *_disk_lsn - dead_tail)) {
			entry = NULL;
			INC_INT_STATS(time_debug12, get_sys_clock() - t1);
			return -1;
		}
		if(*_next_lsn != next_lsn) continue;
		//INC_INT_STATS(int_debug5, 1);
		reserveLSN[workerId][0] = next_lsn; // + size_aligned;
		//rasterizedLSN[workerId][0] = *_next_lsn;
		COMPILER_BARRIER
		if(ATOM_CAS(*_next_lsn, next_lsn, next_lsn + size_aligned))
			break;
		
		PAUSE
		/*
		else
		{
			entry = NULL;
			INC_INT_STATS(time_debug12, get_sys_clock() - t1);
			return -1;
		}
		*/
	}
	INC_INT_STATS(time_debug5, get_sys_clock() - t1);
	INC_INT_STATS(int_debug6, 1);
	/*
	if (next_lsn / _log_buffer_size != (next_lsn + size) / _log_buffer_size) {
		// copy to entry in 2 steps
		uint32_t tail = _log_buffer_size - next_lsn % _log_buffer_size;
		memcpy(entry, _buffer + next_lsn % _log_buffer_size, tail);
		memcpy(entry + tail, _buffer, size - tail);
	} else */

		//mentry = _buffer + (next_lsn % _log_buffer_size);
		entry = _buffer + (next_lsn % _log_buffer_size);

	//entry = mentry;
	next_lsn = next_lsn + size;
	callback_size = size;
	INC_INT_STATS(int_debug_get_next, 1);	
	INC_INT_STATS(time_recover5, get_sys_clock() - t1);
	return next_lsn - 1;
#else
	assert(false);
	return 0;
#endif
}

uint64_t 
LogManager::get_next_log_batch(char * &entry, uint32_t & num)
{
	assert(false); // not implemented
/*
#if LOG_ALGORITHM != LOG_BATCH
	uint64_t next_lsn;
	uint32_t size;
	uint32_t size_aligned;
	//char * mentry = entry;
	uint64_t t1 = get_sys_clock();
	for(;;) {
		//mentry = entry;
		next_lsn = *_next_lsn;
    
		// TODO. 
		// There is a werid bug: for the last block (512-bit) of the file, the data 
		// is corrupted? the assertion in txn.cpp : 449 would fail.
		// Right now, the hack to solve this bug:
		//  	do not read the last few blocks.
		uint32_t dead_tail = _eof? 2048 : 0;
		if (UNLIKELY(next_lsn + sizeof(uint32_t) * 2 >= *_disk_lsn - dead_tail)) {
			//cout << "next " << next_lsn << " " << *_disk_lsn << endl;
			entry = NULL;
			INC_INT_STATS(time_debug11, get_sys_clock() - t1);
			return -1;
		}
		// Assumption. 
		// Each log record has the following format
		//  | checksum (32 bit) | size (32 bit) | ...

		//uint64_t t2 = get_sys_clock();
		// handle the boundary case.
		// uint32_t size_offset = (next_lsn + sizeof(uint32_t)) % _log_buffer_size;
		uint32_t size_offset = next_lsn % _log_buffer_size + sizeof(uint32_t);
		
			//memcpy(&size, _buffer + size_offset, sizeof(uint32_t));
			size = *(uint32_t*) (_buffer + size_offset);
			// round to a cacheline size
		size_aligned = size % 64 == 0 ? size : size + 64 - size % 64;
		//INC_INT_STATS(time_debug6, get_sys_clock() - t2);
		if (UNLIKELY(next_lsn + size_aligned >= *_disk_lsn - dead_tail)) {
			entry = NULL;
			INC_INT_STATS(time_debug12, get_sys_clock() - t1);
			return -1;
		}
		//INC_INT_STATS(int_debug5, 1);
		if(ATOM_CAS(*_next_lsn, next_lsn, next_lsn + size_aligned))
			break;
	}
	INC_INT_STATS(time_debug5, get_sys_clock() - t1);
	INC_INT_STATS(int_debug6, 1);

		//mentry = _buffer + (next_lsn % _log_buffer_size);
		entry = _buffer + (next_lsn % _log_buffer_size);

	//entry = mentry;
	next_lsn = next_lsn + size;
	callback_size = size;
	INC_INT_STATS(int_debug_get_next, 1);	
	INC_INT_STATS(time_recover5, get_sys_clock() - t1);
	return next_lsn - 1;
#else
	assert(false);
	return 0;
#endif
*/
}
 
uint32_t
LogManager::get_next_log_chunk(char * &chunk, uint64_t &size, uint64_t &base_lsn)
{
#if LOG_ALGORITHM == LOG_PARALLEL || LOG_ALGORITHM == LOG_BATCH
	// allocate the next chunk number. 
	// grab a lock on the log file
	// read sequentially the next chunk.
	// release the lock.
	//chunk = new char [_chunk_size];
	chunk = (char*) _mm_malloc(_chunk_size, 512); // need to be aligned
	//if (_logger_id == 3)
	//	pthread_mutex_lock(_mutex);
	
	//INC_INT_STATS(time_debug6, get_sys_clock() - tt);
	//uint64_t tt = get_sys_clock();
	
	uint32_t chunk_num = ATOM_FETCH_ADD(_next_chunk, 1);
	if (chunk_num >= _num_chunks - 1) {
		//pthread_mutex_unlock(_mutex);
		_mm_free(chunk);
		return (uint32_t)-1;
	}
	uint64_t start_lsn = _starting_lsns[chunk_num];
	uint64_t end_lsn = _starting_lsns[chunk_num + 1];
	base_lsn = start_lsn;

	uint64_t fstart = start_lsn / 512 * 512; 
	uint64_t fend = end_lsn;
	if (fend % 512 != 0)
		fend = fend / 512 * 512 + 512; 
	assert(fend - fstart < _chunk_size);

	string path = _file_name + ".0";
    int fd;
    if (g_ramdisk)
        fd = open(path.c_str(), O_RDONLY);
    else
	    fd = open(path.c_str(), O_DIRECT | O_RDONLY);
	lseek(fd, fstart, SEEK_SET);
	
//	printf("chunk_num=%d / %d, start_lsn=%ld, end_lsn=%ld, fstart=%ld, fend=%ld\n", 
//		chunk_num, _num_chunks, start_lsn, end_lsn, fstart, fend);
	
	M_ASSERT(chunk, "start_lsn=%ld, end_lsn=%ld, fstart=%ld, fend=%ld\n", 
		start_lsn, end_lsn, fstart, fend);
	uint32_t sz = read(fd, chunk, fend - fstart);
	// TODO. don't know why it can be off by 512 bytes
	M_ASSERT(sz == fend - fstart || sz == fend - fstart - 512, 
		"chunk_num=%d. sz=%d, fstart=%ld, fend=%ld, fend-fstart=%ld", 
		chunk_num, sz, fstart, fend, fend-fstart);

	chunk = chunk + start_lsn % 512;
	//if (_logger_id == 3)
	//	pthread_mutex_unlock(_mutex);
	size = end_lsn - start_lsn; 
	return chunk_num;
#else
	assert(false);
	return 0;
#endif

}
	
void
LogManager::return_log_chunk(char * buffer, uint32_t chunk_num)
{
	uint64_t start_lsn = _starting_lsns[chunk_num];
	//uint64_t end_lsn = _starting_lsns[chunk_num + 1];
	char * chunk = buffer - start_lsn % 512;
	//delete chunk;
	_mm_free(chunk);
}

void 
LogManager::set_gc_lsn(uint64_t lsn)
{
	*_gc_lsn[GET_THD_ID] = lsn;
}
