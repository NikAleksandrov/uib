// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <poll.h>

#include "uib/uib.h"

static int failed;

static void check(const char *name, int condition)
{
	printf("%-32s %s\n", name, condition ? "OK" : "FAIL");
	if (!condition)
		failed = 1;
}

static int channel_ready(struct uib_comp_channel *channel, int timeout)
{
	struct pollfd poll_fd;

	memset(&poll_fd, 0, sizeof(poll_fd));
	poll_fd.fd = uib_comp_channel_fd(channel);
	poll_fd.events = POLLIN;
	return poll(&poll_fd, 1, timeout) == 1 && poll_fd.revents & POLLIN;
}

int main(void)
{
	struct uib_context context;
	struct uib_comp_channel *channel;
	struct uib_cq *event_cq;
	struct uib_cq *cq;
	struct uib_wc completion;
	struct uib_wc polled[3];
	void *event_context;
	int cookie;

	memset(&context, 0, sizeof(context));
	memset(&completion, 0, sizeof(completion));
	cookie = 0x1234;
	channel = uib_create_comp_channel(&context);
	cq = uib_create_cq_channel(&context, 8, channel, &cookie);
	check("channel.create", channel && cq);
	if (!channel || !cq)
		return 1;

	check("channel.pollable_fd", uib_comp_channel_fd(channel) >= 0);
	check("notify.arm", !uib_req_notify_cq(cq, 0));
	completion.wr_id = 1;
	check("completion.first", !uib_cq_push(cq, &completion));
	completion.wr_id = 2;
	check("completion.second", !uib_cq_push(cq, &completion));
	check("notify.event_ready", channel_ready(channel, 1000));
	event_cq = NULL;
	event_context = NULL;
	check("notify.get_event",
	      !uib_get_cq_event(channel, &event_cq, &event_context));
	check("notify.event_cq", event_cq == cq);
	check("notify.event_context", event_context == &cookie);
	check("notify.one_shot", !channel_ready(channel, 0));
	uib_ack_cq_events(cq, 1);

	check("notify.rearm", !uib_req_notify_cq(cq, 1));
	completion.wr_id = 3;
	check("completion.third", !uib_cq_push(cq, &completion));
	check("notify.rearmed_event", channel_ready(channel, 1000));
	check("notify.get_rearmed",
	      !uib_get_cq_event(channel, &event_cq, &event_context));
	uib_ack_cq_events(cq, 1);
	check("completion.poll_count", uib_poll_cq(cq, 3, polled) == 3);
	check("completion.poll_order", polled[0].wr_id == 1 &&
	      polled[1].wr_id == 2 && polled[2].wr_id == 3);
	check("cq.destroy", !uib_destroy_cq(cq));
	check("channel.destroy", !uib_destroy_comp_channel(channel));

	printf("RESULT %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
