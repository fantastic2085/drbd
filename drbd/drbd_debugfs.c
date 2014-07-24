#define pr_fmt(fmt) "drbd debugfs: " fmt
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/jiffies.h>
#include <linux/list.h>

#include "drbd_int.h"
#include "drbd_req.h"
#include "drbd_debugfs.h"


/**********************************************************************
 * Whenever you change the file format, remember to bump the version. *
 **********************************************************************/

static struct dentry *drbd_debugfs_root;
static struct dentry *drbd_debugfs_version;
static struct dentry *drbd_debugfs_resources;
static struct dentry *drbd_debugfs_minors;

static void seq_print_age_or_dash(struct seq_file *m, bool valid, unsigned long dt)
{
	if (valid)
		seq_printf(m, "\t%d", jiffies_to_msecs(dt));
	else
		seq_printf(m, "\t-");
}

static void __seq_print_rq_state_bit(struct seq_file *m,
	bool is_set, char *sep, const char *set_name, const char *unset_name)
{
	if (is_set && set_name) {
		seq_putc(m, *sep);
		seq_puts(m, set_name);
		*sep = '|';
	} else if (!is_set && unset_name) {
		seq_putc(m, *sep);
		seq_puts(m, unset_name);
		*sep = '|';
	}
}

static void seq_print_rq_state_bit(struct seq_file *m,
	bool is_set, char *sep, const char *set_name)
{
	__seq_print_rq_state_bit(m, is_set, sep, set_name, NULL);
}

/* pretty print enum drbd_req_state_bits req->rq_state */
static void seq_print_request_state(struct seq_file *m, struct drbd_request *req)
{
	struct drbd_device *device = req->device;
	struct drbd_peer_device *peer_device;
	unsigned int s = req->rq_state[0];
	char sep = ' ';
	seq_printf(m, "\t0x%08x", s);
	seq_printf(m, "\tmaster: %s", req->master_bio ? "pending" : "completed");

	/* RQ_WRITE ignored, already reported */
	seq_puts(m, "\tlocal:");
	seq_print_rq_state_bit(m, s & RQ_IN_ACT_LOG, &sep, "in-AL");
	seq_print_rq_state_bit(m, s & RQ_POSTPONED, &sep, "postponed");
	seq_print_rq_state_bit(m, s & RQ_COMPLETION_SUSP, &sep, "suspended");
	sep = ' ';
	seq_print_rq_state_bit(m, s & RQ_LOCAL_PENDING, &sep, "pending");
	seq_print_rq_state_bit(m, s & RQ_LOCAL_COMPLETED, &sep, "completed");
	seq_print_rq_state_bit(m, s & RQ_LOCAL_ABORTED, &sep, "aborted");
	seq_print_rq_state_bit(m, s & RQ_LOCAL_OK, &sep, "ok");
	if (sep == ' ')
		seq_puts(m, " -");

	for_each_peer_device(peer_device, device) {
		s = req->rq_state[1 + peer_device->node_id];
		seq_printf(m, "\tnet[%d]:", peer_device->node_id);
		sep = ' ';
		seq_print_rq_state_bit(m, s & RQ_NET_PENDING, &sep, "pending");
		seq_print_rq_state_bit(m, s & RQ_NET_QUEUED, &sep, "queued");
		seq_print_rq_state_bit(m, s & RQ_NET_SENT, &sep, "sent");
		seq_print_rq_state_bit(m, s & RQ_NET_DONE, &sep, "done");
		seq_print_rq_state_bit(m, s & RQ_NET_SIS, &sep, "sis");
		seq_print_rq_state_bit(m, s & RQ_NET_OK, &sep, "ok");
		if (sep == ' ')
			seq_puts(m, " -");

		seq_printf(m, " :");
		sep = ' ';
		seq_print_rq_state_bit(m, s & RQ_EXP_RECEIVE_ACK, &sep, "B");
		seq_print_rq_state_bit(m, s & RQ_EXP_WRITE_ACK, &sep, "C");
		seq_print_rq_state_bit(m, s & RQ_EXP_BARR_ACK, &sep, "barr");
		if (sep == ' ')
			seq_puts(m, " -");
	}
	seq_printf(m, "\n");
}

#define memberat(PTR, TYPE, OFFSET) (*(TYPE *)((char *)PTR + OFFSET))

static void print_one_age_or_dash(struct seq_file *m, struct drbd_request *req,
				  unsigned int set_mask, unsigned int clear_mask,
				  unsigned long now, size_t offset)
{
	struct drbd_device *device = req->device;
	struct drbd_peer_device *peer_device;

	for_each_peer_device(peer_device, device) {
		unsigned int s = req->rq_state[1 + peer_device->node_id];

		if (s & set_mask && !(s & clear_mask)) {
			unsigned long jif = now - memberat(req, unsigned long, offset);
			seq_printf(m, "\t[%d]%d", peer_device->node_id, jiffies_to_msecs(jif));
			return;
		}
	}
	seq_printf(m, "\t-");
}

