#include <linux/earlysuspend.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/notifier.h>
#include <linux/list.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/kernel_stat.h>
#include <asm/cputime.h>
#include <linux/tick.h>
#include <linux/seq_file.h>
#include <linux/input.h>

/* input boost */

static u64 last_input_time = 0;
#define MIN_INPUT_INTERVAL (50 * USEC_PER_MSEC)

/* input boost end */

struct fg_pid_struct {
	struct pid *pid;
	struct list_head list;
};

LIST_HEAD(fg_pids_list);
static int fg_pid_nr = 0;
static struct pid *fg_pid = NULL;
static struct task_struct *fg_task;

extern void oom_adj_register_notify(struct notifier_block *nb);
extern void oom_adj_unregister_notify(struct notifier_block *nb);

static u32 debug_app_list = 0;
module_param(debug_app_list, uint, 0644);

static unsigned int delay = HZ;
module_param(delay, uint, 0644);

static DECLARE_WAIT_QUEUE_HEAD (jiq_wait);

static bool io_is_busy = true;

struct cpufreq_interactive_cpuinfo {
	struct timer_list cpu_timer;
	struct timer_list cpu_slack_timer;
	spinlock_t load_lock; /* protects the next 4 fields */
	u64 time_in_idle;
	u64 time_in_idle_timestamp;
	u64 cputime_speedadj;
	u64 cputime_speedadj_timestamp;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
	spinlock_t target_freq_lock; /*protects target freq */
	unsigned int target_freq;
	unsigned int floor_freq;
	unsigned int max_freq;
	unsigned int timer_rate;
	int timer_slack_val;
	unsigned int min_sample_time;
	u64 floor_validate_time;
	u64 hispeed_validate_time;
	struct rw_semaphore enable_sem;
	int governor_enabled;
	int prev_load;
	bool limits_changed;
	unsigned int active_time, idle_time;
};

static DEFINE_PER_CPU(struct cpufreq_interactive_cpuinfo, cpuinfo);

static struct task_cputime prev_app_time = {
	.utime = 0,
	.stime = 0,
	.sum_exec_runtime = 0
};

static bool suspend = false, old_suspend = false;
static unsigned int freq = 0, old_freq = 0;
static struct task_struct *old_task;

struct task_struct *zygote = NULL;

static int find_zygote(void) {
	struct task_struct *task, *_zygote = NULL;

	if (zygote != NULL && pid_alive(zygote))
		return true;

	for_each_process(task) {
		if (strcmp(task->comm, "zygote") == 0) {
			_zygote = task;
			break;
		}
	}
	zygote = _zygote;
	return (_zygote != NULL);
}

static void check_list(int pid, int adj) {
	//go through the list and remove any pids with nonzero oom_adj, empty and system pids
	struct list_head *pos = NULL;
	struct list_head *tmp = NULL;
	struct fg_pid_struct *el = NULL;

	struct task_struct *task;

	if (debug_app_list > 0) printk(KERN_ERR "app_monitor: cleaning list");
	list_for_each_safe(pos, tmp, &fg_pids_list) {
		el = list_entry(pos, struct fg_pid_struct, list);
		task = get_pid_task(el->pid, PIDTYPE_PID);
		if (!task || task->signal->oom_adj != 0 || (zygote != NULL && task->parent != zygote && task->real_parent != zygote)) {
			if (debug_app_list > 1) printk(KERN_ERR "app_monitor: removing %s", task->comm);
			put_pid(el->pid);
			list_del(pos);
			kfree(el);
		} else {
			if (debug_app_list > 1) printk(KERN_ERR "app_monitor: leaving %s", task->comm);
		};
	}
	if (debug_app_list > 0) printk(KERN_ERR "app_monitor: cleaning list done");

};

/* random notes:
 * - fs/proc/
 *   - array.c
 *   - stat.c
 * - kernel/posix-cpu-timers.c: thread_group_cputime
 * - include/linux/pid.h
 *
 * void set_user_nice(struct task_struct *p, long nice)
 *
 */
cputime_t utime_start, stime_start, cutime_start, cstime_start;
cputime_t utime_end, stime_end, cutime_end, cstime_end;

