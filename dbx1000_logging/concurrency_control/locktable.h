#pragma once
#include <list>
#include <memory>
#include <sstream>
#include "config.h"
#include "row.h"
#include "txn.h"
#include "row_lock.h"
#include "row_ts.h"
#include "row_mvcc.h"
#include "row_hekaton.h"
#include "row_occ.h"
#include "row_tictoc.h"
#include "row_silo.h"
#include "row_vll.h"
#include "log.h"
#include "serial_log.h"
#include "taurus_log.h"
#include "helper.h"
#include "global.h"
#include "manager.h"
#include <emmintrin.h>
#include <nmmintrin.h>

#if USE_LOCKTABLE
#include "row.h"

#define CALC_PKEY (((uint64_t)row)/sizeof(row_t))

// assume every LOCKTABLE-based alg has 4 logger by default.
#if UPDATE_SIMD
#define G_NUM_LOGGER g_num_logger // 4 // (4) // g_num_logger
#else
#define G_NUM_LOGGER g_num_logger
#endif
// to accerlerate loops dealing with lsn_vec

class LockTableListItem {
    //int atomicLock;
public:
    bool evicted;
    uint64_t key;  // supporting only 2^62 instead of 2^63
    row_t * row;    
#if LOG_ALGORITHM == LOG_TAURUS  
//#if CC_ALG == SILO
    //uint64_t keep;
//#endif
    lsnType * lsn_vec;
    lsnType * readLV;
#elif LOG_ALGORITHM == LOG_SERIAL
    lsnType * lsn;
#endif
    LockTableListItem(uint64_t _key, row_t *_row): key(_key), row(_row) {
        evicted = false; 
#if LOG_ALGORITHM == LOG_TAURUS
/*#if CC_ALG == SILO
        keep =1;
#endif*/
        // we will initialize lsn_vec later
#if COMPRESS_LSN_LT
        lsn_vec = (lsnType*) _mm_malloc(sizeof(lsnType) * (G_NUM_LOGGER+1), ALIGN_SIZE); 
#else
#if UPDATE_SIMD
        assert(g_num_logger <= 4);
        lsn_vec = (lsnType*) _mm_malloc(sizeof(lsnType) * 4, ALIGN_SIZE); 
        readLV = (lsnType*) _mm_malloc(sizeof(lsnType) * 4, ALIGN_SIZE); 
#else
        lsn_vec = (lsnType*) _mm_malloc(sizeof(lsnType) * g_num_logger, ALIGN_SIZE); 
        readLV = (lsnType*) _mm_malloc(sizeof(lsnType) * g_num_logger, ALIGN_SIZE); 
#endif
#endif
#elif LOG_ALGORITHM == LOG_SERIAL
        lsn = (lsnType*) _mm_malloc(sizeof(lsnType), ALIGN_SIZE); 
        *lsn = 0;
#endif
        // TODO: later we need to pre-malloc enough space for the LTI's.
        // do not initialize lsn_vec here
    }
    ~LockTableListItem(){
#if LOG_ALGORITHM == LOG_TAURUS
        _mm_free(lsn_vec);
        _mm_free(readLV);
#endif
    }
};

struct LockTableValue {
    int atomicLock;
    list<LockTableListItem*> li;
    LockTableValue(): atomicLock(0) {}
};


class LockTable
{
    public:

// From Numerical Recipes, 3rd Edition
inline uint64_t uint64hash(uint64_t key)
{
  return key & (locktable_size-1);
  key = key * 0x369dea0f31a53f85 + 0x255992d382208b61;

  key ^= key >> 21;
  key ^= key << 37;
  key ^= key >>  4;

  key *= 0x422e19e1d95d2f0d;

  key ^= key << 20;
  key ^= key >> 41;
  key ^= key <<  5;

  return key;
}
        //uint32_t evictLock;
        LockTableValue * hashMap;
        static LockTable& getInstance()
        {
            static LockTable instance; // Guaranteed to be destroyed.
                                  // Instantiated on first use.
            return instance;
        }
        static void printLockTable()
        {
            LockTable & lt = getInstance();
            for(uint i=0; i<lt.locktable_size; i++)
            {
                LockTableValue &ltv = lt.hashMap[i];
                printf("lt[%d] %d %lu {", i, ltv.atomicLock, ltv.li.size());
                for(auto lti = ltv.li.begin(); lti != ltv.li.end(); lti++)
                {
                    printf("(%d, %" PRIu64 ", %" PRIu64 ", [", (*lti)->evicted, (*lti)->key, (uint64_t)(*lti)->row);
#if LOG_ALGORITHM == LOG_TAURUS
                    for(uint j=0; j<G_NUM_LOGGER; j++)
                    {
                        printf("%" PRIu64 ",", (uint64_t)((*lti)->lsn_vec[j]));
                    }
#endif
                    printf("]), ");
                }
                printf("}\n");
            }
        }