static void seq_print_one_request(struct seq_file *m, struct drbd_request *req, unsigned long now)
{
	/* change anything here, fixup header below! */
	unsigned int s = req->rq_state[0];

#define RQ_HDR_1 "epoch\tsector\tsize\trw"
	seq_printf(m, "0x%x\t%llu\t%u\t%s",
		req->epoch,
		(unsigned long long)req->i.sector, req->i.size >> 9,
		(s & RQ_WRITE) ? "W" : "R");

#define RQ_HDR_2 "\tstart\tin AL\tsubmit"
	seq_printf(m, "\t%d", jiffies_to_msecs(now - req->start_jif));
	seq_print_age_or_dash(m, s & RQ_IN_ACT_LOG, now - req->in_actlog_jif);
	seq_print_age_or_dash(m, s & RQ_LOCAL_PENDING, now - req->pre_submit_jif);

#define RQ_HDR_3 "\tsent\tacked\tdone"
	print_one_age_or_dash(m, req, RQ_NET_SENT, 0, now, offsetof(typeof(*req), pre_send_jif));
	print_one_age_or_dash(m, req, RQ_NET_SENT, RQ_NET_PENDING, now, offsetof(typeof(*req), acked_jif));
	print_one_age_or_dash(m, req, RQ_NET_DONE, 0, now, offsetof(typeof(*req), net_done_jif));

#define RQ_HDR_4 "\tstate\n"
	seq_print_request_state(m, req);
}
#define RQ_HDR RQ_HDR_1 RQ_HDR_2 RQ_HDR_3 RQ_HDR_4

static void seq_print_minor_vnr_req(struct seq_file *m, struct drbd_request *req, unsigned long now)
{
	seq_printf(m, "%u\t%u\t", req->device->minor, req->device->vnr);
	seq_print_one_request(m, req, now);
}

static void seq_print_resource_pending_meta_io(struct seq_file *m, struct drbd_resource *resource, unsigned long now)
{
	struct drbd_device *device;
	unsigned int i;

	seq_puts(m, "minor\tvnr\tstart\tsubmit\tintent\n");
	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, i) {
		struct drbd_md_io tmp;
		/* In theory this is racy,
		 * in the sense that there could have been a
		 * drbd_md_put_buffer(); drbd_md_get_buffer();
		 * between accessing these members here.  */
		tmp = device->md_io;
		if (atomic_read(&tmp.in_use)) {
			seq_printf(m, "%u\t%u\t%d\t",
				device->minor, device->vnr,
				jiffies_to_msecs(now - tmp.start_jif));
			if (time_before(tmp.submit_jif, tmp.start_jif))
				seq_puts(m, "-\t");
			else
				seq_printf(m, "%d\t", jiffies_to_msecs(now - tmp.submit_jif));
			seq_printf(m, "%s\n", tmp.current_use);
		}
	}
	rcu_read_unlock();
}

static void seq_print_waiting_for_AL(struct seq_file *m, struct drbd_resource *resource, unsigned long now)
{
	struct drbd_device *device;
	unsigned int i;

	seq_puts(m, "minor\tvnr\tage\t#waiting\n");
	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, i) {
		unsigned long jif;
		struct drbd_request *req;
		int n = atomic_read(&device->ap_actlog_cnt);
		if (n) {
			spin_lock_irq(&device->resource->req_lock);
			req = list_first_entry_or_null(&device->pending_master_completion[1],
				struct drbd_request, req_pending_master_completion);
			/* if the oldest request does not wait for the activity log
			 * it is not interesting for us here */
			if (req && !(req->rq_state[0] & RQ_IN_ACT_LOG))
				jif = req->start_jif;
			else
				req = NULL;
			spin_unlock_irq(&device->resource->req_lock);
		}
		if (n) {
			seq_printf(m, "%u\t%u\t", device->minor, device->vnr);
			if (req)
				seq_printf(m, "%u\t", jiffies_to_msecs(now - jif));
			else
				seq_puts(m, "-\t");
			seq_printf(m, "%u\n", n);
		}
	}
	rcu_read_unlock();
}

static void seq_print_device_bitmap_io(struct seq_file *m, struct drbd_device *device, unsigned long now)
{
	struct drbd_bm_aio_ctx *ctx;
	unsigned long start_jif;
	unsigned int in_flight;
	unsigned int flags;
	spin_lock_irq(&device->resource->req_lock);
	ctx = list_first_entry_or_null(&device->pending_bitmap_io, struct drbd_bm_aio_ctx, list);
	if (ctx && ctx->done)
		ctx = NULL;
	if (ctx) {
		start_jif = ctx->start_jif;
		in_flight = atomic_read(&ctx->in_flight);
		flags = ctx->flags;
	}
	spin_unlock_irq(&device->resource->req_lock);
	if (ctx) {
		seq_printf(m, "%u\t%u\t%c\t%u\t%u\n",
			device->minor, device->vnr,
			(flags & BM_AIO_READ) ? 'R' : 'W',
			jiffies_to_msecs(now - start_jif),
			in_flight);
	}
}