static int oom_adj_changed(struct notifier_block *self, unsigned long oom_adj, void *t)
{
	struct task_struct *task = (struct task_struct *)t;
	struct task_struct *oldtask;
	struct fg_pid_struct *el;
	struct signal_struct *sig;
	cputime_t ut, st;

	//TODO lock
	//TODO check if threads parent is zygote
	if (
		(oom_adj != 0 && task->pid != fg_pid_nr) ||	//oom_adj of non-fg app changed to non-zero
		(oom_adj == 0 && task->pid == fg_pid_nr)	//oom_adj of fg app set to zero once again
		)
		return NOTIFY_DONE;

	if (task->cred->euid < 10000)				//we don't care about non android apps
		return NOTIFY_DONE;

	if (find_zygote())
		if (task->real_parent != zygote && task->parent != zygote)
			return NOTIFY_DONE;

	if (oom_adj == 0) {
		//add it to the end of the list
		el = kmalloc(sizeof(struct fg_pid_struct), GFP_KERNEL);
		el->pid = get_task_pid(task, PIDTYPE_PID);
		list_add_tail(&(el->list), &fg_pids_list);
	}

	check_list(task->pid, oom_adj);
	if (list_empty(&fg_pids_list)) {
		fg_pid_nr = 0;
		fg_pid = NULL;
		if (fg_task) put_task_struct(fg_task);
		fg_task = NULL;
		wake_up(&jiq_wait);
		printk(KERN_ERR "app_monitor: foreground app list empty");
		//TODO trigger boost?
		return NOTIFY_DONE;
	}

	el = list_first_entry(&fg_pids_list, struct fg_pid_struct, list);
	task = get_pid_task(el->pid, PIDTYPE_PID);

	if (fg_task) put_task_struct(fg_task);
	fg_task = task;

	wake_up(&jiq_wait);

	if (!task) {
		fg_pid_nr = 0;
		fg_pid = NULL;
		printk(KERN_ERR "app_monitor: foreground app unknown");
		//TODO trigger boost?
	} else if (task->pid != fg_pid_nr) {
		printk(KERN_ERR "app_monitor: foreground app changed to %s [pid %d, tgid %d], nice %d, prio %d, is group leader: %d", task->comm, (int)task->pid, (int)task->tgid, task_nice(task), task_prio(task), thread_group_leader(task));
		printk(KERN_ERR "app_monitor: foreground app thread group leader: %s [pid %d, tgid %d], nice %d, prio %d", task->group_leader->comm, (int)task->group_leader->pid, (int)task->group_leader->tgid, task_nice(task->group_leader), task_prio(task->group_leader));
		
		//TODO trigger boost
		if (!fg_pid) {
			printk(KERN_ERR "app_monitor: old task not set - can't calculate cputime used\n");
			goto notfound;
		}
		printk(KERN_ERR "app_monitor: fg_pid\n");
		oldtask = get_pid_task(fg_pid, PIDTYPE_PID);//TGID?
		if (!oldtask) {
			printk(KERN_ERR "app_monitor: old task not found - can't calculate cputime used\n");
			goto notfound;
		}

		set_user_nice(oldtask, 0);
		printk(KERN_ERR "app_monitor: oldtask\n");
		sig = oldtask->signal;
		cutime_end = sig->cutime;
		cstime_end = sig->cstime;
		utime_end = oldtask->utime;
		stime_end = oldtask->stime;
		printk(KERN_ERR "app_monitor: oldtask %lu %lu %lu %lu\n", utime_end, stime_end, cutime_end, cstime_end);
		printk(KERN_ERR "app_monitor: cputime used utime: %lu, stime: %lu, cutime: %lu, cstime: %lu", utime_end-utime_start, stime_end-stime_start, cutime_end-cutime_start, cstime_end-cstime_start);
		thread_group_times(oldtask, &ut, &st);
		printk(KERN_ERR "app_monitor: oldtask thread_group_times: user:%lu system:%lu \n", ut, st);
		put_task_struct(oldtask);
notfound:
		sig = task->signal;
		cutime_start = sig->cutime;
		cstime_start = sig->cstime;
		utime_start = task->utime;
		stime_start = task->stime;
		printk(KERN_ERR "app_monitor: sighand %lu %lu %lu %lu\n", utime_start, stime_start, cutime_start, cstime_start);

		thread_group_cputime(task, &prev_app_time);

		fg_pid_nr = task->pid;
		fg_pid = el->pid;
		set_user_nice(task, -10);	//TODO loop over threads
	}
	return NOTIFY_DONE;
}

static struct notifier_block nb = {
	.notifier_call = &oom_adj_changed,
};

static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
						  cputime64_t *wall)
{
	cputime64_t idle_time;
	cputime64_t cur_wall_time;
	cputime64_t busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());
	busy_time = cputime64_add(kstat_cpu(cpu).cpustat.user,
			kstat_cpu(cpu).cpustat.system);

	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.irq);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.softirq);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.steal);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.nice);

	idle_time = cputime64_sub(cur_wall_time, busy_time);
	if (wall)
		*wall = (cputime64_t)jiffies_to_usecs(cur_wall_time);

	return (cputime64_t)jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu,
					    cputime64_t *wall)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		idle_time = get_cpu_idle_time_jiffy(cpu, wall);
	else if (!io_is_busy)
		idle_time += get_cpu_iowait_time_us(cpu, wall);

	return idle_time;
}

