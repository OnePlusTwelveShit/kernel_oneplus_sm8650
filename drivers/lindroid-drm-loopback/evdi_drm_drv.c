// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 *
 * Based on parts on udlfb.c:
 * Copyright (C) 2009 its respective authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/slab.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/sched/signal.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/prefetch.h>
#include <linux/refcount.h>
#if KERNEL_VERSION(5, 16, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include <drm/drm_vblank.h>
#elif KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE
#else
#include <drm/drmP.h>
#endif
#if KERNEL_VERSION(5, 1, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_probe_helper.h>
#endif
#if KERNEL_VERSION(5, 8, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_managed.h>
#endif
#include <drm/drm_atomic_helper.h>
#include "evdi_drm_drv.h"
#include "evdi_platform_drv.h"
#include "evdi_cursor.h"
#include "evdi_debug.h"
#include "evdi_drm.h"

#if KERNEL_VERSION(6, 8, 0) <= LINUX_VERSION_CODE || defined(EL8)
#define EVDI_DRM_UNLOCKED 0
#else
#define EVDI_DRM_UNLOCKED DRM_UNLOCKED
#endif

static struct drm_driver driver;
int evdi_swap_callback_ioctl(struct drm_device *drm_dev, void *data,
                    struct drm_file *file);
int evdi_add_buff_callback_ioctl(struct drm_device *drm_dev, void *data,
                    struct drm_file *file);

int evdi_destroy_buff_callback_ioctl(struct drm_device *drm_dev, void *data,
                    struct drm_file *file);

int evdi_gbm_add_buf_ioctl(
					struct drm_device *dev,
					void *data,
					struct drm_file *file);

int evdi_get_buff_callback_ioctl(struct drm_device *drm_dev, void *data,
                    struct drm_file *file);

int evdi_gbm_get_buf_ioctl(struct drm_device *dev, void *data,
					struct drm_file *file);

int evdi_gbm_del_buf_ioctl(struct drm_device *dev, void *data,
					struct drm_file *file);

int evdi_gbm_create_buff(struct drm_device *dev, void *data,
					struct drm_file *file);

int evdi_create_buff_callback_ioctl(struct drm_device *drm_dev, void *data,
                    struct drm_file *file);

static struct kmem_cache *evdi_event_cache;
static atomic_t evdi_event_cache_users = ATOMIC_INIT(0);

struct drm_ioctl_desc evdi_painter_ioctls[] = {
	DRM_IOCTL_DEF_DRV(EVDI_CONNECT, evdi_painter_connect_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_REQUEST_UPDATE, evdi_painter_request_update_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_ENABLE_CURSOR_EVENTS, evdi_painter_enable_cursor_events_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_POLL, evdi_poll_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_SWAP_CALLBACK, evdi_swap_callback_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_ADD_BUFF_CALLBACK, evdi_add_buff_callback_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_GET_BUFF_CALLBACK, evdi_get_buff_callback_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_DESTROY_BUFF_CALLBACK, evdi_destroy_buff_callback_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_ADD_BUFF, evdi_gbm_add_buf_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_GET_BUFF, evdi_gbm_get_buf_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_DEL_BUFF, evdi_gbm_del_buf_ioctl, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_CREATE_BUFF, evdi_gbm_create_buff, EVDI_DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_CREATE_BUFF_CALLBACK, evdi_create_buff_callback_ioctl, EVDI_DRM_UNLOCKED),
};

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
static const struct vm_operations_struct evdi_gem_vm_ops = {
	.fault = evdi_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};
#endif

static const struct file_operations evdi_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.mmap = evdi_drm_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.unlocked_ioctl = drm_ioctl,
	.release = drm_release,
	.llseek = noop_llseek,

#if defined(FOP_UNSIGNED_OFFSET)
	.fop_flags = FOP_UNSIGNED_OFFSET,
#endif
};

struct evdi_kreq {
	void			*payload;
	struct completion	done;
	refcount_t		refs;
	atomic_t		waiter_gone;
	int			result;
	void			*reply;
};
static struct kmem_cache *evdi_kreq_cache;

#define EVDI_MAX_FDS   32
#define EVDI_MAX_INTS  256

//Handle short copies due to minor faults on big buffers
static inline int evdi_prefault_readable(const void __user *uaddr, size_t len)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0) || defined(EL8) || defined(EL9)
	return fault_in_readable(uaddr, len);
#else
	return 0;
#endif
}

static inline int evdi_prefault_writeable(void __user *uaddr, size_t len)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0) || defined(EL8) || defined(EL9)
	return fault_in_writeable(uaddr, len);
#else
	return 0;
#endif
}

//Allow partial progress; return -EFAULT only if zero progress
static int evdi_copy_from_user_allow_partial(void *dst, const void __user *src, size_t len)
{
	size_t not;

	if (!len)
		return 0;

	(void)evdi_prefault_readable(src, len);
	prefetchw(dst);
	not = copy_from_user(dst, src, len);
	if (not == len)
		return -EFAULT;

	return 0;
}

static int evdi_copy_to_user_allow_partial(void __user *dst, const void *src, size_t len)
{
	size_t not;

	if (!len)
		return 0;

	prefetch(src);
	(void)evdi_prefault_writeable(dst, len);
	not = copy_to_user(dst, src, len);
	if (not == len)
		return -EFAULT;

	return 0;
}

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
static int evdi_enable_vblank(__always_unused struct drm_device *dev,
			      __always_unused unsigned int pipe)
{
	return 1;
}