static void seq_print_resource_pending_bitmap_io(struct seq_file *m, struct drbd_resource *resource, unsigned long now)
{
	struct drbd_device *device;
	unsigned int i;

	seq_puts(m, "minor\tvnr\trw\tage\t#in-flight\n");
	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, i) {
		seq_print_device_bitmap_io(m, device, now);
	}
	rcu_read_unlock();
}

/* pretty print enum peer_req->flags */
static void seq_print_peer_request_flags(struct seq_file *m, struct drbd_peer_request *peer_req)
{
	unsigned long f = peer_req->flags;
	char sep = ' ';

	__seq_print_rq_state_bit(m, f & EE_SUBMITTED, &sep, "submitted", "preparing");
	__seq_print_rq_state_bit(m, f & EE_APPLICATION, &sep, "application", "internal");
	seq_print_rq_state_bit(m, f & EE_IS_BARRIER, &sep, "barr");
	seq_print_rq_state_bit(m, f & EE_SEND_WRITE_ACK, &sep, "C");
	seq_print_rq_state_bit(m, f & EE_MAY_SET_IN_SYNC, &sep, "set-in-sync");

	if (f & EE_IS_TRIM) {
		seq_putc(m, sep);
		sep = '|';
		if (f & EE_IS_TRIM_USE_ZEROOUT)
			seq_puts(m, "zero-out");
		else
			seq_puts(m, "trim");
	}
	seq_putc(m, '\n');
}

static void seq_print_peer_request(struct seq_file *m,
	struct drbd_device *device, struct list_head *lh,
	unsigned long now)
{
	bool reported_preparing = false;
	struct drbd_peer_request *peer_req;
	list_for_each_entry(peer_req, lh, w.list) {
		if (reported_preparing && !(peer_req->flags & EE_SUBMITTED))
			continue;

		if (device)
			seq_printf(m, "%u\t%u\t", device->minor, device->vnr);

		seq_printf(m, "%llu\t%u\t%c\t%u\t",
			(unsigned long long)peer_req->i.sector, peer_req->i.size >> 9,
			(peer_req->flags & EE_WRITE) ? 'W' : 'R',
			jiffies_to_msecs(now - peer_req->submit_jif));
		seq_print_peer_request_flags(m, peer_req);
		if (peer_req->flags & EE_SUBMITTED)
			break;
		else
			reported_preparing = true;
	}
}

static void seq_print_device_peer_requests(struct seq_file *m,
	struct drbd_device *device, unsigned long now)
{
	seq_puts(m, "minor\tvnr\tsector\tsize\trw\tage\tflags\n");
	spin_lock_irq(&device->resource->req_lock);
	seq_print_peer_request(m, device, &device->active_ee, now);
	seq_print_peer_request(m, device, &device->read_ee, now);
	seq_print_peer_request(m, device, &device->sync_ee, now);
	spin_unlock_irq(&device->resource->req_lock);
	if (test_bit(FLUSH_PENDING, &device->flags)) {
		seq_printf(m, "%u\t%u\t-\t-\tF\t%u\tflush\n",
			device->minor, device->vnr,
			jiffies_to_msecs(now - device->flush_jif));
	}
}

static void seq_print_resource_pending_peer_requests(struct seq_file *m,
	struct drbd_resource *resource, unsigned long now)
{
	struct drbd_device *device;
	unsigned int i;

	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, i) {
		seq_print_device_peer_requests(m, device, now);
	}
	rcu_read_unlock();
}

static void seq_print_resource_transfer_log_summary(struct seq_file *m,
	struct drbd_resource *resource,
	struct drbd_connection *connection,
	unsigned long now)
{
	struct drbd_request *req;
	unsigned int count = 0;
	unsigned int show_state = 0;

	seq_puts(m, "n\tdevice\tvnr\t" RQ_HDR);
	spin_lock_irq(&resource->req_lock);
	list_for_each_entry(req, &resource->transfer_log, tl_requests) {
		struct drbd_device *device = req->device;
		struct drbd_peer_device *peer_device;
		unsigned int tmp = 0;
		unsigned int s;
		++count;

		/* don't disable irq "forever" */
		if (!(count & 0x1ff)) {
			struct drbd_request *req_next;
			kref_get(&req->kref);
			spin_unlock_irq(&resource->req_lock);
			cond_resched();
			spin_lock_irq(&resource->req_lock);
			req_next = list_next_entry(req, tl_requests);
			if (kref_put(&req->kref, drbd_req_destroy))
				req = req_next;
			if (&req->tl_requests == &resource->transfer_log)
				break;
		}

		s = req->rq_state[0];

		/* This is meant to summarize timing issues, to be able to tell
		 * local disk problems from network problems.
		 * Skip requests, if we have shown an even older request with
		 * similar aspects already.  */
		if (req->master_bio == NULL)
			tmp |= 1;
		if ((s & RQ_LOCAL_MASK) && (s & RQ_LOCAL_PENDING))
			tmp |= 2;

		for_each_peer_device(peer_device, device) {
			s = req->rq_state[1 + peer_device->node_id];
			if (s & RQ_NET_MASK) {
				if (!(s & RQ_NET_SENT))
					tmp |= 4;
				if (s & RQ_NET_PENDING)
					tmp |= 8;
				if (!(s & RQ_NET_DONE))
					tmp |= 16;
			}
		}
		if ((tmp & show_state) == tmp)
			continue;
		show_state |= tmp;
		seq_printf(m, "%u\t", count);
		seq_print_minor_vnr_req(m, req, now);
		if (show_state == 0x1f)
			break;
	}
	spin_unlock_irq(&resource->req_lock);
}