static u64 total_cpu_load, max_cpu_load, total_cpu_time;
static u64 update_load(void)
{
	struct cpufreq_interactive_cpuinfo *pcpu;
	u64 now;
	u64 now_idle;
	unsigned int delta_idle;
	unsigned int delta_time;
	u64 active_time;
	int cpu;
	total_cpu_load = max_cpu_load = total_cpu_time = 0;
	for_each_possible_cpu(cpu) {
		pcpu = &per_cpu(cpuinfo, cpu);
		now_idle = get_cpu_idle_time(cpu, &now);
		if (now_idle == 2147483647) {
			//== -1 -> cpu is offline
			pcpu->active_time = 0;
			pcpu->idle_time = 0;
			pcpu->time_in_idle_timestamp = now;
			continue;
		}

		delta_idle = (unsigned int)(now_idle - pcpu->time_in_idle);
		delta_time = (unsigned int)(now - pcpu->time_in_idle_timestamp);

		if (delta_time <= delta_idle)
			active_time = 0;
		else
			active_time = delta_time - delta_idle;

		//pcpu->cputime_speedadj += active_time * pcpu->policy->cur;
		pcpu->active_time = active_time;
		pcpu->idle_time = delta_idle;
		pcpu->time_in_idle = now_idle;
		pcpu->time_in_idle_timestamp = now;

		total_cpu_load += active_time;
		total_cpu_time += active_time  + delta_idle;
		max_cpu_load = max(max_cpu_load, active_time);
	}
	return now;
}

extern void thread_group_cputime(struct task_struct *tsk, struct task_cputime *times);

/* cpufreq */
static int cpufreq_callback(struct notifier_block *nfb,
		unsigned long event, void *data)
{
	struct cpufreq_freqs *freqs = data;

	if (event != CPUFREQ_POSTCHANGE || freqs->cpu != 0)
		return 0;

	freq = freqs->new/1000;
	wake_up(&jiq_wait);
	return 0;
}
static struct notifier_block cpufreq_notifier_block = {
	.notifier_call = cpufreq_callback,
};
/* end cpufreq */

/* early_suspend */
static void app_monitor_suspend(struct early_suspend *handler)
{
	suspend = true;
	wake_up(&jiq_wait);
}

static void app_monitor_resume(struct early_suspend *handler)
{
	suspend = false;
	wake_up(&jiq_wait);
}

static struct early_suspend app_monitor_early_suspend = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB,
	.suspend = app_monitor_suspend,
	.resume = app_monitor_resume,
};
/* end early suspend */

/* input boost */
static void hotplug_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	last_input_time = ktime_to_us(ktime_get());
}

static int hotplug_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void hotplug_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id hotplug_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler hotplug_input_handler = {
	.event          = hotplug_input_event,
	.connect        = hotplug_input_connect,
	.disconnect     = hotplug_input_disconnect,
	.name           = "app_monitor",
	.id_table       = hotplug_ids,
};
/* input boost */

/* seq_file procfs */

static void *my_seq_start(struct seq_file *s, loff_t *pos)
{
	static unsigned long counter = 0;

	/* beginning a new sequence ? */
	if ( *pos == 0 )
	{
		/* yes => return a non null value to begin the sequence */
		update_load();
		return &counter;
	}
	else
	{
		/* no => it's the end of the sequence, return end to stop reading */
		//*pos = 0;
		//return NULL;
	}
}

