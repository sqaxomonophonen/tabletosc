#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <libusb.h>
#include <assert.h>

#define PEN_PKGLEN 10
#define TOUCH_BTN_PKGLEN 64

static void pen_handler(uint8_t* data)
{
	if (data[0] != 2) return;

	if (data[1] == 0x80 || data[1] == 0x00) {
		/* 0x80 seems to mean "I can sense the pen is near, but I don't
		 * have any data to provide on it" while 0x00 seems to mean
		 * "lost contact with pen" */
		printf("PEN: away\n");
		return;
	}

	if ((data[1] & 0xf0) == 0xe0) {
		printf("PEN:");

		int contact = data[1] & 0x1;
		if (contact) printf(" *"); else printf("  ");

		int eraser = data[1] & 0x8;
		if (eraser) printf(" ERZ"); else printf(" PEN");

		int pen_btn = data[1] & 0x4;
		if (pen_btn) printf(" btn"); else printf("    ");

		/* top-left corner is (0,0) */
		int x = data[2] | (data[3] << 8); // range: [0:14720]
		int y = data[4] | (data[5] << 8); // range: [0:9200]
		printf(" x=%.5d y=%.5d", x, y);

		int pressure = data[6] | (data[7] << 8); // range [0:1023]
		printf(" pressure=%.5d", pressure);

		int proximity = data[8]; // range: [0:32] ish, but goes a bit further
		printf(" proximity=%.2d", proximity);

		#if 0
		for (int i = 0; i < PEN_PKGLEN; i++) printf(" %.2x", data[i]);
		#endif

		printf("\n");
	}
}

static void touch_btn_handler(uint8_t* data)
{
	if (data[0] != 2 || data[1] != 1) return;
	if (data[2] != 0x80) return;

	uint8_t btn_mask = data[3];

	printf("BTN ");
	if (btn_mask & 1) printf(" 1"); else printf("  ");
	if (btn_mask & 2) printf(" 2"); else printf("  ");
	if (btn_mask & 4) printf(" 3"); else printf("  ");
	if (btn_mask & 8) printf(" 4"); else printf("  ");
	printf("\n");

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

int main(int argc, char** argv)
{
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

	/* XXX seems I have to set cfg 0 (reset?) and then 1 (only one there
	 * is?), otherwise, libusb_claim_interface() fails.. and even then it
	 * fails the first time?! maybe try claiming interface for a while...
	 * */
	for (int cfg = 0; cfg < 2; cfg++) {
		r = libusb_set_configuration(handle, cfg);
		if (r < 0) {
			fprintf(stderr, "failed to set configuration %d\n", cfg);
			exit(EXIT_FAILURE);
		}
	}

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
