/**
 * author: liulei2010@ict.ac.cn
 * @20130629
 * sequentially scan the page table to check and re-new __access_bit, and cal. the number of hot pages. 
 * add shadow count index
 *
 * Modifications@20150130 recover from an unknown problem. This version works well.
 * By Lei Liu, Hao Yang, Mengyao Xie, Yong Li, Mingjie Xing
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/sem.h>
#include <linux/list.h>
#include <linux/kernel_stat.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/mman.h>
#include <linux/swap.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/ksm.h>
#include <linux/rmap.h>
#include <linux/module.h>
#include <linux/delayacct.h>
#include <linux/init.h>
#include <linux/writeback.h>
#include <linux/memcontrol.h>
#include <linux/mmu_notifier.h>
#include <linux/kallsyms.h>
#include <linux/swapops.h>
#include <linux/elf.h>
#include <linux/sched.h>
#include <linux/delay.h>

/* Adjust these constants as required */
#define ITERATIONS 16 /* the number of sampling loops */
#define PAGE_ALL 30000000 /* the total number of pages */
#define TIME_INTERVAL 1
/* the constants below denote the different ranges of the page number */
#define VH 200 /* VH means very high*/
#define H 150 /* H means high*/
#define M 100 /* M means middle*/
#define L 64 /* L means low */
#define VL_MAX 10 /* VL means very low */
#define VL_MIN 5


/*
 在vmscan.c文件中定义，用于传送哪些页面不应该被换出。
 vmtrapper在扫描完页面访问位后，更新swapped_list。
*/
extern struct list_head swapped_list;

/*swapped_list节点*/
struct tp_page {
	unsigned long long int tp_pte;
	int pg_no;
    struct page *page;
    int reuse_time;
    struct list_head list;
};

/*临时存在vmtrapper应该扫描哪些页面的访问位。*/
LIST_HEAD(tmp_list);
struct which_page {
	unsigned long long int tp_pte;
	int pg_no;
    struct page *page;
    int reuse_time;
    struct list_head list;
};

static int scan_pgtable(void);
static struct task_struct * traverse_all_process(void);

struct timer_list stimer; 
/*the handle of current running benchmark*/
struct task_struct * bench_process;
/* the array that records page access freq., which reflects the page "heat"*/
long long page_heat[PAGE_ALL];
/* pid passed in.Write by xiemengyao.*/
static int process_id;

module_param(process_id, int, S_IRUGO|S_IWUSR);

/**
 * begin to cal. the number of hot pages.
 * And we will re-do it in every TIME_INTERVAL seconds.
 */
static void time_handler(struct timer_list  *timer)
{ 
	int win=0;
	mod_timer(&stimer, jiffies + TIME_INTERVAL*HZ);
	win = scan_pgtable(); /* 1 is win.*/
	if(!win) /* we get no page, maybe something wrong occurs.*/
		printk("sysmon: fail in scanning page table...\n");
}

static int __init timer_init(void)
{
	printk("sysmon: module init!\n");
	stimer.expires = jiffies + TIME_INTERVAL * HZ;
	timer_setup(&stimer, time_handler, 0);
	add_timer(&stimer);
	return 0;
}

static void __exit timer_exit(void)
{
	printk("Unloading sysmon module.\n");
	del_timer(&stimer);/*delete the timer*/
	return;
}

/**
 * Write by xiemengyao.
 * get the process of current running benchmark.
 * The returned value is the pointer to the process.
 */
static struct task_struct * traverse_all_process(void) 
{
	struct pid * pid;
	pid = find_vpid(process_id);
	return pid_task(pid,PIDTYPE_PID);
}