static void evdi_disable_vblank(__always_unused struct drm_device *dev,
				__always_unused unsigned int pipe)
{
}
#endif

static struct drm_driver driver = {
#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE || defined(EL8)
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
#else
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME
			 | DRIVER_ATOMIC,
#endif

	.open = evdi_driver_open,
	.postclose = evdi_driver_postclose,

	/* gem hooks */
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#elif KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
	.gem_free_object_unlocked = evdi_gem_free_object,
#else
	.gem_free_object = evdi_gem_free_object,
#endif

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	.gem_vm_ops = &evdi_gem_vm_ops,
#endif

	.dumb_create = evdi_dumb_create,
	.dumb_map_offset = evdi_gem_mmap,
#if KERNEL_VERSION(5, 12, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	.dumb_destroy = drm_gem_dumb_destroy,
#endif

	.ioctls = evdi_painter_ioctls,
	.num_ioctls = ARRAY_SIZE(evdi_painter_ioctls),

	.fops = &evdi_driver_fops,

	.gem_prime_import = drm_gem_prime_import,
#if KERNEL_VERSION(6, 6, 0) <= LINUX_VERSION_CODE
#else
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
#endif
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	.preclose = evdi_driver_preclose,
	.gem_prime_export = drm_gem_prime_export,
	.gem_prime_get_sg_table = evdi_prime_get_sg_table,
	.enable_vblank = evdi_enable_vblank,
	.disable_vblank = evdi_disable_vblank,
#endif
	.gem_prime_import_sg_table = evdi_prime_import_sg_table,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCH,
};

struct evdi_event *evdi_create_event(struct evdi_device *evdi, enum poll_event_type type, void *data, struct drm_file *file)
{
	struct evdi_event *event;

	event = kmem_cache_zalloc(evdi_event_cache, GFP_KERNEL);
	if (unlikely(!event))
		return NULL;

	INIT_LIST_HEAD(&event->list);
	event->on_queue = false;
	event->owner = file;
	event->type = type;
	event->data = data;
	event->evdi = evdi;

#if !defined(EVDI_HAVE_XARRAY)
	idr_preload(GFP_KERNEL);
#endif
	spin_lock(&evdi->event_lock);
	event->poll_id = atomic_fetch_inc(&evdi->next_event_id);
#if defined(EVDI_HAVE_XARRAY)
	if (xa_err(xa_store(&evdi->event_xa, event->poll_id, event, GFP_NOWAIT))) {
		spin_unlock(&evdi->event_lock);
		kmem_cache_free(evdi_event_cache, event);
		return NULL;
	}
#else
	if (idr_alloc(&evdi->event_idr, event,
		      event->poll_id, event->poll_id + 1, GFP_NOWAIT) < 0) {
		spin_unlock(&evdi->event_lock);
		idr_preload_end();
		kmem_cache_free(evdi_event_cache, event);
		return NULL;
	}
#endif

	list_add_tail(&event->list, &evdi->event_queue);
	event->on_queue = true;
	spin_unlock(&evdi->event_lock);
#if !defined(EVDI_HAVE_XARRAY)
	idr_preload_end();
#endif

	return event;
}

void evdi_event_free(struct evdi_event *event)
{
	if (event)
		kmem_cache_free(evdi_event_cache, event);
}

static int evdi_event_cache_get(void)
{
	if (!evdi_event_cache) {
		evdi_event_cache = kmem_cache_create("evdi_event",
						     sizeof(struct evdi_event),
						     0, SLAB_HWCACHE_ALIGN, NULL);
		if (!evdi_event_cache)
			return -ENOMEM;
	}
	atomic_inc(&evdi_event_cache_users);
	if (!evdi_kreq_cache) {
		evdi_kreq_cache = kmem_cache_create("evdi_kreq",
			sizeof(struct evdi_kreq), 0, SLAB_HWCACHE_ALIGN, NULL);
		if (!evdi_kreq_cache) {
			kmem_cache_destroy(evdi_event_cache);
			evdi_event_cache = NULL;
			atomic_dec(&evdi_event_cache_users);
			return -ENOMEM;
		}
	}
	return 0;
}

static void evdi_event_cache_put(void)
{
	if (atomic_dec_and_test(&evdi_event_cache_users) && evdi_event_cache) {
		kmem_cache_destroy(evdi_event_cache);
		evdi_event_cache = NULL;
		if (evdi_kreq_cache) {
			kmem_cache_destroy(evdi_kreq_cache);
			evdi_kreq_cache = NULL;
		}
	}
}

void evdi_event_unlink_and_free(struct evdi_device *evdi,
                                       struct evdi_event *event)
{
	spin_lock(&evdi->event_lock);
#if defined(EVDI_HAVE_XARRAY)
	xa_erase(&evdi->event_xa, event->poll_id);
#else
	idr_remove(&evdi->event_idr, event->poll_id);
#endif
	if (event->on_queue && !list_empty(&event->list)) {
		list_del_init(&event->list);
		event->on_queue = false;
	}
	spin_unlock(&evdi->event_lock);
	evdi_event_free(event);
}

static inline struct evdi_event *evdi_find_event(struct evdi_device *evdi, u32 poll_id)
{
	struct evdi_event *event;
#if defined(EVDI_HAVE_XARRAY)
	rcu_read_lock();
	event = xa_load(&evdi->event_xa, poll_id);
	rcu_read_unlock();
	return event;
#else
	spin_lock(&evdi->event_lock);
	event = idr_find(&evdi->event_idr, poll_id);
	spin_unlock(&evdi->event_lock);
	return event;
#endif
}

