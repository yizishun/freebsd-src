#ifndef _CONSOLE_IPC_H_
#define _CONSOLE_IPC_H_

#include <stdint.h>

int console_ipc_init(int wait);

/* Requests command */
#define WIRE_CMD_REQ_VM_INFO 	0x0001
#define WIRE_CMD_REQ_GET_IMAGE 	0x0002
#define WIRE_CMD_REQ_POLL_IMAGE	0x0003
#define WIRE_CMD_REQ_KEY_EVENT	0x0004
#define WIRE_CMD_REQ_PTR_EVENT	0x0005

/* Responses command = request | 0x8000 */
#define WIRE_CMD_RESP_VM_INFO 		0x8001
#define WIRE_CMD_RESP_GET_IMAGE 	0x8002
#define WIRE_CMD_RESP_POLL_IMAGE	0x8003
#define WIRE_CMD_RESP_KEY_EVENT		0x8004
#define WIRE_CMD_RESP_PTR_EVENT		0x8005

/* flags */
#define WIRE_FLAG_HAS_FD	0x0001

#define WIRE_VM_NAME_MAX	256
#define WIRE_DEV_ADDR_MAX	64
#define WIRE_MSG_MAX		512

/* header */
struct wire_hdr {
	uint16_t cmd;
	uint16_t flags;
};

/* Per-requests payload */
struct wire_req_vm_info {
	struct wire_hdr hdr;
};

struct wire_req_get_image {
	struct wire_hdr hdr;
};

struct wire_req_poll_image {
	struct wire_hdr hdr;
};

struct wire_req_key_event {
	struct wire_hdr hdr;
	uint32_t down;
	uint32_t keysym;
	uint32_t keycode;
};

struct wire_req_ptr_event {
	struct wire_hdr hdr;
	uint32_t button;
	int32_t x;
	int32_t y;
};

/* Per-response payload */
struct wire_resp_vm_info {
	struct wire_hdr hdr;
	uint32_t code;	/* 0 = success, otherwise is errno code */
	char name[WIRE_VM_NAME_MAX];
	char dev_addr[WIRE_DEV_ADDR_MAX];
};

struct wire_resp_get_image {
	struct wire_hdr hdr;
	uint32_t code;
	uint32_t generation;
	uint32_t vgamode;
	uint32_t width;
	uint32_t height;
};

struct wire_resp_poll_image {
	struct wire_hdr hdr;
	uint32_t code;
	uint32_t generation;
	uint32_t vgamode;
	uint32_t width;
	uint32_t height;
	int32_t dirty_x;
	int32_t dirty_y;
	int32_t dirty_w;
	int32_t dirty_h;
};

struct wire_resp_key_event {
	struct wire_hdr hdr;
	uint32_t code;
};

struct wire_resp_ptr_event {
	struct wire_hdr hdr;
	uint32_t code;
};


#endif /* _CONSOLE_IPC_H_ */
