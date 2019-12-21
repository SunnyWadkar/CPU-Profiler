#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kprobes.h>
#include <linux/hashtable.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/stacktrace.h>
#include <linux/slab.h>
#include <linux/jhash.h>
#include <linux/kallsyms.h>
#include <linux/rbtree.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sunny Wadkar");
MODULE_DESCRIPTION("CPU Profiler");

#define FUNCTION_NAME_LENGTH 20
#define MAX_TRACE_SIZE 51
#define PROFILER_OUTPUT_NO 20

extern unsigned int stack_trace_save_user(unsigned long *store, unsigned int size);
typedef typeof(&stack_trace_save_user) stack_trace_save_user_fnptr;
#define stack_trace_save_user (*(stack_trace_save_user_fnptr) kallsyms_stack_trace_usr_save)
void *kallsyms_stack_trace_usr_save = NULL;

static DEFINE_HASHTABLE(scheduleHashTable,10);

static DEFINE_SPINLOCK(scheduleHashTableLock);

struct rb_root scheduleTreeRoot = RB_ROOT;

struct schedule_rbtree_entry
{
	unsigned int key;
	unsigned int numberOfSchedules;
	unsigned long stackLog[MAX_TRACE_SIZE];
	unsigned int stackLogLength;
	u64 cputime;
	struct rb_node rbentry;
};

struct schedule_hash_entry
{
	//unsigned int process_pid;
	unsigned int numberOfSchedules;
	unsigned long stackLog[MAX_TRACE_SIZE];
	unsigned int stackLogLength;
	u64 scheduleTime;
	struct hlist_node hashTableNode;
};

static bool deletePreviousRBTreeEntry(unsigned int key,u64* time_ret,unsigned int* sc_count)
{
        struct rb_node* temp_node;
        struct schedule_rbtree_entry* del_rbtree_node;
        temp_node = rb_first(&scheduleTreeRoot);
        while(temp_node)
        {
                del_rbtree_node = rb_entry(temp_node, struct schedule_rbtree_entry, rbentry);
                if(del_rbtree_node->key == key)
                {
			*sc_count = del_rbtree_node->numberOfSchedules;
                        *time_ret = del_rbtree_node->cputime;
                        rb_erase(&del_rbtree_node->rbentry,&scheduleTreeRoot);
                        kfree(del_rbtree_node);
                        return true;
                }
                temp_node = rb_next(temp_node);
        }
	*time_ret = 0;
	*sc_count = 0;
        return false;
}

static int storeToScheduleRBTree(unsigned int key, unsigned long* st_log, unsigned int st_len, u64 time)
{
	int i = 0;
	u64 prev_time = 0;
	unsigned int sched_count = 0;
	struct schedule_rbtree_entry *new_node, *attached_node;
	struct rb_node **current_rb_node, *rbparent_node = NULL;
	deletePreviousRBTreeEntry(key,&prev_time,&sched_count);
	new_node = (struct schedule_rbtree_entry*)kmalloc(sizeof(struct schedule_rbtree_entry),GFP_ATOMIC);
	if(new_node != NULL)
	{
		new_node->key = key;
		new_node->numberOfSchedules = sched_count + 1;
		new_node->stackLogLength = st_len;
		new_node->cputime = time + prev_time;
		while((i< st_len) && (st_len <= MAX_TRACE_SIZE))
                {
                        new_node->stackLog[i] = *st_log;
			st_log++;
                        i++;
                }
		current_rb_node = &scheduleTreeRoot.rb_node;
		while(*current_rb_node != NULL)
		{
			rbparent_node = *current_rb_node;
			attached_node = rb_entry(rbparent_node,struct schedule_rbtree_entry,rbentry);
			if(attached_node->cputime < new_node->cputime)
			{
				current_rb_node = &((*current_rb_node)->rb_right);
			}
			else
			{
				current_rb_node = &((*current_rb_node)->rb_left);
			}
		}
		rb_link_node(&new_node->rbentry,rbparent_node,current_rb_node);
		rb_insert_color(&(new_node->rbentry),&scheduleTreeRoot);
		return 0;
	}
	else
	{
		printk(KERN_INFO "No memory to add rbtree node\n");
		return -ENOMEM;
	}
}

void printScheduleRBTreeMaxNodes(struct seq_file *sf, int nodeCount)
{
	int i = 0, j;
	struct rb_node* curr_node;
	struct schedule_rbtree_entry * read_node;
	curr_node = rb_last(&scheduleTreeRoot);
	for(i = 0; i < nodeCount; i++)
	{
		if(curr_node != NULL)
		{
			read_node = rb_entry(curr_node,struct schedule_rbtree_entry,rbentry);
			seq_printf(sf,"PID: %ld \t",read_node->stackLog[0]);
			seq_printf(sf,"Number of Schedules: %d \n",read_node->numberOfSchedules);
			if(read_node->stackLogLength == 1)
                        {
                        	seq_printf(sf,"No Stack Trace available for this task.\n");
                        }
                        else
                        {
                                seq_printf(sf,"Stack Trace:\n");
                                for (j = 1; j < read_node->stackLogLength; j++)
                                {
                                        seq_printf(sf,"%pS\n",(void *)read_node->stackLog[j]);
                                	//seq_printf(sf,"%p\n",(void*)read_node->stackLog[j]);
                                }
                        }
			seq_printf(sf,"CPU Time: %llu rdtsc ticks\n",read_node->cputime);
		}
		curr_node = rb_prev(curr_node);
	}
}

