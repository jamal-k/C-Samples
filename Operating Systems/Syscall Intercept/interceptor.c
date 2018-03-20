#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/current.h>
#include <asm/ptrace.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <asm/unistd.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/syscalls.h>
#include "interceptor.h"


MODULE_DESCRIPTION("My kernel module");
MODULE_AUTHOR("Me");
MODULE_LICENSE("GPL");

//----- System Call Table Stuff ------------------------------------
/* Symbol that allows access to the kernel system call table */
extern void* sys_call_table[];

/* The sys_call_table is read-only => must make it RW before replacing a syscall */
void set_addr_rw(unsigned long addr) {

	unsigned int level;
	pte_t *pte = lookup_address(addr, &level);

	if (pte->pte &~ _PAGE_RW) pte->pte |= _PAGE_RW;

}

/* Restores the sys_call_table as read-only */
void set_addr_ro(unsigned long addr) {

	unsigned int level;
	pte_t *pte = lookup_address(addr, &level);

	pte->pte = pte->pte &~_PAGE_RW;

}
//-------------------------------------------------------------


//----- Data structures and bookkeeping -----------------------
/**
 * This block contains the data structures needed for keeping track of
 * intercepted system calls (including their original calls), pid monitoring
 * synchronization on shared data, etc.
 */

/* List structure - each intercepted syscall may have a list of monitored pids */
struct pid_list {
	pid_t pid;
	struct list_head list;
};


/* Store info about intercepted/replaced system calls */
typedef struct {

	/* Original system call */
	asmlinkage long (*f)(struct pt_regs);

	/* Status: 1=intercepted, 0=not intercepted */
	int intercepted;

	/* Are any PIDs being monitored for this syscall? */
	int monitored;	
	/* List of monitored PIDs */
	int listcount;
	struct list_head my_list;
}mytable;

/* An entry for each system call in this "metadata" table */
mytable table[NR_syscalls];

/* Access to the system call table and your metadata table must be synchronized */
spinlock_t my_table_lock = SPIN_LOCK_UNLOCKED;
spinlock_t sys_call_table_lock = SPIN_LOCK_UNLOCKED;
//-------------------------------------------------------------


//----------LIST OPERATIONS------------------------------------
/**
 * These operations are meant for manipulating the list of pids
 */

/**
 * Add a pid to a syscall's list of monitored pids. 
 * Returns -ENOMEM if the operation is unsuccessful.
 */
static int add_pid_sysc(pid_t pid, int sysc)
{
	struct pid_list *ple=(struct pid_list*)kmalloc(sizeof(struct pid_list), GFP_KERNEL);

	if (!ple)
		return -ENOMEM;

	INIT_LIST_HEAD(&ple->list);
	ple->pid=pid;

	list_add(&ple->list, &(table[sysc].my_list));
	table[sysc].listcount++;

	return 0;
}

/**
 * Remove a pid from a system call's list of monitored pids.
 * Returns -EINVAL if no such pid was found in the list.
 */
static int del_pid_sysc(pid_t pid, int sysc)
{
	struct list_head *i;
	struct pid_list *ple;

	list_for_each(i, &(table[sysc].my_list)) {

		ple=list_entry(i, struct pid_list, list);
		if(ple->pid == pid) {

			list_del(i);
			kfree(ple);

			table[sysc].listcount--;
			/* If there are no more pids in sysc's list of pids, then
			 * stop the monitoring only if it's not for all pids (monitored=2) */
			if(table[sysc].listcount == 0 && table[sysc].monitored == 1) {
				table[sysc].monitored = 0;
			}

			return 0;
		}
	}

	return -EINVAL;
}

/**
 * Remove a pid from all the lists of monitored pids (for all intercepted syscalls).
 * Returns -1 if this process is not being monitored in any list.
 */
