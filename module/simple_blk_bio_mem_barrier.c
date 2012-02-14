/**
 * simple_blk_bio_mem_barrier.c -
 * make_request_fn which do memory read/write with barriers.
 *
 * Copyright(C) 2012, Cybozu Labs, Inc.
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#include "check_kernel.h"
#include <linux/blkdev.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/timer.h>
#include "simple_blk_bio.h"
#include "memblk_data.h"

/*******************************************************************************
 * Static data definition.
 *******************************************************************************/

/* #define PERFORMANCE_DEBUG */


struct bio_work
{
        struct bio *bio;
        struct simple_blk_dev *sdev;
        struct work_struct work;
        /* struct delayed_work dwork; */
        struct timer_list end_timer;
        
#ifdef PERFORMANCE_DEBUG
        struct timespec ts_start;
        struct timespec ts_enq1;
        struct timespec ts_deq1;
        struct timespec ts_enq2;
        struct timespec ts_deq2;
        struct timespec ts_end;
#endif
};

struct kmem_cache *bio_work_cache_ = NULL;
struct workqueue_struct *wq_io_ = NULL;
struct workqueue_struct *wq_misc_ = NULL;

/*******************************************************************************
 * Static functions definition.
 *******************************************************************************/

static void log_bi_rw_flag(struct bio *bio);

static void mdata_exec_discard(struct memblk_data *mdata, u64 block_id, unsigned int n_blocks);
static void mdata_exec_bio(struct memblk_data *mdata, struct bio *bio);

static struct memblk_data* get_mdata_from_sdev(struct simple_blk_dev *sdev);
__UNUSED static struct memblk_data* get_mdata_from_queue(struct request_queue *q);

static struct bio_work* create_bio_work(struct bio *bio, struct simple_blk_dev *sdev, gfp_t gfp_mask);
static void destroy_bio_work(struct bio_work *work);

static void bio_io_worker(struct work_struct *work);
static void bio_worker(struct work_struct *work);

/*******************************************************************************
 * Static functions definition.
 *******************************************************************************/

/**
 * For debug.
 */
static void log_bi_rw_flag(struct bio *bio)
{
        LOGd("bio bi_sector %"PRIu64" bi_size %u bi_vcnt %hu "
             "bi_rw %0lx [%s][%s][%s][%s][%s][%s].\n",
             (u64)bio->bi_sector, bio->bi_size, bio->bi_vcnt,
             bio->bi_rw, 
             (bio->bi_rw & REQ_WRITE ? "REQ_WRITE" : ""),
             (bio->bi_rw & REQ_RAHEAD? "REQ_RAHEAD" : ""),
             (bio->bi_rw & REQ_FLUSH ? "REQ_FLUSH" : ""),
             (bio->bi_rw & REQ_FUA ? "REQ_FUA" : ""),
             (bio->bi_rw & REQ_DISCARD ? "REQ_DISCARD" : ""),
             (bio->bi_rw & REQ_SECURE ? "REQ_SECURE" : ""));
}

/**
 * Currently discard just fills zero.
 */
static void mdata_exec_discard(struct memblk_data *mdata, u64 block_id, unsigned int n_blocks)
{
        unsigned int i;
        for (i = 0; i < n_blocks; i ++) {
                memset(mdata_get_block(mdata, block_id + i), 0, mdata->block_size);
        }
}

/**
 * Read from mdata.
 * CONTEXT:
 * Non-IRQ.
 */
