/*
 * Copyright (C) 2009 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/time.h>
#include <corosync/cpg.h>

#include "sheepdog_proto.h"
#include "collie.h"
#include "list.h"
#include "util.h"
#include "meta.h"
#include "logger.h"
#include "work.h"

struct vm {
	struct sheepdog_vm_list_entry ent;
	struct list_head list;
};

struct node {
	uint32_t nodeid;
	uint32_t pid;
	struct sheepdog_node_list_entry ent;
	struct list_head list;
};

struct message_header {
	uint8_t op;
	uint8_t done;
	uint8_t pad[2];
	uint32_t msg_length;
	struct sheepdog_node_list_entry from;
};

struct join_message {
	struct message_header header;
	uint32_t nodeid;
	uint32_t pid;
	struct sheepdog_node_list_entry master_node;
	uint32_t epoch;
	uint32_t nr_nodes;
	uint32_t nr_sobjs;
	uint32_t pad;
	struct {
		uint32_t nodeid;
		uint32_t pid;
		struct sheepdog_node_list_entry ent;
	} nodes[SD_MAX_NODES];
};

struct vdi_op_message {
	struct message_header header;
	struct sd_vdi_req req;
	struct sd_vdi_rsp rsp;
	uint8_t data[0];
};

struct work_deliver {
	struct message_header *msg;

	struct work work;
};

struct work_confch {
	struct cpg_address *member_list;
	size_t member_list_entries;
	struct cpg_address *left_list;
	size_t left_list_entries;
	struct cpg_address *joined_list;
	size_t joined_list_entries;

	struct work work;
};

static int node_cmp(const void *a, const void *b)
{
	const struct sheepdog_node_list_entry *node1 = a;
	const struct sheepdog_node_list_entry *node2 = b;

	if (node1->id < node2->id)
		return -1;
	if (node1->id > node2->id)
		return 1;
	return 0;
}

static int send_message(cpg_handle_t handle, struct message_header *msg)
{
	struct iovec iov;
	int ret;

	iov.iov_base = msg;
	iov.iov_len = msg->msg_length;
retry:
	ret = cpg_mcast_joined(handle, CPG_TYPE_AGREED, &iov, 1);
	switch (ret) {
	case CS_OK:
		break;
	case CS_ERR_TRY_AGAIN:
		dprintf("failed to send message. try again\n");
		sleep(1);
		goto retry;
	default:
		eprintf("failed to send message, %d\n", ret);
		return -1;
	}
	return 0;
}


static int get_node_idx(struct sheepdog_node_list_entry *ent,
			struct sheepdog_node_list_entry *entries, int nr_nodes)
{
	ent = bsearch(ent, entries, nr_nodes, sizeof(*ent), node_cmp);
	if (!ent)
		return -1;

	return ent - entries;
}

static void get_node_list(struct sd_node_req *req,
			  struct sd_node_rsp *rsp, void *data)
{
	int nr_nodes;
	struct node *node;

	nr_nodes = build_node_list(&sys->node_list, data);
	rsp->data_length = nr_nodes * sizeof(struct sheepdog_node_list_entry);
	rsp->nr_nodes = nr_nodes;
	rsp->local_idx = get_node_idx(&sys->this_node, data, nr_nodes);

	if (list_empty(&sys->node_list)) {
		rsp->master_idx = -1;
		return;
	}
	node = list_first_entry(&sys->node_list, struct node, list);
	rsp->master_idx = get_node_idx(&node->ent, data, nr_nodes);
}

static void get_vm_list(struct sd_rsp *rsp, void *data)
{
	int nr_vms;
	struct vm *vm;

	struct sheepdog_vm_list_entry *p = data;
	list_for_each_entry(vm, &sys->vm_list, list) {
		*p++ = vm->ent;
	}

	nr_vms = p - (struct sheepdog_vm_list_entry *)data;
	rsp->data_length = nr_vms * sizeof(struct sheepdog_vm_list_entry);
}

void cluster_queue_request(struct work *work, int idx)
{
	struct request *req = container_of(work, struct request, work);
	struct sd_req *hdr = (struct sd_req *)&req->rq;
	struct sd_rsp *rsp = (struct sd_rsp *)&req->rp;
	struct vdi_op_message *msg;
	int ret = SD_RES_SUCCESS;

	eprintf("%p %x\n", req, hdr->opcode);

	switch (hdr->opcode) {
	case SD_OP_GET_NODE_LIST:
		get_node_list((struct sd_node_req *)hdr,
			      (struct sd_node_rsp *)rsp, req->data);
		break;
	case SD_OP_GET_VM_LIST:
		get_vm_list(rsp, req->data);
		break;
	default:
		/* forward request to group */
		goto forward;
	}

	rsp->result = ret;
	return;