int evdi_swap_callback_ioctl(struct drm_device *drm_dev, void *data,
                    struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;
	struct drm_evdi_add_buff_callabck *cmd = data;
	struct evdi_event *event = evdi_find_event(evdi, cmd->poll_id);
	struct evdi_kreq *kreq;

	if (unlikely(!event))
		return -EINVAL;

	kreq = (struct evdi_kreq *)event->reply_data;
	if (!kreq) {
		evdi_event_unlink_and_free(evdi, event);
		return 0;
	}

	kreq->result = 0;
	kreq->reply = NULL;
	complete(&kreq->done);
	if (refcount_dec_and_test(&kreq->refs))
		kmem_cache_free(evdi_kreq_cache, kreq);

	evdi_event_unlink_and_free(evdi, event);
	return 0;
}

int evdi_add_buff_callback_ioctl(struct drm_device *drm_dev, void *data,
                    struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;
	struct drm_evdi_add_buff_callabck *cmd = data;
	struct evdi_event *event = evdi_find_event(evdi, cmd->poll_id);
	struct evdi_kreq *kreq;
	int *buff_id_ptr;


	if (unlikely(!event))
		return -EINVAL;

	kreq = (struct evdi_kreq *)event->reply_data;
	if (unlikely(!kreq)) {
		evdi_event_unlink_and_free(evdi, event);
		return 0;
	}

	buff_id_ptr = kzalloc(sizeof(int), GFP_KERNEL);
	if (unlikely(!buff_id_ptr))
		return -ENOMEM;

	*buff_id_ptr = cmd->buff_id;
	kreq->reply = buff_id_ptr;
	kreq->result = 0;
	complete(&kreq->done);
	if (atomic_read(&kreq->waiter_gone)) {
		kfree(buff_id_ptr);
		kreq->reply = NULL;
	}
	if (refcount_dec_and_test(&kreq->refs))
		kmem_cache_free(evdi_kreq_cache, kreq);

	evdi_event_unlink_and_free(evdi, event);
	return 0;
}

