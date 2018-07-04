#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <libusb.h>
#include <assert.h>
#include <string.h>

#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#define PEN_PKGLEN 10
#define TOUCH_BTN_PKGLEN 64

#define FLAG_BTN1      (1<<0)
#define FLAG_BTN2      (1<<1)
#define FLAG_BTN3      (1<<2)
#define FLAG_BTN4      (1<<3)
#define FLAG_PENBTN    (1<<4)
#define FLAG_ERASER    (1<<5)
#define FLAG_PROXIMITY (1<<6)
#define FLAG_CONTACT   (1<<7)

int osc_fd;
int osc_buffer_length;
uint8_t osc_buffer[2048];

static void osc_open(char* host, char* service)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	struct addrinfo *result;
	int err;
	if ((err = getaddrinfo(host, service, &hints, &result)) != 0) {
		fprintf(stderr, "getaddrinfo for %s: %s\n", host, gai_strerror(err));
		exit(EXIT_FAILURE);
	}

	for (struct addrinfo* rp = result; rp != NULL; rp = rp->ai_next) {
		osc_fd = socket(
			rp->ai_family,
			rp->ai_socktype,
			rp->ai_protocol);
		if (osc_fd == -1) continue;
		if (connect(osc_fd, rp->ai_addr, rp->ai_addrlen) != -1) break;
		close(osc_fd);
	}

	if (osc_fd == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	freeaddrinfo(result);
}

static void osc_begin()
{
	osc_buffer_length = 0;
}

static void osc_str(char* s)
{
	char c;
	int n = 0;
	while ((c = *(s++))) {
		osc_buffer[osc_buffer_length++] = c;
		n++;
	}
	int zero_padding = 4 - (n & 3);
	for (int i = 0; i < zero_padding; i++) osc_buffer[osc_buffer_length++] = 0;
}

static void osc_f32(float f)
{
	union {
		float f;
		uint8_t b[4];
	} v;
	v.f = f;
	for (int i = 0; i < 4; i++) osc_buffer[osc_buffer_length++] = v.b[3 - i];
}

#if 0
static void osc_i32(int32_t i)
{
	union {
		float i;
		uint8_t b[4];
	} v;
	v.i = i;
	for (int i = 0; i < 4; i++) osc_buffer[osc_buffer_length++] = v.b[3 - i];
}
#endif

static void osc_end()
{
	if (osc_buffer_length == 0) return;
	if (write(osc_fd, osc_buffer, osc_buffer_length) != osc_buffer_length) {
		perror("write");
	}
	osc_buffer_length = 0;
}



struct osc_state {
	float x, y, pressure, proximity;
	int flags;
	int counter;
} osc_state;


static void push_state(struct osc_state new_state)
{
	if (memcmp(&osc_state, &new_state, sizeof new_state) == 0) {
		/* no state change; don't send */
		return;
	}
	osc_state = new_state;

	#if DEBUG
	printf("x=%.3f y=%.3f prox=%.3f pres=%.3f flags=%.2x counter=%d\n",
		osc_state.x,
		osc_state.y,
		osc_state.proximity,
		osc_state.pressure,
		osc_state.flags,
		osc_state.counter);
	#endif

	osc_begin();
	osc_str("/tablet");
	osc_str(",ffffff");
	osc_f32(osc_state.x);
	osc_f32(osc_state.y);
	osc_f32(osc_state.pressure);
	osc_f32(osc_state.proximity);
	osc_f32(osc_state.flags);
	osc_f32(osc_state.counter);
	osc_end();
}

static void pen_handler(uint8_t* data)
{
	if (data[0] != 2) return;

	struct osc_state new_state = osc_state;

	if (data[1] == 0x80 || data[1] == 0x00) {
		/* 0x80 seems to mean "I can sense the pen is near, but I don't
		 * have any data to provide on it" while 0x00 seems to mean
		 * "lost contact with pen" */
		#if DEBUG
		printf("PEN: away\n");
		#endif
		new_state.flags &= ~FLAG_PENBTN;
		new_state.flags &= ~FLAG_PROXIMITY;
		new_state.flags &= ~FLAG_CONTACT;
		new_state.flags &= ~FLAG_ERASER;
		new_state.proximity = 0;
		new_state.pressure = 0;
	} else if ((data[1] & 0xf0) == 0xe0) {
		#if DEBUG
		printf("PEN:");
		#endif

		new_state.flags |= FLAG_PROXIMITY;

		int contact = data[1] & 0x1;
		#if DEBUG
		if (contact) printf(" *"); else printf("  ");
		#endif
		if (contact) new_state.flags |= FLAG_CONTACT; else new_state.flags &= ~FLAG_CONTACT;

		int eraser = data[1] & 0x8;
		#if DEBUG
		if (eraser) printf(" ERZ"); else printf(" PEN");
		#endif
		if (eraser) new_state.flags |= FLAG_ERASER; else new_state.flags &= ~FLAG_ERASER;


		int pen_btn = data[1] & 0x4;
		#if DEBUG
		if (pen_btn) printf(" btn"); else printf("    ");
		#endif
		if (pen_btn) new_state.flags |= FLAG_PENBTN; else new_state.flags &= ~FLAG_PENBTN;

		/* top-left corner is (0,0) */
		int x = data[2] | (data[3] << 8); // range: [0:14720]
		int y = data[4] | (data[5] << 8); // range: [0:9200]
		#if DEBUG
		printf(" x=%.5d y=%.5d", x, y);
		#endif
		new_state.x = (float)x / 14720.0f;
		new_state.y = (float)y / 9200.0f;

		int pressure = data[6] | (data[7] << 8); // range [0:1023]
		#if DEBUG
		printf(" pressure=%.5d", pressure);
		#endif
		new_state.pressure = (float)pressure / 1023.0f;

		int proximity = data[8]; // range: [0:31] ish, but goes a bit further
		#if DEBUG
		printf(" proximity=%.2d", proximity);
		#endif
		new_state.proximity = (float)proximity / 31.0f;

		#if 0
		for (int i = 0; i < PEN_PKGLEN; i++) printf(" %.2x", data[i]);
		#endif

		#if DEBUG
		printf("\n");
		#endif
	}

	push_state(new_state);
}