static void mdata_exec_bio(struct memblk_data *mdata, struct bio *bio)
{
        int i;
        sector_t sector;
        u64 block_id;
        struct bio_vec *bvec;
        u8 *buffer_bio;
        unsigned int is_write;
        unsigned int n_blk;

        ASSERT(bio);

        sector = bio->bi_sector;
        block_id = (u64)sector;

        if (bio->bi_rw & REQ_DISCARD) {
                log_bi_rw_flag(bio);
                if (bio->bi_rw & REQ_SECURE) {
                        mdata_exec_discard(mdata, block_id, bio->bi_size / mdata->block_size);
                }
                return;
        }

        if (bio->bi_rw & REQ_FLUSH && bio->bi_size == 0) {
                log_bi_rw_flag(bio);
                LOGd("REQ_FLUSH\n");
                return;
        }

        if (bio->bi_rw & REQ_FUA && bio->bi_size == 0) {
                log_bi_rw_flag(bio);
                LOGd("REQ_FUA\n");
                return;
        }
        
        is_write = bio->bi_rw & REQ_WRITE;
        
        bio_for_each_segment(bvec, bio, i) {
                buffer_bio = (u8 *)__bio_kmap_atomic(bio, i, KM_USER0);
                ASSERT(bio_cur_bytes(bio) % mdata->block_size == 0);

                n_blk = bio_cur_bytes(bio) / mdata->block_size;
                if (is_write) {
                        mdata_write_blocks(mdata, block_id, n_blk, buffer_bio);
                } else {
                        mdata_read_blocks(mdata, block_id, n_blk, buffer_bio);
                }
                block_id += n_blk;
               __bio_kunmap_atomic(bio, KM_USER0);
        }
}

/**
 * Get mdata from a sdev.
 */
static struct memblk_data* get_mdata_from_sdev(struct simple_blk_dev *sdev)
{
        ASSERT(sdev);
        return (struct memblk_data *)sdev->private_data;
}

/**
 * Get mdata from a queue.
 */
static struct memblk_data* get_mdata_from_queue(struct request_queue *q)
{
        return get_mdata_from_sdev(sdev_get_from_queue(q));
}

/**
 * Create a bio_work.
 *
 * RETURN:
 * NULL if failed.
 * CONTEXT:
 * Any.
 */
static struct bio_work* create_bio_work(struct bio *bio, struct simple_blk_dev *sdev, gfp_t gfp_mask)
{
        struct bio_work *work;

        ASSERT(bio);
        ASSERT(sdev);

        work = kmem_cache_alloc(bio_work_cache_, gfp_mask);
        if (!work) {
                goto error0;
        }
        work->bio = bio;
        work->sdev = sdev;
        INIT_WORK(&work->work, bio_worker);
        return work;
error0:
        return NULL;
}

/**
 * Destory a bio_work.
 */
static void destroy_bio_work(struct bio_work *work)
{
        ASSERT(work);
        kmem_cache_free(bio_work_cache_, work);
}

#if 0
/**
 * BIO ENDIO worker.
 * CONTEXT:
 * Non-IRQ.
 */
static void bio_endio_worker(struct work_struct *work)
{
        struct delayed_work *dwork =
                container_of(work, struct delayed_work, work);
        struct bio_work *bio_work = container_of(dwork, struct bio_work, dwork);
        struct bio *bio = bio_work->bio;
        /* struct simple_blk_dev *sdev = bio_work->sdev; */
        /* struct memblk_data *mdata = get_mdata_from_sdev(sdev); */

        bio_endio(bio, 0);
        destroy_bio_work(bio_work);
}
#endif

/**
 * BIO ENDIO callback for timer.
 * CONTEXT:
 * IRQ.
 */
static void bio_endio_timer_callback(unsigned long data)
{
        struct bio_work *bio_work = (struct bio_work *)data;
        
        bio_endio(bio_work->bio, 0);
        del_timer(&bio_work->end_timer);
        destroy_bio_work(bio_work);
}

/**
 * BIO IO worker.
 * CONTEXT:
 * Non-IRQ.
 */
