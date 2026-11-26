// SPDX-License-Identifier: GPL-2.0
/* Native XGF frame estimator for the 6.18 scheduler. */
#include <linux/interrupt.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/sched/clock.h>
#include <linux/seqlock.h>
#include <linux/smp.h>
#include <linux/sort.h>
#include <trace/events/sched.h>

#include "xgf_estimator.h"

#define XGF_SLOTS 512
#define XGF_BATCH 8
#define XGF_MAX_TASKS 100
#define XGF_MAX_EDGES 256
#define XGF_STREAMS 32
#define XGF_STREAM_GAP_NS (50ULL * NSEC_PER_MSEC)

/* For a switch, peer is zero (runnable) or -1 (blocked). A positive peer
 * describes a waking edge from tid to peer during the current CPU slice.
 */
struct xgf_sample {
	u64 begin, end, exec;
	int tid, peer;
};

struct xgf_ring {
	seqcount_t seq;
	u64 head, since, exec_base;
	bool exec_valid;
	int running;
	struct xgf_sample samples[XGF_SLOTS];
};

static DEFINE_PER_CPU(struct xgf_ring, rings);
static DEFINE_MUTEX(capture_lock);
static u64 capture_start;

static void append_sample(struct xgf_ring *r, int tid, int peer, u64 ts, u64 exec)
{
	struct xgf_sample *s = &r->samples[r->head++ % XGF_SLOTS];

	s->begin = r->since;
	s->end = ts;
	s->exec = exec;
	s->tid = tid;
	s->peer = peer;
}

static void on_switch(void *data, bool preempt, struct task_struct *prev,
		      struct task_struct *next, unsigned int prev_state)
{
	struct xgf_ring *r = this_cpu_ptr(&rings);
	unsigned long flags;
	u64 ts, exec = U64_MAX;

	local_irq_save(flags);
	ts = local_clock();
	write_seqcount_begin(&r->seq);
	/* The outgoing task has been accounted under its rq lock. */
	if (r->exec_valid && r->running == prev->pid)
		exec = prev->se.sum_exec_runtime - r->exec_base;
	append_sample(r, prev->pid, !preempt && prev_state ? -1 : 0, ts, exec);
	r->exec_base = next->se.sum_exec_runtime;
	r->exec_valid = true;
	r->since = ts;
	r->running = next->pid;
	write_seqcount_end(&r->seq);
	local_irq_restore(flags);
}

static void on_waking(void *data, struct task_struct *p)
{
	struct xgf_ring *r;
	unsigned long flags;

	/* An interrupt wake is not a dependency on the interrupted task. */
	if (!in_task() || current == p || current->tgid != p->tgid)
		return;
	r = this_cpu_ptr(&rings);
	local_irq_save(flags);
	write_seqcount_begin(&r->seq);
	append_sample(r, current->pid, p->pid, local_clock(), U64_MAX);
	write_seqcount_end(&r->seq);
	local_irq_restore(flags);
}

/* Also cover a render task already on CPU which never switches in a frame. */
static void seed_cpu(void *unused)
{
	struct xgf_ring *r = this_cpu_ptr(&rings);
	unsigned long flags;

	local_irq_save(flags);
	if (!r->since) {
		write_seqcount_begin(&r->seq);
		r->since = local_clock();
		r->running = current->pid;
		write_seqcount_end(&r->seq);
	}
	local_irq_restore(flags);
}

struct xgf_stream {
	u64 bufid, end, ema;
	int pid, tgid, count;
	int tids[XGF_MAX_TASKS];
	u8 age[XGF_MAX_TASKS];
};

static struct xgf_stream streams[XGF_STREAMS];

void xgf_cus_set_boost(bool enable)
{
	int cpu, ret;

	mutex_lock(&capture_lock);
	if (enable == !!capture_start)
		goto out;
	if (!enable) {
		unregister_trace_sched_waking(on_waking, NULL);
		unregister_trace_sched_switch(on_switch, NULL);
		tracepoint_synchronize_unregister();
		capture_start = 0;
		memset(streams, 0, sizeof(streams));
		goto out;
	}
	for_each_possible_cpu(cpu) {
		struct xgf_ring *r = per_cpu_ptr(&rings, cpu);

		seqcount_init(&r->seq);
		r->since = 0;
		r->exec_valid = false;
		r->running = 0;
		r->head = 0;
	}
	ret = register_trace_sched_switch(on_switch, NULL);
	if (ret)
		goto failed;
	ret = register_trace_sched_waking(on_waking, NULL);
	if (ret) {
		unregister_trace_sched_switch(on_switch, NULL);
		tracepoint_synchronize_unregister();
		goto failed;
	}
	on_each_cpu(seed_cpu, NULL, 1);
	capture_start = local_clock();
	goto out;
failed:
	pr_warn_ratelimited("fpsgo: XGF scheduler capture registration failed (%d)\n", ret);
out:
	mutex_unlock(&capture_lock);
}