static void *my_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	//unsigned long *tmp_v = (unsigned long *)v;
	//(*tmp_v)++;
	//(*pos)++;
	//return NULL;
	unsigned long j0, j1; /* jiffies */
	int cpu;
	struct cpufreq_interactive_cpuinfo *pcpu;
	struct task_cputime app_time;
	unsigned long long temp_rtime;
	long timeout;

	j0 = jiffies;

	timeout = wait_event_interruptible_timeout(jiq_wait, freq != old_freq || fg_task != old_task || suspend != old_suspend, delay);
	update_load();

	j1 = jiffies;
	seq_printf(s, "%9lu %9lu (%li)", j0, j1, timeout);
	seq_printf(s, ", suspend: %d, freq: %4u", old_suspend, old_freq);
	if (old_task != NULL) {
		thread_group_cputime(old_task, &app_time);
		temp_rtime=app_time.sum_exec_runtime-prev_app_time.sum_exec_runtime;
		do_div(temp_rtime, 1000);
		seq_printf(s, ", app: gid %5d, utime %3lu, stime %3lu, rtime %6llu", old_task->cred->euid, app_time.utime-prev_app_time.utime, app_time.stime-prev_app_time.stime, temp_rtime);
		prev_app_time = app_time;
	} else
		seq_printf(s, ", app: gid %5d, utime %3lu, stime %3lu, rtime %6llu", -1, (unsigned long int)0, (unsigned long int)0, (unsigned long long)0);

	seq_printf(s, ", cpu: load %llu total %llu max %llu", total_cpu_load, total_cpu_time, max_cpu_load);
	for_each_possible_cpu(cpu) {
		pcpu = &per_cpu(cpuinfo, cpu);
		seq_printf(s, " cpu%d: active %6d idle %6d", cpu, pcpu->active_time, pcpu->idle_time);
	}
	seq_printf(s, "\n");
	old_suspend = suspend;
	old_freq = freq;
	if (old_task != fg_task) {
		if (old_task) put_task_struct(old_task);
		if (fg_task) get_task_struct(fg_task);
		old_task = fg_task;
	}
	return 1;
}

static void my_seq_stop(struct seq_file *s, void *v)
{
/* nothing to do, we use a static value in start() */
}

static int my_seq_show(struct seq_file *s, void *v)
{
//	loff_t *spos = (loff_t *) v;

//	seq_printf(s, "%Ld\n", *spos);
	return 0;
}

static struct seq_operations my_seq_ops = {
	.start = my_seq_start,
	.next  = my_seq_next,
	.stop  = my_seq_stop,
	.show  = my_seq_show
};

static int my_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &my_seq_ops);
};

static struct file_operations my_file_ops = {
	.owner   = THIS_MODULE,
	.open    = my_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};
/* end seq_file procfs */

static int __init app_monitor_init(void)
{
	struct task_struct *task;
	struct fg_pid_struct *el;
	struct proc_dir_entry *entry;
	int ret;

	oom_adj_register_notify(&nb);
	entry = create_proc_entry("app_monitor", 0, NULL);
	if (entry) {
		entry->proc_fops = &my_file_ops;
	}
	register_early_suspend(&app_monitor_early_suspend);
	cpufreq_register_notifier(&cpufreq_notifier_block, CPUFREQ_TRANSITION_NOTIFIER);
	ret = input_register_handler(&hotplug_input_handler);
	if (ret)
		pr_err("Cannot register hotplug input handler.\n");
	printk(KERN_INFO "Zen foreground app monitor driver registered\n");
	find_zygote();
	for_each_process(task) {
		//printk(KERN_ERR "app_monitor: checking %s, %d, %d, zygote: %d, %d", task->comm, task->cred->euid, task->signal->oom_adj, task->parent == zygote, task->real_parent == zygote);
		if (task->cred->euid >= 10000 && task->signal->oom_adj == 0 && (zygote == NULL || (task->parent == zygote || task->real_parent == zygote))) {
			el = kmalloc(sizeof(struct fg_pid_struct), GFP_KERNEL);
			el->pid = get_task_pid(task, PIDTYPE_PID);
			list_add_tail(&(el->list), &fg_pids_list);
		}
	}
	if (!list_empty(&fg_pids_list)) {
		el = list_first_entry(&fg_pids_list, struct fg_pid_struct, list);
		task = get_pid_task(el->pid, PIDTYPE_PID);
		if (task) {
			thread_group_cputime(task, &prev_app_time);
			fg_pid_nr = task->pid;
			fg_pid = el->pid;
			fg_task = task;
			set_user_nice(task, -10);	//TODO loop over threads
			put_task_struct(task);
		}
	}

	debug_app_list = 2;
	check_list(0, 0);
	debug_app_list = 0;

	utime_start = stime_start = cutime_start = cstime_start =  utime_end = stime_end = cutime_end = cstime_end = cputime_zero;
	return 0;
}
 
static void __exit app_monitor_exit(void)
{
	cpufreq_unregister_notifier(&cpufreq_notifier_block, CPUFREQ_TRANSITION_NOTIFIER);
	unregister_early_suspend(&app_monitor_early_suspend);
	input_unregister_handler(&hotplug_input_handler);
	remove_proc_entry("app_monitor", NULL);
	oom_adj_unregister_notify(&nb);
	printk(KERN_INFO "Zen foreground app monitor driver unregistered\n");
}
 
module_init(app_monitor_init);
module_exit(app_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Marcin Kaluza <marcin.kaluza@trioptimum.com>");
MODULE_DESCRIPTION("Test module");