forward:
	msg = zalloc(sizeof(*msg) + hdr->data_length);
	if (!msg) {
		eprintf("out of memory\n");
		return;
	}

	msg->header.op = SD_MSG_VDI_OP;
	msg->header.done = 0;
	msg->header.msg_length = sizeof(*msg) + hdr->data_length;
	msg->header.from = sys->this_node;
	msg->req = *((struct sd_vdi_req *)&req->rq);
	msg->rsp = *((struct sd_vdi_rsp *)&req->rp);
	if (hdr->flags & SD_FLAG_CMD_WRITE)
		memcpy(msg->data, req->data, hdr->data_length);

	list_add(&req->pending_list, &sys->pending_list);

	send_message(sys->handle, (struct message_header *)msg);

	free(msg);
}

static struct vm *lookup_vm(struct list_head *entries, char *name)
{
	struct vm *vm;

	list_for_each_entry(vm, entries, list) {
		if (!strcmp((char *)vm->ent.name, name))
			return vm;
	}

	return NULL;
}

static void group_handler(int listen_fd, int events, void *data)
{
	cpg_dispatch(sys->handle, CPG_DISPATCH_ALL);
}

static void print_node_list(void)
{
	struct node *node;
	char name[128];
	list_for_each_entry(node, &sys->node_list, list) {
		dprintf("%c nodeid: %x, pid: %d, ip: %s\n",
			node_cmp(&node->ent, &sys->this_node) ? ' ' : 'l',
			node->nodeid, node->pid,
			addr_to_str(name, sizeof(name), node->ent.addr, node->ent.port));
	}
}

static void add_node(uint32_t nodeid, uint32_t pid,
		     struct sheepdog_node_list_entry *sd_ent)
{
	struct node *node;

	node = zalloc(sizeof(*node));
	if (!node) {
		eprintf("out of memory\n");
		return;
	}
	node->nodeid = nodeid;
	node->pid = pid;
	node->ent = *sd_ent;
	list_add_tail(&node->list, &sys->node_list);
}

static int is_master(void)
{
	struct node *node;

	if (!sys->synchronized)
		return 0;

	if (list_empty(&sys->node_list))
		return 1;

	node = list_first_entry(&sys->node_list, struct node, list);
	if (node_cmp(&node->ent, &sys->this_node) == 0)
		return 1;

	return 0;
}

static void join(struct join_message *msg)
{
	struct node *node;

	if (!sys->synchronized)
		return;

	if (!is_master())
		return;

	if (msg->nr_sobjs)
		sys->nr_sobjs = msg->nr_sobjs;

	msg->epoch = sys->epoch;
	msg->nr_sobjs = sys->nr_sobjs;
	list_for_each_entry(node, &sys->node_list, list) {
		msg->nodes[msg->nr_nodes].nodeid = node->nodeid;
		msg->nodes[msg->nr_nodes].pid = node->pid;
		msg->nodes[msg->nr_nodes].ent = node->ent;
		msg->nr_nodes++;
	}
}