static int storeToScheduleHashTable(unsigned int key, unsigned long* st_log, unsigned int st_len, u64 time)
{
	struct schedule_hash_entry* node;
	int i = 0;
	hash_for_each_possible(scheduleHashTable,node,hashTableNode,key)
	{
		if(node != NULL)
		{
			node->numberOfSchedules += 1;
			node->scheduleTime += time;
			return 0;
		}
	}
	node = (struct schedule_hash_entry*)kmalloc(sizeof(struct schedule_hash_entry),GFP_ATOMIC);
	if(node != NULL)
	{
		//node->process_pid = st_log[0];
		node->numberOfSchedules = 1;
		while((i< st_len) && (st_len <= MAX_TRACE_SIZE))
		{
			node->stackLog[i] = st_log[i];
			i++;
		}
		node->stackLogLength = st_len;
		node->scheduleTime = time;
 		hash_add(scheduleHashTable,&node->hashTableNode,key);
		return 0;
	}
	else
	{
		printk(KERN_INFO "No memory to add hash map node\n");
		return -ENOMEM;
	}
}

static void printScheduleHashTable(struct seq_file *sf)
{
	int hashBucket, i;
	struct schedule_hash_entry* read_node;
	if(!(hash_empty(scheduleHashTable)))
	{
		hash_for_each(scheduleHashTable,hashBucket,read_node,hashTableNode)
		{
			if(read_node != NULL)
			{
				//seq_printf(sf,"PID: %d \t",(unsigned int)read_node->stackLog[0]);
				if(read_node->stackLogLength == 1)
				{
					seq_printf(sf,"No Stack Trace available for this task.\n");
				}
				else
				{
                        		seq_printf(sf,"Stack Trace:\n");
					for (i = 1; i < read_node->stackLogLength; i++)
					{
						//seq_printf(sf,"%*c%pS\n",5,' ', (void *)read_node->stackLog[i]);
						seq_printf(sf,"%p\n",(void*)read_node->stackLog[i]);
					}
				}
				//seq_printf(sf,"Number of Schedules: %d \n",read_node->numberOfSchedules);
                        	seq_printf(sf,"CPU Time: %llu rdtsc ticks\n",read_node->scheduleTime);
			}
		}
	}
}

static char probedFunctionName[FUNCTION_NAME_LENGTH] = "pick_next_task_fair";

unsigned long stackTraceLog[MAX_TRACE_SIZE];
u64 timestamp;

static int  perftop_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	unsigned int task_pid,st_length;
	unsigned long flags;
	u32 stackTraceHashKey;
	u64 task_timestamp;
	u64* entry_data = (u64*)ri->data;
	unsigned long current_task_struct_pointer = regs->si;
	struct task_struct* curr_task = (struct task_struct*)current_task_struct_pointer;
	task_pid = (unsigned int)curr_task->pid;
	task_timestamp = rdtsc() - *entry_data;
	spin_lock_irqsave(&scheduleHashTableLock,flags);
	stackTraceLog[0] = task_pid;
	if(curr_task->mm == NULL)
	{
		st_length = stack_trace_save(&stackTraceLog[1],MAX_TRACE_SIZE-1,6);
	}
	else
	{
		st_length = stack_trace_save_user(&stackTraceLog[1],MAX_TRACE_SIZE-1);
	}
        stackTraceHashKey = jhash2((u32*)stackTraceLog,(st_length+1)*2,0);
	storeToScheduleHashTable(stackTraceHashKey,stackTraceLog,st_length+1,task_timestamp);
	storeToScheduleRBTree(stackTraceHashKey,stackTraceLog,st_length+1,task_timestamp);
	spin_unlock_irqrestore(&scheduleHashTableLock,flags);
	return 0;
}

static int perftop_return_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	u64* ret_data = (u64*)ri->data;
	*ret_data = rdtsc();
	return 0;
}

static struct kretprobe perftopKernelReturnProbe = {
        .handler = perftop_return_handler,
        .entry_handler = perftop_entry_handler,
        .maxactive = NR_CPUS,
        .data_size = sizeof(u64),
};

static int perftop_exec(struct seq_file* p_out, void* v)
{
	unsigned long flags;
	spin_lock_irqsave(&scheduleHashTableLock,flags);
	//printScheduleHashTable(p_out);
	printScheduleRBTreeMaxNodes(p_out,PROFILER_OUTPUT_NO);
	spin_unlock_irqrestore(&scheduleHashTableLock,flags);
	return 0;
}

static int perftop_open(struct inode* inode, struct file* file)
{
	return single_open(file,perftop_exec,NULL);
}

static const struct file_operations perftop_fops = {
	.owner = THIS_MODULE,
	.open = perftop_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static int __init perftop_init(void)
{
	int kprobeRegistrationStatus;
	kallsyms_stack_trace_usr_save = (void*)kallsyms_lookup_name("stack_trace_save_user");
	perftopKernelReturnProbe.kp.symbol_name = probedFunctionName;
	kprobeRegistrationStatus = register_kretprobe(&perftopKernelReturnProbe);
	if(kprobeRegistrationStatus < 0)
	{
		printk("Module perftop insertion failed. Cannot register a kretprobe for function %s!\n",probedFunctionName);
		return -1;
	}
	proc_create("perftop",0,NULL,&perftop_fops);
	printk("Module perftop inserted\n");
	return 0;
}

static void __exit perftop_exit(void)
{
	unregister_kretprobe(&perftopKernelReturnProbe);
	remove_proc_entry("perftop",NULL);
	printk("Module perftop removed\n");
}

module_init(perftop_init);

module_exit(perftop_exit);
