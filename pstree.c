#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

#include "pstree.h"
#include "restorer.h"
#include "util.h"

struct pstree_item *root_item;

void free_pstree(struct pstree_item *root_item)
{
	struct pstree_item *item = root_item, *parent;

	while (item) {
		if (!list_empty(&item->children)) {
			item = list_first_entry(&item->children, struct pstree_item, list);
			continue;
		}

		parent = item->parent;
		list_del(&item->list);
		xfree(item->threads);
		xfree(item);
		item = parent;
	}
}

struct pstree_item *__alloc_pstree_item(bool rst)
{
	struct pstree_item *item;

	item = xzalloc(sizeof(*item) + (rst ? sizeof(item->rst[0]) : 0));
	if (!item)
		return NULL;

	INIT_LIST_HEAD(&item->children);
	item->threads = NULL;
	item->nr_threads = 0;
	item->pid.virt = -1;
	item->pid.real = -1;
	item->born_sid = -1;

	return item;
}

struct pstree_item *pstree_item_next(struct pstree_item *item)
{
	if (!list_empty(&item->children)) {
		item = list_first_entry(&item->children, struct pstree_item, list);
		return item;
	}

	while (1) {
		if (item->parent == NULL) {
			item = NULL;
			break;
		}
		if (item->list.next == &item->parent->children) {
			item = item->parent;
			continue;
		} else {
			item = list_entry(item->list.next, struct pstree_item, list);
			break;
		}
	}

	return item;
}

int dump_pstree(struct pstree_item *root_item)
{
	struct pstree_item *item = root_item;
	struct pstree_entry e;
	int ret = -1, i;
	int pstree_fd;

	pr_info("\n");
	pr_info("Dumping pstree (pid: %d)\n", root_item->pid.real);
	pr_info("----------------------------------------\n");

	pstree_fd = open_image(CR_FD_PSTREE, O_DUMP);
	if (pstree_fd < 0)
		return -1;

	for_each_pstree_item(item) {
		pr_info("Process: %d(%d)\n", item->pid.virt, item->pid.real);

		e.pid		= item->pid.virt;
		e.ppid		= item->parent ? item->parent->pid.virt : 0;
		e.pgid		= item->pgid;
		e.sid		= item->sid;
		e.nr_threads	= item->nr_threads;

		if (write_img(pstree_fd, &e))
			goto err;

		for (i = 0; i < item->nr_threads; i++) {
			if (write_img_buf(pstree_fd,
					  &item->threads[i].virt, sizeof(u32)))
				goto err;
		}
	}
	ret = 0;

err:
	pr_info("----------------------------------------\n");
	close(pstree_fd);
	return ret;
}

static int max_pid = 0;
int prepare_pstree(void)
{
	int ret = 0, i, ps_fd;
	struct pstree_item *pi, *parent = NULL;

	pr_info("Reading image tree\n");

	ps_fd = open_image_ro(CR_FD_PSTREE);
	if (ps_fd < 0)
		return ps_fd;

	while (1) {
		struct pstree_entry e;

		ret = read_img_eof(ps_fd, &e);
		if (ret <= 0)
			break;

		ret = -1;
		pi = alloc_pstree_item_with_rst();
		if (pi == NULL)
			break;

		pi->pid.virt = e.pid;
		if (e.pid > max_pid)
			max_pid = e.pid;

		pi->pgid = e.pgid;
		if (e.pgid > max_pid)
			max_pid = e.pgid;

		pi->sid = e.sid;
		if (e.sid > max_pid)
			max_pid = e.sid;

		if (e.ppid == 0) {
			BUG_ON(root_item);
			root_item = pi;
			pi->parent = NULL;
			INIT_LIST_HEAD(&pi->list);
		} else {
			/*
			 * Fast path -- if the pstree image is not edited, the
			 * parent of any item should have already being restored
			 * and sit among the last item's ancestors.
			 */
			while (parent) {
				if (parent->pid.virt == e.ppid)
					break;
				parent = parent->parent;
			}

			if (parent == NULL)
				for_each_pstree_item(parent)
					if (parent->pid.virt == e.ppid)
						break;

			if (parent == NULL) {
				pr_err("Can't find a parent for %d", pi->pid.virt);
				xfree(pi);
				break;
			}

			pi->parent = parent;
			list_add(&pi->list, &parent->children);
		}

		parent = pi;

		pi->nr_threads = e.nr_threads;
		pi->threads = xmalloc(e.nr_threads * sizeof(struct pid));
		if (!pi->threads)
			break;

		ret = 0;
		for (i = 0; i < e.nr_threads; i++) {
			ret = read_img_buf(ps_fd, &pi->threads[i].virt, sizeof(u32));
			if (ret < 0)
				break;
		}
		if (ret < 0)
			break;

		task_entries->nr += e.nr_threads;
		task_entries->nr_tasks++;
	}

	close(ps_fd);
	return ret;
}