void xgf_cus_delete_render_info(int pid, unsigned long long bufid)
{
	int i;

	mutex_lock(&capture_lock);
	for (i = 0; i < XGF_STREAMS; i++) {
		if (streams[i].pid == pid && streams[i].bufid == bufid) {
			streams[i].pid = 0;
			break;
		}
	}
	mutex_unlock(&capture_lock);
}

struct xgf_frame {
	u64 start, end, ds, de, es, ee;
	int pid, edges, targets;
	u64 deq, enq;
	struct xgf_sample edge[XGF_MAX_EDGES];
	int target[XGF_MAX_TASKS]; /* Render first, then explicit SPID/MAGT. */
	u64 runtime[XGF_MAX_TASKS];
	u8 age[XGF_MAX_TASKS];
};

/* Protected by capture_lock; no frame allocation or large kernel stack. */
static struct xgf_frame frame;

static u64 overlap(u64 start, u64 end, u64 a, u64 b)
{
	a = max(start, a);
	b = min(end, b);
	return b > a ? b - a : 0;
}

static int add_tid(int *list, int *n, int cap, int tid)
{
	int i;

	if (tid <= 0)
		return -1;
	for (i = 0; i < *n; i++)
		if (list[i] == tid)
			return i;
	if (i == cap)
		return -1;
	list[(*n)++] = tid;
	return i;
}

/* Complete slices have exact scheduler runtime. Prorate a slice that
 * straddles a frame or queue boundary. An open slice has only residency.
 */
static u64 slice_exec(const struct xgf_sample *s, u64 duration)
{
	u64 span = s->end - s->begin;

	if (!duration || s->exec >= span)
		return duration;
	if (duration == span)
		return s->exec;
	return mul_u64_u64_div_u64(duration, s->exec, span);
}

static int account(const struct xgf_sample *s)
{
	u64 a = max(s->begin, frame.start);
	u64 b = min(s->end, frame.end);
	int i;

	if (s->end >= frame.start && s->end <= frame.end &&
	    (s->peer == frame.pid || (s->peer < 0 && s->tid == frame.pid))) {
		if (frame.edges == XGF_MAX_EDGES)
			return -EOVERFLOW;
		frame.edge[frame.edges++] = *s;
	}
	if (s->peer > 0 || b <= a || !s->tid)
		return 0;
	for (i = 0; i < frame.targets; i++) {
		if (s->tid != frame.target[i])
			continue;
		frame.runtime[i] += slice_exec(s, b - a);
		if (!i) {
			frame.deq += slice_exec(s, overlap(a, b, frame.ds, frame.de));
			frame.enq += slice_exec(s, overlap(a, b, frame.es, frame.ee));
		}
		break;
	}
	return 0;
}

/* Snapshot open execution and walk back only to the first event before
 * the frame. Copy small batches under seqcount; readers never stall writers.
 * A wrapped slot must not be interpreted as part of the requested frame.
 */
static int scan_cpu(struct xgf_ring *r)
{
	struct xgf_sample batch[XGF_BATCH], open;
	u64 pos, head, limit, first;
	unsigned int seq;
	int i, n, ret;

	do {
		seq = read_seqcount_begin(&r->seq);
		pos = r->head;
		open = (struct xgf_sample){ .begin = r->since, .end = frame.end,
			.exec = U64_MAX, .tid = r->running };
	} while (read_seqcount_retry(&r->seq, seq));
	ret = account(&open);
	if (ret)
		return ret;
	limit = pos > XGF_SLOTS ? pos - XGF_SLOTS : 0;
	while (pos > limit) {
		n = min_t(u64, XGF_BATCH, pos - limit);
		first = pos - n;
		do {
			seq = read_seqcount_begin(&r->seq);
			head = r->head;
			for (i = 0; i < n; i++)
				batch[i] = r->samples[(pos - 1 - i) % XGF_SLOTS];
		} while (read_seqcount_retry(&r->seq, seq));
		if (head - first > XGF_SLOTS)
			return -EOVERFLOW;
		for (i = 0; i < n; i++) {
			if (batch[i].end < frame.start)
				return 0;
			ret = account(&batch[i]);
			if (ret)
				return ret;
		}
		pos = first;
	}
	return limit ? -EOVERFLOW : 0;
}

static int edge_cmp(const void *a, const void *b)
{
	const struct xgf_sample *x = a, *y = b;

	if (x->end != y->end)
		return x->end < y->end ? -1 : 1;
	/* A sleep at the same timestamp precedes its wake. */
	return (x->peer > 0) - (y->peer > 0);
}

static struct xgf_stream *get_stream(int pid, int tgid, u64 bufid)
{
	struct xgf_stream *s, *oldest = &streams[0];
	int i;

	for (i = 0; i < XGF_STREAMS; i++) {
		s = &streams[i];
		if (s->pid == pid && s->tgid == tgid && s->bufid == bufid) {
			/* Explicit UX frame windows need not be contiguous. */
			if (s->end && s->end <= frame.start &&
			    frame.start - s->end <= XGF_STREAM_GAP_NS)
				return s;
			oldest = s;
			break;
		}
		if (!s->pid || (oldest->pid && s->end < oldest->end))
			oldest = s;
	}
	*oldest = (struct xgf_stream){.pid = pid, .tgid = tgid, .bufid = bufid};
	return oldest;
}

