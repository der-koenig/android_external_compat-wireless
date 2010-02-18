/*
 * Copyright 2007-2010	Luis R. Rodriguez <mcgrof@winlab.rutgers.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Compatibility file for Linux wireless for kernels 2.6.25.
 */

#include <net/compat.h>

/* All things not in 2.6.22, 2.6.23 and 2.6.24 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25))

#include <linux/miscdevice.h>

/*
 * Backport work for QoS dependencies (kernel/pm_qos_params.c)
 *
 * ipw2100 now makes use of
 * pm_qos_add_requirement(), 
 * pm_qos_update_requirement() and
 * pm_qos_remove_requirement() from it
 *
 * mac80211 use the network latency to determine if to enable or not
 * dynamic PS. mac80211 also and registers a notifier for when
 * the latency changes. Since older kernels do no thave pm-qos stuff
 * we just implement it completley here and register it upon cfg80211
 * init. I haven't tested ipw2100 on 2.6.24 though.
 *
 * This is copied from the kernel written by mark gross mgross@linux.intel.com
 */

/*
 * locking rule: all changes to target_value or requirements or notifiers lists
 * or pm_qos_object list and pm_qos_objects need to happen with pm_qos_lock
 * held, taken with _irqsave.  One lock to rule them all
 */
struct requirement_list {
	struct list_head list;
	union {
		s32 value;
		s32 usec;
		s32 kbps;
	};
	char *name;
};

static s32 max_compare(s32 v1, s32 v2);
static s32 min_compare(s32 v1, s32 v2);

struct pm_qos_object {
	struct requirement_list requirements;
	struct blocking_notifier_head *notifiers;
	struct miscdevice pm_qos_power_miscdev;
	char *name;
	s32 default_value;
	s32 target_value;
	s32 (*comparitor)(s32, s32);
};

static struct pm_qos_object null_pm_qos;
static BLOCKING_NOTIFIER_HEAD(cpu_dma_lat_notifier);
static struct pm_qos_object cpu_dma_pm_qos = {
	.requirements = {LIST_HEAD_INIT(cpu_dma_pm_qos.requirements.list)},
	.notifiers = &cpu_dma_lat_notifier,
	.name = "cpu_dma_latency",
	.default_value = 2000 * USEC_PER_SEC,
	.target_value = 2000 * USEC_PER_SEC,
	.comparitor = min_compare
};

static BLOCKING_NOTIFIER_HEAD(network_lat_notifier);
static struct pm_qos_object network_lat_pm_qos = {
	.requirements = {LIST_HEAD_INIT(network_lat_pm_qos.requirements.list)},
	.notifiers = &network_lat_notifier,
	.name = "network_latency",
	.default_value = 2000 * USEC_PER_SEC,
	.target_value = 2000 * USEC_PER_SEC,
	.comparitor = min_compare
};


static BLOCKING_NOTIFIER_HEAD(network_throughput_notifier);
static struct pm_qos_object network_throughput_pm_qos = {
	.requirements =
		{LIST_HEAD_INIT(network_throughput_pm_qos.requirements.list)},
	.notifiers = &network_throughput_notifier,
	.name = "network_throughput",
	.default_value = 0,
	.target_value = 0,
	.comparitor = max_compare
};


static struct pm_qos_object *pm_qos_array[] = {
	&null_pm_qos,
	&cpu_dma_pm_qos,
	&network_lat_pm_qos,
	&network_throughput_pm_qos
};

static DEFINE_SPINLOCK(pm_qos_lock);

static ssize_t pm_qos_power_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *f_pos);
static int pm_qos_power_open(struct inode *inode, struct file *filp);
static int pm_qos_power_release(struct inode *inode, struct file *filp);

static const struct file_operations pm_qos_power_fops = {
        .write = pm_qos_power_write,
        .open = pm_qos_power_open,
        .release = pm_qos_power_release,
};

/* static helper functions */
static s32 max_compare(s32 v1, s32 v2)
{
	return max(v1, v2);
}

static s32 min_compare(s32 v1, s32 v2)
{
	return min(v1, v2);
}