/* TODO: transfer_log and friends should be moved to resource */
static int in_flight_summary_show(struct seq_file *m, void *pos)
{
	struct drbd_resource *resource = m->private;
	struct drbd_connection *connection;
	unsigned long jif = jiffies;

	connection = first_connection(resource);
	/* This does not happen, actually.
	 * But be robust and prepare for future code changes. */
	if (!connection || !kref_get_unless_zero(&connection->kref))
		return -ESTALE;

	/* BUMP me if you change the file format/content/presentation */
	seq_printf(m, "v: %u\n\n", 0);

	seq_puts(m, "oldest bitmap IO\n");
	seq_print_resource_pending_bitmap_io(m, resource, jif);
	seq_putc(m, '\n');

	seq_puts(m, "meta data IO\n");
	seq_print_resource_pending_meta_io(m, resource, jif);
	seq_putc(m, '\n');

	seq_puts(m, "socket buffer stats\n");
	/* for each connection ... once we have more than one */
	rcu_read_lock();
	if (connection->data.socket) {
		/* open coded SIOCINQ, the "relevant" part */
		struct tcp_sock *tp = tcp_sk(connection->data.socket->sk);
		int answ = tp->rcv_nxt - tp->copied_seq;
		seq_printf(m, "unread receive buffer: %u Byte\n", answ);
		/* open coded SIOCOUTQ, the "relevant" part */
		answ = tp->write_seq - tp->snd_una;
		seq_printf(m, "unacked send buffer: %u Byte\n", answ);
	}
	rcu_read_unlock();
	seq_putc(m, '\n');

	seq_puts(m, "oldest peer requests\n");
	seq_print_resource_pending_peer_requests(m, resource, jif);
	seq_putc(m, '\n');

	seq_puts(m, "application requests waiting for activity log\n");
	seq_print_waiting_for_AL(m, resource, jif);
	seq_putc(m, '\n');

	seq_puts(m, "oldest application requests\n");
	seq_print_resource_transfer_log_summary(m, resource, connection, jif);
	seq_putc(m, '\n');

	jif = jiffies - jif;
	if (jif)
		seq_printf(m, "generated in %d ms\n", jiffies_to_msecs(jif));
	kref_put(&connection->kref, drbd_destroy_connection);
	return 0;
}

/* simple_positive(file->f_dentry) respectively debugfs_positive(),
 * but neither is "reachable" from here.
 * So we have our own inline version of it above.  :-( */
static inline int debugfs_positive(struct dentry *dentry)
{
        return dentry->d_inode && !d_unhashed(dentry);
}

/* make sure at *open* time that the respective object won't go away. */
static int drbd_single_open(struct file *file, int (*show)(struct seq_file *, void *),
		                void *data, struct kref *kref,
				void (*release)(struct kref *))
{
	struct dentry *parent;
	int ret = -ESTALE;

	/* Are we still linked,
	 * or has debugfs_remove() already been called? */
	parent = file->f_dentry->d_parent;
	/* not sure if this can happen: */
	if (!parent || !parent->d_inode)
		goto out;
	/* serialize with d_delete() */
	mutex_lock(&parent->d_inode->i_mutex);
	/* Make sure the object is still alive */
	if (debugfs_positive(file->f_dentry)
	&& kref_get_unless_zero(kref))
		ret = 0;
	mutex_unlock(&parent->d_inode->i_mutex);
	if (!ret) {
		ret = single_open(file, show, data);
		if (ret)
			kref_put(kref, release);
	}
out:
	return ret;
}

static int in_flight_summary_open(struct inode *inode, struct file *file)
{
	struct drbd_resource *resource = inode->i_private;
	return drbd_single_open(file, in_flight_summary_show, resource,
				&resource->kref, drbd_destroy_resource);
}

static int in_flight_summary_release(struct inode *inode, struct file *file)
{
	struct drbd_resource *resource = inode->i_private;
	kref_put(&resource->kref, drbd_destroy_resource);
	return single_release(inode, file);
}

static const struct file_operations in_flight_summary_fops = {
	.owner		= THIS_MODULE,
	.open		= in_flight_summary_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= in_flight_summary_release,
};