/* Keep observed dependencies for at most the caller's frame horizon. */
static void update_history(struct xgf_stream *s, int horizon, int *dep, int *n, int cap)
{
	int i, count = *n;

	for (i = 0; i < s->count; i++) {
		int j, prev = *n;

		if (s->age[i] + 1 >= horizon)
			continue;
		j = add_tid(dep, n, cap, s->tids[i]);
		if (j == prev)
			frame.age[j] = s->age[i] + 1;
	}
	for (i = 0; i < *n; i++) {
		s->tids[i] = dep[i];
		s->age[i] = i < count ? 0 : frame.age[i];
	}
	s->count = *n;
}

int xgf_cus_est_runtime(int pid, unsigned long long bufid, int tgid, int spid,
	int max_paths, int max_tasks, int ema2, int dep_frames, int dividend, int extra_sub,
	int *raw_dep, int *raw_count, int raw_cap, int *dep, int *dep_count, int dep_cap,
	int *magt, int magt_count, unsigned long long *raw, unsigned long long *ema,
	unsigned long long *deq, unsigned long long *enq,
	unsigned long long deq_start, unsigned long long deq_end,
	unsigned long long enq_start, unsigned long long enq_end,
	unsigned long long start, unsigned long long end)
{
	struct xgf_stream *s;
	/* Require an observed block before charging its waker's CPU slice. */
	u64 blocked = 0, runtime, causal = 0;
	int cpu, i, ret, n = 0, rn = 0, paths = 0;
	int capacity = clamp(min(max_tasks, dep_cap), 1, XGF_MAX_TASKS);

	if (dep_cap <= 0 || max_tasks <= 0 || !dep || !dep_count || !raw || !ema || !deq || !enq)
		return -EINVAL;
	*dep_count = 0;
	if (raw_count)
		*raw_count = 0;
	*raw = *ema = *deq = *enq = 0;
	mutex_lock(&capture_lock);
	if (!capture_start || start < capture_start || end <= start || end > local_clock()) {
		ret = -ENODATA;
		goto out;
	}
	frame.start = start;
	frame.end = end;
	frame.ds = deq_start;
	frame.de = deq_end;
	frame.es = enq_start;
	frame.ee = enq_end;
	frame.pid = pid;
	frame.edges = frame.targets = 0;
	frame.enq = frame.deq = 0;
	memset(frame.runtime, 0, sizeof(frame.runtime));
	add_tid(frame.target, &frame.targets, capacity, pid);
	add_tid(frame.target, &frame.targets, capacity, spid);
	for (i = 0; i < magt_count && frame.targets < capacity; i++)
		add_tid(frame.target, &frame.targets, capacity, magt[i]);
	for_each_possible_cpu(cpu) {
		ret = scan_cpu(per_cpu_ptr(&rings, cpu));
		if (ret)
			goto out;
	}
	for (i = 0; i < frame.targets; i++)
		add_tid(dep, &n, capacity, frame.target[i]);
	sort(frame.edge, frame.edges, sizeof(frame.edge[0]), edge_cmp, NULL);
	for (i = 0; i < frame.edges; i++) {
		const struct xgf_sample *e = &frame.edge[i];
		u64 a, work;

		if (e->peer < 0) {
			blocked = e->end;
			continue;
		}
		if (!blocked)
			continue;
		a = max(blocked, e->begin);
		blocked = 0;
		if (a >= e->end || paths >= max_paths)
			continue;
		work = e->end - a;
		if (extra_sub)
			work -= overlap(a, e->end, deq_start, deq_end) +
				overlap(a, e->end, enq_start, enq_end);
		causal += work;
		add_tid(dep, &n, capacity, e->tid);
		if (raw_dep && raw_count && raw_cap - rn >= 3) {
			raw_dep[rn++] = -100;
			raw_dep[rn++] = pid;
			raw_dep[rn++] = e->tid;
		}
		paths++;
	}
	runtime = frame.runtime[0];
	if (extra_sub)
		runtime -= min(runtime, frame.deq + frame.enq);
	runtime += causal;
	/* Explicit workers can execute in parallel. Use the longest observed
	 * execution demand, rather than charging overlapping full CPU times.
	 */
	for (i = 1; i < frame.targets; i++)
		runtime = max(runtime, frame.runtime[i]);
	runtime = min(runtime, end - start);
	s = get_stream(pid, tgid, bufid);
	dividend = clamp(dividend, 1, 9);
	/* EMA1 is the supported filter. The vendor EMA2 model is unavailable. */
	s->ema = s->end ? div64_u64(runtime * dividend + s->ema * (10 - dividend), 10) : runtime;
	s->end = end;
	update_history(s, clamp(dep_frames, 1, 20), dep, &n, capacity);
	*dep_count = n;
	if (raw_count)
		*raw_count = rn;
	*raw = runtime;
	*ema = s->ema;
	*deq = frame.deq;
	*enq = frame.enq;
	ret = 1; /* XGFF tests explicitly for 1 on successful estimation. */
out:
	mutex_unlock(&capture_lock);
	return ret;
}