static void update_cluster_info(struct join_message *msg)
{
	int i;
	int ret, nr_nodes = msg->nr_nodes;
	struct node *node, *e;
	struct sheepdog_node_list_entry entry[SD_MAX_NODES];

	if (!sys->nr_sobjs)
		sys->nr_sobjs = msg->nr_sobjs;

	if (sys->synchronized)
		goto out;

	list_for_each_entry_safe(node, e, &sys->node_list, list) {
		list_del(&node->list);
		free(node);
	}

	INIT_LIST_HEAD(&sys->node_list);
	for (i = 0; i < nr_nodes; i++)
		add_node(msg->nodes[i].nodeid, msg->nodes[i].pid,
			 &msg->nodes[i].ent);

	sys->epoch = msg->epoch;
	sys->synchronized = 1;

	nr_nodes = build_node_list(&sys->node_list, entry);

	ret = epoch_log_write(sys->epoch, (char *)entry,
			      nr_nodes * sizeof(struct sheepdog_node_list_entry));
	if (ret < 0)
		eprintf("can't write epoch %u\n", sys->epoch);

	/* we are ready for object operations */
	update_epoch_store(sys->epoch);
out:
	add_node(msg->nodeid, msg->pid, &msg->header.from);

	nr_nodes = build_node_list(&sys->node_list, entry);

	ret = epoch_log_write(sys->epoch + 1, (char *)entry,
			      nr_nodes * sizeof(struct sheepdog_node_list_entry));
	if (ret < 0)
		eprintf("can't write epoch %u\n", sys->epoch + 1);

	sys->epoch++;

	update_epoch_store(sys->epoch);

	print_node_list();
}

static void vdi_op(struct vdi_op_message *msg)
{
	const struct sd_vdi_req *hdr = &msg->req;
	struct sd_vdi_rsp *rsp = &msg->rsp;
	void *data = msg->data;
	int ret = SD_RES_SUCCESS, is_current;
	uint64_t oid = 0;

	switch (hdr->opcode) {
	case SD_OP_NEW_VDI:
		ret = add_vdi(data, hdr->data_length, hdr->vdi_size, &oid,
			      hdr->base_oid, hdr->tag, hdr->copies, hdr->flags);
		break;
	case SD_OP_LOCK_VDI:
	case SD_OP_GET_VDI_INFO:
		ret = lookup_vdi(data, &oid, hdr->tag, 1, &is_current);
		if (ret != SD_RES_SUCCESS)
			break;
		if (is_current)
			rsp->flags = SD_VDI_RSP_FLAG_CURRENT;
		break;
	case SD_OP_RELEASE_VDI:
		break;
	case SD_OP_MAKE_FS:
		ret = make_super_object(&msg->req);
		break;
	default:
		ret = SD_RES_SYSTEM_ERROR;
		eprintf("opcode %d is not implemented\n", hdr->opcode);
		break;
	}

	rsp->oid = oid;
	rsp->result = ret;
}

static void vdi_op_done(struct vdi_op_message *msg)
{
	const struct sd_vdi_req *hdr = &msg->req;
	struct sd_vdi_rsp *rsp = &msg->rsp;
	void *data = msg->data;
	struct vm *vm;
	struct request *req;
	int ret = msg->rsp.result;

	switch (hdr->opcode) {
	case SD_OP_NEW_VDI:
		break;
	case SD_OP_LOCK_VDI:
		if (lookup_vm(&sys->vm_list, (char *)data)) {
			ret = SD_RES_VDI_LOCKED;
			break;
		}

		vm = zalloc(sizeof(*vm));
		if (!vm) {
			ret = SD_RES_UNKNOWN;
			break;
		}
		strcpy((char *)vm->ent.name, (char *)data);
		memcpy(vm->ent.host_addr, msg->header.from.addr,
		       sizeof(vm->ent.host_addr));
		vm->ent.host_port = msg->header.from.port;

		list_add(&vm->list, &sys->vm_list);
		break;
	case SD_OP_RELEASE_VDI:
		vm = lookup_vm(&sys->vm_list, (char *)data);
		if (!vm) {
			ret = SD_RES_VDI_NOT_LOCKED;
			break;
		}

		list_del(&vm->list);
		free(vm);
		break;
	case SD_OP_GET_VDI_INFO:
		break;
	case SD_OP_MAKE_FS:
		if (ret == SD_RES_SUCCESS) {
			sys->nr_sobjs = ((struct sd_so_req *)hdr)->copies;
			eprintf("%d\n", sys->nr_sobjs);
		}

		break;
	default:
		eprintf("unknown operation %d\n", hdr->opcode);
		ret = SD_RES_UNKNOWN;
	}

	if (node_cmp(&sys->this_node, &msg->header.from) != 0)
		return;

	req = list_first_entry(&sys->pending_list, struct request, pending_list);

	rsp->result = ret;
	memcpy(req->data, data, rsp->data_length);
	memcpy(&req->rp, rsp, sizeof(req->rp));
	list_del(&req->pending_list);
	req->done(req);
}