/*pgtable sequential scan and count for __access_bits.*/
static int scan_pgtable(void)
{
	pgd_t *pgd = NULL;
	p4d_t *p4d = NULL;
	pud_t *pud = NULL;
	pmd_t *pmd = NULL;
	pte_t *ptep, pte;

	spinlock_t *ptl;

	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long start = 0; /*the start of address.*/
	unsigned long end = 0;   /*the end of address.*/
	unsigned long address = 0;
	int number_hotpages = 0; /* the number of hot pages */
	int number_vpages = 0;
	int cycle_index = 0; /* the loop counter, which denotes the ITERATIONS. */
	/* the array that records the number of hot page in every cycle */
	int hot_page[ITERATIONS];
	int j = 0;
	int pg_count = 0; /* The temporary counter*/
	int number_current_pg = 0;/* The temporary counter*/

	/* some variables that describe page "heat" */
	int hig = 0;
	int mid = 0;
	int low = 0;
	int llow = 0;
	int lllow = 0;
	int llllow = 0;
	int all_pages = 0;/* the total number of pages */
	/*the average number of hot pages in each iteration.*/
	long avg_hotpage=0;
	/*the total number of memory accesses across all pages*/
	long num_access=0;
	/* avg utilization of each page */
	int avg_page_utilization = 0;

	/*get the handle of current running benchmark*/
	bench_process = traverse_all_process();

	struct which_page *np, *tmp, *p;
	struct tp_page *np_swap, *tmp_swap, *p_swap;

	if(bench_process == NULL)
	{
		printk("sysmon: get no process handle in scan_pgtable function...exit&trying again...\n");
		return 0;
	}
	else /*get the process*/
		mm = bench_process->mm;
	if(mm == NULL)
	{
		printk("sysmon: error mm is NULL, return back & trying...\n");       
		return 0;
	}

	for(j = 0; j < PAGE_ALL; j++)
    {
		page_heat[j] = -1;
    }
	for(j = 0; j < ITERATIONS; j++)
		hot_page[j] = 0;

	for(cycle_index = 0; cycle_index < ITERATIONS; cycle_index++)
	{
		number_hotpages = 0;
		/*scan each vma*/
		number_vpages = 0;
		for(vma = mm->mmap; vma; vma = vma->vm_next)
		{
			start = vma->vm_start; 
			end = vma->vm_end;
			mm = vma->vm_mm;
			/*in each vma, we check all pages */
			pg_count = 0;
			for(address = start; address < end; address += PAGE_SIZE)
			{
				/*scan page table for each page in this VMA*/
				pgd = pgd_offset(mm, address);

				if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
					continue;
				/*
				 * by asjbzhu@gmail.com 20220711
				 * extend the page table directory to 5 levels
				 * the new page table directory is p4d
				 * For ARM64 specific
				 * */
				p4d = p4d_offset(pgd,address);
				if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
					continue;
                /*****End 20220711 by jbzhu******/
				pud = pud_offset(p4d,address);
				if (pud_none(*pud) || unlikely(pud_bad(*pud)))
					continue;

				pmd = pmd_offset(pud, address);
				if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)))
					continue;

				ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
				pte = *ptep;

				number_current_pg = pg_count + number_vpages;
				if(pte_present(pte)) 
				{
					/*
					 检查pte中的保留位
					 如果不为0，则说明该pte对应的页面被换出过，然后清除访问位并把该pte的号码记录下来
					*/
					if(pte.pte & 0x00f0000000000000){
						//printk(KERN_INFO"catch pte = %Lx\n", (u64)pte_val(pte));
						/*清访问位*/
						if(pte_young(pte)){
							pte = pte_mkold(pte);
							set_pte_at(mm, address, ptep, pte);
						}
						/*如果该pte已经写入临时list，则不需要再写入*/
						list_for_each_entry_safe(np, tmp, &tmp_list, list){
							//printk(KERN_INFO"page NO. %d\n", np->pg_no);
							if(number_current_pg == np->pg_no){
								//printk(KERN_INFO"page NO. %d\n", np->pg_no);
								continue;
							}
						}
						p = (struct which_page *)kmalloc(sizeof(struct which_page), GFP_KERNEL);
						p->tp_pte = pte.pte;
						p->pg_no = number_current_pg;
						p->page = pte_page(pte);
						p->reuse_time = 0;
						
						/*将该pte编号写入临时list*/
						list_add_tail(&p->list, &tmp_list);
					}
				}
				else /*no page pte_none*/ 
				{
					pte_unmap_unlock(ptep, ptl);
					continue;
				}
				pg_count++;
				pte_unmap_unlock(ptep, ptl);  
			} /*end for(adddress .....)*/
			number_vpages += (int)(end - start)/PAGE_SIZE;
		} /*end for(vma ....)*/

		/*count the number of hot pages*/
		if(bench_process == NULL)
		{   
			printk("sysmon: get no process handle in scan_pgtable function...exit&trying again...\n");
			return 0;
		}   
		else /* get the process*/
			mm = bench_process->mm;
		if(mm == NULL)
		{   
			printk("sysmon: error mm is NULL, return back & trying...\n");
			return 0;
		}


		number_vpages = 0;
		for(vma = mm->mmap; vma; vma = vma->vm_next)
		{
			start = vma->vm_start;
			end = vma->vm_end;
			/*scan each page in this VMA*/
			mm = vma->vm_mm;
			pg_count = 0; 
			for(address = start; address < end; address += PAGE_SIZE)
			{
				/*scan page table for each page in this VMA*/
				pgd = pgd_offset(mm, address);
				if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
					continue;
				/*
				 * by asjbzhu@gmail.com 20220711
				 * extend the page table directory to 5 levels
				 * the new page table directory is p4d
				 * */
				p4d = p4d_offset(pgd,address);
				if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
					continue;
				/************************/

				pud = pud_offset(p4d,address);
				if (pud_none(*pud) || unlikely(pud_bad(*pud)))
					continue;
				pmd = pmd_offset(pud, address);
				if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)))
					continue;

				ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
				pte = *ptep;

                number_current_pg = pg_count + number_vpages;
				if(pte_present(pte))
				{
					/*只处理tmp_list中的页*/
					list_for_each_entry_safe(np, tmp, &tmp_list, list){
						/*如果匹配成功*/
						if(number_current_pg == np->pg_no){
							printk("page no: %d", np->pg_no);
							//printk("pg_no: %d, number_current_pg: %d", np->pg_no, number_current_pg);
							/*如果该页被访问了，reuse+1*/
							if(pte_young(pte)){
								np->reuse_time = np->reuse_time + 1;
							}
						}
					}
				}
				pg_count++;
				pte_unmap_unlock(ptep, ptl);
			} /*end for(address ......)*/
			number_vpages += (int)(end - start)/PAGE_SIZE;
		} /*end for(vma .....) */ 
		
	} /*end ITERATIONS times repeats*/

	/*遍历双向链表，凡事处理了的页，就删除*/
  	list_for_each_entry_safe(np, tmp, &tmp_list, list){
		printk("vmtrapper No.: %d, reuse_time: %d", np->pg_no, np->reuse_time);
		
        list_del(&np->list);
        kfree(np);
  	}

	/*
	list_for_each_entry_safe(np_swap, tmp_swap, &swapped_list, list){
		printk("reuse times: %d", np_swap->reuse_time);
        list_del(&np->list);
        kfree(np);
  	}
	*/

	return 1;
}

module_init(timer_init);
module_exit(timer_exit);
MODULE_AUTHOR("leiliu");
MODULE_LICENSE("GPL");