static void bio_io_worker(struct work_struct *work)
{
        struct bio_work *bio_work = container_of(work, struct bio_work, work);
        struct bio *bio = bio_work->bio;
        struct simple_blk_dev *sdev = bio_work->sdev;
        struct memblk_data *mdata = get_mdata_from_sdev(sdev);

#ifdef PERFORMANCE_DEBUG
        getnstimeofday(&bio_work->ts_deq2);
#endif
        mdata_exec_bio(mdata, bio);
#if 1
        /* Delay with timer. */
        setup_timer(&bio_work->end_timer, bio_endio_timer_callback,
                    (unsigned long)bio_work);
        bio_work->end_timer.expires = jiffies + msecs_to_jiffies(5);
        add_timer(&bio_work->end_timer);

#elif 0
        /* Delay with workqueue. */
        INIT_DELAYED_WORK(&bio_work->dwork, bio_endio_worker);
        queue_delayed_work(wq_io_, &bio_work->dwork, msecs_to_jiffies(5));
        
#else        
        bio_endio(bio, 0); /* Never fail. */
#ifdef PERFORMANCE_DEBUG
        getnstimeofday(&bio_work->ts_end);
        LOGd("start %ld enq1 %ld deq1 %ld enq2 %ld deq2 %ld end.\n",
             timespec_sub(bio_work->ts_enq1, bio_work->ts_start).tv_nsec,
             timespec_sub(bio_work->ts_deq1, bio_work->ts_enq1).tv_nsec,
             timespec_sub(bio_work->ts_enq2, bio_work->ts_deq1).tv_nsec,
             timespec_sub(bio_work->ts_deq2, bio_work->ts_enq2).tv_nsec,
             timespec_sub(bio_work->ts_end, bio_work->ts_deq2).tv_nsec);
#endif
        destroy_bio_work(bio_work);
#endif
}

/**
 * BIO worker (serialized).
 */
static void bio_worker(struct work_struct *work)
{
        struct bio_work *bio_work = container_of(work, struct bio_work, work);
        struct bio *bio = bio_work->bio;
        bool isDone = false;

#ifdef PERFORMANCE_DEBUG
        getnstimeofday(&bio_work->ts_deq1);
#endif
        if (bio->bi_rw & REQ_FLUSH) {
                LOGd("flush wq_io_ workqueue.\n");
                flush_workqueue(wq_io_);
                if (bio->bi_size == 0) {
                        bio_endio(bio, 0);
                        isDone = true;
                }
        }
        if (isDone) {
#ifdef PERFORMANCE_DEBUG
        getnstimeofday(&bio_work->ts_end);
        LOGd("start %ld enq1 %ld deq1 %ld end.\n",
             timespec_sub(bio_work->ts_enq1, bio_work->ts_start).tv_nsec,
             timespec_sub(bio_work->ts_deq1, bio_work->ts_enq1).tv_nsec,
             timespec_sub(bio_work->ts_end, bio_work->ts_deq1).tv_nsec);
#endif
                destroy_bio_work(bio_work);
        } else {
                INIT_WORK(work, bio_io_worker);
                queue_work(wq_io_, work);
#ifdef PERFORMANCE_DEBUG
                getnstimeofday(&bio_work->ts_enq2);
#endif
        }
}

/*******************************************************************************
 * Global functions definition.
 *******************************************************************************/

/**
 * Make request.
 *
 * CONTEXT:
 * IRQ.
 */
void simple_blk_bio_make_request(struct request_queue *q, struct bio *bio)
{
        struct bio_work *bio_work;
        struct simple_blk_dev *sdev = sdev_get_from_queue(q);
        
        ASSERT(bio);

        bio_work = create_bio_work(bio, sdev, GFP_ATOMIC);
        if (!bio_work) {
                LOGe("create_bio_work() failed.\n");
                goto error0;
        }
#ifdef PERFORMANCE_DEBUG
        getnstimeofday(&bio_work->ts_start);
#endif
        queue_work(wq_misc_, &bio_work->work);
#ifdef PERFORMANCE_DEBUG
        getnstimeofday(&bio_work->ts_enq1);
#endif
        /* bio_endio(bio, 0); */
error0:
        bio_endio(bio, -EIO);
}

