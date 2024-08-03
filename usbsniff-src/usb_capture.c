/*  (c) Valentin Dudouyt, 2013 - 2014
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pcap.h>
#include <pcap/usb.h>
#include <libusb.h>
#include <assert.h>
#include <math.h>
#include "common.h"

#define MAX_PACKET_SIZE 4096
#define OUT(...) do { printf(__VA_ARGS__); if(out) { fprintf(out, __VA_ARGS__); } } while(0);

void print_help_and_exit(char **argv) {
	fprintf(stderr, "Usage: %s <vid>:<pid> [<filename>]\n", argv[0]);
	exit(-1);
}

libusb_device *find_usb_device(libusb_context *ctx, unsigned int vid, unsigned int pid) {
	libusb_device **devs;
	int devices_count = libusb_get_device_list(ctx, &devs); // All devices in system
	// Looking for device with desired vid / pid
	struct libusb_device_descriptor desc;
	int i;
	for(i = 0; i < devices_count; i++) {
		int result = libusb_get_device_descriptor(devs[i], &desc);
		assert(result >= 0);
		if(desc.idVendor == vid && desc.idProduct == pid)
			return(devs[i]);
	}
	return(NULL); // default
}

long double track_time(int64_t ts_sec, int32_t ts_usec) {
	// Args: absolute timestamp (sec + usec)
	// Returns: relative timestamp
	static long double time_diff, current_timestamp, prev_timestamp = 0;
	current_timestamp = ts_sec + ts_usec / pow(10,6);
	time_diff = prev_timestamp ? current_timestamp - prev_timestamp : 0;
	prev_timestamp = current_timestamp;
	return(time_diff);
}

unsigned char bus_number, device_number;
char errbuf[PCAP_ERRBUF_SIZE];
FILE *out;

void process_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet) {
	#ifdef LINUX
	pcap_usb_header *usb_header = (pcap_usb_header *) packet;
	if(usb_header->device_address != device_number)
		return;
	// TODO: have a hash table here?
	static int last_urb_id = 0; // Used for mapping request with it's response 
	unsigned int last_request_type;
	if(usb_header->setup.bmRequestType == 128 && usb_header->setup.bRequest == 0x06) {
		// This one seems being requested each time
		// when we start a data capture.
		// I don't think that we are insterested in it's contents,
		// so just display a message.
		printf("# GET DESCRIPTOR Request DEVICE\n");
		// Mark as awaiting response
		last_urb_id = usb_header->id;
		last_request_type = usb_header->setup.bmRequestType;
		return;
	}
	if(last_urb_id && last_request_type == 128) {
		// We do not need it's response as well
		printf("# GET DESCRIPTOR Response DEVICE\n");
		last_urb_id = last_request_type = 0;
		return;
	}

	// Transfer type
	char transfer_type[5], direction[4];
	switch(usb_header->transfer_type) {
		case URB_ISOCHRONOUS:
			sprintf(transfer_type, "ISOC");
			break;
		case URB_INTERRUPT:
			sprintf(transfer_type, "INTR");
			break;
		case URB_CONTROL:
			sprintf(transfer_type, "CTRL");
			break;
		case URB_BULK:
			sprintf(transfer_type, "BULK");
			break;
	}
	if( usb_header->endpoint_number & URB_TRANSFER_IN )
		sprintf(direction, "IN");
	else
		sprintf(direction, "OUT");


	// Output
	char prefix[19], hex[MAX_PACKET_SIZE * 2];
	if(usb_header->data_len > MAX_PACKET_SIZE) {
		printf("# Large data block omitted\n");
		if(out)
			fprintf(out, "# Large data block omitted\n");
	} else if(usb_header->data_len) {
		/* Printing transfer type & endpoint*/
		sprintf(prefix, "%s_%s(%d.%d):",
			transfer_type,
			direction,
			0, // avoid redundancy
			usb_header->endpoint_number & 0x7F);
		OUT("%-14s", prefix);

		/* Printing packet's setup */
		if(usb_header->transfer_type == URB_CONTROL) {
			pcap_usb_setup *setup = &(usb_header->setup);
			OUT("%02x:%02x:%04x:%04x:",
					setup->bmRequestType,
					setup->bRequest,
					setup->wValue,
					setup->wIndex);
		}

		/* Printing payload */
		const unsigned char *raw_data = packet + header->len - usb_header->data_len;
		void (*print_packet)(FILE *out, pcap_usb_header *usb_header, const unsigned char *raw_data);
		buf_to_hex(raw_data, usb_header->data_len, hex);
		OUT("%s", hex);

		/* Printing timestamp */
		long double time_diff = track_time(usb_header->ts_sec, usb_header->ts_usec);
		OUT(" # %.16Lf\n", time_diff);
	}
	#endif
}

void pcap_list_devices() {
	pcap_if_t *alldevs, *d;
	int result = pcap_findalldevs(&alldevs, errbuf);
	assert(result != -1);
	// Print the list
	for(d= alldevs; d != NULL; d= d->next)
	{
		printf("%s\n", d->name);
	}
	pcap_freealldevs(alldevs);
}

int main(int argc, char *argv[])
{
	// Parsing args
	if(argc < 2 || argc > 3)
		print_help_and_exit(argv);
	int result, vid, pid;
	result = sscanf(argv[1], "%x:%x", &vid, &pid);
	if(!result)
		print_help_and_exit(argv);
	out = NULL;
	if(argc > 2) {
		// Opening log file
		out = fopen ( argv[2], "w" );
		if(!out) {
			perror("Couldn't open log");
			exit(-1);
		}
		setvbuf(out, NULL, _IONBF, 0);
	}

	// Dealing with libusb
	libusb_device_handle *dev_handle;
	libusb_context *ctx;
	result = libusb_init(&ctx); // Obtain libusb_context
	assert(result >= 0);
	libusb_set_debug(ctx, 3);
	libusb_device *usbdev = find_usb_device(ctx, vid, pid);
	assert(usbdev);
	bus_number = libusb_get_bus_number(usbdev); // Which bus to sniff
	device_number = libusb_get_device_address(usbdev); // Filter
	fprintf(stderr, "bus_number=%d, device_number=%d\n", bus_number, device_number);

	char pcap_if_name[10];
	#ifdef LINUX
		system("modprobe usbmon");
		sprintf(pcap_if_name, "usbmon%d", bus_number);
	#else 
		#error "Unsupported platform"
	#endif

	printf("pcap device: %s\n", pcap_if_name);
	pcap_t *handle = pcap_open_live(pcap_if_name, BUFSIZ, 1, 1000, errbuf);
	if (handle == NULL) {
		fprintf(stderr, "Couldn't open device %s: %s\n", pcap_if_name, errbuf);
		#ifdef LINUX
		fprintf(stderr, "Please check for modules list.\n");
		#endif
		return(2);
	}
	pcap_loop(handle, -1, process_packet, NULL);
	printf("exit\n");
	pcap_close(handle);
	if(out)
		fclose(out);
}
