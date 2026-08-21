#include <sys/un.h>
#include <sys/socket.h>
#include <errno.h>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <machine/cpufunc.h>
#include <machine/specialreg.h>

#include <zlib.h>

#include "bhyvegc.h"
#include "config.h"
#include "console.h"
#include "debug.h"
#include "console_ipc.h"
#include "sys/ioccom.h"
#include "sys/stat.h"
#include "dma-buf.h"

#define	CONSOLE_IPC_MAX_WIDTH			2000
#define	CONSOLE_IPC_MAX_HEIGHT			1200
#define	PIX_PER_CELL	32
#define	PIXCELL_MASK	0x1F

struct console_ipc_softc {
	int		sfd;
	pthread_t	tid;

	int		width, height;

	int		conn_wait;

	pthread_mutex_t mtx;
	pthread_mutex_t pixfmt_mtx;
	pthread_cond_t  cond;

	int		hw_crc;
	uint32_t	*crc;		/* WxH crc cells */
	uint32_t	*crc_tmp;	/* buffer to store single crc row */
	int		crc_width, crc_height;

	char		*fbname;
	int		fbnamelen;
};

struct console_ipc_rect {
	int x;
	int y;
	int w;
	int h;
};

static int
sse42_supported(void)
{
	u_int cpu_registers[4], ecx;

	do_cpuid(1, cpu_registers);

	ecx = cpu_registers[2];

	return ((ecx & CPUID2_SSE42) != 0);
}

/*
 * Calculate CRC32 using SSE4.2; Intel or AMD Bulldozer+ CPUs only
 */
static __inline uint32_t
fast_crc32(void *buf, int len, uint32_t crcval)
{
	uint32_t q = len / sizeof(uint32_t);
	uint32_t *p = (uint32_t *)buf;

	while (q--) {
		asm volatile (
			".byte 0xf2, 0xf, 0x38, 0xf1, 0xf1;"
			:"=S" (crcval)
			:"0" (crcval), "c" (*p)
		);
		p++;
	}

	return (crcval);
}

static struct console_ipc_rect
console_ipc_crc_check(struct console_ipc_softc *sc, struct bhyvegc_image *gc_image) {

	uint32_t *p;
	uint32_t *crc_p, *orig_crc;
	int x, y;
	int min_x, min_y;
	int max_x, max_y;
	int cellwidth;
	int w, h;
	int xcells, ycells;
	int rem_x;   /* remainder for resolutions not x32 pixels ratio */
	int changes;

	/* Clear old Csc values when the size changes */
	if (sc->crc_width != gc_image->width ||
	    sc->crc_height != gc_image->height) {
		memset(sc->crc, 0, sizeof(uint32_t) *
		    howmany(CONSOLE_IPC_MAX_WIDTH, PIX_PER_CELL) *
		    howmany(CONSOLE_IPC_MAX_HEIGHT, PIX_PER_CELL));
		sc->crc_width = gc_image->width;
		sc->crc_height = gc_image->height;
	}

       	/* A size update counts as an update in itself */
       	if (sc->width != gc_image->width ||
            sc->height != gc_image->height) {
		sc->width = gc_image->width;
		sc->height = gc_image->height;
		return (
			(struct console_ipc_rect) {
				0,
				0,
				gc_image->width,
				gc_image->height
			}
		);
	}

	/*
	 * Calculate the checksum for each 32x32 cell. Send each that
	 * has changed since the last scan.
	 */

	w = sc->crc_width;
	h = sc->crc_height;
	xcells = howmany(sc->crc_width, PIX_PER_CELL);
	ycells = howmany(sc->crc_height, PIX_PER_CELL);

	rem_x = w & PIXCELL_MASK;

	p = gc_image->data;

	/*
	 * Go through all cells and calculate crc. If significant number
	 * of changes, then send entire screen.
	 * crc_tmp is dual purpose: to store the new crc and to flag as
	 * a cell that has changed.
	 */
	crc_p = sc->crc_tmp - xcells;
	orig_crc = sc->crc - xcells;
	changes = 0;
	min_x = CONSOLE_IPC_MAX_WIDTH;
	min_y = CONSOLE_IPC_MAX_HEIGHT;
	max_x = 0;
	max_y = 0;
	memset(sc->crc_tmp, 0, sizeof(uint32_t) * xcells * ycells);
	for (y = 0; y < h; y++) {
		if ((y & PIXCELL_MASK) == 0) {
			crc_p += xcells;
			orig_crc += xcells;
		}

		for (x = 0; x < xcells; x++) {
			if (x == (xcells - 1) && rem_x > 0)
				cellwidth = rem_x;
			else
				cellwidth = PIX_PER_CELL;

			if (sc->hw_crc)
				crc_p[x] = fast_crc32(p,
				             cellwidth * sizeof(uint32_t),
				             crc_p[x]);
			else
				crc_p[x] = (uint32_t)crc32(crc_p[x],
				             (Bytef *)p,
				             cellwidth * sizeof(uint32_t));

			p += cellwidth;

			/* check for crc delta if last row in cell */
			if ((y & PIXCELL_MASK) == PIXCELL_MASK || y == (h-1)) {
				if (orig_crc[x] != crc_p[x]) {
					orig_crc[x] = crc_p[x];
					crc_p[x] = 1;
					changes++;
					int cell_l = x * PIX_PER_CELL;
					int cell_r = cell_l + cellwidth;
					int cell_t = y & ~PIXCELL_MASK;
					int cell_b = y + 1;
					if (cell_l < min_x) min_x = cell_l;
					if (cell_r > max_x) max_x = cell_r;
					if (cell_t < min_y) min_y = cell_t;
					if (cell_b > max_y) max_y = cell_b;
				} else {
					crc_p[x] = 0;
				}
			}
		}
	}
	if (changes > 0) {
		EPRINTLN("update!!, x = %d, y = %d, w = %d, h = %d", min_x, min_y, max_x-min_x, max_y-min_y);
		return (
			(struct console_ipc_rect) {
				.x = min_x,
				.y = min_y,
				.w = max_x - min_x,
				.h = max_y - min_y,
			}
		);
	} else {
		return ((struct console_ipc_rect) { 0 });
	}
}