/**
 * Create memory data.
 * @sdev a simple block device. must not be NULL.
 * REUTRN:
 * true in success, or false.
 * CONTEXT:
 * Non-IRQ.
 */
bool create_private_data(struct simple_blk_dev *sdev)
{
        struct memblk_data *mdata = NULL;
        u64 capacity;
        unsigned int block_size;

        ASSERT(sdev);
        
        capacity = sdev->capacity;
        block_size = sdev->blksiz.lbs;
        mdata = mdata_create(capacity, block_size, GFP_KERNEL);
        
        if (!mdata) {
                goto error0;
        }
        sdev->private_data = (void *)mdata;
        return true;
#if 0
error1:
        mdata_destroy(mdata);
#endif
error0:
        return false;
}

/**
 * Destory memory data.
 * @sdev a simple block device. must not be NULL.
 * RETURN:
 * true in success, or false.
 * CONTEXT:
 * Non-IRQ.
 */
void destroy_private_data(struct simple_blk_dev *sdev)
{
        ASSERT(sdev);
        mdata_destroy(sdev->private_data);
}

/**
 * Accept REQ_DISCARD, REQ_FLUSH, and REQ_FUA.
 */
void customize_sdev(struct simple_blk_dev *sdev)
{
        struct request_queue *q;
        ASSERT(sdev);
        q = sdev->queue;
        
        /* Accept REQ_DISCARD. */
        /* q->limits.discard_granularity = PAGE_SIZE; */
        q->limits.discard_granularity = sdev->blksiz.lbs;
	q->limits.max_discard_sectors = UINT_MAX;
	q->limits.discard_zeroes_data = 1;
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, q);
	queue_flag_set_unlocked(QUEUE_FLAG_SECDISCARD, q);

        /* Accept REQ_FLUSH and REQ_FUA. */
        /* blk_queue_flush(q, REQ_FLUSH | REQ_FUA); */
        blk_queue_flush(q, REQ_FLUSH);
}

/**
 * Initialize kmem_cache.
 */
bool pre_register(void)
{
        bio_work_cache_ = kmem_cache_create("bio_work_cache", sizeof(struct bio_work), 0, 0, NULL);
        if (!bio_work_cache_) {
                LOGe("bio_work_cache creation failed.\n");
                goto error0;
        }

        /* wq_io_ = create_singlethread_workqueue("simple_blk_bio_mem_barrier_io"); */
        /* wq_io_ = create_workqueue("simple_blk_bio_mem_barrier_io"); */
        /* wq_io_ = alloc_workqueue("simple_blk_bio_mem_barrier_io", */
        /*                          WQ_MEM_RECLAIM | WQ_NON_REENTRANT, 0); */
        wq_io_ = alloc_workqueue("simple_blk_bio_mem_barrier_io",
                                 WQ_MEM_RECLAIM, 0);
        if (!wq_io_) {
                LOGe("create io queue failed.\n");
                goto error1;
        }
        wq_misc_ = create_singlethread_workqueue("simple_blk_bio_mem_barrier_misc");
        /* wq_misc_ = alloc_workqueue("simple_blk_bio_mem_barrier_misc", */
        /*                            WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_NON_REENTRANT, 1); */
        if (!wq_misc_) {
                LOGe("create misc queue failed.\n");
                goto error2;
        }

        return true;
#if 0
error3:
        destroy_workqueue(wq_misc_);
#endif
error2:
        destroy_workqueue(wq_io_);
error1:
        kmem_cache_destroy(bio_work_cache_);
error0:
        return false;
}

/**
 * Finalize kmem_cache.
 */
void post_unregister(void)
{
        destroy_workqueue(wq_misc_);
        destroy_workqueue(wq_io_);
        kmem_cache_destroy(bio_work_cache_);
}

/* end of file. */