void drbd_debugfs_resource_add(struct drbd_resource *resource)
{
	struct dentry *dentry;
	if (!drbd_debugfs_resources)
		return;

	dentry = debugfs_create_dir(resource->name, drbd_debugfs_resources);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	resource->debugfs_res = dentry;

	dentry = debugfs_create_dir("volumes", resource->debugfs_res);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	resource->debugfs_res_volumes = dentry;

	dentry = debugfs_create_dir("connections", resource->debugfs_res);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	resource->debugfs_res_connections = dentry;

	dentry = debugfs_create_file("in_flight_summary", S_IRUSR|S_IRGRP,
			resource->debugfs_res, resource,
			&in_flight_summary_fops);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	resource->debugfs_res_in_flight_summary = dentry;
	return;

fail:
	drbd_debugfs_resource_cleanup(resource);
	drbd_err(resource, "failed to create debugfs dentry\n");
}

static void drbd_debugfs_remove(struct dentry **dp)
{
	debugfs_remove(*dp);
	*dp = NULL;
}

void drbd_debugfs_resource_cleanup(struct drbd_resource *resource)
{
	/* Older kernels have a broken implementation of
	 * debugfs_remove_recursive (prior to upstream commit 776164c1f)
	 * That unfortunately includes a number of "enterprise" kernels.
	 * Even older kernels do not even have the _recursive() helper at all.
	 * For now, remember all debugfs nodes we created,
	 * and call debugfs_remove on all of them separately.
	 */
	/* it is ok to call debugfs_remove(NULL) */
	drbd_debugfs_remove(&resource->debugfs_res_in_flight_summary);
	drbd_debugfs_remove(&resource->debugfs_res_connections);
	drbd_debugfs_remove(&resource->debugfs_res_volumes);
	drbd_debugfs_remove(&resource->debugfs_res);
}

static void seq_print_one_timing_detail(struct seq_file *m,
	const struct drbd_thread_timing_details *tdp,
	unsigned long now)
{
	struct drbd_thread_timing_details td;
	/* No locking...
	 * use temporary assignment to get at consistent data. */
	do {
		td = *tdp;
	} while (td.cb_nr != tdp->cb_nr);
	if (!td.cb_addr)
		return;
	seq_printf(m, "%u\t%d\t%s:%u\t%ps\n",
			td.cb_nr,
			jiffies_to_msecs(now - td.start_jif),
			td.caller_fn, td.line,
			td.cb_addr);
}

static void seq_print_timing_details(struct seq_file *m,
		const char *title,
		unsigned int cb_nr, struct drbd_thread_timing_details *tdp, unsigned long now)
{
	unsigned int start_idx;
	unsigned int i;

	seq_printf(m, "%s\n", title);
	/* If not much is going on, this will result in natural ordering.
	 * If it is very busy, we will possibly skip events, or even see wrap
	 * arounds, which could only be avoided with locking.
	 */
	start_idx = cb_nr % DRBD_THREAD_DETAILS_HIST;
	for (i = start_idx; i < DRBD_THREAD_DETAILS_HIST; i++)
		seq_print_one_timing_detail(m, tdp+i, now);
	for (i = 0; i < start_idx; i++)
		seq_print_one_timing_detail(m, tdp+i, now);
}

static int callback_history_show(struct seq_file *m, void *ignored)
{
	struct drbd_connection *connection = m->private;
	struct drbd_resource *resource = connection->resource;
	unsigned long jif = jiffies;

	/* BUMP me if you change the file format/content/presentation */
	seq_printf(m, "v: %u\n\n", 0);

	seq_puts(m, "n\tage\tcallsite\tfn\n");
	seq_print_timing_details(m, "sender", connection->s_cb_nr, connection->s_timing_details, jif);
	seq_print_timing_details(m, "receiver", connection->r_cb_nr, connection->r_timing_details, jif);
	seq_print_timing_details(m, "worker", resource->w_cb_nr, resource->w_timing_details, jif);
	return 0;
}

static int callback_history_open(struct inode *inode, struct file *file)
{
	struct drbd_connection *connection = inode->i_private;
	return drbd_single_open(file, callback_history_show, connection,
				&connection->kref, drbd_destroy_connection);
}

static int callback_history_release(struct inode *inode, struct file *file)
{
	struct drbd_connection *connection = inode->i_private;
	kref_put(&connection->kref, drbd_destroy_connection);
	return single_release(inode, file);
}

static const struct file_operations connection_callback_history_fops = {
	.owner		= THIS_MODULE,
	.open		= callback_history_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= callback_history_release,
};

static int connection_oldest_requests_show(struct seq_file *m, void *ignored)
{
	struct drbd_connection *connection = m->private;
	unsigned long now = jiffies;
	struct drbd_request *r1, *r2;

	/* BUMP me if you change the file format/content/presentation */
	seq_printf(m, "v: %u\n\n", 0);

	spin_lock_irq(&connection->resource->req_lock);
	r1 = connection->todo.req_next;
	if (r1)
		seq_print_minor_vnr_req(m, r1, now);
	r2 = connection->req_ack_pending;
	if (r2 && r2 != r1) {
		r1 = r2;
		seq_print_minor_vnr_req(m, r1, now);
	}
	r2 = connection->req_not_net_done;
	if (r2 && r2 != r1)
		seq_print_minor_vnr_req(m, r2, now);
	spin_unlock_irq(&connection->resource->req_lock);
	return 0;
}

