// SPDX-License-Identifier: GPL-2.0
/*
 * ntsync.c - NT synchronization primitive emulation
 *
 * Copyright (c) 2024-2025 Zebediah Figura <zfigura@codeweavers.com>
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/hrtimer.h>
#include <linux/atomic.h>
#include <linux/overflow.h>
#include <linux/file.h>
#include <linux/compat.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/anon_inodes.h>

#include <uapi/linux/ntsync.h>

#define NTSYNC_NAME	"ntsync"

enum ntsync_type {
	NTSYNC_TYPE_SEM,
	NTSYNC_TYPE_MUTEX,
	NTSYNC_TYPE_EVENT,
};

struct ntsync_device {
	struct mutex wait_all_lock;
	struct file *file;
};

struct ntsync_obj {
	spinlock_t lock;
	int dev_locked;
	enum ntsync_type type;
	struct file *file;
	struct ntsync_device *dev;

	union {
		struct {
			__u32 count;
			__u32 max;
		} sem;
		struct {
			__u32 count;
			pid_t owner;
			bool ownerdead;
		} mutex;
		struct {
			bool manual;
			bool signaled;
		} event;
	} u;

	struct list_head any_waiters;
	struct list_head all_waiters;
	atomic_t all_hint;
};

struct ntsync_q_entry {
	struct list_head node;
	struct ntsync_q *q;
	struct ntsync_obj *obj;
	__u32 index;
};

struct ntsync_q {
	struct task_struct *task;
	__u32 owner;
	atomic_t signaled;
	bool all;
	bool ownerdead;
	__u32 count;
	struct ntsync_q_entry entries[];
};

/* Forward declarations for ioctl helpers */
static int ntsync_sem_release(struct ntsync_obj *obj, void __user *arg);
static int ntsync_mutex_unlock(struct ntsync_obj *obj, void __user *arg);
static int ntsync_mutex_kill(struct ntsync_obj *obj, void __user *arg);
static int ntsync_event_set(struct ntsync_obj *obj, void __user *arg);
static int ntsync_event_reset(struct ntsync_obj *obj, void __user *arg);
static int ntsync_event_pulse(struct ntsync_obj *obj, void __user *arg);
static int ntsync_read_obj(struct ntsync_obj *obj, void __user *arg, int type);

static void ntsync_obj_get(struct ntsync_obj *obj)
{
	get_file(obj->file);
}

static void ntsync_obj_put(struct ntsync_obj *obj)
{
	fput(obj->file);
}

static int ntsync_obj_release(struct inode *inode, struct file *file)
{
	struct ntsync_obj *obj = file->private_data;

	kfree(obj);
	return 0;
}

static long ntsync_obj_ioctl(struct file *file, unsigned int cmd,
			     unsigned long param)
{
	struct ntsync_obj *obj = file->private_data;
	void __user *arg = (void __user *)param;

	switch (cmd) {
	case NTSYNC_IOC_SEM_RELEASE:
		return ntsync_sem_release(obj, arg);
	case NTSYNC_IOC_SEM_READ:
		return ntsync_read_obj(obj, arg, NTSYNC_TYPE_SEM);
	case NTSYNC_IOC_MUTEX_UNLOCK:
		return ntsync_mutex_unlock(obj, arg);
	case NTSYNC_IOC_MUTEX_KILL:
		return ntsync_mutex_kill(obj, arg);
	case NTSYNC_IOC_MUTEX_READ:
		return ntsync_read_obj(obj, arg, NTSYNC_TYPE_MUTEX);
	case NTSYNC_IOC_EVENT_SET:
		return ntsync_event_set(obj, arg);
	case NTSYNC_IOC_EVENT_RESET:
		return ntsync_event_reset(obj, arg);
	case NTSYNC_IOC_EVENT_PULSE:
		return ntsync_event_pulse(obj, arg);
	case NTSYNC_IOC_EVENT_READ:
		return ntsync_read_obj(obj, arg, NTSYNC_TYPE_EVENT);
	default:
		return -ENOIOCTLCMD;
	}
}

static const struct file_operations ntsync_obj_fops = {
	.owner		= THIS_MODULE,
	.release	= ntsync_obj_release,
	.unlocked_ioctl	= ntsync_obj_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct ntsync_obj *ntsync_alloc_obj(struct ntsync_device *dev,
					   enum ntsync_type type)
{
	struct ntsync_obj *obj;
	struct file *file;
	int fd;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		kfree(obj);
		return ERR_PTR(fd);
	}

	file = anon_inode_getfile("[ntsync]", &ntsync_obj_fops, obj, O_CLOEXEC);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		kfree(obj);
		return ERR_PTR(PTR_ERR(file));
	}

	spin_lock_init(&obj->lock);
	obj->dev_locked = false;
	obj->type = type;
	obj->file = file;
	obj->dev = dev;
	INIT_LIST_HEAD(&obj->any_waiters);
	INIT_LIST_HEAD(&obj->all_waiters);
	atomic_set(&obj->all_hint, 0);

	fd_install(fd, file);

	return obj;
}