        bool inline try_evict_item(LockTableListItem* & lti)
        {
#if LOG_ALGORITHM == LOG_TAURUS
#if COMPRESS_LSN_LT
            for(uint64_t i=1; i<lti->lsn_vec[0]; i++)
            {
                uint64_t index = (lti->lsn_vec[i]) & 31;
                uint64_t lsn_i = (lti->lsn_vec[i]) >> 5;
                if(lsn_i + g_locktable_evict_buffer > log_manager->_logger[index]->get_persistent_lsn()){
                    return false;
                }
            }
#else
            for(uint32_t i=0; i<G_NUM_LOGGER; i++) // place of canEvict(), check if locktable item's lsn is smaller than psn
                if(lti->lsn_vec[i] + g_locktable_evict_buffer > log_manager->_logger[i]->get_persistent_lsn())
                {
                    return false;
                }
#endif
#elif LOG_ALGORITHM == LOG_SERIAL
            if(lti->lsn[0] > log_manager->_logger[0]->get_persistent_lsn())
                return false;
#endif
            lti->evicted = true; // execute the eviction
            return true;
        }

        void inline try_evict_locktable_bucket(LockTableValue & ltv)  // should be inside the lock session
        {
            for(list<LockTableListItem*>::iterator it = ltv.li.begin(); it != ltv.li.end(); )
                {
                    //LockTableListItem & lti = **it;
#if CC_ALG == NO_WAIT
                    if((*it)->evicted || (*it)->row->manager->lock_type != LOCK_NONE_T)
#elif CC_ALG == SILO && ATOMIC_WORD
                    //if((*it)->evicted || ((*it)->keep || ((*it)->row->manager->_tid_word & LOCK_BIT)))
                    if((*it)->evicted || ((*it)->row->manager->_tid_word & LOCK_BIT))
#else
                    assert(false); // not implemented
#endif
                    {
                        it++;
                        continue;  // we do not evict items that hold locks
                    }
                    if(try_evict_item(*it))
                    {
                        //delete *it; // release the memory of lti
                        //it = ltv.li.erase(it);
                        (*it)->evicted = true;
                    }
                    //try_evict_item(*it);
                    it++;
                }
        }

        void try_evict()
        {
            for(uint64_t i=0; i<locktable_size; i++)
            {
                LockTableValue &ltv = hashMap[i];
                if(ATOM_CAS(ltv.atomicLock, 0, 1)) // otherwise we skip this
                {
                    try_evict_locktable_bucket(ltv);
                    //COMPILER_BARRIER
                    ltv.atomicLock = 0;  // release the lock
                }
            }
        }