static int connection_oldest_requests_open(struct inode *inode, struct file *file)
{
	struct drbd_connection *connection = inode->i_private;
	return drbd_single_open(file, connection_oldest_requests_show, connection,
				&connection->kref, drbd_destroy_connection);
}

static int connection_oldest_requests_release(struct inode *inode, struct file *file)
{
	struct drbd_connection *connection = inode->i_private;
	kref_put(&connection->kref, drbd_destroy_connection);
	return single_release(inode, file);
}

static const struct file_operations connection_oldest_requests_fops = {
	.owner		= THIS_MODULE,
	.open		= connection_oldest_requests_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= connection_oldest_requests_release,
};

void drbd_debugfs_connection_add(struct drbd_connection *connection)
{
	struct dentry *conns_dir = connection->resource->debugfs_res_connections;
	char conn_name[SHARED_SECRET_MAX];
	struct dentry *dentry;
	if (!conns_dir)
		return;

	rcu_read_lock();
	strcpy(conn_name, rcu_dereference((connection)->net_conf)->name);
	rcu_read_unlock();

	dentry = debugfs_create_dir(conn_name, conns_dir);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	connection->debugfs_conn = dentry;

	dentry = debugfs_create_file("callback_history", S_IRUSR|S_IRGRP,
			connection->debugfs_conn, connection,
			&connection_callback_history_fops);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	connection->debugfs_conn_callback_history = dentry;

	dentry = debugfs_create_file("oldest_requests", S_IRUSR|S_IRGRP,
			connection->debugfs_conn, connection,
			&connection_oldest_requests_fops);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	connection->debugfs_conn_oldest_requests = dentry;
	return;

fail:
	drbd_debugfs_connection_cleanup(connection);
	drbd_err(connection, "failed to create debugfs dentry\n");
}

void drbd_debugfs_connection_cleanup(struct drbd_connection *connection)
{
	drbd_debugfs_remove(&connection->debugfs_conn_callback_history);
	drbd_debugfs_remove(&connection->debugfs_conn_oldest_requests);
	drbd_debugfs_remove(&connection->debugfs_conn);
}

static int device_act_log_extents_show(struct seq_file *m, void *ignored)
{
	struct drbd_device *device = m->private;

	/* BUMP me if you change the file format/content/presentation */
	seq_printf(m, "v: %u\n\n", 0);

	if (get_ldev_if_state(device, D_FAILED)) {
		lc_seq_printf_stats(m, device->act_log);
		lc_seq_dump_details(m, device->act_log, "", NULL);
		put_ldev(device);
	}
	return 0;
}

static int device_oldest_requests_show(struct seq_file *m, void *ignored)
{
	struct drbd_device *device = m->private;
	struct drbd_resource *resource = device->resource;
	unsigned long now = jiffies;
	struct drbd_request *r1, *r2;
	int i;

	/* BUMP me if you change the file format/content/presentation */
	seq_printf(m, "v: %u\n\n", 0);

	seq_puts(m, RQ_HDR);
	spin_lock_irq(&resource->req_lock);
	/* WRITE, then READ */
	for (i = 1; i >= 0; --i) {
		r1 = list_first_entry_or_null(&device->pending_master_completion[i],
			struct drbd_request, req_pending_master_completion);
		r2 = list_first_entry_or_null(&device->pending_completion[i],
			struct drbd_request, req_pending_local);
		if (r1)
			seq_print_one_request(m, r1, now);
		if (r2 && r2 != r1)
			seq_print_one_request(m, r2, now);
	}
	spin_unlock_irq(&resource->req_lock);
	return 0;
}

static int device_data_gen_id_show(struct seq_file *m, void *ignored)
{
	struct drbd_device *device = m->private;
	struct drbd_md *md;
	int bitmap_index, max_peers, i;

	if (!get_ldev_if_state(device, D_FAILED))
		return -ENODEV;

	max_peers = device->bitmap->bm_max_peers;
	md = &device->ldev->md;

	spin_lock_irq(&md->uuid_lock);
	seq_printf(m, "0x%016llX\n", drbd_current_uuid(device));

	for (bitmap_index = 0; bitmap_index < max_peers; bitmap_index++) {
		int node_id = md->peers[bitmap_index].node_id;
		if (node_id == -1)
			continue;
		seq_printf(m, " [%d]0x%016llX", node_id, md->peers[bitmap_index].bitmap_uuid);
	}
	seq_printf(m, "\n");

	for (i = 0; i < HISTORY_UUIDS; i++)
		seq_printf(m, "0x%016llX\n\n", drbd_history_uuid(device, i));
	spin_unlock_irq(&md->uuid_lock);
	put_ldev(device);
	return 0;
}

