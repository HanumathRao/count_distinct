/*
* count_distinct.c - alternative to COUNT(DISTINCT ...)
* Copyright (C) Tomas Vondra, 2013
*
*/

#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>

#include "postgres.h"
#include "utils/datum.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "nodes/execnodes.h"
#include "access/tupmacs.h"
#include "utils/pg_crc.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* if set to 1, the table resize will be profiled */
#define DEBUG_PROFILE       1
#define DEBUG_HISTOGRAM     1   /* prints bucket size histogram */

#if DEBUG_PROFILE
#define MAX_HISTOGRAM_STEPS 1024
    static int steps_histogram[MAX_HISTOGRAM_STEPS];
#endif

#if (PG_VERSION_NUM >= 90000)

#define GET_AGG_CONTEXT(fname, fcinfo, aggcontext)  \
    if (! AggCheckCallContext(fcinfo, &aggcontext)) {   \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
    }

#define CHECK_AGG_CONTEXT(fname, fcinfo)  \
    if (! AggCheckCallContext(fcinfo, NULL)) {   \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
    }
    
#elif (PG_VERSION_NUM >= 80400)

#define GET_AGG_CONTEXT(fname, fcinfo, aggcontext)  \
    if (fcinfo->context && IsA(fcinfo->context, AggState)) {  \
        aggcontext = ((AggState *) fcinfo->context)->aggcontext;  \
    } else if (fcinfo->context && IsA(fcinfo->context, WindowAggState)) {  \
        aggcontext = ((WindowAggState *) fcinfo->context)->wincontext;  \
    } else {  \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
        aggcontext = NULL;  \
    }

#define CHECK_AGG_CONTEXT(fname, fcinfo)  \
    if (!(fcinfo->context &&  \
        (IsA(fcinfo->context, AggState) ||  \
        IsA(fcinfo->context, WindowAggState))))  \
    {  \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
    }
    
#else

#define GET_AGG_CONTEXT(fname, fcinfo, aggcontext)  \
    if (fcinfo->context && IsA(fcinfo->context, AggState)) {  \
        aggcontext = ((AggState *) fcinfo->context)->aggcontext;  \
    } else {  \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
        aggcontext = NULL;  \
    }

#define CHECK_AGG_CONTEXT(fname, fcinfo)  \
    if (!(fcinfo->context &&  \
        (IsA(fcinfo->context, AggState))))  \
    {  \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
    }

/* backward compatibility with 8.3 (macros copied mostly from src/include/access/tupmacs.h) */

#if SIZEOF_DATUM == 8

#define fetch_att(T,attbyval,attlen) \
( \
    (attbyval) ? \
    ( \
        (attlen) == (int) sizeof(Datum) ? \
            *((Datum *)(T)) \
        : \
      ( \
        (attlen) == (int) sizeof(int32) ? \
            Int32GetDatum(*((int32 *)(T))) \
        : \
        ( \
            (attlen) == (int) sizeof(int16) ? \
                Int16GetDatum(*((int16 *)(T))) \
            : \
            ( \
                AssertMacro((attlen) == 1), \
                CharGetDatum(*((char *)(T))) \
            ) \
        ) \
      ) \
    ) \
    : \
    PointerGetDatum((char *) (T)) \
)
#else                           /* SIZEOF_DATUM != 8 */

#define fetch_att(T,attbyval,attlen) \
( \
    (attbyval) ? \
    ( \
        (attlen) == (int) sizeof(int32) ? \
            Int32GetDatum(*((int32 *)(T))) \
        : \
        ( \
            (attlen) == (int) sizeof(int16) ? \
                Int16GetDatum(*((int16 *)(T))) \
            : \
            ( \
                AssertMacro((attlen) == 1), \
                CharGetDatum(*((char *)(T))) \
            ) \
        ) \
    ) \
    : \
    PointerGetDatum((char *) (T)) \
)
#endif   /* SIZEOF_DATUM == 8 */

#define att_addlength_pointer(cur_offset, attlen, attptr) \
( \
    ((attlen) > 0) ? \
    ( \
        (cur_offset) + (attlen) \
    ) \
    : (((attlen) == -1) ? \
    ( \
        (cur_offset) + VARSIZE_ANY(attptr) \
    ) \
    : \
    ( \
        AssertMacro((attlen) == -2), \
        (cur_offset) + (strlen((char *) (attptr)) + 1) \
    )) \
)

#define att_align_nominal(cur_offset, attalign) \
( \
    ((attalign) == 'i') ? INTALIGN(cur_offset) : \
     (((attalign) == 'c') ? (long) (cur_offset) : \
      (((attalign) == 'd') ? DOUBLEALIGN(cur_offset) : \
       ( \
            AssertMacro((attalign) == 's'), \
            SHORTALIGN(cur_offset) \
       ))) \
)
    