int evdi_get_buff_callback_ioctl(struct drm_device *drm_dev, void *data,
                     struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;
	struct drm_evdi_get_buff_callabck *cmd = data;
	struct evdi_event *event = evdi_find_event(evdi, cmd->poll_id);
	struct evdi_gralloc_buf *gralloc_buf;
	struct evdi_kreq *kreq;
	int i;
	int fd_ints[EVDI_MAX_FDS];
	size_t ints_sz, files_sz;
	void *mem;

	if (unlikely(!event))
		return -EINVAL;

	if (cmd->numFds < 0 || cmd->numInts < 0 ||
	    cmd->numFds > EVDI_MAX_FDS || cmd->numInts > EVDI_MAX_INTS)
		return -EINVAL;

	ints_sz = sizeof(int) * cmd->numInts;
	files_sz = sizeof(struct file *) * cmd->numFds;
	mem = kzalloc(sizeof(struct evdi_gralloc_buf) + ints_sz + files_sz, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	gralloc_buf = mem;
	gralloc_buf->version = cmd->version;
	gralloc_buf->numFds = cmd->numFds;
	gralloc_buf->numInts = cmd->numInts;
	gralloc_buf->data_ints = (int *)((char *)mem + sizeof(struct evdi_gralloc_buf));
	gralloc_buf->data_files = (struct file **)((char *)gralloc_buf->data_ints + ints_sz);

	if (cmd->numInts &&
	    evdi_copy_from_user_allow_partial(gralloc_buf->data_ints,
					      (const void __user *)cmd->data_ints,
					      sizeof(int) * cmd->numInts)) {
		kfree(gralloc_buf);
		return -EFAULT;
	}
	if (evdi_copy_from_user_allow_partial(fd_ints,
					      (const void __user *)cmd->fd_ints,
					      sizeof(int) * cmd->numFds)) {
		kfree(gralloc_buf);
		return -EFAULT;
	}
	
	for (i = 0; i < cmd->numFds; i++) {
		gralloc_buf->data_files[i] = fget(fd_ints[i]);
		if (!gralloc_buf->data_files[i]) {
			EVDI_ERROR("evdi_get_buff_callback_ioctl: Failed to open fake fb %d\n", fd_ints[i]);
			while (--i >= 0) {
				if (gralloc_buf->data_files[i])
					fput(gralloc_buf->data_files[i]);
			}
			kfree(gralloc_buf);
			return -EINVAL;
		}
	}

	kreq = (struct evdi_kreq *)event->reply_data;
	if (!kreq) {
		for (i = 0; i < gralloc_buf->numFds; i++) {
			if (gralloc_buf->data_files[i])
				fput(gralloc_buf->data_files[i]);
		}
		kfree(gralloc_buf);
		evdi_event_unlink_and_free(evdi, event);
		return 0;
	}

	kreq->reply = gralloc_buf;
	kreq->result = 0;
	complete(&kreq->done);
	if (atomic_read(&kreq->waiter_gone)) {
		for (i = 0; i < gralloc_buf->numFds; i++)
			if (gralloc_buf->data_files[i]) fput(gralloc_buf->data_files[i]);
		kfree(gralloc_buf);
		kreq->reply = NULL;
	}
	if (refcount_dec_and_test(&kreq->refs))
		kmem_cache_free(evdi_kreq_cache, kreq);

	evdi_event_unlink_and_free(evdi, event);
	return 0;
}

int evdi_destroy_buff_callback_ioctl(struct drm_device *drm_dev, void *data,
                     struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;
	struct drm_evdi_add_buff_callabck *cmd = data;
	struct evdi_event *event = evdi_find_event(evdi, cmd->poll_id);
	struct evdi_kreq *kreq;

	if (unlikely(!event)) {
		EVDI_ERROR("evdi_destroy_buff_callback_ioctl: event is null\n");
		return -EINVAL;
	}

	kreq = (struct evdi_kreq *)event->reply_data;
	if (unlikely(!kreq)) {
		evdi_event_unlink_and_free(evdi, event);
		return 0;
	}

	kreq->result = 0;
	kreq->reply = NULL;
	complete(&kreq->done);
	if (refcount_dec_and_test(&kreq->refs))
		kmem_cache_free(evdi_kreq_cache, kreq);

	evdi_event_unlink_and_free(evdi, event);
	return 0;
}

int evdi_create_buff_callback_ioctl(struct drm_device *drm_dev, void *data,
                    struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;
	struct drm_evdi_create_buff_callabck *cmd = data;
	struct drm_evdi_create_buff_callabck *buf = kmemdup(data, sizeof(*buf), GFP_KERNEL);
	struct evdi_event *event;
	struct evdi_kreq *kreq;
	if (!buf)
		return -ENOMEM;

	event = evdi_find_event(evdi, cmd->poll_id);

	if (unlikely(!event)) {
		kfree(buf);
		return -EINVAL;
	}

	kreq = (struct evdi_kreq *)event->reply_data;
	if (unlikely(!kreq)) {
		kfree(buf);
		evdi_event_unlink_and_free(evdi, event);
		return 0;
	}

	kreq->result = 0;
	kreq->reply = buf;
	complete(&kreq->done);
	if (atomic_read(&kreq->waiter_gone)) {
		kfree(buf);
		kreq->reply = NULL;
	}
	if (refcount_dec_and_test(&kreq->refs))
		kmem_cache_free(evdi_kreq_cache, kreq);

	evdi_event_unlink_and_free(evdi, event);
	return 0;
}

int evdi_gbm_add_buf_ioctl(struct drm_device *dev, void *data,
					struct drm_file *file)
{
	struct file *memfd_file;
	struct file *fd_file;
	int version, numFds, numInts, fd;
	ssize_t bytes_read;
	struct evdi_gralloc_buf *add_gralloc_buf;
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_gbm_add_buf *cmd = data;
	struct evdi_event *event;
	struct evdi_kreq *kreq;
	loff_t pos;
	int i, ret;
	int fd_array[EVDI_MAX_FDS];
	size_t ints_sz, files_sz;
	void *mem;
	struct {
		int version;
		int numFds;
		int numInts;
	} hdr;

	memfd_file = fget(cmd->fd);
	if (!memfd_file) {
		EVDI_ERROR("Failed to open fake fb: %d\n", cmd->fd);
		return -EINVAL;
	}

	pos = 0; /* Initialize offset */
	bytes_read = kernel_read(memfd_file, &hdr, sizeof(hdr), &pos);
	if (bytes_read != sizeof(hdr)) {
		EVDI_ERROR("Failed to read header from memfd, bytes_read=%zd\n", bytes_read);
		fput(memfd_file);
		return -EIO;
	}

	version = hdr.version;
	numFds = hdr.numFds;
	numInts = hdr.numInts;

	if (numFds < 0 || numInts < 0 || numFds > EVDI_MAX_FDS || numInts > EVDI_MAX_INTS) {
		EVDI_ERROR("Invalid memfd header: numFds=%d numInts=%d\n", numFds, numInts);
		fput(memfd_file);
		return -EINVAL;
	}
	ints_sz = sizeof(int) * numInts;
	files_sz = sizeof(struct file *) * numFds;
	mem = kzalloc(sizeof(struct evdi_gralloc_buf) + ints_sz + files_sz, GFP_KERNEL);
	if (!mem) {
		fput(memfd_file);
		return -ENOMEM;
	}
	add_gralloc_buf = mem;
	add_gralloc_buf->numFds = numFds;
	add_gralloc_buf->numInts = numInts;
	add_gralloc_buf->data_ints = (int *)((char *)mem + sizeof(struct evdi_gralloc_buf));
	add_gralloc_buf->data_files = (struct file **)((char *)add_gralloc_buf->data_ints + ints_sz);
	add_gralloc_buf->memfd_file = memfd_file;

	if (numFds) {
		bytes_read = kernel_read(memfd_file, fd_array, sizeof(int) * numFds, &pos);
		if (bytes_read != sizeof(int) * numFds) {
			EVDI_ERROR("Failed to read fd array from memfd, bytes_read=%zd\n", bytes_read);
			kfree(add_gralloc_buf);
			fput(memfd_file);
			return -EIO;
		}
		for (i = 0; i < numFds; i++) {
			fd = fd_array[i];
			fd_file = fget(fd);
			if (!fd_file) {
				EVDI_ERROR("Failed to open fake fb's %d fd file: %d\n", cmd->fd, fd);
				kfree(add_gralloc_buf);
				fput(memfd_file);
				return -EINVAL;
			}
			add_gralloc_buf->data_files[i] = fd_file;
		}
	}

	bytes_read = kernel_read(memfd_file, add_gralloc_buf->data_ints, sizeof(int) *numInts, &pos);
	if (bytes_read != sizeof(int) *numInts) {
		EVDI_ERROR("Failed to read ints from memfd, bytes_read=%zd\n", bytes_read);
		for (i = 0; i < numFds; i++) {
			if (add_gralloc_buf->data_files[i])
				fput(add_gralloc_buf->data_files[i]);
		}
		kfree(add_gralloc_buf);
		fput(memfd_file);
		return -EIO;
	}

	event = evdi_create_event(evdi, add_buf, add_gralloc_buf, file);
	if (!event)
		return -ENOMEM;

	kreq = kmem_cache_zalloc(evdi_kreq_cache, GFP_KERNEL);
	if (!kreq)
		return -ENOMEM;

	init_completion(&kreq->done);
	refcount_set(&kreq->refs, 2);
	atomic_set(&kreq->waiter_gone, 0);
	kreq->payload = add_gralloc_buf;
	kreq->result = 0;
	kreq->reply = NULL;
	event->reply_data = kreq;

	wake_up_interruptible(&evdi->poll_ioct_wq);

	ret = wait_for_completion_interruptible_timeout(&kreq->done, EVDI_WAIT_TIMEOUT);
	if (ret <= 0) {
		EVDI_ERROR("evdi_gbm_add_buf_ioctl: wait failed: %d\n", ret);
		atomic_set(&kreq->waiter_gone, 1);
		if (refcount_dec_and_test(&kreq->refs)) {
			kmem_cache_free(evdi_kreq_cache, kreq);
		}
		return ret ? ret : -ETIMEDOUT;
	}
	if (kreq->result < 0) {
		int err = kreq->result;
		if (refcount_dec_and_test(&kreq->refs)) {
			kmem_cache_free(evdi_kreq_cache, kreq);
		}
		return err;
	}
	if (kreq->reply) {
		cmd->id = *((int *)kreq->reply);
		kfree(kreq->reply);
	}
	if (refcount_dec_and_test(&kreq->refs))
		kmem_cache_free(evdi_kreq_cache, kreq);

	return 0;
}

int evdi_gbm_get_buf_ioctl(struct drm_device *dev, void *data,
					struct drm_file *file)
{
	struct drm_evdi_gbm_get_buff *cmd = data;
	struct evdi_gralloc_buf_user *gralloc_buf = kzalloc(sizeof(struct evdi_gralloc_buf_user), GFP_KERNEL);
	struct evdi_gralloc_buf *gralloc_buf_tmp = NULL;
	struct evdi_device *evdi = dev->dev_private;
	int fd_tmp, ret;
	struct evdi_event *event;
	struct evdi_kreq *kreq;
	int i;
	int installed_fds[EVDI_MAX_FDS];

	event = evdi_create_event(evdi, get_buf, &cmd->id, file);
	if (unlikely(!event))
		return -ENOMEM;

	kreq = kmem_cache_zalloc(evdi_kreq_cache, GFP_KERNEL);
	if (unlikely(!kreq))
		return -ENOMEM;

	init_completion(&kreq->done);
	refcount_set(&kreq->refs, 2);
	atomic_set(&kreq->waiter_gone, 0);
	kreq->payload = &cmd->id;
	kreq->result = 0;
	kreq->reply = NULL;
	event->reply_data = kreq;

	wake_up_interruptible(&evdi->poll_ioct_wq);

	ret = wait_for_completion_interruptible_timeout(&kreq->done, EVDI_WAIT_TIMEOUT);
	if (unlikely(ret <= 0)) {
		EVDI_ERROR("evdi_gbm_get_buf_ioctl: wait failed: %d\n", ret);
		kfree(gralloc_buf);
		atomic_set(&kreq->waiter_gone, 1);
		if (refcount_dec_and_test(&kreq->refs)) {
			kmem_cache_free(evdi_kreq_cache, kreq);
		}
		return ret ? ret : -ETIMEDOUT;
	}
	if (unlikely(kreq->result < 0)) {
		ret = kreq->result;
		kfree(gralloc_buf);
		if (refcount_dec_and_test(&kreq->refs)) {
			kmem_cache_free(evdi_kreq_cache, kreq);
		}
		return ret;
	}

	gralloc_buf_tmp = (struct evdi_gralloc_buf *)kreq->reply;
	if (!gralloc_buf || !gralloc_buf_tmp) {
		ret = -ENOMEM;
		goto err_event;
	}
	if (gralloc_buf_tmp->numFds < 0 || gralloc_buf_tmp->numFds > EVDI_MAX_FDS ||
		gralloc_buf_tmp->numInts < 0 || gralloc_buf_tmp->numInts > EVDI_MAX_INTS) {
		ret = -EINVAL;
		goto err_event;
	}
	gralloc_buf->version = gralloc_buf_tmp->version;
	gralloc_buf->numFds = gralloc_buf_tmp->numFds;
	gralloc_buf->numInts = gralloc_buf_tmp->numInts;
	memcpy(&gralloc_buf->data[gralloc_buf->numFds], gralloc_buf_tmp->data_ints, sizeof(int)*gralloc_buf->numInts);

	for (i = 0; i < gralloc_buf->numFds; i++) {
		fd_tmp = get_unused_fd_flags(O_RDWR);
		if (fd_tmp < 0) {
			while (--i >= 0)
				put_unused_fd(installed_fds[i]);
			ret = fd_tmp;
			goto err_event;
		}
		installed_fds[i] = fd_tmp;
		gralloc_buf->data[i] = fd_tmp;
	}

	if (evdi_copy_to_user_allow_partial((void __user *)cmd->native_handle,
					    gralloc_buf,
					    sizeof(int) * (3 + gralloc_buf->numFds + gralloc_buf->numInts))) {
		EVDI_ERROR("Failed to copy file descriptor to userspace\n");
		for (i = 0; i < gralloc_buf->numFds; i++)
			put_unused_fd(installed_fds[i]);
		ret = -EFAULT;
		goto err_event;
	}

	for (i = 0; i < gralloc_buf->numFds; i++)
		fd_install(installed_fds[i], gralloc_buf_tmp->data_files[i]);

	kfree(gralloc_buf);
	if (gralloc_buf_tmp)
		kfree(gralloc_buf_tmp);

	if (refcount_dec_and_test(&kreq->refs))
		kmem_cache_free(evdi_kreq_cache, kreq);

	return 0;

err_event:
	kfree(gralloc_buf);
	if (gralloc_buf_tmp) {
		for (i = 0; i < gralloc_buf_tmp->numFds; i++)
			if (gralloc_buf_tmp->data_files[i])
				fput(gralloc_buf_tmp->data_files[i]);

		kfree(gralloc_buf_tmp);
	};
	atomic_set(&kreq->waiter_gone, 1);
	if (refcount_dec_and_test(&kreq->refs)) {
		kmem_cache_free(evdi_kreq_cache, kreq);
	}
	return ret ? ret : -ETIMEDOUT;
}

int evdi_gbm_del_buf_ioctl(struct drm_device *dev, void *data,
					struct drm_file *file)
{
	struct drm_evdi_gbm_del_buff *cmd = data;
	struct evdi_device *evdi = dev->dev_private;
	struct evdi_event *event;
	struct evdi_kreq *kreq;
	int ret;

	event = evdi_create_event(evdi, destroy_buf, &cmd->id, file);
	if (unlikely(!event))
		return -ENOMEM;

	kreq = kmem_cache_zalloc(evdi_kreq_cache, GFP_KERNEL);
	if (unlikely(!kreq))
		return -ENOMEM;

	init_completion(&kreq->done);
	refcount_set(&kreq->refs, 2);
	atomic_set(&kreq->waiter_gone, 0);
	kreq->payload = &cmd->id;
	kreq->result = 0;
	kreq->reply = NULL;
	event->reply_data = kreq;

	wake_up_interruptible(&evdi->poll_ioct_wq);
	ret = wait_for_completion_interruptible_timeout(&kreq->done, EVDI_WAIT_TIMEOUT);
	if (ret == 0) {
		EVDI_ERROR("evdi_gbm_del_buf_ioctl: wait timed out\n");
		atomic_set(&kreq->waiter_gone, 1);
		if (refcount_dec_and_test(&kreq->refs)) {
			kmem_cache_free(evdi_kreq_cache, kreq);
		}
		return -ETIMEDOUT;
	} else if (ret < 0) {
		EVDI_ERROR("evdi_gbm_get_buf_ioctl: wait_event_interruptible interrupted: %d\n", ret);
		atomic_set(&kreq->waiter_gone, 1);
		if (refcount_dec_and_test(&kreq->refs)) {
			kmem_cache_free(evdi_kreq_cache, kreq);
		}
		return ret;
	}

	ret = kreq->result;
	if (ret < 0)
		EVDI_ERROR("evdi_gbm_get_buf_ioctl: user ioctl failled\n");

	if (refcount_dec_and_test(&kreq->refs))
		kmem_cache_free(evdi_kreq_cache, kreq);

	return ret ? ret : 0;
}

int evdi_gbm_create_buff (struct drm_device *dev, void *data,
					struct drm_file *file)
{
	int ret;
	struct drm_evdi_gbm_create_buff *cmd = data;
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_create_buff_callabck *cb_cmd;
	struct evdi_event *event = evdi_create_event(evdi, create_buf, cmd, file);
	struct evdi_kreq *kreq;
	if (unlikely(!event))
		return -ENOMEM;

	kreq = kmem_cache_zalloc(evdi_kreq_cache, GFP_KERNEL);
	if (unlikely(!kreq))
		return -ENOMEM;

	init_completion(&kreq->done);
	refcount_set(&kreq->refs, 2);
	atomic_set(&kreq->waiter_gone, 0);
	kreq->payload = cmd;
	kreq->result = 0;
	kreq->reply = NULL;
	event->reply_data = kreq;

	wake_up_interruptible(&evdi->poll_ioct_wq);

	ret = wait_for_completion_interruptible_timeout(&kreq->done, EVDI_WAIT_TIMEOUT);
	if (ret <= 0) {
		EVDI_ERROR("evdi_gbm_create_buff: wait failed: %d\n", ret);
		atomic_set(&kreq->waiter_gone, 1);
		if (refcount_dec_and_test(&kreq->refs)) {
			kmem_cache_free(evdi_kreq_cache, kreq);
		}
		return ret ? ret : -ETIMEDOUT;
	}
	if (kreq->result < 0) {
		ret = kreq->result;
		if (refcount_dec_and_test(&kreq->refs)) {
			kmem_cache_free(evdi_kreq_cache, kreq);
		}
		return ret;
	}
	cb_cmd = (struct drm_evdi_create_buff_callabck *)kreq->reply;
	if (evdi_copy_to_user_allow_partial((void __user *)cmd->id, &cb_cmd->id, sizeof(int)) ||
	    evdi_copy_to_user_allow_partial((void __user *)cmd->stride, &cb_cmd->stride, sizeof(int))) {
		ret = -EFAULT;
		goto err_event;
	}

	kfree(cb_cmd);
	if (refcount_dec_and_test(&kreq->refs))
		kmem_cache_free(evdi_kreq_cache, kreq);

	return 0;

err_event:
	if (cb_cmd) {
		kfree(cb_cmd);
	}
	atomic_set(&kreq->waiter_gone, 1);
	if (refcount_dec_and_test(&kreq->refs)) {
		kmem_cache_free(evdi_kreq_cache, kreq);
	}
	return ret ? ret : -ETIMEDOUT;
}

int evdi_poll_ioctl(struct drm_device *drm_dev, void *data,
                    struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;
	struct drm_evdi_poll *cmd = data;
	struct evdi_event *event;
	int fd, fd_tmp, ret;
	ssize_t bytes_write;
	loff_t pos;
	int i;

	EVDI_CHECKPT();

	if (!evdi) {
		EVDI_ERROR("evdi is null\n");
		return -ENODEV;
	}

	ret = wait_event_interruptible(evdi->poll_ioct_wq,
				  atomic_read(&evdi->poll_stopping) ||
				  !list_empty(&evdi->event_queue));

	if (ret < 0) {
		EVDI_ERROR("evdi_poll_ioctl: Wait interrupted by signal\n");
		return ret;
	}

	//If woken when stopping, interrupt
	if (unlikely(atomic_read(&evdi->poll_stopping)))
		return -EINTR;

	spin_lock(&evdi->event_lock);

	if (list_empty(&evdi->event_queue)) {
		spin_unlock(&evdi->event_lock);
		return -EAGAIN;
	}

	event = list_first_entry(&evdi->event_queue, struct evdi_event, list);
	list_del_init(&event->list);
	event->on_queue = false;

	spin_unlock(&evdi->event_lock);

	cmd->event = event->type;
	cmd->poll_id = event->poll_id;

	switch(cmd->event) {
		case add_buf:
			{
			struct evdi_gralloc_buf *add_gralloc_buf = event->data;
			int reserved_fd_tmps[EVDI_MAX_FDS];

			fd = get_unused_fd_flags(O_RDWR);
			if (fd < 0) {
				EVDI_ERROR("Failed to allocate file descriptor\n");
				return fd;
			}

			if (add_gralloc_buf->numFds < 0 || add_gralloc_buf->numFds > EVDI_MAX_FDS) {
				put_unused_fd(fd);
				return -EINVAL;
			}
			for (i = 0; i < add_gralloc_buf->numFds; i++) {
				fd_tmp = get_unused_fd_flags(O_RDWR);
				if (fd_tmp < 0) {
					while (--i >= 0)
						put_unused_fd(reserved_fd_tmps[i]);
					put_unused_fd(fd);
					return fd_tmp;
				}
				reserved_fd_tmps[i] = fd_tmp;
			}

			for (i = 0; i < add_gralloc_buf->numFds; i++)
				fput(add_gralloc_buf->data_files[i]);

			pos = sizeof(int) * 3;
			bytes_write = kernel_write(add_gralloc_buf->memfd_file,
						   reserved_fd_tmps,
						   sizeof(int) * add_gralloc_buf->numFds,
						   &pos);
			if (bytes_write != sizeof(int) * add_gralloc_buf->numFds) {
				EVDI_ERROR("Failed to write fd array\n");
				for (i = 0; i < add_gralloc_buf->numFds; i++)
					put_unused_fd(reserved_fd_tmps[i]);
				put_unused_fd(fd);
				kfree(add_gralloc_buf);
				return -EFAULT;
			}

			if (evdi_copy_to_user_allow_partial((void __user *)cmd->data, &fd, sizeof(fd))) {
				EVDI_ERROR("Failed to copy file descriptor to userspace\n");
				for (i = 0; i < add_gralloc_buf->numFds; i++)
					put_unused_fd(reserved_fd_tmps[i]);

				put_unused_fd(fd);
				kfree(add_gralloc_buf);
				return -EFAULT;
			}
			fd_install(fd, add_gralloc_buf->memfd_file);
			for (i = 0; i < add_gralloc_buf->numFds; i++)
				fd_install(reserved_fd_tmps[i], add_gralloc_buf->data_files[i]);

			kfree(add_gralloc_buf);
			break;
			}
		case create_buf:
			if (evdi_copy_to_user_allow_partial((void __user *)cmd->data,
							    event->data,
							    sizeof(struct drm_evdi_gbm_create_buff))) {
				return -EFAULT;
			}
			break;
		case get_buf:
		case swap_to:
		case destroy_buf:
			if (evdi_copy_to_user_allow_partial((void __user *)cmd->data,
							    event->data, sizeof(int))) {
				return -EFAULT;
			}
			break;
		default:
			EVDI_ERROR("unknown event: %d\n", cmd->event);
	}

	return 0;
}

static void evdi_cancel_events_for_file(struct evdi_device *evdi,
                                        struct drm_file *file)
{
	struct evdi_event *event, *tmp;
	EVDI_INFO("Going to drain events\n");
	spin_lock(&evdi->event_lock);

	list_for_each_entry_safe(event, tmp, &evdi->event_queue, list) {
		if (event->owner != file)
			continue;

#if defined(EVDI_HAVE_XARRAY)
		xa_erase(&evdi->event_xa, event->poll_id);
#else
		idr_remove(&evdi->event_idr, event->poll_id);
#endif
		list_del_init(&event->list);
		event->on_queue = false;
		if (event->reply_data) {
			struct evdi_kreq *kreq = (struct evdi_kreq *)event->reply_data;
			kreq->result = -ECANCELED;
			complete_all(&kreq->done);
			if (refcount_dec_and_test(&kreq->refs))
				kmem_cache_free(evdi_kreq_cache, kreq);
		}
		evdi_event_free(event);
	}

	spin_unlock(&evdi->event_lock);
}

static void evdi_drm_device_release_cb(__always_unused struct drm_device *dev,
				       __always_unused void *ptr)
{
	struct evdi_device *evdi = dev->dev_private;

	evdi_cursor_free(evdi->cursor);
	evdi_painter_cleanup(evdi->painter);
	kfree(evdi);
	dev->dev_private = NULL;
	EVDI_INFO("Evdi drm_device removed.\n");
	evdi_event_cache_put();

	EVDI_TEST_HOOK(evdi_testhook_drm_device_destroyed());
}

static int evdi_drm_device_init(struct drm_device *dev)
{
	struct evdi_device *evdi;
	int ret;

	EVDI_CHECKPT();
	evdi = kzalloc(sizeof(struct evdi_device), GFP_KERNEL);
	if (!evdi)
		return -ENOMEM;

	evdi->ddev = dev;
	evdi->dev_index = dev->primary->index;
	evdi->cursor_events_enabled = false;
	dev->dev_private = evdi;
	evdi->poll_event = none;
	init_waitqueue_head(&evdi->poll_ioct_wq);
	init_waitqueue_head(&evdi->poll_response_ioct_wq);
	atomic_set(&evdi->poll_stopping, 0);
	mutex_init(&evdi->poll_lock);
	init_completion(&evdi->poll_completion);
	evdi->poll_data_size = -1;

	ret = evdi_event_cache_get();
	if (ret)
		goto err_free;

	spin_lock_init(&evdi->event_lock);
	INIT_LIST_HEAD(&evdi->event_queue);
#if defined(EVDI_HAVE_XARRAY)
	xa_init_flags(&evdi->event_xa, XA_FLAGS_ALLOC);
#else
	idr_init(&evdi->event_idr);
#endif
	atomic_set(&evdi->next_event_id, 1);

	ret = evdi_painter_init(evdi);
	if (ret)
		goto err_free;
	ret =  evdi_cursor_init(&evdi->cursor);
	if (ret)
		goto err_free;

	evdi_modeset_init(dev);

	ret = drm_vblank_init(dev, 1);
	if (ret)
		goto err_init;
	drm_kms_helper_poll_init(dev);

#if KERNEL_VERSION(5, 8, 0) <= LINUX_VERSION_CODE || defined(EL8)
	ret = drmm_add_action_or_reset(dev, evdi_drm_device_release_cb, NULL);
	if (ret)
		goto err_init;
#endif

	return 0;

err_init:
err_free:
	EVDI_ERROR("Failed to setup drm device %d\n", ret);
	evdi_cursor_free(evdi->cursor);
	kfree(evdi->painter);
	kfree(evdi);
	dev->dev_private = NULL;
	return ret;
}

int evdi_driver_open(struct drm_device *dev, __always_unused struct drm_file *file)
{
	char buf[100];

	evdi_log_process(buf, sizeof(buf));
	if (dev && dev->dev_private) {
		struct evdi_device *evdi = dev->dev_private;
		atomic_set(&evdi->poll_stopping, 0);
	}
	EVDI_INFO("(card%d) Opened by %s\n", dev->primary->index, buf);
	return 0;
}

static void evdi_driver_close(struct drm_device *drm_dev, struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;

	EVDI_CHECKPT();
	if (evdi)
		evdi_painter_close(evdi, file);
}

void evdi_driver_preclose(struct drm_device *drm_dev, struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;
	if (evdi) {
		atomic_set(&evdi->poll_stopping, 1);
		wake_up_interruptible_all(&evdi->poll_ioct_wq);
		wake_up_all(&evdi->poll_response_ioct_wq);
	}
	evdi_driver_close(drm_dev, file);
}

void evdi_driver_postclose(struct drm_device *dev, struct drm_file *file)
{
	char buf[100];

	evdi_log_process(buf, sizeof(buf));
	evdi_driver_close(dev, file);
	evdi_cancel_events_for_file(dev->dev_private, file);
	EVDI_INFO("(card%d) Closed by %s\n", dev->primary->index, buf);
}

struct drm_device *evdi_drm_device_create(struct device *parent)
{
	struct drm_device *dev = NULL;
	int ret;

	dev = drm_dev_alloc(&driver, parent);
	if (IS_ERR(dev))
		return dev;

	ret = evdi_drm_device_init(dev);
	if (ret)
		goto err_free;

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_free;

	return dev;

err_free:
	drm_dev_put(dev);
	return ERR_PTR(ret);
}

static void evdi_drm_device_deinit(struct drm_device *dev)
{
	drm_kms_helper_poll_fini(dev);
	evdi_modeset_cleanup(dev);
	drm_atomic_helper_shutdown(dev);
}

int evdi_drm_device_remove(struct drm_device *dev)
{
	drm_dev_unplug(dev);
	evdi_drm_device_deinit(dev);
#if KERNEL_VERSION(5, 8, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	evdi_drm_device_release_cb(dev, NULL);
#endif
	drm_dev_put(dev);
	return 0;
}