        bool release_lock(row_t *row, access_t type, txn_man * txn, char * data, lsnType * lsn_vec, lsnType * max_lsn, RC rc_in)
        {
            /*
            stringstream ss;
            ss << GET_THD_ID << " Release " << row << endl;
            cout << ss.str();
            */
            INC_INT_STATS(int_debug9, 1);
            uint64_t starttime = get_sys_clock();
            uint64_t pkey = CALC_PKEY; //row->get_primary_key();
            uint64_t hashedKey = uint64hash(pkey) & (locktable_size - 1);
            LockTableValue &ltv = hashMap[hashedKey];
            //bool notfound = true;
            #if CC_ALG == SILO
            // do quick lock release
            if(rc_in==Abort)
            {
                row->manager->release();
                // we do not have to change the lsn_vector if it is released with the txn being aborted.
                return true;
            }
            #endif
            while(!ATOM_CAS(ltv.atomicLock, 0, 1)) PAUSE;
            uint64_t afterCAS = get_sys_clock();
            INC_INT_STATS(time_debug8, afterCAS - starttime);
            uint64_t counter = 0;
            for(list<LockTableListItem*>::iterator it = ltv.li.begin(); it != ltv.li.end(); it++)
            {
                counter ++;
                if((*it)->key == pkey && !(*it)->evicted)
                {
                    //notfound = false;
#if LOG_ALGORITHM == LOG_TAURUS
                    if(rc_in != Abort)  // update the value
                    {
#if COMPRESS_LSN_LT
                        assert(false);
                        uint32_t lsnVecHash[G_NUM_LOGGER];
                        memset(lsnVecHash, 0, sizeof(lsnVecHash));
                        for(uint64_t i=1; i<(*it)->lsn_vec[0]; i++)
                        {
                            uint64_t index = ((*it)->lsn_vec[i]) & 31;
                            uint64_t lsn_i = ((*it)->lsn_vec[i]) >> 5;
                            if(lsn_i < lsn_vec[index])
                                (*it)->lsn_vec[i] = (lsn_vec[index] << 5) | index;
                            lsnVecHash[index] = 1;
                        }
                        // add other constraint
                        for(uint64_t i=0; i<G_NUM_LOGGER; i++)
                            if(!lsnVecHash[i] && lsn_vec[i] > 0)
                            {
                                (*it)->lsn_vec[(*it)->lsn_vec[0]] = (lsn_vec[i] << 5) | i;
                                (*it)->lsn_vec[0]++;
                            }
#else                   // when releasing the lock, only update writeLV.
                        if(type==WR)
                        {
#if UPDATE_SIMD
                            __m128i * LV = (__m128i*) lsn_vec;
                            __m128i * writeLV = (__m128i*) (*it)->lsn_vec;
                            *writeLV = _mm_max_epu32(*LV, *writeLV);
#else
                            for(uint32_t i=0; i<G_NUM_LOGGER; i++)
                                if((*it)->lsn_vec[i] < lsn_vec[i])
                                    (*it)->lsn_vec[i] = lsn_vec[i];
#endif
                        }
                        else
                        {
#if UPDATE_SIMD
                            __m128i * LV = (__m128i*) lsn_vec;
                            __m128i * readLV = (__m128i*)(*it)->readLV;
                            *readLV = _mm_max_epu32(*LV, *readLV);
#else
                            for(uint32_t i=0; i<G_NUM_LOGGER; ++i)
                                if((*it)->readLV[i] < lsn_vec[i])
                                    (*it)->readLV[i] = lsn_vec[i];
#endif
                        }
                        
#endif
/*
#if CC_ALG == SILO
                        (*it)->keep = 0;
#endif*/
                    }    
#elif LOG_ALGORITHM == LOG_SERIAL
                    if(rc_in != Abort && (*it)->lsn[0] < *max_lsn)
                        (*it)->lsn[0] = *max_lsn;
#endif                
                        // already latched, no need to atomically update
                        //while(((*it)->lsn_vec[i] < lsn_vec[i])&&(!ATOM_CAS((*it)->lsn_vec[i], (*it)->lsn_vec[i], lsn_vec[i])));
#if CC_ALG == SILO 
                    volatile uint64_t * v = &(row->manager->_tid_word);
                    assert(*v & LOCK_BIT);
                    *v = *v & (~LOCK_BIT);
// for SILO we already done the memory update in silo_validate
#else
                    row->return_row(type, txn, data);
#endif
                    /*
                    if (false)  // we do not evict here
                    //if(row->manager->lock_type == LOCK_NONE_T)
                    // But we cannot do it here. It would be very inefficient. We add it to a recent accessed queue.
                    // we might want a priority queue.
                    {
                        delete *it; // release the memory of lti
                        ltv.li.erase(it);
                        ltv.atomicLock = 0;
                        return true;
                    }
                    */
                    //COMPILER_BARRIER
                    ltv.atomicLock = 0;
                    INC_INT_STATS(time_debug9, get_sys_clock() - afterCAS);
                    INC_INT_STATS(int_debug10, counter);
                    return false;
                    // break;  // anyway we have found the key
                }
            }
            
//#if CC_ALG == NO_WAIT
            assert(false); // currently no evict will fail.
//#else
//            ltv.atomicLock = 0;
//#endif            
            INC_INT_STATS(time_debug9, get_sys_clock() - afterCAS);
            INC_INT_STATS(int_debug10, counter);
            //ltv.atomicLock = 0;
            //return false;
            // orig_r->return_row(type, txn, newdata);

            /*
            uint64_t pkey = CALC_PKEY //row->get_primary_key();
            uint64_t pkey_wo_lock = pkey<<1;
            uint64_t pkey_w_lock = (pkey<<1)+1;
            uint64_t orihashedKey = uint64hash(pkey) & (locktable_size-1);
            uint64_t hashedKey = orihashedKey;
            do {
                LockTableListItem &lti = hashMap[hashedKey];
                while(lti.key == pkey_w_lock); // wait if found busy, should be quick
                if(ATOM_CAS(lti.key, pkey_wo_lock, 0))
                {
                    // found the key and released
                    break;
                }
                hashedKey++;
                hashedKey &= locktable_size - 1;
            } while(hashedKey!=orihashedKey);
            return RCOK;
            */
           return false;
        }

