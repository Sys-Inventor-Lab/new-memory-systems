/* Monitor reuse pattern and swap time of iswap */
/* Support all processes */
/* No output when unloading module */

/* change list to hashtable @20230925 */
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
#include <linux/hashtable.h>

/* Adjust these constants as required */
#define ITERATIONS 4 /* the number of sampling loops */
#define TIME_INTERVAL 3

/* for monitor */
#define PAGES 30000

/*
 在vmscan.c文件中定义，用于传送哪些页面不应该被换出。
 vmtrapper在扫描完页面访问位后，更新swapped_list。
*/
//DEFINE_HASHTABLE(vrt_swapped_table, 17);
//DEFINE_HASHTABLE(phy_swapped_table, 17);
// extern struct hlist_head vrt_swapped_table;
/* vrt_swapped_table node */
extern struct hlist_head vrt_swapped_table[1<<17];
extern struct hlist_head phy_swapped_table[1<<17];

struct vrt_page {
	//unsigned long long tp_pte;
	unsigned long long pg_no; //key
    unsigned int reuse_time;
    struct hlist_node node;
};

/* phy_swapped_table node */
struct phy_page {
    struct page *page; //key
	struct hlist_node* vrt_node_addr;
    struct hlist_node node;
};

static int scan_pgtable(void);
//static struct task_struct * traverse_all_process(void);

struct timer_list stimer; 
/*the handle of current running benchmark*/


/**
 * begin to cal. the number of hot pages.
 * And we will re-do it in every TIME_INTERVAL seconds.
 */
static void time_handler(struct timer_list* timer) {
	//printk("sysmon: start scan!\n");
    int win = 0;
    win = scan_pgtable();
    if(!win) {/* we get no page, maybe something wrong occurs.*/
	    printk("sysmon: fail in scanning page table...\n");
	}
	mod_timer(&stimer, jiffies + TIME_INTERVAL * HZ);
}

static int __init timer_init(void) {
	printk("sysmon: module init!\n");
	stimer.expires = jiffies + TIME_INTERVAL * HZ;
	timer_setup(&stimer, time_handler, 0);
	add_timer(&stimer);
	return 0;
}

static void __exit timer_exit(void) {
	printk("Unloading sysmon module.\n");
	del_timer(&stimer);/*delete the timer*/
	return;
}

/**
 * Write by xiemengyao.
 * get the process of current running benchmark.
 * The returned value is the pointer to the process.
 */
/*
static struct task_struct * traverse_all_process(void) {
	struct pid * pid;
	pid = find_vpid(pid_number);
	return pid_task(pid,PIDTYPE_PID);
}
*/

static struct mm_struct* get_process_mm(struct task_struct* bench_process) {
	struct mm_struct *mm;
	if(bench_process == NULL) {
		//printk("sysmon: get no process handle in scan_pgtable function...exit & trying again...\n");
		return NULL;
	} else {/*get the process*/
		mm = bench_process->mm;
		return mm;
	}
}

