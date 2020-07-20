/*
 * zsmapbench.c
 *
 * Microbenchmark for zsmalloc allocation mapping
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include "linux/zpool.h"

static int zsmb_kthread(void *ptr)
{
	struct zpool *pool;
	unsigned long *handles, completed = 0;
	cycles_t start, end, dt;
	int i, err;
	char *buf;
	/*
	 * This size is roughly 40% of PAGE_SIZE an results in an
	 * underlying zspage size of 2 pages.  See the
	 * get_pages_per_zspage() function in zsmalloc for details.
	 * The third allocation in this class will span two pages.
	*/
	int size = 1632;
	int handles_nr = 3;
	int spanned_index = handles_nr - 1;

	pr_info("starting zsmb_kthread\n");

	pool = zpool_create_pool("zsmalloc", "zsmapbench", GFP_NOIO, NULL);
	if (!pool)
		return -ENOMEM;

	handles = (unsigned long *)kmalloc(handles_nr * sizeof(unsigned long),
					GFP_KERNEL);
	if (!handles) {
		pr_info("kmalloc failed\n");
		return -ENOMEM;
	}
	memset(handles, 0, sizeof(unsigned long) * handles_nr);

	for (i = 0; i < handles_nr; i++) {
		err = zpool_malloc(pool, size, GFP_NOIO | __GFP_HIGHMEM, &handles[i]);
		if (err) {
			pr_err("zs_malloc failed\n");
			err = -ENOMEM;
			goto free;
		}
	}

	start = get_cycles();

	while (unlikely(!kthread_should_stop())) {
		buf = zpool_map_handle(pool, handles[spanned_index], ZPOOL_MM_RW);
		if (unlikely(!buf)) {
			pr_err("zs_map_object failed\n");
			err = -EINVAL;
			goto free;
		}
		zpool_unmap_handle(pool, handles[spanned_index]);
		completed++;
		cond_resched();
	}

	end = get_cycles();

	dt = end - start;
	pr_info("%llu cycles\n",(unsigned long long)dt);
	pr_info("%lu mappings\n",completed);
	pr_info("%llu cycles/map\n",(unsigned long long)dt/completed);

	pr_info("stopping zsmb_kthread\n");
	err = 0;

free:
	for (i = 0; i < handles_nr; i++)
		if (handles[i])
			zpool_free(pool, handles[i]);
	if (handles)
		kfree(handles);
	zpool_destroy_pool(pool);
	return err;		
}

/*
 * This benchmark isn't made to handle changes in the cpu online mask.
 * Please don't hotplug while the benchmark runs.
*/
static DEFINE_PER_CPU(struct task_struct *, pcpu_kthread);
static bool single_threaded;
module_param(single_threaded, bool, 0);

static int __init zsmb_init(void)
{
	struct task_struct **kthread;
	int cpu;

	pr_info("running zsmapbench...\n");

	for_each_online_cpu(cpu) {
		kthread = per_cpu_ptr(&pcpu_kthread, cpu);
		*kthread =
			kthread_create(zsmb_kthread, NULL, "zsmb_kthread");
		if (IS_ERR(*kthread))
			return IS_ERR(*kthread);
		kthread_bind(*kthread, cpu);
		if (single_threaded)
			break;
	}

	for_each_online_cpu(cpu) {
		kthread = per_cpu_ptr(&pcpu_kthread, cpu);
		wake_up_process(*kthread);
		if (single_threaded)
			break;
	}

	/* Run for about one second */
	msleep(1000);

	for_each_online_cpu(cpu) {
		kthread = per_cpu_ptr(&pcpu_kthread, cpu);
		kthread_stop(*kthread);
		if (single_threaded)
			break;
	}

	pr_info("zsmapbench complete\n");

	return -0x1000;
}

module_init(zsmb_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Seth Jennings <sjenning@linux.vnet.ibm.com");
MODULE_DESCRIPTION("Microbenchmark for zsmalloc mapping methods");