#endif

#define COMPUTE_CRC32(hash, value, length) \
    INIT_CRC32(hash); \
    COMP_CRC32(hash, value, length); \
    FIN_CRC32(hash);

/* hash table parameters */
#define HTAB_INIT_SIZE      16       /* initial hash table size (only 16 elements) - keep 2^N */
#define HTAB_GROW_THRESHOLD 0.75     /* bucket growth step (number of elements, not bytes) */
#define HTAB_GROW_FACTOR    2        /* how much to grow the table? (size * grow_factor) */
#define HTAB_NEIGHBOURS     8        /* try to place it within this number of elements first */
#define HASH_PROBING_STEP   13       /* and then try this step to minimize clustering (this
                                        needs to be relatively prime to INIT_SIZE) */

static int primes = {101, 107, 113, 127, 137, 151, 163, 179};
                                        
/* Structures used to keep the data - bucket and hash table. */

/* A single value in the hash table, along with it's 32-bit hash (so that we
 * don't need to compute it over and over).
 * 
 * The value is stored inline - for example to store int32 (4B) value, the palloc
 * would look like this
 * 
 *     palloc(offsetof(hash_element_t, value) + sizeof(int32))
 * 
 * and similarly for other data types. The important thing is that the structure
 * needs to be fixed length so that buckets can contain an array of items. So for
 * varlena types, there needs to be a pointer (either 4B or 8B) with value stored
 * somewhere else.
 * 
 * See HASH_ELEMENT_SIZE/GET_ELEMENT for evaluation of the element size and
 * accessing a particular item in a bucket.
 * 
 * TODO Is it really efficient to keep the hash, or should we save a bit of memory
 * and recompute the hash every time?
 */

typedef struct hash_info_t {
    uint32 is_used  : 1;    /* info whether the element is used or not */
    uint32 hash     : 31;   /* hash 2^31 buckets should be enough I guess ;-) */
} hash_info_t;

typedef struct hash_element_t {
    
    hash_info_t info;   /* used + 31-bit hash of this particular element */
    char    value[1];   /* the value itself (trick: fixed-length will be in-place) */
    
} hash_element_t;

/* A hash table - a collection of buckets. */
typedef struct hash_table_t {
    
    uint16  length;     /* length of the value (depends on the actual data type) */
    uint32  nelements;  /* number of available elements (HTAB_INIT_SIZE) */
    uint32  nitems;     /* current number of elements of the hash table */
    
    /* a linear hash table with (nelements) elements - array of hash_element_t */
    char   *elements;
    
} hash_table_t;

#define HASH_ELEMENT_SIZE(length) \
    (length + offsetof(hash_element_t, value))

#define GET_ELEMENT(elements, index, length) \
    (hash_element_t*)(elements + index * HASH_ELEMENT_SIZE(length))

/* prototypes */
PG_FUNCTION_INFO_V1(count_distinct_append_int32);
PG_FUNCTION_INFO_V1(count_distinct_append_int64);
PG_FUNCTION_INFO_V1(count_distinct);

Datum count_distinct_append_int32(PG_FUNCTION_ARGS);
Datum count_distinct_append_int64(PG_FUNCTION_ARGS);
Datum count_distinct(PG_FUNCTION_ARGS);

static bool add_element_to_table(hash_table_t * htab, char * value);
static bool element_exists_in_bucket(hash_table_t * htab, uint32 hash, char * value, uint32 *idx);
static void resize_hash_table(hash_table_t * htab);
static hash_table_t * init_hash_table(int length);

#if DEBUG_PROFILE
static void print_table_stats(hash_table_t * htab);
#endif

Datum
count_distinct_append_int32(PG_FUNCTION_ARGS)
{
    
    int32          value;
    hash_table_t  *htab;
    
    MemoryContext oldcontext;
    MemoryContext aggcontext;
    
    /* OK, we do want to skip NULL values altogether */
    if (PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0))
            PG_RETURN_NULL();
        else
            /* if there already is a state accumulated, don't forget it */
            PG_RETURN_DATUM(PG_GETARG_DATUM(0));
    }

    GET_AGG_CONTEXT("count_distinct_append_int32", fcinfo, aggcontext);

    oldcontext = MemoryContextSwitchTo(aggcontext);
        
    if (PG_ARGISNULL(0)) {
        htab = init_hash_table(sizeof(int32));
    } else {
        htab = (hash_table_t *)PG_GETARG_POINTER(0);
    }
    
    /* we can be sure the value is not null (see the check above) */
    
    /* prepare the element structure (hash + value) */
    value = PG_GETARG_INT32(1);
    
    /* add the value into the hash table, check if we need to resize the table */
    add_element_to_table(htab, (char*)&value);
    
    if (htab->nitems > HTAB_GROW_THRESHOLD * htab->nelements) {
        
        /* do we need to increase the hash table size? only if we have too many elements in a bucket
         * (on average) and the table is not too large already */
        resize_hash_table(htab);
        
    }

    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(htab);

}

