#ifndef __FILE_LOCK_H__
#define __FILE_LOCK_H__

#include "crtools.h"
#include "protobuf.h"
#include "../protobuf/file-lock.pb-c.h"

#define FL_POSIX	1
#define FL_FLOCK	2

/* for posix fcntl() and lockf() */
#ifndef F_RDLCK
#define F_RDLCK		0
#define F_WRLCK		1
#define F_UNLCK		2
#endif

/* operations for bsd flock(), also used by the kernel implementation */
#define LOCK_MAND	32	/* This is a mandatory flock ... */
#define LOCK_READ	64	/* which allows concurrent read operations */
#define LOCK_WRITE	128	/* which allows concurrent write operations */
#define LOCK_RW		192	/* which allows concurrent read & write ops */

struct file_lock {
	long long	fl_id;
	char		fl_flag[10];
	char		fl_type[15];
	char		fl_option[10];

	pid_t		fl_owner;
	int		maj, min;
	unsigned long	i_no;
	long long	start;
	char		end[32];

	struct list_head list;		/* list of all file locks */
};

extern struct list_head file_lock_list;

extern struct file_lock *alloc_file_lock(void);
extern void free_file_locks(void);
extern int dump_one_file_lock(FileLockEntry *fle, const struct cr_fdset *fdset);

#endif /* __FILE_LOCK_H__ */