        RC get_row(row_t * row, access_t type, txn_man * txn, char *& data, lsnType * lsn_vec, lsnType * max_lsn, bool tryLock=false, uint64_t tid=UINT64_MAX, bool tryOnce=false)
        // if tryLock is true then it will return immediately if the hash table item is locked.
        {
            /*
            stringstream ss;
            ss << GET_THD_ID << " Lock " << row << endl;
            cout << ss.str();
            */
            INC_INT_STATS(int_debug5, 1);  // the number of get_row
            uint64_t starttime = get_sys_clock();
            RC ret = RCOK;
            uint64_t pkey = CALC_PKEY;//row->get_primary_key();
            uint64_t hashedKey = uint64hash(pkey) & (locktable_size - 1);
            LockTableValue &ltv = hashMap[hashedKey];
            //bool notfound = true;
            if(tryLock && ltv.atomicLock == 1)
                return Abort;
#if CC_ALG == SILO
            // pre-abort
            uint64_t v = row->manager->_tid_word;
            if(v & LOCK_BIT)
                return Abort;
            //if(!ATOM_CAS(row->manager->_tid_word, v, (v | LOCK_BIT)))
            //    return Abort;
            //LockTableListItem *lti = &(*ltv.li.front());
            //if(!lti->evicted && lti->key == pkey && )
            if(tryOnce)
            {
                if(!ATOM_CAS(ltv.atomicLock, 0, 1))
                    return Abort;
                // otherwise we have got the lock
            }
            else
            {
                while(!ATOM_CAS(ltv.atomicLock, 0, 1))
                {
                    //if(row->manager->_tid_word != tid) // check both if locked and if not modified at the same time.
                    if(row->manager->_tid_word & LOCK_BIT) // do not perform write tid check
                        return Abort;
                    PAUSE
                }
            }
            
#else
            lock_t lt = (type == RD || type == SCAN)? LOCK_SH_T : LOCK_EX_T;
            if(row->manager->conflict_lock(lt, row->manager->lock_type)) // do not perform write tid check
                return Abort;
            if(tryOnce)
            {
                if(!ATOM_CAS(ltv.atomicLock, 0, 1))
                    return Abort;
                // otherwise we have got the lock
            }
            else{
                while(!ATOM_CAS(ltv.atomicLock, 0, 1))
                {
                    if(row->manager->conflict_lock(lt, row->manager->lock_type)) // do not perform write tid check
                            return Abort;
                    PAUSE
                }
            }
#endif
            uint64_t afterCAS = get_sys_clock();
            INC_INT_STATS(time_debug0, afterCAS - starttime);
            INC_INT_STATS(int_num_get_row, 1);
            INC_INT_STATS(int_locktable_volume, ltv.li.size());
            uint32_t counter = 0;
            for(list<LockTableListItem*>::iterator it = ltv.li.begin(); it != ltv.li.end(); it++)
            {
                counter ++;
                auto &lti = *it;
                if(lti->key == pkey)
                {
#if CC_ALG == SILO
                    if(data==NULL)
                    {
                        // we do not need sync operations
                        volatile uint64_t * v = &(row->manager->_tid_word);
                        if(*v & LOCK_BIT)
                        {
                            /*stringstream ss;
                            ss << GET_THD_ID << " Abort " << row << endl;
                            cout << ss.str();*/
                            ret = Abort;
                        }
                        else {
                            *v = *v | LOCK_BIT;
                            ret = RCOK;
                        }
                    }
                    else
#endif
                    ret = lti->row->get_row(type, txn, data);
                    INC_INT_STATS(time_debug4, get_sys_clock() - afterCAS);
                    lti->evicted = false; // just in case lti was previously evicted; It's okay to re-use the previous lsn_vec
#if LOG_ALGORITHM == LOG_TAURUS
/*#if CC_ALG == SILO
                    lti->keep = 1;                    
#endif*/
#if COMPRESS_LSN_LT
                    assert(false);
                    for(uint64_t i=1; i<lti->lsn_vec[0]; i++)
                    {
                        uint64_t index = (lti->lsn_vec[i]) & 31;
                        uint64_t lsn_i = (lti->lsn_vec[i]) >> 5;
                        if(lsn_i > lsn_vec[index]){
                            lsn_vec[index] = lsn_i;
                        }
                    }
#else
                        
                        if(type == WR)
                        {
                            //#pragma simd
                            //#pragma vector aligned
#if UPDATE_SIMD
                            __m128i * readLV = (__m128i*)lti->readLV;
                            __m128i * writeLV = (__m128i*)lti->lsn_vec;
                            __m128i * LV = (__m128i*) lsn_vec;
                            *LV = _mm_max_epu32(*LV, _mm_max_epu32(*readLV, *writeLV));
#else
                            lsnType * readLV = lti->readLV;
                            lsnType * writeLV = lti->lsn_vec;
                            for(uint32_t i=0; i<G_NUM_LOGGER; ++i)
                            {
                                auto readLVI = readLV[i];
                                auto writeLVI = writeLV[i];
                                auto maxLVI = readLVI > writeLVI ? readLVI : writeLVI;
                                if(maxLVI > lsn_vec[i])
                                    lsn_vec[i] = maxLVI;
                            }
#endif
                        }
                        else
                        {
                            //#pragma simd
                            //#pragma vector aligned
#if UPDATE_SIMD
                            __m128i * writeLV = (__m128i*)lti->lsn_vec;
                            __m128i * LV = (__m128i*) lsn_vec;
                            *LV = _mm_max_epu32(*LV, *writeLV);
#else
                            lsnType * writeLV = lti->lsn_vec;
                            for(uint32_t i=0; i<G_NUM_LOGGER; ++i)
                            {
                                auto writeLVI = writeLV[i];
                                if(writeLVI > lsn_vec[i])
                                    lsn_vec[i] = writeLVI;
                            }
#endif
                        }
                    
#endif
#elif LOG_ALGORITHM == LOG_SERIAL
                    if(lti->lsn[0] > *max_lsn) 
                        *max_lsn = lti->lsn[0];
#endif
                    //notfound = false;
                    //COMPILER_BARRIER
                    ltv.atomicLock = 0;
                    INC_INT_STATS(int_debug4, counter);
                    INC_INT_STATS(time_debug1, get_sys_clock() - afterCAS);
                    return ret; // assuming there is only one
                }
            }
            INC_INT_STATS(int_debug6, 1);
            uint64_t afterSearch = get_sys_clock();
            INC_INT_STATS(int_debug4, counter);
            INC_INT_STATS(time_debug1, afterSearch - afterCAS);
            // try to use previously evicted items
            for(list<LockTableListItem*>::iterator it = ltv.li.begin(); it != ltv.li.end(); it++)
            {
                 auto &lti = *it;
#if CC_ALG == NO_WAIT
                if(lti->evicted || (lti->row->manager->lock_type==LOCK_NONE_T && try_evict_item(lti))) // we do not need to actually set 'lti->evicted = true' here.
#elif CC_ALG == SILO && ATOMIC_WORD
                //if(lti->evicted || (lti->keep == 0 && (lti->row->manager->_tid_word & LOCK_BIT)==0 && try_evict_item(lti))) // comment same as above
                if(lti->evicted || ((lti->row->manager->_tid_word & LOCK_BIT)==0 && try_evict_item(lti))) // comment same as above
#else
                assert(false); // not implemented
#endif
                {
                    lti->key = pkey;
                    lti->row = row;
                    lti->evicted = false;
                    row->_lti_addr = (void*) &(*lti);
                    
#if LOG_ALGORITHM == LOG_TAURUS
//#if CC_ALG == SILO
//                    lti->keep=1;
//#endif
#if COMPRESS_LSN_LT
                    lti->lsn_vec[0] = 1; // starting point: the lsn vector is empty.
#else
#if UPDATE_SIMD
                    for(uint32_t i=0; i<G_NUM_LOGGER; ++i)
                    {
                        // do not need to atomic latch here.
                        lti->lsn_vec[i] = (lsnType) log_manager->_logger[i]->get_persistent_lsn();
                    }
                    __m128i * writeLV = (__m128i*) lti->lsn_vec;
                    __m128i * readLV = (__m128i*) lti->readLV;
                    __m128i * LV = (__m128i*) lsn_vec;
                    * readLV = * LV;
                    * LV = _mm_max_epu32(*LV, *writeLV);
#else
                    for(uint32_t i=0; i<G_NUM_LOGGER; ++i)
                    {
                        // do not need to atomic latch here.
                        lti->lsn_vec[i] = (lsnType) log_manager->_logger[i]->get_persistent_lsn();
                        lti->readLV[i] = lti->lsn_vec[i];
                        if(lti->lsn_vec[i] > lsn_vec[i])
                            lsn_vec[i] = lti->lsn_vec[i];
                        // TODO: SIMD here
                    }
#endif
#endif
#endif

#if CC_ALG == SILO
                    if(data==NULL)
                    {
                        // we do not need sync operations
                        volatile uint64_t * v = &(row->manager->_tid_word);
                        if(*v & LOCK_BIT)
                        {
                            /*stringstream ss;
                            ss << GET_THD_ID << " Abort " << row << endl;
                            cout << ss.str();*/
                            ret = Abort;
                        }
                        else {
                            *v = *v | LOCK_BIT;
                            ret = RCOK;
                        }
                    }
                    else
#endif
                        ret = row->get_row(type, txn, data);
                    ltv.atomicLock = 0;
                     
                    INC_INT_STATS(time_debug2, get_sys_clock() - afterSearch);
                    return ret;
                }
                 
            }
            
            INC_INT_STATS(int_debug7, 1);
            uint64_t afterReuse = get_sys_clock();
            INC_INT_STATS(time_debug2, afterReuse - afterSearch);
            // otherwise if full
            LockTableListItem *lti = new LockTableListItem(pkey, row);
            
            ltv.li.push_front(lti);
#if CC_ALG == SILO
                    if(data==NULL)
                    {
                        // we do not need sync operations
                        volatile uint64_t * v = &(row->manager->_tid_word);
                        if(*v & LOCK_BIT)
                        {
                            /*stringstream ss;
                            ss << GET_THD_ID << " Abort " << row << endl;
                            cout << ss.str();*/
                            ret = Abort;
                        }
                        else {
                            *v = *v | LOCK_BIT;
                            ret = RCOK;
                        }
                    }
                    else
#endif
            ret = row->get_row(type, txn, data);
#if LOG_ALGORITHM == LOG_TAURUS
#if COMPRESS_LSN_LT
            lti->lsn_vec[0] = 1; // starting point: the lsn vector is empty.
#else
            for(uint32_t i=0; i<G_NUM_LOGGER; ++i)
            {
                // do not need to atomic latch here.
                lti->lsn_vec[i] = log_manager->_logger[i]->get_persistent_lsn();
                lti->readLV[i] = lti->lsn_vec[i];
                if(lti->lsn_vec[i] > lsn_vec[i])
                    lsn_vec[i] = lti->lsn_vec[i];
                // TODO: optimize the copy process here
            }
#endif
#endif
            //copy PSN to lti's lsn_vec
            //COMPILER_BARRIER
            row->_lti_addr = (void*) lti; // this must be updated after lti->lsn_vec is ready.
            ltv.atomicLock = 0;
            INC_INT_STATS(time_debug3, get_sys_clock() - afterReuse);
            return ret;
        }