Datum
count_distinct_append_int64(PG_FUNCTION_ARGS)
{

    int64          value;
    hash_table_t  *htab;
    
    MemoryContext oldcontext;
    MemoryContext aggcontext;
    
    /* OK, we do want to skip NULL values altogether */
    if (PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0))
            PG_RETURN_NULL();
        else
            /* if there already is a state accumulated, don't forget it */
            PG_RETURN_DATUM(PG_GETARG_DATUM(0));
    }

    GET_AGG_CONTEXT("count_distinct_append_int64", fcinfo, aggcontext);

    oldcontext = MemoryContextSwitchTo(aggcontext);
        
    if (PG_ARGISNULL(0)) {
        htab = init_hash_table(sizeof(int64));
    } else {
        htab = (hash_table_t *)PG_GETARG_POINTER(0);
    }
    
    /* we can be sure the value is not null (see the check above) */
    
    /* prepare the element structure (hash + value) */
    value = PG_GETARG_INT64(1);
    
    /* add the value into the hash table, check if we need to resize the table */
    add_element_to_table(htab, (char*)&value);
    
    if (htab->nitems > HTAB_GROW_THRESHOLD * htab->nelements) {
        /* do we need to increase the hash table size? only if we have too many elements in a bucket
         * (on average) and the table is not too large already */
        resize_hash_table(htab);
    }
    
    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(htab);

}

Datum
count_distinct(PG_FUNCTION_ARGS)
{
    
    hash_table_t * htab;
    
    CHECK_AGG_CONTEXT("count_distinct", fcinfo);
    
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    
    htab = (hash_table_t *)PG_GETARG_POINTER(0);

#if DEBUG_PROFILE
    print_table_stats(htab);
#endif
    
    PG_RETURN_INT64(htab->nitems);

}

static
bool add_element_to_table(hash_table_t * htab, char * value) {
    
    uint32 hash;
    uint32 idx;

    hash_element_t * element;
    
    /* compute the hash and keep only the first 4 bytes */
    COMPUTE_CRC32(hash, value, htab->length);

    /* we want only 31 bits of the hash */
    hash = hash & (0xFFFFFFFF >> 1);
    
    /* the ideal position within the array */
    idx = hash % htab->nelements;
    
    /* not it's not, so let's add it to the hash table */
    if (! element_exists_in_bucket(htab, hash, value, &idx)) {
        
        /* get the element at the free index */
        element = GET_ELEMENT(htab->elements, idx, htab->length);
        
        element->info.is_used = 1;
        element->info.hash = hash;
        memcpy(&element->value, value, htab->length);
        
        htab->nitems += 1;
        
        return TRUE;
    
    }
    
    return FALSE;

}

static
bool element_exists_in_bucket(hash_table_t * htab, uint32 hash, char * value, uint32 * idx) {
    
    /* FIXME extend this to return the pointer to the free space (or NULL), so that we don't need to search for it again */
    
    int i;
    hash_element_t * element;
    int index = *idx;
    int probing_step = 0;
    
    /* try to search in the neighbourhood first (simple linear probing with step = 1) */
    for (i = 0; i <= HTAB_NEIGHBOURS; i++) {
        
        element = GET_ELEMENT(htab->elements, index, htab->length);
        
        /* this element is not used at all, so we know the element is not in the hash table */
        if (element->info.is_used == 0) {

#if DEBUG_PROFILE
            steps_histogram[i]++;
#endif
            
            *idx = index;
            return FALSE;
            
        /* the hashes match, so let's investigate the value */
        } else if (element->info.hash == hash) {
            if (memcmp(element->value, value, htab->length) == 0) {
                /* the hash and values match, so the value is in the table */

#if DEBUG_PROFILE
                steps_histogram[i]++;
#endif

                return TRUE;
            }
        }
        
        index = (index + 1) % htab->nelements;
        
    }
    
    /* OK, we've searched in the neighbourhood, and we haven't found the value -> let's search
     * the whole table with HASH_PROBING_STEP - we know there are htab->nitems values, and we're
     * using prime step (relatively to nelements), so we're going either to hit all the elements
     * or at least one empty value in htab->nitems steps.
     */
    
    /* reset the index pointer */
    index = *idx;
    
    probing_step = primes[(hash >> 8) % 8];
    
    for (i = 0; i < htab->nitems; i++) {
        
        /* we can skip to the first 'far' element, because index=0 was inspected above */
        index = (index + HASH_PROBING_STEP) % htab->nelements;
        
        element = GET_ELEMENT(htab->elements, index, htab->length);
        
        /* this element is not used at all, so we know the element is not in the hash table */
        if (element->info.is_used == 0) {

#if DEBUG_PROFILE
            steps_histogram[(i < MAX_HISTOGRAM_STEPS) ? i : (MAX_HISTOGRAM_STEPS-1)]++;
#endif
            *idx = index;
            return FALSE;
            
        /* the hashes match, so let's investigate the value */
        } else if (element->info.hash == hash) {
            if (memcmp(element->value, value, htab->length) == 0) {
                /* the hash and values match, so the value is in the table */
#if DEBUG_PROFILE
                steps_histogram[(i < MAX_HISTOGRAM_STEPS) ? i : (MAX_HISTOGRAM_STEPS-1)]++;
#endif
                return TRUE;
            }
        }
        
    }

    /* no luck, so the element is not in the hash table */
    elog(ERROR, "the table is full (elements=%d, step=%d)", htab->nelements, HASH_PROBING_STEP);
    return FALSE;
    
}