static void update_target(int target)
{
	s32 extreme_value;
	struct requirement_list *node;
	unsigned long flags;
	int call_notifier = 0;

	spin_lock_irqsave(&pm_qos_lock, flags);
	extreme_value = pm_qos_array[target]->default_value;
	list_for_each_entry(node,
			&pm_qos_array[target]->requirements.list, list) {
		extreme_value = pm_qos_array[target]->comparitor(
				extreme_value, node->value);
	}
	if (pm_qos_array[target]->target_value != extreme_value) {
		call_notifier = 1;
		pm_qos_array[target]->target_value = extreme_value;
		pr_debug(KERN_ERR "new target for qos %d is %d\n", target,
			pm_qos_array[target]->target_value);
	}
	spin_unlock_irqrestore(&pm_qos_lock, flags);

	if (call_notifier)
		blocking_notifier_call_chain(pm_qos_array[target]->notifiers,
			(unsigned long) extreme_value, NULL);
}

static int register_pm_qos_misc(struct pm_qos_object *qos)
{
	qos->pm_qos_power_miscdev.minor = MISC_DYNAMIC_MINOR;
	qos->pm_qos_power_miscdev.name = qos->name;
	qos->pm_qos_power_miscdev.fops = &pm_qos_power_fops;

	return misc_register(&qos->pm_qos_power_miscdev);
}

static int find_pm_qos_object_by_minor(int minor)
{
	int pm_qos_class;

	for (pm_qos_class = 0;
		pm_qos_class < PM_QOS_NUM_CLASSES; pm_qos_class++) {
		if (minor ==
			pm_qos_array[pm_qos_class]->pm_qos_power_miscdev.minor)
			return pm_qos_class;
	}
	return -1;
}

/**
 * pm_qos_requirement - returns current system wide qos expectation
 * @pm_qos_class: identification of which qos value is requested
 *
 * This function returns the current target value in an atomic manner.
 */
int pm_qos_requirement(int pm_qos_class)
{
	int ret_val;
	unsigned long flags;

	spin_lock_irqsave(&pm_qos_lock, flags);
	ret_val = pm_qos_array[pm_qos_class]->target_value;
	spin_unlock_irqrestore(&pm_qos_lock, flags);

	return ret_val;
}
EXPORT_SYMBOL_GPL(pm_qos_requirement);

/**
 * pm_qos_add_requirement - inserts new qos request into the list
 * @pm_qos_class: identifies which list of qos request to us
 * @name: identifies the request
 * @value: defines the qos request
 *
 * This function inserts a new entry in the pm_qos_class list of requested qos
 * performance charactoistics.  It recomputes the agregate QoS expectations for
 * the pm_qos_class of parrameters.
 */