static int del_pid(pid_t pid)
{
	struct list_head *i, *n;
	struct pid_list *ple;
	int ispid = 0, s = 0;

	for(s = 1; s < NR_syscalls; s++) {

		list_for_each_safe(i, n, &(table[s].my_list)) {

			ple=list_entry(i, struct pid_list, list);
			if(ple->pid == pid) {

				list_del(i);
				ispid = 1;
				kfree(ple);

				table[s].listcount--;
				/* If there are no more pids in sysc's list of pids, then
				 * stop the monitoring only if it's not for all pids (monitored=2) */
				if(table[s].listcount == 0 && table[s].monitored == 1) {
					table[s].monitored = 0;
				}
			}
		}
	}

	if (ispid) return 0;
	return -1;
}

/**
 * Clear the list of monitored pids for a specific syscall.
 */
static void destroy_list(int sysc) {

	struct list_head *i, *n;
	struct pid_list *ple;

	list_for_each_safe(i, n, &(table[sysc].my_list)) {

		ple=list_entry(i, struct pid_list, list);
		list_del(i);
		kfree(ple);
	}

	table[sysc].listcount = 0;
	table[sysc].monitored = 0;
}

/**
 * Check if two pids have the same owner - useful for checking if a pid 
 * requested to be monitored is owned by the requesting process.
 * When requesting to start monitoring for a pid, only the 
 * owner of that pid is allowed to request that.
 */
static int check_pids_same_owner(pid_t pid1, pid_t pid2) {

	struct task_struct *p1 = pid_task(find_vpid(pid1), PIDTYPE_PID);
	struct task_struct *p2 = pid_task(find_vpid(pid2), PIDTYPE_PID);
	if(p1->real_cred->uid != p2->real_cred->uid)
		return -EPERM;
	return 0;
}

/**
 * Check if a pid is already being monitored for a specific syscall.
 * Returns 1 if it already is, or 0 if pid is not in sysc's list.
 */
static int check_pid_monitored(int sysc, pid_t pid) {

	struct list_head *i;
	struct pid_list *ple;

	list_for_each(i, &(table[sysc].my_list)) {

		ple=list_entry(i, struct pid_list, list);
		if(ple->pid == pid) 
			return 1;
		
	}
	return 0;	
}
//----------------------------------------------------------------

//----- Intercepting exit_group ----------------------------------
/**
 * Since a process can exit without its owner specifically requesting
 * to stop monitoring it, we must intercept the exit_group system call
 * so that we can remove the exiting process's pid from *all* syscall lists.
 */  

/** 
 * Stores original exit_group function - after all, we must restore it 
 * when our kernel module exits.
 */
void (*orig_exit_group)(int);

/**
 * Our custom exit_group system call.
 * When a process exits, make sure to remove that pid from all lists.
 */
void my_exit_group(int status)
{
	spin_lock(&my_table_lock);
	del_pid(current->pid); //remove pid from all lists
	spin_unlock(&my_table_lock);
	orig_exit_group(status); //call original exit_group
}
//----------------------------------------------------------------



/** 
 * This is the generic interceptor function.
 * It should just log a message and call the original syscall.
 * 
 * - Check first to see if the syscall is being monitored for the current->pid. 
 * - Convention for the "monitored" flag in the mytable struct: 
 *     monitored=0 => not monitored
 *     monitored=1 => some pids are monitored, check the corresponding my_list
 *     monitored=2 => all pids are monitored for this syscall
 */
asmlinkage long interceptor(struct pt_regs reg) {
	
	int s;
	s = reg.ax; //syscall number
	
	//log message if pid monitored
	if ((table[s].monitored == 1 && check_pid_monitored(s, current->pid)) ||
		(table[s].monitored == 2 && !check_pid_monitored(s, current->pid))) {
		log_message(current->pid, reg.ax, reg.bx, reg.cx, reg.dx, reg.si, reg.di, reg.bp);
	}
	
	return table[s].f(reg); //call original system call
}