static void touch_btn_handler(uint8_t* data)
{
	if (data[0] != 2 || data[1] != 1) return;
	if (data[2] != 0x80) return;

	struct osc_state new_state = osc_state;

	uint8_t btn_mask = data[3];

	#if DEBUG
	printf("BTN ");
	if (btn_mask & 1) printf(" 1"); else printf("  ");
	if (btn_mask & 2) printf(" 2"); else printf("  ");
	if (btn_mask & 4) printf(" 3"); else printf("  ");
	if (btn_mask & 8) printf(" 4"); else printf("  ");
	printf("\n");
	#endif

	if (btn_mask & 1) new_state.flags |= FLAG_BTN1; else new_state.flags &= ~FLAG_BTN1;
	if (btn_mask & 2) new_state.flags |= FLAG_BTN2; else new_state.flags &= ~FLAG_BTN2;
	if (btn_mask & 4) new_state.flags |= FLAG_BTN3; else new_state.flags &= ~FLAG_BTN3;
	if (btn_mask & 8) new_state.flags |= FLAG_BTN4; else new_state.flags &= ~FLAG_BTN4;

	if (!(new_state.flags & FLAG_BTN1) && (osc_state.flags & FLAG_BTN1)) new_state.counter--;
	if (!(new_state.flags & FLAG_BTN4) && (osc_state.flags & FLAG_BTN4)) new_state.counter++;

	push_state(new_state);


	#if 0
	for (int i = 0; i < TOUCH_BTN_PKGLEN; i++) printf(" %.2x", data[i]);
	printf("\n");
	#endif
}

static void handle_transfer(struct libusb_transfer* transfer)
{
	if (transfer->actual_length > 0) {
		void(*fn)(uint8_t*) = transfer->user_data;
		fn(transfer->buffer);
	}
	int r = libusb_submit_transfer(transfer);
	if (r < 0) fprintf(stderr, "libusb_submit_transfer failed %d\n", r);
}

static void setup_transfer(libusb_device_handle *handle, int endpoint, int bufsz, void(*msg_handler)(uint8_t*))
{
	struct libusb_transfer* transfer = libusb_alloc_transfer(0);
	assert(transfer != NULL);

	uint8_t* buf = malloc(bufsz);
	assert(buf != NULL);

	libusb_fill_interrupt_transfer(
		transfer,
		handle,
		endpoint | LIBUSB_ENDPOINT_IN,
		buf,
		bufsz,
		handle_transfer,
		msg_handler,
		1000);

	int r = libusb_submit_transfer(transfer);
	if (r < 0) fprintf(stderr, "libusb_submit_transfer failed %d\n", r);
}