#define drbd_debugfs_device_attr(name)						\
static int device_ ## name ## _open(struct inode *inode, struct file *file)	\
{										\
	struct drbd_device *device = inode->i_private;				\
	return drbd_single_open(file, device_ ## name ## _show, device,		\
				&device->kref, drbd_destroy_device);		\
}										\
static int device_ ## name ## _release(struct inode *inode, struct file *file)	\
{										\
	struct drbd_device *device = inode->i_private;				\
	kref_put(&device->kref, drbd_destroy_device);				\
	return single_release(inode, file);					\
}										\
static const struct file_operations device_ ## name ## _fops = {		\
	.owner		= THIS_MODULE,						\
	.open		= device_ ## name ## _open,				\
	.read		= seq_read,						\
	.llseek		= seq_lseek,						\
	.release	= device_ ## name ## _release,				\
};

drbd_debugfs_device_attr(oldest_requests)
drbd_debugfs_device_attr(act_log_extents)
drbd_debugfs_device_attr(data_gen_id)

void drbd_debugfs_device_add(struct drbd_device *device)
{
	struct dentry *vols_dir = device->resource->debugfs_res_volumes;
	char minor_buf[8]; /* MINORMASK, MINORBITS == 20; */
	char vnr_buf[8];   /* volume number vnr is even 16 bit only; */
	char *slink_name = NULL;

	struct dentry *dentry;
	if (!vols_dir || !drbd_debugfs_minors)
		return;

	snprintf(vnr_buf, sizeof(vnr_buf), "%u", device->vnr);
	dentry = debugfs_create_dir(vnr_buf, vols_dir);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	device->debugfs_vol = dentry;

	snprintf(minor_buf, sizeof(minor_buf), "%u", device->minor);
	slink_name = kasprintf(GFP_KERNEL, "../resources/%s/volumes/%u",
			device->resource->name, device->vnr);
	if (!slink_name)
		goto fail;
	dentry = debugfs_create_symlink(minor_buf, drbd_debugfs_minors, slink_name);
	kfree(slink_name);
	slink_name = NULL;
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	device->debugfs_minor = dentry;

#define DCF(name)	do {					\
	dentry = debugfs_create_file(#name, S_IRUSR|S_IRGRP,	\
			device->debugfs_vol, device,		\
			&device_ ## name ## _fops);		\
	if (IS_ERR_OR_NULL(dentry))				\
		goto fail;					\
	device->debugfs_vol_ ## name = dentry;			\
	} while (0)

	DCF(oldest_requests);
	DCF(act_log_extents);
	DCF(data_gen_id);
#undef DCF
	return;

fail:
	drbd_debugfs_device_cleanup(device);
	drbd_err(device, "failed to create debugfs entries\n");
}

void drbd_debugfs_device_cleanup(struct drbd_device *device)
{
	drbd_debugfs_remove(&device->debugfs_minor);
	drbd_debugfs_remove(&device->debugfs_vol_oldest_requests);
	drbd_debugfs_remove(&device->debugfs_vol_act_log_extents);
	drbd_debugfs_remove(&device->debugfs_vol_data_gen_id);
	drbd_debugfs_remove(&device->debugfs_vol);
}

static int drbd_single_open_peer_device(struct file *file,
					int (*show)(struct seq_file *, void *),
					struct drbd_peer_device *peer_device)
{
	struct drbd_device *device = peer_device->device;
	struct drbd_connection *connection = peer_device->connection;
	bool got_connection, got_device;
	struct dentry *parent;

	parent = file->f_dentry->d_parent;
	if (!parent || !parent->d_inode)
		goto out;
	mutex_lock(&parent->d_inode->i_mutex);
	if (!debugfs_positive(file->f_dentry))
		goto out_unlock;

	got_connection = kref_get_unless_zero(&connection->kref);
	got_device = kref_get_unless_zero(&device->kref);

	if (got_connection && got_device) {
		int ret;
		mutex_unlock(&parent->d_inode->i_mutex);
		ret = single_open(file, show, peer_device);
		if (ret) {
			kref_put(&connection->kref, drbd_destroy_connection);
			kref_put(&device->kref, drbd_destroy_device);
		}
		return ret;
	}

	if (got_connection)
		kref_put(&connection->kref, drbd_destroy_connection);
	if (got_device)
		kref_put(&device->kref, drbd_destroy_device);
out_unlock:
	mutex_unlock(&parent->d_inode->i_mutex);
out:
	return -ESTALE;
}

static void resync_dump_detail(struct seq_file *m, struct lc_element *e)
{
       struct bm_extent *bme = lc_entry(e, struct bm_extent, lce);

       seq_printf(m, "%5d %s %s %s\n", bme->rs_left,
		  test_bit(BME_NO_WRITES, &bme->flags) ? "NO_WRITES" : "---------",
		  test_bit(BME_LOCKED, &bme->flags) ? "LOCKED" : "------",
		  test_bit(BME_PRIORITY, &bme->flags) ? "PRIORITY" : "--------"
		  );
}