static void __sd_deliver(struct work *work, int idx)
{
	struct work_deliver *w = container_of(work, struct work_deliver, work);
	struct message_header *m = w->msg;
	char name[128];

	dprintf("op: %d, done: %d, size: %d, from: %s\n",
		m->op, m->done, m->msg_length,
		addr_to_str(name, sizeof(name), m->from.addr, m->from.port));

	if (!m->done) {
		if (!is_master())
			return;

		switch (m->op) {
		case SD_MSG_JOIN:
			join((struct join_message *)m);
			break;
		case SD_MSG_VDI_OP:
			vdi_op((struct vdi_op_message *)m);
			break;
		default:
			eprintf("unknown message %d\n", m->op);
			break;
		}

		m->done = 1;
		send_message(sys->handle, m);
	} else {
		switch (m->op) {
		case SD_MSG_JOIN:
			update_cluster_info((struct join_message *)m);
			break;
		case SD_MSG_VDI_OP:
			vdi_op_done((struct vdi_op_message *)m);
			break;
		default:
			eprintf("unknown message %d\n", m->op);
			break;
		}
	}
}

static void __sd_deliver_done(struct work *work, int idx)
{
	struct work_deliver *w = container_of(work, struct work_deliver, work);
/* 	struct message_header *m = w->msg; */
/* 	struct cluster_info *ci = w->ci; */

	/*
	 * FIXME: we want to recover only after all nodes are fully
	 * synchronized
	 */

	/* disabled for now */
/* 	if (m->done && m->op == SD_MSG_JOIN) */
/* 		start_recovery(ci, ci->epoch, 1); */

	free(w->msg);
	free(w);
}

static void sd_deliver(cpg_handle_t handle, const struct cpg_name *group_name,
		       uint32_t nodeid, uint32_t pid, void *msg, size_t msg_len)
{
	struct work_deliver *w;
	struct message_header *m = msg;
	char name[128];

	dprintf("op: %d, done: %d, size: %d, from: %s\n",
		m->op, m->done, m->msg_length,
		addr_to_str(name, sizeof(name), m->from.addr, m->from.port));

	w = zalloc(sizeof(*w));
	if (!w)
		return;

	w->msg = zalloc(msg_len);
	if (!w->msg)
		return;
	memcpy(w->msg, msg, msg_len);

	w->work.fn = __sd_deliver;
	w->work.done = __sd_deliver_done;

	if (m->op == SD_MSG_JOIN)
		w->work.attr = WORK_ORDERED;

	queue_work(dobj_queue, &w->work);
}

