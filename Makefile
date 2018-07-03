CFLAGS=-I. --std=c99 -Wall
LINK=./libusb-1.0.0.dylib

main: main.c
	$(CC) $(CFLAGS) $< -o $@ $(LINK)
	install_name_tool -add_rpath . ./main