static int peer_device_resync_extents_show(struct seq_file *m, void *ignored)
{
	struct drbd_peer_device *peer_device = m->private;
	struct drbd_device *device = peer_device->device;

	/* BUMP me if you change the file format/content/presentation */
	seq_printf(m, "v: %u\n\n", 0);

	if (get_ldev_if_state(device, D_FAILED)) {
		lc_seq_printf_stats(m, peer_device->resync_lru);
		lc_seq_dump_details(m, peer_device->resync_lru, "rs_left flags", resync_dump_detail);
		put_ldev(device);
	}
	return 0;
}

#define drbd_debugfs_peer_device_attr(name)					\
static int peer_device_ ## name ## _open(struct inode *inode, struct file *file)\
{										\
	struct drbd_peer_device *peer_device = inode->i_private;		\
	return drbd_single_open_peer_device(file,				\
					    peer_device_ ## name ## _show,	\
					    peer_device);			\
}										\
static int peer_device_ ## name ## _release(struct inode *inode, struct file *file)\
{										\
	struct drbd_peer_device *peer_device = inode->i_private;		\
	kref_put(&peer_device->connection->kref, drbd_destroy_connection);	\
	kref_put(&peer_device->device->kref, drbd_destroy_device);		\
	return single_release(inode, file);					\
}										\
static const struct file_operations peer_device_ ## name ## _fops = {		\
	.owner		= THIS_MODULE,						\
	.open		= peer_device_ ## name ## _open,			\
	.read		= seq_read,						\
	.llseek		= seq_lseek,						\
	.release	= peer_device_ ## name ## _release,			\
};

drbd_debugfs_peer_device_attr(resync_extents)

#define DCF(name)	do {						\
	dentry = debugfs_create_file(#name, S_IRUSR|S_IRGRP,		\
			peer_device->debugfs_peer_dev, peer_device,	\
			&peer_device_ ## name ## _fops);		\
	if (IS_ERR_OR_NULL(dentry))					\
		goto fail;						\
	peer_device->debugfs_peer_dev_ ## name = dentry;		\
	} while (0)


void drbd_debugfs_peer_device_add(struct drbd_peer_device *peer_device)
{
	struct dentry *conn_dir = peer_device->connection->debugfs_conn;
	struct dentry *dentry;
	char vnr_buf[8];

	if (!conn_dir)
		return;

	snprintf(vnr_buf, sizeof(vnr_buf), "%u", peer_device->device->vnr);
	dentry = debugfs_create_dir(vnr_buf, conn_dir);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	peer_device->debugfs_peer_dev = dentry;

	DCF(resync_extents);
	return;

fail:
	drbd_debugfs_peer_device_cleanup(peer_device);
	drbd_err(peer_device, "failed to create debugfs entries\n");
}

void drbd_debugfs_peer_device_cleanup(struct drbd_peer_device *peer_device)
{
	drbd_debugfs_remove(&peer_device->debugfs_peer_dev_resync_extents);
	drbd_debugfs_remove(&peer_device->debugfs_peer_dev);
}

static int drbd_version_show(struct seq_file *m, void *ignored)
{
	seq_printf(m, "# %s\n", drbd_buildtag());
	seq_printf(m, "VERSION=%s\n", REL_VERSION);
	seq_printf(m, "API_VERSION=%u\n", API_VERSION);
	seq_printf(m, "PRO_VERSION_MIN=%u\n", PRO_VERSION_MIN);
	seq_printf(m, "PRO_VERSION_MAX=%u\n", PRO_VERSION_MAX);
	return 0;
}

static int drbd_version_open(struct inode *inode, struct file *file)
{
	return single_open(file, drbd_version_show, NULL);
}

static struct file_operations drbd_version_fops = {
	.owner = THIS_MODULE,
	.open = drbd_version_open,
	.llseek = seq_lseek,
	.read = seq_read,
	.release = single_release,
};

/* not __exit, may be indirectly called
 * from the module-load-failure path as well. */
void drbd_debugfs_cleanup(void)
{
	drbd_debugfs_remove(&drbd_debugfs_resources);
	drbd_debugfs_remove(&drbd_debugfs_minors);
	drbd_debugfs_remove(&drbd_debugfs_version);
	drbd_debugfs_remove(&drbd_debugfs_root);
}

int __init drbd_debugfs_init(void)
{
	struct dentry *dentry;

	dentry = debugfs_create_dir("drbd", NULL);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	drbd_debugfs_root = dentry;

	dentry = debugfs_create_file("version", 0444, drbd_debugfs_root, NULL, &drbd_version_fops);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	drbd_debugfs_version = dentry;

	dentry = debugfs_create_dir("resources", drbd_debugfs_root);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	drbd_debugfs_resources = dentry;

	dentry = debugfs_create_dir("minors", drbd_debugfs_root);
	if (IS_ERR_OR_NULL(dentry))
		goto fail;
	drbd_debugfs_minors = dentry;
	return 0;

fail:
	drbd_debugfs_cleanup();
	if (dentry)
		return PTR_ERR(dentry);
	else
		return -EINVAL;
}