static int ntsync_create_sem(struct ntsync_device *dev, void __user *arg)
{
	struct ntsync_sem_args __user *uargs = arg;
	struct ntsync_sem_args args;
	struct ntsync_obj *obj;

	if (copy_from_user(&args, uargs, sizeof(args)))
		return -EFAULT;

	if (!args.max)
		return -EINVAL;

	obj = ntsync_alloc_obj(dev, NTSYNC_TYPE_SEM);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	obj->u.sem.count = args.count;
	obj->u.sem.max = args.max;

	return 0;
}

static int ntsync_create_mutex(struct ntsync_device *dev, void __user *arg)
{
	struct ntsync_mutex_args __user *uargs = arg;
	struct ntsync_mutex_args args;
	struct ntsync_obj *obj;

	if (copy_from_user(&args, uargs, sizeof(args)))
		return -EFAULT;

	obj = ntsync_alloc_obj(dev, NTSYNC_TYPE_MUTEX);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	obj->u.mutex.owner = args.owner;
	obj->u.mutex.count = args.count;

	return 0;
}

static int ntsync_create_event(struct ntsync_device *dev, void __user *arg)
{
	struct ntsync_event_args __user *uargs = arg;
	struct ntsync_event_args args;
	struct ntsync_obj *obj;

	if (copy_from_user(&args, uargs, sizeof(args)))
		return -EFAULT;

	obj = ntsync_alloc_obj(dev, NTSYNC_TYPE_EVENT);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	obj->u.event.manual = args.manual;
	obj->u.event.signaled = args.signaled;

	return 0;
}

static void try_wake_any_sem(struct ntsync_obj *obj)
{
	struct ntsync_q_entry *entry;
	struct ntsync_q *q;

	list_for_each_entry(entry, &obj->any_waiters, node) {
		q = entry->q;

		if (obj->u.sem.count) {
			obj->u.sem.count--;
			atomic_set(&q->signaled, 1);
			wake_up_process(q->task);
			return;
		}
	}
}

static void try_wake_any_mutex(struct ntsync_obj *obj)
{
	struct ntsync_q_entry *entry;
	struct ntsync_q *q;

	list_for_each_entry(entry, &obj->any_waiters, node) {
		q = entry->q;

		if (!obj->u.mutex.count) {
			obj->u.mutex.count = 1;
			obj->u.mutex.owner = q->owner;
			obj->u.mutex.ownerdead = false;
			atomic_set(&q->signaled, 1);
			wake_up_process(q->task);
			return;
		}
	}
}

static void try_wake_any_event(struct ntsync_obj *obj)
{
	struct ntsync_q_entry *entry;
	struct ntsync_q *q;

	if (!obj->u.event.signaled)
		return;

	list_for_each_entry(entry, &obj->any_waiters, node) {
		q = entry->q;

		if (!obj->u.event.manual)
			obj->u.event.signaled = false;

		atomic_set(&q->signaled, 1);
		wake_up_process(q->task);
		return;
	}
}

static void try_wake_any(struct ntsync_obj *obj)
{
	switch (obj->type) {
	case NTSYNC_TYPE_SEM:
		try_wake_any_sem(obj);
		break;
	case NTSYNC_TYPE_MUTEX:
		try_wake_any_mutex(obj);
		break;
	case NTSYNC_TYPE_EVENT:
		try_wake_any_event(obj);
		break;
	}
}

static int ntsync_sem_release(struct ntsync_obj *obj, void __user *arg)
{
	__u32 __user *uarg = arg;
	__u32 count;

	if (get_user(count, uarg))
		return -EFAULT;

	spin_lock(&obj->lock);

	if (obj->u.sem.count + count < obj->u.sem.count ||
	    obj->u.sem.count + count > obj->u.sem.max) {
		spin_unlock(&obj->lock);
		return -EINVAL;
	}

	obj->u.sem.count += count;

	try_wake_any(obj);

	spin_unlock(&obj->lock);

	return put_user(obj->u.sem.count, uarg);
}

