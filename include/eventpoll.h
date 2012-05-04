#ifndef EVENTPOLL_H__
#define EVENTPOLL_H__

#include <sys/types.h>
#include <unistd.h>

#include "compiler.h"
#include "types.h"
#include "files.h"
#include "crtools.h"

extern int is_eventpoll_link(int lfd);
extern int dump_one_eventpoll(int lfd, u32 id, const struct fd_parms *p);
extern int collect_eventpoll(void);
extern void show_eventpoll(int fd, struct cr_options *o);
extern void show_eventpoll_tfd(int fd, struct cr_options *o);

#endif /* EVENTPOLL_H__ */