        RC updateLSN(row_t * row, lsnType * lsn_vec)
        {
#if LOG_ALGORITHM == LOG_TAURUS
            for(uint32_t i=0; i<G_NUM_LOGGER; ++i)
            {
                uint64_t temp;
                LockTableListItem * lti = (LockTableListItem*) row->_lti_addr;
                if(lti == NULL)
                {
                    // do not need to atomic latch here.
                    temp = log_manager->_logger[i]->get_persistent_lsn(); 
                }
                else
                {
                    temp = lti->lsn_vec[i];
                    // this is actually correct since we do not garbage collect locktable items
                    // though this might lead to higher lsn_vec (false dependencies)
                }
                if(temp > lsn_vec[i])
                    lsn_vec[i] = temp;
            }
#endif            
            return RCOK;
        }
    private:
        uint64_t locktable_size;
        LockTable() { // assuming single thread
            //evictLock = 0;
            uint64_t table_size = g_synth_table_size / g_virtual_part_cnt;
            locktable_size = g_locktable_modifier * g_thread_cnt * g_req_per_query;
#if WORKLOAD == TPCC
            locktable_size *= 50;
#endif
            uint32_t k;
            for(k=0; k<64; k++) if(locktable_size >> k) continue; else break;
            locktable_size = 1ull<<k;
            //hashMap = (LockTableListItem *) _mm_malloc(sizeof(LockTableListItem) * locktable_size, ALIGN_SIZE);
            //new (hashMap) LockTableListItem();
            hashMap = (LockTableValue *) _mm_malloc(sizeof(LockTableValue) * locktable_size, ALIGN_SIZE);
            //std::uninitialized_fill_n(hashMap, locktable_size, LockTableValue());
            for(uint64_t i=0; i<locktable_size; i++)
            {
                new (hashMap+i) LockTableValue();
                for(uint32_t k=0; k<g_locktable_init_slots; k++)
                {
                    LockTableListItem *lti = new LockTableListItem(-1, NULL);
                    // Assumption: no pkey == -1.
                    LockTableValue & ltv = hashMap[i];
                    lti->evicted = true;
                    ltv.li.push_front(lti);
/*                    
#if LOG_ALGORITHM == LOG_TAURUS
#if COMPRESS_LSN_LT
                    lti->lsn_vec[0] = 1; // starting point: the lsn vector is empty.
#else
                    for(uint32_t i=0; i<G_NUM_LOGGER; ++i)
                    {
                        // do not need to atomic latch here.
                        lti->lsn_vec[i] = 0;
                        // TODO: optimize the copy process here
                    }
#endif
#endif
*/
                }
            }
            // need to know when this happen
            //new (hashMap) LockTableValue();
            cout << "Locktable Initialized, size " << locktable_size << ", schema table size " << table_size << endl;
        }

        // C++ 03
        // ========
        // Don't forget to declare these two. You want to make sure they
        // are unacceptable otherwise you may accidentally get copies of
        // your singleton appearing.
        // LockTable(LockTable const&);              // Don't Implement
        // void operator=(LockTable const&); // Don't implement

        // C++ 11
        // =======
        // We can use the better technique of deleting the methods
        // we don't want.
    public:
        LockTable(LockTable const&) = delete;
        void operator=(LockTable const&) = delete;
};

#endif