void set_cfg(libusb_device_handle* handle, int cfg) {
	int r = libusb_set_configuration(handle, cfg);
	if (r < 0) {
		fprintf(stderr, "failed to set configuration %d\n", cfg);
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char** argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
		fprintf(stderr, "Sends OSC packages to <host>:<port> with the following \",ffffff\" format:\n");
		fprintf(stderr, "  x: range [0:1]\n");
		fprintf(stderr, "  y: range [0:1]\n");
		fprintf(stderr, "  pressure: range [0:1]\n");
		fprintf(stderr, "  proximity: range [0:1] (and a bit beyond 1)\n");
		fprintf(stderr, "  flags (integer cast to float):\n");
		fprintf(stderr, "     0x01: button 1\n");
		fprintf(stderr, "     0x02: button 2\n");
		fprintf(stderr, "     0x04: button 3\n");
		fprintf(stderr, "     0x08: button 4\n");
		fprintf(stderr, "     0x10: pen button\n");
		fprintf(stderr, "     0x20: eraser (otherwise pen/tip)\n");
		fprintf(stderr, "     0x40: proximity (set as soon as x/y data is received)\n");
		fprintf(stderr, "     0x80: contact (set when pen touches)\n");
		fprintf(stderr, "  counter (incremented with button 4, decremented with button 1)\n");
		exit(EXIT_FAILURE);
	}

	osc_open(argv[1], argv[2]);

	printf("initializing...\n");
	int r;
	r = libusb_init(NULL);
	if (r < 0) {
		fprintf(stderr, "libusb_init failed\n");
		exit(EXIT_FAILURE);
	}

	libusb_device_handle *handle = libusb_open_device_with_vid_pid(NULL, 1386, 222);
	if (handle == NULL) {
		fprintf(stderr, "failed to open Wacom Bamboo device\n");
		exit(EXIT_FAILURE);
	}

	/* on macOS I have to do this weird dance, otherwise
	 * libusb_claim_interface fails, but only when the device has just been
	 * physically connected... I suspect it has something to do with
	 * fooling macOS into releasing the device? */
	set_cfg(handle, 0);
	set_cfg(handle, 1);
	set_cfg(handle, 0);
	set_cfg(handle, 1);

	/*
	interface 0 is stylus, and 1 is touch, I believe?
	*/
	for (int interface = 0; interface < 2; interface++) {
		r = libusb_claim_interface(handle, interface);
		if (r < 0) {
			fprintf(stderr, "failed to claim interface %d\n", interface);
			exit(EXIT_FAILURE);
		}

	}

	#if 0
	for (int endpoint = 1; endpoint <= 2; endpoint++) {
		r = libusb_clear_halt(handle, endpoint | LIBUSB_ENDPOINT_IN);
		if (r < 0) {
			fprintf(stderr, "failed to clear halt on endpoint %d\n", endpoint);
			exit(EXIT_FAILURE);
		}
	}
	#endif

	/* do some magick control transfer; derived from the linux wacom
	 * driver....
	 *  bmRequestType = 33 = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE (this is standard USB)
	 *  bmRequest = 9 = HID_REQ_SET_REPORT (probably also standard USB; used all over)
	 *  wValue = 770 = (((rtype + 1) << 8) | reportnum) where
	 *      rtype = HID_FEATURE_REPORT = 0x2 (probably also standard USB; used all over)
	 *      reportnum = 0x2; comes from the Wacom driver; same as data[0] for some reason
	 *  wIndex = 0 (interface)
	 *  data = {0x2, 0x2}; this comes from the Wacom driver
	 */
	uint8_t data[2];
	data[0] = 2; data[1] = 2;
	r = libusb_control_transfer(handle, 33, 9, 770, 0, data, 2, 1000);
	if (r < 0) {
		fprintf(stderr, "libusb_control_transfer failed %d\n", r);
		exit(EXIT_FAILURE);
	}

	setup_transfer(handle, 1, PEN_PKGLEN, pen_handler);
	setup_transfer(handle, 2, TOUCH_BTN_PKGLEN, touch_btn_handler);

	printf("entering loop....\n");
	for (;;) {
		r = libusb_handle_events(NULL);
		if (r < 0) {
			fprintf(stderr, "libusb_handle_events failed\n");
			exit(EXIT_FAILURE);
		}
	}

	return EXIT_SUCCESS;
}

#if 0
// code used to discover vendor/product id of Wacom Bamboo
int main(int argc, char** argv)
{
	int r;
	r = libusb_init(NULL);
	if (r < 0) {
		fprintf(stderr, "libusb_init failed\n");
		exit(EXIT_FAILURE);
	}

	libusb_device **devs;
	int n = libusb_get_device_list(NULL, &devs);

	if (n == 0) {
		fprintf(stderr, "found no devices\n");
		exit(EXIT_FAILURE);
	}

	for (libusb_device** dev = devs; *dev != NULL; dev++) {
		struct libusb_device_descriptor desc;
		r = libusb_get_device_descriptor(*dev, &desc);
		if (r < 0) {
			fprintf(stderr, "failed to get descriptor\n");
			exit(EXIT_FAILURE);
		}

		printf("%04x:%04x (bus %d, device %d)",
			desc.idVendor, desc.idProduct,
			libusb_get_bus_number(*dev), libusb_get_device_address(*dev));

		uint8_t path[8];
		r = libusb_get_port_numbers(*dev, path, sizeof(path));
		if (r > 0) {
			printf(" path: %d", path[0]);
			for (int i = 1; i < r; i++)
				printf(".%d", path[i]);
		}
		printf("\n");

		libusb_device_handle *handle = NULL;
		if (libusb_open(*dev, &handle) != LIBUSB_SUCCESS) {
			printf("could not open\n");
			continue;
		}

		uint8_t str[256];

		if (desc.iManufacturer && libusb_get_string_descriptor_ascii(handle, desc.iManufacturer, str, sizeof str) > 0) {
			printf("manufacturer: %s    vendor id: %d, product id: %d\n", str, desc.idVendor, desc.idProduct);
			//manufacturer: Wacom Co.,Ltd.    vendor id: 1386, product id: 222
		}
	}

	return EXIT_SUCCESS;
}
#endif