static void
console_ipc_handle(struct console_ipc_softc *sc, int cfd) {
	char buf[WIRE_MSG_MAX];
	struct wire_hdr *hdr;
	int len;
	for (;;) {
		len = recv(cfd, buf, sizeof(buf), 0);
		if (len == 0)
			break;
		hdr = (struct wire_hdr *)buf;
		switch (hdr->cmd) {
		case WIRE_CMD_REQ_VM_INFO: {
			struct wire_resp_vm_info resp = { 0 };
			struct wire_hdr resp_hdr = { 0 };
			if (len != sizeof(struct wire_req_vm_info)) {
				resp.code = EINVAL;
				resp_hdr.cmd = WIRE_CMD_RESP_VM_INFO;
				resp.hdr = resp_hdr;
				send(cfd, &resp, sizeof(resp), 0);
				break;
			}
			resp_hdr.cmd = WIRE_CMD_RESP_VM_INFO;
			resp.code = 0;
			resp.hdr = resp_hdr;
			strncpy(resp.name, sc->fbname, sc->fbnamelen);
			/* TODO: dev addr */
			strncpy(resp.dev_addr, "fbuf", sizeof("fbuf"));
			send(cfd, &resp, sizeof(resp), 0);
			break;
		}
		case WIRE_CMD_REQ_GET_IMAGE: {
			struct wire_req_get_image *req;
			struct wire_resp_get_image resp = { 0 };
			struct wire_hdr resp_hdr = { 0 };
			struct bhyvegc_image *gc_image;
			if (len != sizeof(struct wire_req_get_image)) {
				resp.code = EINVAL;
				resp_hdr.cmd = WIRE_CMD_RESP_GET_IMAGE;
				resp.hdr = resp_hdr;
				send(cfd, &resp, sizeof(resp), 0);
				break;
			}
			req = (struct wire_req_get_image *)buf;

			console_refresh();
			gc_image = console_get_image();

			resp_hdr.cmd = WIRE_CMD_RESP_GET_IMAGE;
			resp.code = 0;
			resp.hdr = resp_hdr;
			resp.generation = 1;
			resp.vgamode = gc_image->vgamode;
			resp.height = gc_image->height;
			resp.width = gc_image->width;
			if (req->hdr.flags & WIRE_FLAG_HAS_FD) {
				struct iovec iov = { &resp, sizeof(resp) };
				char cmsg[CMSG_SPACE(sizeof(int))];
				struct msghdr m = {
					NULL, 0, &iov, 1, cmsg, sizeof(cmsg), 0
				};
				struct cmsghdr *c = CMSG_FIRSTHDR(&m);
				c->cmsg_len = CMSG_LEN(sizeof(int));
				c->cmsg_level = SOL_SOCKET;
				c->cmsg_type = SCM_RIGHTS;
				struct dma_buf_sync sync = {
					.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW
				};
				ioctl(gc_image->dmabuf, DMA_BUF_IOCTL_SYNC, &sync);
				*(int*)CMSG_DATA(c) = gc_image->dmabuf;
				sendmsg(cfd, &m, 0);
			} else {
				send(cfd, &resp, sizeof(resp), 0);
			}
			break;
		}
		case WIRE_CMD_REQ_POLL_IMAGE: {
			struct wire_req_poll_image *req;
			struct wire_resp_poll_image resp = { 0 };
			struct wire_hdr resp_hdr = { 0 };
			struct console_ipc_rect dirty;
			struct bhyvegc_image *gc_image;
			if (len != sizeof(struct wire_req_poll_image)) {
				resp.code = EINVAL;
				resp_hdr.cmd = WIRE_CMD_RESP_POLL_IMAGE;
				resp.hdr = resp_hdr;
				send(cfd, &resp, sizeof(resp), 0);
				break;
			}
			req = (struct wire_req_poll_image *)buf;

			console_refresh();
			gc_image = console_get_image();

			struct dma_buf_sync sync1 = {
				.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW
			};
			ioctl(gc_image->dmabuf, DMA_BUF_IOCTL_SYNC, &sync1);
			/* specific fbuf code */
			dirty = console_ipc_crc_check(sc, gc_image);

			resp_hdr.cmd = WIRE_CMD_RESP_POLL_IMAGE;
			resp.code = 0;
			resp.hdr = resp_hdr;
			/* XXX: fbuf will never change dmabuf fd */
			resp.generation = 1;
			resp.vgamode = gc_image->vgamode;
			resp.height = gc_image->height;
			resp.width = gc_image->width;
			resp.dirty_h = dirty.h;
			resp.dirty_w = dirty.w;
			resp.dirty_x = dirty.x;
			resp.dirty_y = dirty.y;
			if (req->hdr.flags & WIRE_FLAG_HAS_FD) {
				struct iovec iov = { &resp, sizeof(resp) };
				char cmsg[CMSG_SPACE(sizeof(int))];
				struct msghdr m = {
					NULL, 0, &iov, 1, cmsg, sizeof(cmsg), 0
				};
				struct cmsghdr *c = CMSG_FIRSTHDR(&m);
				c->cmsg_len = CMSG_LEN(sizeof(int));
				c->cmsg_level = SOL_SOCKET;
				c->cmsg_type = SCM_RIGHTS;
				struct dma_buf_sync sync2 = {
					.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW
				};
				ioctl(gc_image->dmabuf, DMA_BUF_IOCTL_SYNC, &sync2);
				*(int*)CMSG_DATA(c) = gc_image->dmabuf;
				sendmsg(cfd, &m, 0);
			} else {
				send(cfd, &resp, sizeof(resp), 0);
			}
			break;
		}
		case WIRE_CMD_REQ_KEY_EVENT: {
			struct wire_req_key_event *event;
			struct wire_resp_key_event resp = { 0 };
			struct wire_hdr resp_hdr = { 0 };
			if (len != sizeof(struct wire_req_key_event)) {
				resp.code = EINVAL;
				resp_hdr.cmd = WIRE_CMD_RESP_KEY_EVENT;
				resp.hdr = resp_hdr;
				send(cfd, &resp, sizeof(resp), 0);
				break;
			}
			event = (struct wire_req_key_event *)buf;
			PRINTLN("keyboard, %d, %d, %d", event->down, event->keysym, event->keycode);
			console_key_event(
				event->down,
				event->keysym,
				event->keycode
			);
			resp_hdr.cmd = WIRE_CMD_RESP_KEY_EVENT;
			resp.code = 0;
			resp.hdr = resp_hdr;
			send(cfd, &resp, sizeof(resp), 0);
			break;
		}
		case WIRE_CMD_REQ_PTR_EVENT: {
			struct wire_req_ptr_event *event;
			struct wire_resp_ptr_event resp = { 0 };
			struct wire_hdr resp_hdr = { 0 };
			if (len != sizeof(struct wire_req_ptr_event)) {
				resp.code = EINVAL;
				resp_hdr.cmd = WIRE_CMD_RESP_PTR_EVENT;
				resp.hdr = resp_hdr;
				send(cfd, &resp, sizeof(resp), 0);
				break;
			}
			event = (struct wire_req_ptr_event *)buf;
			console_ptr_event(
				event->button,
				event->x,
				event->y
			);
			resp_hdr.cmd = WIRE_CMD_RESP_PTR_EVENT;
			resp.code = 0;
			resp.hdr = resp_hdr;
			send(cfd, &resp, sizeof(resp), 0);
			break;
		}
		}
	}
}