static int ntsync_mutex_unlock(struct ntsync_obj *obj, void __user *arg)
{
	struct ntsync_mutex_args __user *uarg = arg;
	struct ntsync_mutex_args args;

	if (copy_from_user(&args, uarg, sizeof(args)))
		return -EFAULT;

	spin_lock(&obj->lock);

	if (!obj->u.mutex.count || obj->u.mutex.owner != args.owner) {
		spin_unlock(&obj->lock);
		return -EINVAL;
	}

	if (args.owner != current->tgid) {
		obj->u.mutex.ownerdead = true;
		spin_unlock(&obj->lock);
		return -ECHILD;
	}

	obj->u.mutex.count = 0;
	obj->u.mutex.owner = 0;

	try_wake_any(obj);

	spin_unlock(&obj->lock);

	return 0;
}

static int ntsync_mutex_kill(struct ntsync_obj *obj, void __user *arg)
{
	__u32 __user *uarg = arg;
	__u32 owner;

	if (get_user(owner, uarg))
		return -EFAULT;

	spin_lock(&obj->lock);

	if (!obj->u.mutex.count || obj->u.mutex.owner != owner) {
		spin_unlock(&obj->lock);
		return -EINVAL;
	}

	obj->u.mutex.ownerdead = true;
	obj->u.mutex.count = 0;
	obj->u.mutex.owner = 0;

	try_wake_any(obj);

	spin_unlock(&obj->lock);

	return 0;
}

static int ntsync_event_set(struct ntsync_obj *obj, void __user *arg)
{
	__u32 __user *uarg = arg;

	spin_lock(&obj->lock);

	obj->u.event.signaled = true;

	try_wake_any(obj);

	spin_unlock(&obj->lock);

	return put_user(obj->u.event.signaled, uarg);
}

static int ntsync_event_reset(struct ntsync_obj *obj, void __user *arg)
{
	__u32 __user *uarg = arg;

	spin_lock(&obj->lock);

	obj->u.event.signaled = false;

	spin_unlock(&obj->lock);

	return put_user(obj->u.event.signaled, uarg);
}

static int ntsync_event_pulse(struct ntsync_obj *obj, void __user *arg)
{
	__u32 __user *uarg = arg;

	spin_lock(&obj->lock);

	obj->u.event.signaled = false;

	try_wake_any(obj);

	spin_unlock(&obj->lock);

	return put_user(obj->u.event.signaled, uarg);
}

static int ntsync_read_obj(struct ntsync_obj *obj, void __user *arg, int type)
{
	switch (type) {
	case NTSYNC_TYPE_SEM: {
		struct ntsync_sem_args __user *uarg = arg;
		struct ntsync_sem_args args;

		spin_lock(&obj->lock);
		args.count = obj->u.sem.count;
		args.max = obj->u.sem.max;
		spin_unlock(&obj->lock);

		if (copy_to_user(uarg, &args, sizeof(args)))
			return -EFAULT;
		return 0;
	}
	case NTSYNC_TYPE_MUTEX: {
		struct ntsync_mutex_args __user *uarg = arg;
		struct ntsync_mutex_args args;

		spin_lock(&obj->lock);
		args.owner = obj->u.mutex.owner;
		args.count = obj->u.mutex.count;
		spin_unlock(&obj->lock);

		if (copy_to_user(uarg, &args, sizeof(args)))
			return -EFAULT;
		return 0;
	}
	case NTSYNC_TYPE_EVENT: {
		struct ntsync_event_args __user *uarg = arg;
		struct ntsync_event_args args;

		spin_lock(&obj->lock);
		args.manual = obj->u.event.manual;
		args.signaled = obj->u.event.signaled;
		spin_unlock(&obj->lock);

		if (copy_to_user(uarg, &args, sizeof(args)))
			return -EFAULT;
		return 0;
	}
	default:
		return -EINVAL;
	}
}

static struct ntsync_obj *get_obj(struct ntsync_device *dev, int fd)
{
	struct file *file;
	struct ntsync_obj *obj;

	file = fget(fd);
	if (!file)
		return ERR_PTR(-EBADF);

	if (file->f_op != &ntsync_obj_fops) {
		fput(file);
		return ERR_PTR(-EINVAL);
	}

	obj = file->private_data;
	ntsync_obj_get(obj);

	return obj;
}

static void put_obj(struct ntsync_obj *obj)
{
	ntsync_obj_put(obj);
}

static int setup_wait(struct ntsync_device *dev,
		      struct ntsync_wait_args *args, bool all,
		      struct ntsync_q **q_out)
{
	struct ntsync_q *q;
	int i, ret;

	if (args->count > NTSYNC_MAX_WAIT_COUNT)
		return -EINVAL;

