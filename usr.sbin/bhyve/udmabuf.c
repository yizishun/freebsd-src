/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Zishun Yi <zis@freebsd.org>
 *
 * udmabuf helper functions for FreeBSD via netlink
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <netlink/netlink.h>
#include <netlink/netlink_generic.h>
#include <netlink/netlink_snl.h>
#include <netlink/netlink_snl_generic.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "udmabuf.h"

struct udmabuf_dev {
	struct snl_state ss;
	uint16_t family_id;
};

struct nl_parsed_reply {
	int dmabuf_fd;
};

static const struct snl_field_parser nlf_p_empty[] = {};

#define	_OUT(_field)	offsetof(struct nl_parsed_reply, _field)
static const struct snl_attr_parser ap_reply[] = {
	{
		.type = UDMABUF_ATTR_DMABUF,
		.off = _OUT(dmabuf_fd),
		.cb = snl_attr_get_uint32,
	},
};
#undef _OUT
SNL_DECLARE_PARSER(reply_parser, struct genlmsghdr, nlf_p_empty, ap_reply);

static struct udmabuf_dev *
udmabuf_open(void)
{
	static struct udmabuf_dev *dev;
	static bool first = true;
	socklen_t optlen;
	int val;

	if (!first)
		return (dev);
	first = false;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL)
		return (NULL);

	if (!snl_init(&dev->ss, NETLINK_GENERIC)) {
		free(dev);
		dev = NULL;
		return (NULL);
	}

	val = 1;
	optlen = sizeof(val);
	if (setsockopt(dev->ss.fd, SOL_NETLINK, NETLINK_SND_SYNC, &val,
	    optlen) == -1)
		goto fail;

	dev->family_id = snl_get_genl_family(&dev->ss, UDMABUF_FAMILY_NAME);
	if (dev->family_id == 0)
		goto fail;

	return (dev);

fail:
	snl_free(&dev->ss);
	free(dev);
	dev = NULL;
	return (NULL);
}

static int
nl_wait_reply_fd(struct udmabuf_dev *dev, uint32_t seq)
{
	struct nl_parsed_reply reply;
	struct nlmsghdr *hdr;
	struct nlmsgerr *err;
	int out_err, out_fd;

	out_err = 0;
	out_fd = -1;
	while ((hdr = snl_read_message(&dev->ss)) != NULL) {
		if (hdr->nlmsg_seq != seq)
			continue;

		if (hdr->nlmsg_type == NLMSG_ERROR) {
			err = (struct nlmsgerr *)NLMSG_DATA(hdr);
			if (err->error != 0)
				out_err = err->error;
		} else if (hdr->nlmsg_type == dev->family_id) {
			reply.dmabuf_fd = -1;
			if (snl_parse_nlmsg(&dev->ss, hdr, &reply_parser,
			    &reply))
				out_fd = reply.dmabuf_fd;
		}

		if (hdr->nlmsg_type == NLMSG_DONE ||
		    hdr->nlmsg_type == NLMSG_ERROR)
			break;
	}

	if (out_err != 0) {
		errno = -out_err;
		return (-1);
	}
	return (out_fd);
}

bool
udmabuf_available(void)
{
	return (udmabuf_open() != NULL);
}

int
udmabuf_do_create(const struct udmabuf_item *item, uint32_t flags)
{
	struct udmabuf_dev *dev;
	struct snl_writer nw;
	struct nlmsghdr *hdr;
	uint32_t seq;
	int fd;

	dev = udmabuf_open();
	if (dev == NULL)
		return (-1);

	snl_init_writer(&dev->ss, &nw);

	if (snl_create_genl_msg_request(&nw, dev->family_id,
	    UDMABUF_CMD_CREATE) == NULL)
		return (-1);
	if (!snl_add_msg_attr_u32(&nw, UDMABUF_ATTR_FLAGS, flags))
		return (-1);
	if (!snl_add_msg_attr_u32(&nw, UDMABUF_ATTR_MEMFD, item->memfd))
		return (-1);
	if (!snl_add_msg_attr_u64(&nw, UDMABUF_ATTR_OFFSET, item->offset))
		return (-1);
	if (!snl_add_msg_attr_u64(&nw, UDMABUF_ATTR_SIZE, item->size))
		return (-1);

	hdr = snl_finalize_msg(&nw);
	if (hdr == NULL)
		return (-1);
	seq = hdr->nlmsg_seq;

	if (!snl_send_msgs(&nw))
		return (-1);

	fd = nl_wait_reply_fd(dev, seq);
	return (fd);
}

int
udmabuf_do_create_list(const struct udmabuf_item *items, uint32_t flags,
    uint32_t count)
{
	struct udmabuf_dev *dev;
	struct snl_writer nw;
	struct nlmsghdr *hdr;
	struct udmabuf_item item;
	uint32_t seq;
	int fd, i, off_item, off_list;

	if (count < 1) {
		errno = EINVAL;
		return (-1);
	}

	if (count == 1) {
		item.memfd = items[0].memfd;
		item.offset = items[0].offset;
		item.size = items[0].size;
		return (udmabuf_do_create(&item, flags));
	}

	dev = udmabuf_open();
	if (dev == NULL)
		return (-1);

	snl_init_writer(&dev->ss, &nw);

	if (snl_create_genl_msg_request(&nw, dev->family_id,
	    UDMABUF_CMD_CREATE_LIST) == NULL)
		return (-1);
	if (!snl_add_msg_attr_u32(&nw, UDMABUF_ATTR_FLAGS, flags))
		return (-1);

	off_list = snl_add_msg_attr_nested(&nw, UDMABUF_ATTR_LISTS);
	if (off_list == 0)
		return (-1);

	for (i = 0; (uint32_t)i < count; i++) {
		off_item = snl_add_msg_attr_nested(&nw, UDMABUF_ATTR_ITEM);
		if (off_item == 0)
			return (-1);

		if (!snl_add_msg_attr_u32(&nw, UDMABUF_ATTR_MEMFD,
		    items[i].memfd))
			return (-1);
		if (!snl_add_msg_attr_u64(&nw, UDMABUF_ATTR_OFFSET,
		    items[i].offset))
			return (-1);
		if (!snl_add_msg_attr_u64(&nw, UDMABUF_ATTR_SIZE,
		    items[i].size))
			return (-1);

		snl_end_attr_nested(&nw, off_item);
	}
	snl_end_attr_nested(&nw, off_list);

	hdr = snl_finalize_msg(&nw);
	if (hdr == NULL)
		return (-1);
	seq = hdr->nlmsg_seq;

	if (!snl_send_msgs(&nw))
		return (-1);

	fd = nl_wait_reply_fd(dev, seq);
	return (fd);
}