int prepare_pstree_ids(void)
{
	struct pstree_item *item, *child, *helper, *tmp;
	LIST_HEAD(helpers);

	/*
	 * Some task can be reparented to init. A helper task should be added
	 * for restoring sid of such tasks. The helper tasks will be exited
	 * immediately after forking children and all children will be
	 * reparented to init.
	 */
	list_for_each_entry(item, &root_item->children, list) {
		if (item->sid == root_item->sid || item->sid == item->pid.virt)
			continue;

		helper = alloc_pstree_item();
		if (helper == NULL)
			return -1;
		helper->sid = item->sid;
		helper->pgid = item->sid;
		helper->pid.virt = item->sid;
		helper->state = TASK_HELPER;
		helper->parent = root_item;
		list_add_tail(&helper->list, &helpers);
		task_entries->nr_helpers++;

		pr_info("Add a helper %d for restoring SID %d\n",
				helper->pid.virt, helper->sid);

		child = list_entry(item->list.prev, struct pstree_item, list);
		item = child;

		list_for_each_entry_safe_continue(child, tmp, &root_item->children, list) {
			if (child->sid != helper->sid)
				continue;
			if (child->sid == child->pid.virt)
				continue;

			pr_info("Attach %d to the temporary task %d\n",
					child->pid.virt, helper->pid.virt);

			child->parent = helper;
			list_move(&child->list, &helper->children);
		}
	}

	/* Try to connect helpers to session leaders */
	for_each_pstree_item(item) {
		if (!item->parent) /* skip the root task */
			continue;

		if (item->state == TASK_HELPER)
			continue;

		if (item->sid != item->pid.virt) {
			struct pstree_item *parent;

			if (item->parent->sid == item->sid)
				continue;

			/* the task could fork a child before and after setsid() */
			parent = item->parent;
			while (parent && parent->pid.virt != item->sid) {
				if (parent->born_sid != -1 && parent->born_sid != item->sid) {
					pr_err("Can't determing with which sid (%d or %d)"
						"the process %d was born\n",
						parent->born_sid, item->sid, parent->pid.virt);
					return -1;
				}
				parent->born_sid = item->sid;
				pr_info("%d was born with sid %d\n", parent->pid.virt, item->sid);
				parent = parent->parent;
			}

			if (parent == NULL) {
				pr_err("Can't find a session leader for %d\n", item->sid);
				return -1;
			}

			continue;
		}

		pr_info("Session leader %d\n", item->sid);

		/* Try to find helpers, who should be connected to the leader */
		list_for_each_entry(child, &helpers, list) {
			if (child->state != TASK_HELPER)
				continue;

			if (child->sid != item->sid)
				continue;

			child->pgid = item->pgid;
			child->pid.virt = ++max_pid;
			child->parent = item;
			list_move(&child->list, &item->children);

			pr_info("Attach %d to the task %d\n",
					child->pid.virt, item->pid.virt);

			break;
		}
	}

	/* All other helpers are session leaders for own sessions */
	list_splice(&helpers, &root_item->children);

	return 0;
}

bool restore_before_setsid(struct pstree_item *child)
{
	int csid = child->born_sid == -1 ? child->sid : child->born_sid;

	if (child->parent->born_sid == csid)
		return true;

	return false;
}