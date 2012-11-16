/**
 * base_req.h - Definition for base_req operations.
 *
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#ifndef WALB_BASE_REQ_H_KERNEL
#define WALB_BASE_REQ_H_KERNEL

#include "check_kernel.h"
#include <linux/blkdev.h>
#include <linux/workqueue.h>
#include "simple_blk.h"

/*******************************************************************************
 * Global functions prototype.
 *******************************************************************************/

/* Make requrest for simpl_blk_bio_* modules. */
void simple_blk_req_request_fn(struct request_queue *q);

/* Called before register. */
bool pre_register(void);
/* Called before unregister. */
void pre_unregister(void);
/* Called after unregister. */
void post_unregister(void);

/* Create private data for sdev. */
bool create_private_data(struct simple_blk_dev *sdev);
/* Destroy private data for ssev. */
void destroy_private_data(struct simple_blk_dev *sdev);
/* Customize sdev after register before start. */
void customize_sdev(struct simple_blk_dev *sdev);

/* Workqueue type. */
enum workqueue_type get_workqueue_type(void);

#endif /* WALB_BASE_REQ_H_KERNEL */