static void __sd_confch(struct work *work, int idx)
{
	struct work_confch *w = container_of(work, struct work_confch, work);
	struct node *node, *e;
	int i;

	const struct cpg_address *member_list = w->member_list;
	size_t member_list_entries = w->member_list_entries;
	const struct cpg_address *left_list = w->left_list;
	size_t left_list_entries = w->left_list_entries;
	const struct cpg_address *joined_list = w->joined_list;
	size_t joined_list_entries = w->joined_list_entries;

	if (member_list_entries == joined_list_entries - left_list_entries &&
	    sys->this_nodeid == member_list[0].nodeid &&
	    sys->this_pid == member_list[0].pid)
		sys->synchronized = 1;

	for (i = 0; i < left_list_entries; i++) {
		list_for_each_entry_safe(node, e, &sys->node_list, list) {
			int nr;
			unsigned pid;
			struct sheepdog_node_list_entry e[SD_MAX_NODES];

			if (node->nodeid != left_list[i].nodeid ||
			    node->pid != left_list[i].pid)
				continue;

			pid = node->pid;

			list_del(&node->list);
			free(node);

			nr = build_node_list(&sys->node_list, e);
			epoch_log_write(sys->epoch + 1, (char *)e,
					nr * sizeof(struct sheepdog_node_list_entry));

			sys->epoch++;

			update_epoch_store(sys->epoch);
		}
	}

	for (i = 0; i < joined_list_entries; i++) {
		if (sys->this_nodeid == joined_list[i].nodeid &&
		    sys->this_pid == joined_list[i].pid) {
			struct join_message msg;

			msg.header.op = SD_MSG_JOIN;
			msg.header.done = 0;
			msg.header.msg_length = sizeof(msg);
			msg.header.from = sys->this_node;
			msg.nodeid = sys->this_nodeid;
			msg.pid = sys->this_pid;
			msg.nr_sobjs = nr_sobjs;

			send_message(sys->handle, (struct message_header *)&msg);

			eprintf("%d\n", i);

			break;
		}
	}

	if (left_list_entries == 0)
		return;

	print_node_list();
}

static void __sd_confch_done(struct work *work, int idx)
{
	struct work_confch *w = container_of(work, struct work_confch, work);

	/* FIXME: worker threads can't call start_recovery */
	if (w->left_list_entries) {
		if (w->left_list_entries > 1)
			eprintf("we can't handle %Zd\n", w->left_list_entries);
		start_recovery(sys->epoch, 0);
	}

	free(w->member_list);
	free(w->left_list);
	free(w->joined_list);
	free(w);
}

static void sd_confch(cpg_handle_t handle, const struct cpg_name *group_name,
		      const struct cpg_address *member_list,
		      size_t member_list_entries,
		      const struct cpg_address *left_list,
		      size_t left_list_entries,
		      const struct cpg_address *joined_list,
		      size_t joined_list_entries)
{
	struct work_confch *w = NULL;
	int i, size;

	dprintf("confchg nodeid %x\n", member_list[0].nodeid);
	dprintf("%zd %zd %zd\n", member_list_entries, left_list_entries,
		joined_list_entries);
	for (i = 0; i < member_list_entries; i++) {
		dprintf("[%d] node_id: %d, pid: %d, reason: %d\n", i,
			member_list[i].nodeid, member_list[i].pid,
			member_list[i].reason);
	}

	w = zalloc(sizeof(*w));
	if (!w)
		return;

	size = sizeof(struct cpg_address) * member_list_entries;
	w->member_list = zalloc(size);
	if (!w->member_list)
		goto err;
	memcpy(w->member_list, member_list, size);
	w->member_list_entries = member_list_entries;

	size = sizeof(struct cpg_address) * left_list_entries;
	w->left_list = zalloc(size);
	if (!w->left_list)
		goto err;
	memcpy(w->left_list, left_list, size);
	w->left_list_entries = left_list_entries;

	size = sizeof(struct cpg_address) * joined_list_entries;
	w->joined_list = zalloc(size);
	if (!w->joined_list)
		goto err;
	memcpy(w->joined_list, joined_list, size);
	w->joined_list_entries = joined_list_entries;

	w->work.fn = __sd_confch;
	w->work.done = __sd_confch_done;
	w->work.attr = WORK_ORDERED;

	queue_work(dobj_queue, &w->work);
	return;
err:
	if (!w)
		return;

	if (w->member_list)
		free(w->member_list);
	if (w->left_list)
		free(w->left_list);
	if (w->joined_list)
		free(w->joined_list);
}