/*pgtable sequential scan and count for __access_bits.*/
static int scan_pgtable(void) {
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
	int number_vpages = 0;
	/* the array that records the number of hot page in every cycle */
	unsigned long long pg_count = 0; /* The temporary counter*/
	unsigned long long number_current_pg = 0;/* The temporary counter*/

	// struct vrt_page *np_swap, *tmp_swap, *p_swap;
	struct vrt_page* new_vrt, *cur;
	struct phy_page* new_phy;

    struct task_struct * bench_process;

    for_each_process(bench_process) {
        //printk("sysmon: pid = %d", bench_process->pid);
    	if((mm = get_process_mm(bench_process)) == NULL) {
    	//	printk("sysmon: error %d mm is NULL, return back & trying...\n", bench_process->pid);       
    	    continue;
        }

    	// get swapped pages and add to vrt_swapped_table
    	for(vma = mm->mmap; vma; vma = vma->vm_next) {
    		start = vma->vm_start;
    		end = vma->vm_end;
    		mm = vma->vm_mm;
    
    		pg_count = 0;

    		for(address = start; address < end; address += PAGE_SIZE) {
    			pgd = pgd_offset(mm, address);
    			if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd))) {
    				continue;
    			}
    			p4d = p4d_offset(pgd,address);
    			if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d))) {
    				continue;
    			}
    			pud = pud_offset(p4d,address);
    			if (pud_none(*pud) || unlikely(pud_bad(*pud))) {
    				continue;
    			}
    			pmd = pmd_offset(pud, address);
    			if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd))) {
    				continue;
    			}

    			number_current_pg = pg_count + number_vpages;

    			ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
    			pte = *ptep;

    			if(pte_present(pte)) {
    				/*
    				 检查pte中的保留位
    				 如果不为0，则说明该pte对应的页面被换出过，然后清除访问位并把该pte的号码记录下来
    				*/
    				if(pte.pte & 0X00f0000000000000) {
    					/*清访问位*/
    					if(pte_young(pte)){
    						pte = pte_mkold(pte);
    						set_pte_at(mm, address, ptep, pte);
    					}
    					//printk(KERN_INFO"catch pte = %Lx\n", (u64)pte_val(pte));
    					hash_for_each_possible(vrt_swapped_table, cur, node, number_current_pg) {
            				if(number_current_pg == cur->pg_no) {
    							goto page_exist;
    						}
        				}
    					/* add page to swapped_table */
    					new_vrt = (struct vrt_page*)kmalloc(sizeof(struct vrt_page), GFP_KERNEL);
    					//new_vrt->tp_pte = pte.pte;
    					new_vrt->pg_no = number_current_pg;
    					new_vrt->reuse_time = 0;
    					hash_add(vrt_swapped_table, &(new_vrt->node), number_current_pg);

    					new_phy = (struct phy_page*)kmalloc(sizeof(struct phy_page), GFP_KERNEL);
    					new_phy->page = pte_page(pte);
    					new_phy->vrt_node_addr = &(new_vrt->node);
    					hash_add(phy_swapped_table, &(new_phy->node), (u64)(new_phy->page));
    					//printk("add success!\n");
    				}
    			}
    		page_exist:
    			pte_unmap_unlock(ptep, ptl);
    			pg_count++;

    		}// address scan end
    		number_vpages += (int)(end - start) / PAGE_SIZE;
    	} // vma scan end


		// get reuse pattern
		if((mm = get_process_mm(bench_process)) == NULL) {
			//printk("sysmon: error mm is NULL, return back & trying...\n");       
			return 0;
		}
		number_vpages = 0;
		for(vma = mm->mmap; vma; vma = vma->vm_next) {
			start = vma->vm_start;
			end = vma->vm_end;

			mm = vma->vm_mm;
			pg_count = 0;
			
			for(address = start; address < end; address += PAGE_SIZE) {
				pgd = pgd_offset(mm, address);
				if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd))) {
					continue;
				}
				p4d = p4d_offset(pgd,address);
				if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d))) {
					continue;
				}
				pud = pud_offset(p4d,address);
				if (pud_none(*pud) || unlikely(pud_bad(*pud))) {
					continue;
				}
				pmd = pmd_offset(pud, address);
				if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd))) {
					continue;
				}
				
				number_current_pg = pg_count + number_vpages;

				ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
				pte = *ptep;

				if(pte_present(pte)) {
					hash_for_each_possible(vrt_swapped_table, cur, node, number_current_pg) {
						if(number_current_pg == cur->pg_no) {
							/* 如果该页被访问了，reuse+1 */
							if(pte_young(pte)) {
								cur->reuse_time = (cur->reuse_time >> 1) | (1 << 31);
							} else {
								cur->reuse_time = (cur->reuse_time >> 1) & (~(1 << 31));
							}
						}
					}
				}
				pte_unmap_unlock(ptep, ptl);
				pg_count++;
			} //address scan end
			number_vpages += (int)(end - start) / PAGE_SIZE;
		} //vma scan end


		// sampling reuse pattern
		// remove access bit of swapped pages
		if((mm = get_process_mm(bench_process)) == NULL) {
			//printk("sysmon: error mm is NULL, return back & trying...\n");       
			return 0;
		}
		for(vma = mm->mmap; vma; vma = vma->vm_next) {
			start = vma->vm_start;
			end = vma->vm_end;

			mm = vma->vm_mm;
			
			for(address = start; address < end; address += PAGE_SIZE) {
				pgd = pgd_offset(mm, address);
				if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd))) {
					continue;
				}
				p4d = p4d_offset(pgd,address);
				if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d))) {
					continue;
				}
				pud = pud_offset(p4d,address);
				if (pud_none(*pud) || unlikely(pud_bad(*pud))) {
					continue;
				}
				pmd = pmd_offset(pud, address);
				if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd))) {
					continue;
				}
				
				ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
				pte = *ptep;

				if(pte_present(pte)) {
					if(pte.pte & 0X00f0000000000000) {
						if(pte_young(pte)) {
							pte = pte_mkold(pte);
							set_pte_at(mm, address, ptep, pte);
						}
					}
				}
				pte_unmap_unlock(ptep, ptl);
			} //address scan end
		} //vma scan end

    }

		

	return 1;
}

module_init(timer_init);
module_exit(timer_exit);
MODULE_AUTHOR("leiliu");
MODULE_LICENSE("GPL");