static void *
console_ipc_thr(void *arg)
{
	struct console_ipc_softc *sc;
	sigset_t set;

	int cfd;

	sc = arg;

	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
		perror("pthread_sigmask");
		return (NULL);
	}

	for (;;) {
		cfd = accept(sc->sfd, NULL, NULL);
		if (sc->conn_wait) {
			pthread_mutex_lock(&sc->mtx);
			pthread_cond_signal(&sc->cond);
			pthread_mutex_unlock(&sc->mtx);
			sc->conn_wait = 0;
		}
		console_ipc_handle(sc, cfd);
		close(cfd);
	}

	/* NOTREACHED */
	return (NULL);
}

int console_ipc_init(int wait) {
	struct sockaddr_un sun;
	struct console_ipc_softc *sc;
	int e = 0;
	int cnt;
	const char *hostname = "/tmp/bhyve.sock";

	sc = calloc(1, sizeof(struct console_ipc_softc));

	/* specific fbuf code */
	cnt = howmany(CONSOLE_IPC_MAX_WIDTH, PIX_PER_CELL) *
	    howmany(CONSOLE_IPC_MAX_HEIGHT, PIX_PER_CELL);
	sc->crc = calloc(cnt, sizeof(uint32_t));
	sc->crc_tmp = calloc(cnt, sizeof(uint32_t));
	sc->crc_width = CONSOLE_IPC_MAX_WIDTH;
	sc->crc_height = CONSOLE_IPC_MAX_HEIGHT;

	sc->fbnamelen = asprintf(&sc->fbname, "bhyve:%s",
	    get_config_value("name"));
	if (sc->fbnamelen < 0) {
		EPRINTLN("console_ipc: failed to allocate memory for VNC title");
		goto error;
	}

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	if (strlcpy(sun.sun_path, hostname, sizeof(sun.sun_path)) >=
		sizeof(sun.sun_path)) {
		EPRINTLN("console_ipc: socket path too long");
		return (-1);
	}
	sc->sfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (sc->sfd < 0) {
		perror("socket");
		goto error;
	}
	unlink(hostname);
	e = bind(sc->sfd, (struct sockaddr *)&sun, SUN_LEN(&sun));
	if (e < 0) {
		perror("bind");
		goto error;
	}
	//TODO:
	chmod(hostname, 0666);

	/* XXX: may be more console in the future */
	if (listen(sc->sfd, 1) < 0) {
		perror("listen");
		goto error;
	}

	sc->hw_crc = sse42_supported();

	sc->conn_wait = wait;
	if (wait) {
		pthread_mutex_init(&sc->mtx, NULL);
		pthread_cond_init(&sc->cond, NULL);
	}

	pthread_mutex_init(&sc->pixfmt_mtx, NULL);
	pthread_create(&sc->tid, NULL, console_ipc_thr, sc);
	pthread_set_name_np(sc->tid, "console_ipc");

	if (wait) {
		PRINTLN("Waiting for console_ipc client...");
		pthread_mutex_lock(&sc->mtx);
		pthread_cond_wait(&sc->cond, &sc->mtx);
		pthread_mutex_unlock(&sc->mtx);
		PRINTLN("console_ipc client connected");
	}
	return (0);

error:
	if (sc->sfd >= 0) close(sc->sfd);
	if (sc->crc) free(sc->crc);
	if (sc->crc_tmp) free(sc->crc_tmp);
	if (sc->fbname) free(sc->fbname);
	free(sc);
	return (-1);
}