	q = kzalloc(struct_size(q, entries, args->count), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	q->task = current;
	q->owner = args->owner;
	atomic_set(&q->signaled, 0);
	q->all = all;
	q->ownerdead = false;
	q->count = args->count;

	for (i = 0; i < args->count; i++) {
		int fd;

		if (get_user(fd, &((__u32 __user *)args->objs)[i])) {
			kfree(q);
			return -EFAULT;
		}

		q->entries[i].obj = get_obj(dev, fd);
		if (IS_ERR(q->entries[i].obj)) {
			ret = PTR_ERR(q->entries[i].obj);
			while (i--)
				put_obj(q->entries[i].obj);
			kfree(q);
			return ret;
		}
		q->entries[i].q = q;
		q->entries[i].index = i;
	}

	*q_out = q;
	return 0;
}

static int ntsync_wait_any(struct ntsync_device *dev, void __user *arg)
{
	struct ntsync_wait_args args;
	struct ntsync_q *q;
	int ret, i;
	ktime_t timeout;

	if (copy_from_user(&args, arg, sizeof(args)))
		return -EFAULT;

	ret = setup_wait(dev, &args, false, &q);
	if (ret)
		return ret;

	if (args.timeout != U64_MAX) {
		if (args.flags & NTSYNC_WAIT_REALTIME)
			timeout = ktime_get_real();
		else
			timeout = ktime_get();
		timeout += ns_to_ktime(args.timeout);
	} else {
		timeout = KTIME_MAX;
	}

	current->state = TASK_INTERRUPTIBLE;

	for (i = 0; i < args.count; i++) {
		struct ntsync_obj *obj = q->entries[i].obj;

		spin_lock(&obj->lock);

		if (obj->type == NTSYNC_TYPE_SEM && obj->u.sem.count) {
			obj->u.sem.count--;
			atomic_set(&q->signaled, 1);
			args.index = i;
			spin_unlock(&obj->lock);
			goto done;
		}
		if (obj->type == NTSYNC_TYPE_MUTEX && !obj->u.mutex.count) {
			obj->u.mutex.count = 1;
			obj->u.mutex.owner = q->owner;
			obj->u.mutex.ownerdead = false;
			atomic_set(&q->signaled, 1);
			args.index = i;
			spin_unlock(&obj->lock);
			goto done;
		}
		if (obj->type == NTSYNC_TYPE_EVENT && obj->u.event.signaled) {
			if (!obj->u.event.manual)
				obj->u.event.signaled = false;
			atomic_set(&q->signaled, 1);
			args.index = i;
			spin_unlock(&obj->lock);
			goto done;
		}

		list_add_tail(&q->entries[i].node, &obj->any_waiters);
		spin_unlock(&obj->lock);
	}

	if (timeout != KTIME_MAX) {
		if (timeout <= ktime_get()) {
			ret = -ETIMEDOUT;
			goto removed;
		}
		schedule_hrtimeout_range(&timeout, HRTIMER_MODE_ABS, 0);
	} else {
		schedule();
	}

	if (signal_pending(current)) {
		ret = -ERESTARTSYS;
		goto removed;
	}

	if (atomic_read(&q->signaled)) {
		args.index = q->entries[0].index;
		goto done;
	}

	ret = -ETIMEDOUT;
	goto removed;

removed:
	for (i = 0; i < args.count; i++) {
		struct ntsync_obj *obj = q->entries[i].obj;

		spin_lock(&obj->lock);
		list_del(&q->entries[i].node);
		spin_unlock(&obj->lock);
	}

done:
	current->state = TASK_RUNNING;

	for (i = 0; i < args.count; i++)
		put_obj(q->entries[i].obj);

	if (!ret) {
		if (q->ownerdead)
			ret = -ECHILD;
		else if (copy_to_user(arg, &args, sizeof(args)))
			ret = -EFAULT;
	}

	kfree(q);
	return ret;
}

static int ntsync_wait_all(struct ntsync_device *dev, void __user *arg)
{
	struct ntsync_wait_args args;
	struct ntsync_q *q;
	int ret, i, locked_count = 0;
	ktime_t timeout;

	if (copy_from_user(&args, arg, sizeof(args)))
		return -EFAULT;

	ret = setup_wait(dev, &args, true, &q);
	if (ret)
		return ret;

	if (args.timeout != U64_MAX) {
		if (args.flags & NTSYNC_WAIT_REALTIME)
			timeout = ktime_get_real();
		else
			timeout = ktime_get();
		timeout += ns_to_ktime(args.timeout);
	} else {
		timeout = KTIME_MAX;
	}

	mutex_lock(&dev->wait_all_lock);

	for (i = 0; i < args.count; i++) {
		struct ntsync_obj *obj = q->entries[i].obj;

		spin_lock(&obj->lock);
		obj->dev_locked = true;
		spin_unlock(&obj->lock);
		locked_count = i + 1;
	}

	current->state = TASK_INTERRUPTIBLE;

try_again:
	for (i = 0; i < args.count; i++) {
		struct ntsync_obj *obj = q->entries[i].obj;

		if (obj->type == NTSYNC_TYPE_SEM && !obj->u.sem.count)
			goto sleep;
		if (obj->type == NTSYNC_TYPE_MUTEX && obj->u.mutex.count &&
		    obj->u.mutex.owner != q->owner)
			goto sleep;
		if (obj->type == NTSYNC_TYPE_EVENT && !obj->u.event.signaled)
			goto sleep;
	}

	for (i = 0; i < args.count; i++) {
		struct ntsync_obj *obj = q->entries[i].obj;

		switch (obj->type) {
		case NTSYNC_TYPE_SEM:
			obj->u.sem.count--;
			break;
		case NTSYNC_TYPE_MUTEX:
			obj->u.mutex.count = 1;
			obj->u.mutex.owner = q->owner;
			obj->u.mutex.ownerdead = false;
			break;
		case NTSYNC_TYPE_EVENT:
			if (!obj->u.event.manual)
				obj->u.event.signaled = false;
			break;
		}

		list_add_tail(&q->entries[i].node, &obj->all_waiters);
	}

	atomic_set(&q->signaled, 1);
	goto done_wake;

sleep:
	if (timeout != KTIME_MAX) {
		if (timeout <= ktime_get()) {
			ret = -ETIMEDOUT;
			goto done;
		}
		schedule_hrtimeout_range(&timeout, HRTIMER_MODE_ABS, 0);
	} else {
		schedule();
	}

	if (signal_pending(current)) {
		ret = -ERESTARTSYS;
		goto done;
	}

	if (atomic_read(&q->signaled))
		goto done;

	goto try_again;

done_wake:
	args.index = 0;

done:
	for (i = 0; i < locked_count; i++) {
		struct ntsync_obj *obj = q->entries[i].obj;

		list_del(&q->entries[i].node);

		spin_lock(&obj->lock);
		obj->dev_locked = false;
		spin_unlock(&obj->lock);
	}

	mutex_unlock(&dev->wait_all_lock);

	current->state = TASK_RUNNING;

	for (i = 0; i < args.count; i++)
		put_obj(q->entries[i].obj);

	if (!ret) {
		if (q->ownerdead)
			ret = -ECHILD;
		else if (copy_to_user(arg, &args, sizeof(args)))
			ret = -EFAULT;
	}

	kfree(q);
	return ret;
}

static long ntsync_device_ioctl(struct file *file, unsigned int cmd,
				unsigned long param)
{
	struct ntsync_device *dev = file->private_data;
	void __user *arg = (void __user *)param;

	switch (cmd) {
	case NTSYNC_IOC_CREATE_SEM:
		return ntsync_create_sem(dev, arg);
	case NTSYNC_IOC_CREATE_MUTEX:
		return ntsync_create_mutex(dev, arg);
	case NTSYNC_IOC_CREATE_EVENT:
		return ntsync_create_event(dev, arg);
	case NTSYNC_IOC_WAIT_ANY:
		return ntsync_wait_any(dev, arg);
	case NTSYNC_IOC_WAIT_ALL:
		return ntsync_wait_all(dev, arg);
	default:
		return -ENOIOCTLCMD;
	}
}

static int ntsync_device_release(struct inode *inode, struct file *file)
{
	struct ntsync_device *dev = file->private_data;

	mutex_destroy(&dev->wait_all_lock);
	kfree(dev);
	return 0;
}

static int ntsync_device_open(struct inode *inode, struct file *file)
{
	struct ntsync_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_init(&dev->wait_all_lock);
	dev->file = file;
	file->private_data = dev;

	return 0;
}

static const struct file_operations ntsync_fops = {
	.owner		= THIS_MODULE,
	.open		= ntsync_device_open,
	.release	= ntsync_device_release,
	.unlocked_ioctl	= ntsync_device_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice ntsync_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= NTSYNC_NAME,
	.fops	= &ntsync_fops,
	.mode	= 0666,
};

module_misc_device(ntsync_misc);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zebediah Figura");
MODULE_DESCRIPTION("NT synchronization primitive emulation");