int pm_qos_add_requirement(int pm_qos_class, char *name, s32 value)
{
	struct requirement_list *dep;
	unsigned long flags;

	dep = kzalloc(sizeof(struct requirement_list), GFP_KERNEL);
	if (dep) {
		if (value == PM_QOS_DEFAULT_VALUE)
			dep->value = pm_qos_array[pm_qos_class]->default_value;
		else
			dep->value = value;
		dep->name = kstrdup(name, GFP_KERNEL);
		if (!dep->name)
			goto cleanup;

		spin_lock_irqsave(&pm_qos_lock, flags);
		list_add(&dep->list,
			&pm_qos_array[pm_qos_class]->requirements.list);
		spin_unlock_irqrestore(&pm_qos_lock, flags);
		update_target(pm_qos_class);

		return 0;
	}

cleanup:
	kfree(dep);
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(pm_qos_add_requirement);

/**
 * pm_qos_update_requirement - modifies an existing qos request
 * @pm_qos_class: identifies which list of qos request to us
 * @name: identifies the request
 * @value: defines the qos request
 *
 * Updates an existing qos requierement for the pm_qos_class of parameters along
 * with updating the target pm_qos_class value.
 *
 * If the named request isn't in the lest then no change is made.
 */
int pm_qos_update_requirement(int pm_qos_class, char *name, s32 new_value)
{
	unsigned long flags;
	struct requirement_list *node;
	int pending_update = 0;

	spin_lock_irqsave(&pm_qos_lock, flags);
	list_for_each_entry(node,
		&pm_qos_array[pm_qos_class]->requirements.list, list) {
		if (strcmp(node->name, name) == 0) {
			if (new_value == PM_QOS_DEFAULT_VALUE)
				node->value =
				pm_qos_array[pm_qos_class]->default_value;
			else
				node->value = new_value;
			pending_update = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&pm_qos_lock, flags);
	if (pending_update)
		update_target(pm_qos_class);

	return 0;
}
EXPORT_SYMBOL_GPL(pm_qos_update_requirement);

/**
 * pm_qos_remove_requirement - modifies an existing qos request
 * @pm_qos_class: identifies which list of qos request to us
 * @name: identifies the request
 *
 * Will remove named qos request from pm_qos_class list of parrameters and
 * recompute the current target value for the pm_qos_class.
 */
void pm_qos_remove_requirement(int pm_qos_class, char *name)
{
	unsigned long flags;
	struct requirement_list *node;
	int pending_update = 0;

	spin_lock_irqsave(&pm_qos_lock, flags);
	list_for_each_entry(node,
		&pm_qos_array[pm_qos_class]->requirements.list, list) {
		if (strcmp(node->name, name) == 0) {
			kfree(node->name);
			list_del(&node->list);
			kfree(node);
			pending_update = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&pm_qos_lock, flags);
	if (pending_update)
		update_target(pm_qos_class);
}
EXPORT_SYMBOL_GPL(pm_qos_remove_requirement);

/**
 * pm_qos_add_notifier - sets notification entry for changes to target value
 * @pm_qos_class: identifies which qos target changes should be notified.
 * @notifier: notifier block managed by caller.
 *
 * will register the notifier into a notification chain that gets called
 * uppon changes to the pm_qos_class target value.
 */
 int pm_qos_add_notifier(int pm_qos_class, struct notifier_block *notifier)
{
	int retval;

	retval = blocking_notifier_chain_register(
			pm_qos_array[pm_qos_class]->notifiers, notifier);

	return retval;
}
EXPORT_SYMBOL_GPL(pm_qos_add_notifier);

/**
 * pm_qos_remove_notifier - deletes notification entry from chain.
 * @pm_qos_class: identifies which qos target changes are notified.
 * @notifier: notifier block to be removed.
 *
 * will remove the notifier from the notification chain that gets called
 * uppon changes to the pm_qos_class target value.
 */
int pm_qos_remove_notifier(int pm_qos_class, struct notifier_block *notifier)
{
	int retval;

	retval = blocking_notifier_chain_unregister(
			pm_qos_array[pm_qos_class]->notifiers, notifier);

	return retval;
}
EXPORT_SYMBOL_GPL(pm_qos_remove_notifier);

#define PID_NAME_LEN sizeof("process_1234567890")
static char name[PID_NAME_LEN];

static int pm_qos_power_open(struct inode *inode, struct file *filp)
{
	int ret;
	long pm_qos_class;

	pm_qos_class = find_pm_qos_object_by_minor(iminor(inode));
	if (pm_qos_class >= 0) {
		filp->private_data = (void *)pm_qos_class;
		sprintf(name, "process_%d", current->pid);
		ret = pm_qos_add_requirement(pm_qos_class, name,
					PM_QOS_DEFAULT_VALUE);
		if (ret >= 0)
			return 0;
	}

	return -EPERM;
}

static int pm_qos_power_release(struct inode *inode, struct file *filp)
{
	int pm_qos_class;

	pm_qos_class = (long)filp->private_data;
	sprintf(name, "process_%d", current->pid);
	pm_qos_remove_requirement(pm_qos_class, name);

	return 0;
}

static ssize_t pm_qos_power_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *f_pos)
{
	s32 value;
	int pm_qos_class;

	pm_qos_class = (long)filp->private_data;
	if (count != sizeof(s32))
		return -EINVAL;
	if (copy_from_user(&value, buf, sizeof(s32)))
		return -EFAULT;
	sprintf(name, "process_%d", current->pid);
	pm_qos_update_requirement(pm_qos_class, name, value);

	return  sizeof(s32);
}

/*
 * Will be called by cfg80211, if your non-cfg80211 driver needs this just
 * call it right at the start of your probe.
 */
int compat_pm_qos_power_init(void)
{
	int ret = 0;

	ret = register_pm_qos_misc(&cpu_dma_pm_qos);
	if (ret < 0) {
		printk(KERN_ERR "pm_qos_param: cpu_dma_latency setup failed\n");
		return ret;
	}

	ret = register_pm_qos_misc(&network_lat_pm_qos);
	if (ret < 0) {
		printk(KERN_ERR "pm_qos_param: network_latency setup failed\n");
		return ret;
	}

	ret = register_pm_qos_misc(&network_throughput_pm_qos);
	if (ret < 0)
		printk(KERN_ERR
			"pm_qos_param: network_throughput setup failed\n");

	return ret;
}

int compat_pm_qos_power_deinit(void)
{
	int ret = 0;

	ret = misc_deregister(&cpu_dma_pm_qos.pm_qos_power_miscdev);
	if (ret < 0) {
		printk(KERN_ERR "pm_qos_param: cpu_dma_latency deinit failed\n");
		return ret;
	}

	ret = misc_deregister(&network_lat_pm_qos.pm_qos_power_miscdev);
	if (ret < 0) {
		printk(KERN_ERR "pm_qos_param: network_latency deinit failed\n");
		return ret;
	}

	ret = misc_deregister(&network_throughput_pm_qos.pm_qos_power_miscdev);
	if (ret < 0)
		printk(KERN_ERR
			"pm_qos_param: network_throughput deinit failed\n");

	return ret;
}

/**
 * The following things are out of ./lib/vsprintf.c
 * The new iwlwifi driver is using them.
 */

/**
 * strict_strtoul - convert a string to an unsigned long strictly
 * @cp: The string to be converted
 * @base: The number base to use
 * @res: The converted result value
 *
 * strict_strtoul converts a string to an unsigned long only if the
 * string is really an unsigned long string, any string containing
 * any invalid char at the tail will be rejected and -EINVAL is returned,
 * only a newline char at the tail is acceptible because people generally
 * change a module parameter in the following way:
 *
 * 	echo 1024 > /sys/module/e1000/parameters/copybreak
 *
 * echo will append a newline to the tail.
 *
 * It returns 0 if conversion is successful and *res is set to the converted
 * value, otherwise it returns -EINVAL and *res is set to 0.
 *
 * simple_strtoul just ignores the successive invalid characters and
 * return the converted value of prefix part of the string.
 */
int strict_strtoul(const char *cp, unsigned int base, unsigned long *res);

/**
 * strict_strtol - convert a string to a long strictly
 * @cp: The string to be converted
 * @base: The number base to use
 * @res: The converted result value
 *
 * strict_strtol is similiar to strict_strtoul, but it allows the first
 * character of a string is '-'.
 *
 * It returns 0 if conversion is successful and *res is set to the converted
 * value, otherwise it returns -EINVAL and *res is set to 0.
 */
int strict_strtol(const char *cp, unsigned int base, long *res);

#define define_strict_strtoux(type, valtype)				\
int strict_strtou##type(const char *cp, unsigned int base, valtype *res)\
{									\
	char *tail;							\
	valtype val;							\
	size_t len;							\
									\
	*res = 0;							\
	len = strlen(cp);						\
	if (len == 0)							\
		return -EINVAL;						\
									\
	val = simple_strtou##type(cp, &tail, base);			\
	if ((*tail == '\0') ||						\
		((len == (size_t)(tail - cp) + 1) && (*tail == '\n'))) {\
		*res = val;						\
		return 0;						\
	}								\
									\
	return -EINVAL;							\
}									\

#define define_strict_strtox(type, valtype)				\
int strict_strto##type(const char *cp, unsigned int base, valtype *res)	\
{									\
	int ret;							\
	if (*cp == '-') {						\
		ret = strict_strtou##type(cp+1, base, res);		\
		if (!ret)						\
			*res = -(*res);					\
	} else								\
		ret = strict_strtou##type(cp, base, res);		\
									\
	return ret;							\
}									\

define_strict_strtoux(l, unsigned long)
define_strict_strtox(l, long)

EXPORT_SYMBOL(strict_strtoul);
EXPORT_SYMBOL(strict_strtol);

#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25) */