int build_node_list(struct list_head *node_list,
		    struct sheepdog_node_list_entry *entries)
{
	struct node *node;
	int nr = 0;

	list_for_each_entry(node, node_list, list) {
		if (entries)
			memcpy(entries + nr, &node->ent, sizeof(*entries));
		nr++;
	}
	if (entries)
		qsort(entries, nr, sizeof(*entries), node_cmp);

	return nr;
}

int create_cluster(int port)
{
	int fd, ret;
	cpg_handle_t cpg_handle;
	struct addrinfo hints, *res, *res0;
	char name[INET6_ADDRSTRLEN];
	struct cpg_name group = { 8, "sheepdog" };
	cpg_callbacks_t cb = { &sd_deliver, &sd_confch };
	unsigned int nodeid = 0;
	uint64_t hval;
	int i;

	ret = cpg_initialize(&cpg_handle, &cb);
	if (ret != CS_OK) {
		eprintf("Failed to initialize cpg, %d\n", ret);
		eprintf("Is corosync running?\n");
		return -1;
	}

join_retry:
	ret = cpg_join(cpg_handle, &group);
	switch (ret) {
	case CS_OK:
		break;
	case CS_ERR_TRY_AGAIN:
		dprintf("Failed to join the sheepdog group, try again\n");
		sleep(1);
		goto join_retry;
	case CS_ERR_SECURITY:
		eprintf("Permission error.\n");
		exit(1);
	default:
		eprintf("Failed to join the sheepdog group, %d\n", ret);
		exit(1);
		break;
	}

	ret = cpg_local_get(cpg_handle, &nodeid);
	if (ret != CS_OK) {
		eprintf("Failed to get the local node's identifier, %d\n", ret);
		exit(1);
	}

	sys->handle = cpg_handle;
	sys->this_nodeid = nodeid;
	sys->this_pid = getpid();

	gethostname(name, sizeof(name));

	memset(&hints, 0, sizeof(hints));

	hints.ai_socktype = SOCK_STREAM;
	ret = getaddrinfo(name, NULL, &hints, &res0);
	if (ret)
		exit(1);

	for (res = res0; res; res = res->ai_next) {
		if (res->ai_family == AF_INET) {
			struct sockaddr_in *addr;
			addr = (struct sockaddr_in *)res->ai_addr;

			if (((char *) &addr->sin_addr)[0] == 127)
				continue;

			memset(sys->this_node.addr, 0, 12);
			memcpy(sys->this_node.addr + 12, &addr->sin_addr, 4);
			break;
		} else if (res->ai_family == AF_INET6) {
			struct sockaddr_in6 *addr;
			uint8_t localhost[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
						  0, 0, 0, 0, 0, 0, 0, 1 };

			addr = (struct sockaddr_in6 *)res->ai_addr;

			if (memcmp(&addr->sin6_addr, localhost, 16) == 0)
				continue;

			memcpy(sys->this_node.addr, &addr->sin6_addr, 16);
			break;
		} else
			dprintf("unknown address family\n");
	}

	if (res == NULL) {
		eprintf("failed to get address info\n");
		return -1;
	}

	freeaddrinfo(res0);

	sys->this_node.port = port;

	hval = fnv_64a_buf(&sys->this_node.port, sizeof(sys->this_node.port),
			   FNV1A_64_INIT);
	for (i = ARRAY_SIZE(sys->this_node.addr) - 1; i >= 0; i--)
		hval = fnv_64a_buf(&sys->this_node.addr[i], 1, hval);

	sys->this_node.id = hval;

	sys->synchronized = 0;
	INIT_LIST_HEAD(&sys->node_list);
	INIT_LIST_HEAD(&sys->vm_list);
	INIT_LIST_HEAD(&sys->pending_list);
	cpg_context_set(cpg_handle, sys);

	cpg_fd_get(cpg_handle, &fd);
	register_event(fd, group_handler, NULL);
	return 0;
}