/**
 * My system call - this function is called whenever a user issues a MY_CUSTOM_SYSCALL system call.
 * The parameters for this system call indicate one of 4 actions/commands:
 *      - REQUEST_SYSCALL_INTERCEPT to intercept the 'syscall' argument
 *      - REQUEST_SYSCALL_RELEASE to de-intercept the 'syscall' argument
 *      - REQUEST_START_MONITORING to start monitoring for 'pid' whenever it issues 'syscall' 
 *      - REQUEST_STOP_MONITORING to stop monitoring for 'pid'
 *      For the last two, if pid=0, that translates to "all pids".
 */
asmlinkage long my_syscall(int cmd, int syscall, int pid) {
	
	//invalid syscall number
	if (syscall < 0 || syscall == MY_CUSTOM_SYSCALL || syscall >= NR_syscalls) {
		return -EINVAL;
	}
	
	if (cmd == REQUEST_SYSCALL_INTERCEPT) {
		
		//not root
		if (current_uid() != 0) {
			return -EPERM;
		}
		
		//system call already intercepted
		if (table[syscall].intercepted == 1) {
			return -EBUSY;
		}
		
		//save original system call and set intercepted flag
		spin_lock(&my_table_lock);
		table[syscall].f = sys_call_table[syscall];
		table[syscall].intercepted = 1;
		spin_unlock(&my_table_lock);
		
		//replace system call with interceptor
		spin_lock(&sys_call_table_lock);
		set_addr_rw((unsigned long) sys_call_table);
		sys_call_table[syscall] = &interceptor;
		set_addr_ro((unsigned long) sys_call_table);
		spin_unlock(&sys_call_table_lock);
	
	} else if (cmd == REQUEST_SYSCALL_RELEASE) {
		
		//not root
		if (current_uid() != 0) {
			return -EPERM;
		}
		
		//system call hasn't been intercepted yet, so can't release
		if (table[syscall].intercepted == 0) {
			return -EINVAL;
		}
		
		//restore original system call
		spin_lock(&sys_call_table_lock);
		set_addr_rw((unsigned long) sys_call_table);
		sys_call_table[syscall] = table[syscall].f;
		set_addr_ro((unsigned long) sys_call_table);
		spin_unlock(&sys_call_table_lock);
		
		//reset intercepted status
		spin_lock(&my_table_lock);
		table[syscall].intercepted = 0;
		spin_unlock(&my_table_lock);
		
	} else if (cmd == REQUEST_START_MONITORING) {
		
		//invalid pid
		if (pid < 0 || (pid != 0 && pid_task(find_vpid(pid), PIDTYPE_PID) == NULL)) {
			return -EINVAL;
		}
		
		if (current_uid() != 0) {
			if (pid != 0) {
				if (check_pids_same_owner(current->pid, pid) != 0) {
					//pid requested not owned by calling process
					return -EPERM;
				}
			} else { //start monitoring all pids only allowed for a superuser
				return -EPERM;
			}
		}
		
		//can't start monitoring if system call hasn't been intercepted
		if (table[syscall].intercepted == 0) {
			return -EINVAL;
		}
		
		//pid already being monitored
		if ((table[syscall].monitored == 1 && check_pid_monitored(syscall, pid) == 1) ||
			(table[syscall].monitored == 2 && check_pid_monitored(syscall, pid) == 0)) {
				return -EBUSY;
		}
		
		if (pid == 0) {
			
			//clear blacklist, and start monitoring all pids
			spin_lock(&my_table_lock);
			destroy_list(syscall);
			table[syscall].monitored = 2;
			spin_unlock(&my_table_lock);
		
		} else {
			
			if (table[syscall].monitored == 2) {
				
				//blacklist removal for pid to start monitoring
				spin_lock(&my_table_lock);
				if (del_pid_sysc(pid, syscall) != 0) {
					spin_unlock(&my_table_lock);
					return -EBUSY; //pid not in blacklist, so already being monitored
				}
				spin_unlock(&my_table_lock);
				return 0;
				
			}
			
			//add pid to list of monitored pids
			spin_lock(&my_table_lock);
			if (add_pid_sysc(pid, syscall) != 0) {
				spin_unlock(&my_table_lock);
				return -ENOMEM; //not enough memory available for list addition
			}
			table[syscall].monitored = 1;
			spin_unlock(&my_table_lock);
			
		}
		
	} else if (cmd == REQUEST_STOP_MONITORING) {
		
		//invalid pid
		if (pid < 0 || (pid != 0 && pid_task(find_vpid(pid), PIDTYPE_PID) == NULL)) {
			return -EINVAL;
		}
		
		if (current_uid() != 0) {
			if (pid != 0) {
				if (check_pids_same_owner(current->pid, pid) != 0) {
					//pid requested not owned by calling process
					return -EPERM;
				}
			} else { //stop monitoring all pids command only allowed for a superuser
				return -EPERM;
			}
		}
		
		//system call hasn't been intercepted, or pid not being monitored
		if ((table[syscall].intercepted == 0) ||
			(table[syscall].monitored == 0) ||
			(table[syscall].monitored == 1 && !check_pid_monitored(syscall, pid)) ||
			(table[syscall].monitored == 2 && check_pid_monitored(syscall, pid))) {
			return -EINVAL;
		}
		
		if (pid == 0) {
			
			//stop monitoring all pids for syscall
			spin_lock(&my_table_lock);
			destroy_list(syscall);
			spin_unlock(&my_table_lock);
			
		} else {
			
			if (table[syscall].monitored == 2) {
				
				//blacklist addition for pid to stop monitoring
				spin_lock(&my_table_lock);
				if (add_pid_sysc(pid, syscall) != 0) {
					spin_unlock(&my_table_lock);
					return -ENOMEM; //no memory available for blacklist addition
				}
				spin_unlock(&my_table_lock);
				return 0;
				
			}
			
			//remove pid from list of monitored pids
			spin_lock(&my_table_lock);
			if (del_pid_sysc(pid, syscall) != 0) {
				spin_unlock(&my_table_lock);
				return -EINVAL; //pid already not being monitored
			}
			spin_unlock(&my_table_lock);
			
		}
		
	} else {
		//invalid command
		return -EINVAL;
	}
	
	return 0;
}