static
hash_table_t * init_hash_table(int length) {
    
    hash_table_t * htab = (hash_table_t *)palloc(sizeof(hash_table_t));
    
    htab->length    = length;
    htab->nelements = HTAB_INIT_SIZE;
    htab->nitems = 0;
        
    /* the memory is zeroed */
    htab->elements = palloc0(HTAB_INIT_SIZE * HASH_ELEMENT_SIZE(htab->length));

    return htab;
    
}

static
void resize_hash_table(hash_table_t * htab) {
    
    int i;
    char * elements;
    hash_element_t * element;
    
#if DEBUG_PROFILE
    struct timeval start_time, end_time;
    
    print_table_stats(htab);
    gettimeofday(&start_time, NULL);
#endif
    
    /* basic sanity checks */
    assert(htab != NULL);
    
    /* double the hash table size */
    htab->nitems = 0; /* we'll essentially re-add all the elements, which will set this back */
    
    /* but zero the new buckets, just to be sure (the size is in bytes) */
    elements = htab->elements;
    htab->elements = palloc0(HASH_ELEMENT_SIZE(htab->length) * (int)(htab->nelements * HTAB_GROW_FACTOR));
    
    /* now let's loop through the old buckets and re-add all the elements */
    for (i = 0; i < htab->nelements; i++) {

        element = GET_ELEMENT(elements, i, htab->length);
        
        if (element->info.is_used) {
            add_element_to_table(htab, element->value);
        }
        
    }
    
    /* free the old elements */
    pfree(elements);
    
    /* finally, let's update the number of buckets */
    htab->nelements = (int)(htab->nelements * HTAB_GROW_FACTOR);
    
#if DEBUG_PROFILE

    gettimeofday(&end_time, NULL);
    print_table_stats(htab);
    
    elog(WARNING, "RESIZE: items=%d [%d => %d] duration=%ld us",
                htab->nitems, htab->nelements/HTAB_GROW_FACTOR, htab->nelements,
                (end_time.tv_sec - start_time.tv_sec)*1000000 + (end_time.tv_usec - start_time.tv_usec));
    
#endif
    
}

#if DEBUG_PROFILE
static 
void print_table_stats(hash_table_t * htab) {
    
    int i, s = 0, d = 0;
    
    elog(WARNING, "===== hash table stats =====");
    elog(WARNING, " items: %d", htab->nitems);
    elog(WARNING, " elements: %d", htab->nelements);
    elog(WARNING, " filled: %.2f", (100.0 * htab->nitems / htab->nelements));
       
#if DEBUG_HISTOGRAM
    
    /* now print the histogram (if enabled) */
    elog(WARNING, "--------- histogram ---------");

    for (i = 0; i < MAX_HISTOGRAM_STEPS; i++) {
        s += steps_histogram[i];
    }
        
    for (i = 0; i < MAX_HISTOGRAM_STEPS; i++) {
        if (steps_histogram[i] != 0) {
            d += steps_histogram[i];
            elog(WARNING, "%d => %d  [ %.4f%% ]  [ %.4f%% ]", i, steps_histogram[i], (100.0 * steps_histogram[i])/s, (100.0 * d)/s);
        }
    }
    
#endif
    
    elog(WARNING, "============================");
    
}
#endif