/**
 *
 */
long (*orig_custom_syscall)(void);


/**
 * Module initialization.
 */
static int init_function(void) {
	
	int s;
	
	spin_lock(&sys_call_table_lock);
	
	//save original system calls
	orig_custom_syscall = sys_call_table[MY_CUSTOM_SYSCALL];
	orig_exit_group = sys_call_table[__NR_exit_group];
	
	set_addr_rw((unsigned long) sys_call_table);
	
	//replace system call table entries
	sys_call_table[MY_CUSTOM_SYSCALL] = &my_syscall;
	sys_call_table[__NR_exit_group] = &my_exit_group;
	
	set_addr_ro((unsigned long) sys_call_table);
	
	spin_unlock(&sys_call_table_lock);
	
	spin_lock(&my_table_lock);
	
	//initialize table for bookkeeping
	for (s = 0; s < NR_syscalls; s++) {
		table[s].intercepted = 0;
		table[s].monitored = 0;
		table[s].listcount = 0;
		INIT_LIST_HEAD (&(table[s].my_list));
	}
	
	spin_unlock(&my_table_lock);
	
	return 0;
}

/**
 * Module exits.
 */
static void exit_function(void) {
	
	int s;
	
	spin_lock(&my_table_lock);
	
	//deintercept all system calls and cleanup all pid lists
	for (s = 0; s < NR_syscalls; s++) {
		destroy_list(s);
		table[s].intercepted = 0;
	}
	
	spin_unlock(&my_table_lock);
	
	spin_lock(&sys_call_table_lock);
	set_addr_rw((unsigned long) sys_call_table);
	
	//restore original system calls
	sys_call_table[MY_CUSTOM_SYSCALL] = orig_custom_syscall;
	sys_call_table[__NR_exit_group] = orig_exit_group;
	
	set_addr_ro((unsigned long) sys_call_table);
	spin_unlock(&sys_call_table_lock);
	
}

module_init(init_function);
module_exit(exit_function);
