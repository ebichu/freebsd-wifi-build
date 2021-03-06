/*
 * Copyright (C) 2007 Ubiquiti Networks, Inc.
 * Copyright (C) 2008 Lukas Kuna <ValXdater@seznam.cz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <zlib.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <zlib.h>
#include "fw.h"

typedef struct fw_layout_data {
	char		name[PATH_MAX];
	u_int32_t	kern_start;
	u_int32_t	kern_entry;
	u_int32_t	firmware_max_length;
} fw_layout_t;

fw_layout_t fw_layout_data[] = {
	{
		.name		=	"XS2",
		.kern_start	=	0x00040000,
		.kern_entry	=	0x80041000,
		.firmware_max_length=	0x006A0000,
	},
	{
		.name		=	"XS5",
		.kern_start	=	0xbe030000,
		.kern_entry	=	0x80041000,
		.firmware_max_length=	0x00390000,
	},
	{
		.name		=	"RS",
		.kern_start	=	0xbf030000,
		.kern_entry	=	0x80060000,
		.firmware_max_length=	0x00B00000,
	},
	{
		.name		=	"RSPRO",
		.kern_start	=	0xbf030000,
		.kern_entry	=	0x80050100,
		.firmware_max_length=	0x00B00000,
	},
	{
		.name		=	"LS-SR71",
		.kern_start	=	0xbf030000,
		.kern_entry	=	0x80060000,
		.firmware_max_length=	0x00640000,
	},
	{
		.name		=	"XS2-8",
		.kern_start	=	0xa8030000,
		.kern_entry	=	0x80041000,
		.firmware_max_length=	0x006C0000,
	},
	{
		.name		=	"XM",
		.kern_start	=	0x9f050000,
		.kern_entry	=	0x80002000,
		.firmware_max_length=	0x006A0000,
	},
	{
		.name		=	"PB42",
		.kern_start	=	0xbf030000,
		.kern_entry	=	0x80060000,
		.firmware_max_length=	0x00B00000,
	},
	{	.name		=	"",
	},
};

typedef struct part_data {
	char 	partition_name[64];
	int  	partition_index;
	u_int32_t	partition_baseaddr;
	u_int32_t	partition_startaddr;
	u_int32_t	partition_memaddr;
	u_int32_t	partition_entryaddr;
	u_int32_t  partition_length;

	char	filename[PATH_MAX];
	struct stat stats;
} part_data_t;

#define MAX_SECTIONS	8
#define DEFAULT_OUTPUT_FILE 	"firmware-image.bin"
#define DEFAULT_VERSION		"UNKNOWN"

#define OPTIONS "B:C:c:hv:o:r:k:"

static int debug = 1;

typedef struct image_info {
	char version[256];
	char outputfile[PATH_MAX];
	u_int32_t	part_count;
	part_data_t parts[MAX_SECTIONS];
	struct {
		int enable;	/* enable cfgfs? */
		size_t size;	/* size of config partition */
	} cfg;
} image_info_t;

static void write_header(void* mem, const char* version)
{
	header_t* header = mem;
	memset(header, 0, sizeof(header_t));

	memcpy(header->magic, MAGIC_HEADER, MAGIC_LENGTH);
	strncpy(header->version, version, sizeof(header->version));
	header->crc = htonl(crc32(0L, (unsigned char *)header,
				sizeof(header_t) - 2 * sizeof(u_int32_t)));
	header->pad = 0L;
}


static void write_signature(void* mem, u_int32_t sig_offset)
{
	/* write signature */
	signature_t* sign = (signature_t*)(mem + sig_offset);
	memset(sign, 0, sizeof(signature_t));

	memcpy(sign->magic, MAGIC_END, MAGIC_LENGTH);
	sign->crc = htonl(crc32(0L,(unsigned char *)mem, sig_offset));
	sign->pad = 0L;
}

static int write_part(void* mem, part_data_t* d)
{
	char* addr;
	int fd;
	part_t* p = mem;
	part_crc_t* crc = mem + sizeof(part_t) + d->stats.st_size;

	fd = open(d->filename, O_RDONLY);
	if (fd < 0)
	{
		ERROR("Failed opening file '%s'\n", d->filename);
		return -1;
	}

	if ((addr=(char*)mmap(0, d->stats.st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
	{
		ERROR("Failed mmaping memory for file '%s'\n", d->filename);
		close(fd);
		return -2;
	}

	memcpy(mem + sizeof(part_t), addr, d->stats.st_size);
	munmap(addr, d->stats.st_size);

	memset(p->name, 0, sizeof(p->name));
	strncpy(p->magic, MAGIC_PART, MAGIC_LENGTH);
	strncpy(p->name, d->partition_name, sizeof(p->name));
	p->index = htonl(d->partition_index);
	p->data_size = htonl(d->stats.st_size);
	p->part_size = htonl(d->partition_length);
	p->baseaddr = htonl(d->partition_baseaddr);
	p->memaddr = htonl(d->partition_memaddr);
	p->entryaddr = htonl(d->partition_entryaddr);

	crc->crc = htonl(crc32(0L, mem, d->stats.st_size + sizeof(part_t)));
	crc->pad = 0L;

	return 0;
}

static void usage(const char* progname)
{
	INFO("Version %s\n"
             "Usage: %s [options]\n"
	     "\t-v <version string>\t - firmware version information, default: %s\n"
	     "\t-o <output file>\t - firmware output file, default: %s\n"
	     "\t-k <kernel file>\t\t - kernel file\n"
	     "\t-r <rootfs file>\t\t - rootfs file\n"
	     "\t-C <cfgfs size>\t\t - enable 'cfg' partition; size in bytes\n"
	     "\t-c <cfgfs file>\t\t - configfs file\n"
	     "\t-B <board name>\t\t - choose firmware layout for specified board (XS2, XS5, RS, XM)\n"
	     "\t-h\t\t\t - this help\n", VERSION,
	     progname, DEFAULT_VERSION, DEFAULT_OUTPUT_FILE);
}

static void print_image_info(const image_info_t* im)
{
	int i = 0;
	INFO("Firmware version: '%s'\n"
	     "Output file: '%s'\n"
	     "Part count: %u\n",
	     im->version, im->outputfile,
	     im->part_count);

	for (i = 0; i < im->part_count; ++i)
	{
		const part_data_t* d = &im->parts[i];
		INFO(" %10s: %8ld bytes (free: %8ld)\n",
		     d->partition_name,
		     d->stats.st_size,
		     d->partition_length - d->stats.st_size);
	}
}



static u_int32_t filelength(const char* file)
{
	FILE *p;
	int ret = -1;

	if ( (p = fopen(file, "rb") ) == NULL) return (-1);

	fseek(p, 0, SEEK_END);
	ret = ftell(p);

	fclose (p);

	return (ret);
}

static int create_image_layout(const char* kernelfile, const char* rootfsfile,
    const char *cfgfsfile, char* board_name, image_info_t* im)
{
	part_data_t* kernel = &im->parts[0];
	part_data_t* rootfs = &im->parts[1];
	part_data_t* cfgfs = &im->parts[2];

	fw_layout_t* p;
	im->part_count = 0;

	p = &fw_layout_data[0];
	while ((strlen(p->name) != 0) && (strncmp(p->name, board_name, sizeof(board_name)) != 0))
		p++;
	if (p->name == NULL) {
		printf("BUG! Unable to find default fw layout!\n");
		exit(-1);
	}

	printf("board = %s\n", p->name);
	strcpy(kernel->partition_name, "kernel");
	kernel->partition_index = 1;
	kernel->partition_baseaddr = p->kern_start;
	if ( (kernel->partition_length = filelength(kernelfile)) < 0) return (-1);
	kernel->partition_memaddr = p->kern_entry;
	kernel->partition_entryaddr = p->kern_entry;
	strncpy(kernel->filename, kernelfile, sizeof(kernel->filename));
	im->part_count++;

	printf("kernel: %d bytes (base 0x%08x)\n", kernel->partition_length,
	    kernel->partition_baseaddr);
	printf("rootfs: %d bytes\n", filelength(rootfsfile));
	if (cfgfs)
		printf("cfgfs: %d bytes\n", im->cfg.size);

	printf("total: (%d bytes)\n",
	    kernel->partition_length +
	    filelength(rootfsfile) +
	    (cfgfs != NULL ? im->cfg.size : 0));

	/*
	 * This is dirty - cfgfs isn't calculated here, it's subtracted from
	 * rootfs. I'll fix that later. :)
	 */
	strcpy(rootfs->partition_name, "rootfs");
	if (filelength(rootfsfile) + kernel->partition_length > p->firmware_max_length)
		return (-2);

	rootfs->partition_index = 2;
	rootfs->partition_baseaddr = kernel->partition_baseaddr + kernel->partition_length;
	rootfs->partition_length = p->firmware_max_length - kernel->partition_length;
	rootfs->partition_memaddr = 0x00000000;
	rootfs->partition_entryaddr = 0x00000000;
	strncpy(rootfs->filename, rootfsfile, sizeof(rootfs->filename));
	im->part_count++;


	/*
	 * If cfg is enabled, subtract the cfg size from the
	 * rootfs entry.
	 */
	if (im->cfg.enable) {
		rootfs->partition_length -= im->cfg.size;
		strcpy(cfgfs->partition_name, "cfg");
		cfgfs->partition_index = 3;
		cfgfs->partition_baseaddr = kernel->partition_baseaddr +
		    kernel->partition_length + rootfs->partition_length;
		cfgfs->partition_length = im->cfg.size;
		cfgfs->partition_memaddr = 0x00000000;
		cfgfs->partition_entryaddr = 0x00000000;
		strncpy(cfgfs->filename, cfgfsfile, sizeof(rootfs->filename));
		im->part_count++;
	}

	printf("root: %d 0x%08x\n", rootfs->partition_length, rootfs->partition_baseaddr);

	if (im->cfg.enable)
		printf("cfg: %d 0x%08x\n", cfgfs->partition_length,
		    cfgfs->partition_baseaddr);

	return 0;
}

/**
 * Checks the availability and validity of all image components.
 * Fills in stats member of the part_data structure.
 */
static int validate_image_layout(image_info_t* im)
{
	int i;

	if (im->part_count == 0 || im->part_count > MAX_SECTIONS)
	{
		ERROR("Invalid part count '%d'\n", im->part_count);
		return -1;
	}

	for (i = 0; i < im->part_count; ++i)
	{
		part_data_t* d = &im->parts[i];
		int len = strlen(d->partition_name);
		if (len == 0 || len > 16)
		{
			ERROR("Invalid partition name '%s' of the part %d\n",
					d->partition_name, i);
			return -1;
		}
		if (stat(d->filename, &d->stats) < 0)
		{
			ERROR("Couldn't stat file '%s' from part '%s'\n",
				       	d->filename, d->partition_name);
			return -2;
		}
		if (d->stats.st_size == 0)
		{
			ERROR("File '%s' from part '%s' is empty!\n",
				       	d->filename, d->partition_name);
			return -3;
		}
		if (d->stats.st_size > d->partition_length) {
			ERROR("File '%s' too big (%d) - max size: 0x%08X (exceeds %lu bytes)\n",
				       	d->filename, i, d->partition_length,
					d->stats.st_size - d->partition_length);
			return -4;
		}
	}

	return 0;
}

static int build_image(image_info_t* im)
{
	char* mem;
	char* ptr;
	u_int32_t mem_size;
	FILE* f;
	int i;

	// build in-memory buffer
	mem_size = sizeof(header_t) + sizeof(signature_t);
	for (i = 0; i < im->part_count; ++i)
	{
		part_data_t* d = &im->parts[i];
		mem_size += sizeof(part_t) + d->stats.st_size + sizeof(part_crc_t);
	}

	mem = (char*)calloc(mem_size, 1);
	if (mem == NULL)
	{
		ERROR("Cannot allocate memory chunk of size '%u'\n", mem_size);
		return -1;
	}

	// write header
	write_header(mem, im->version);
	ptr = mem + sizeof(header_t);
	// write all parts
	for (i = 0; i < im->part_count; ++i)
	{
		part_data_t* d = &im->parts[i];
		int rc;
		if ((rc = write_part(ptr, d)) != 0)
		{
			ERROR("ERROR: failed writing part %u '%s'\n", i, d->partition_name);
		}
		ptr += sizeof(part_t) + d->stats.st_size + sizeof(part_crc_t);
	}
	// write signature
	write_signature(mem, mem_size - sizeof(signature_t));

	// write in-memory buffer into file
	if ((f = fopen(im->outputfile, "w")) == NULL)
	{
		ERROR("Can not create output file: '%s'\n", im->outputfile);
		return -10;
	}

	if (fwrite(mem, mem_size, 1, f) != 1)
	{
		ERROR("Could not write %d bytes into file: '%s'\n",
				mem_size, im->outputfile);
		return -11;
	}

	free(mem);
	fclose(f);
	return 0;
}


int main(int argc, char* argv[])
{
	char kernelfile[PATH_MAX];
	char rootfsfile[PATH_MAX];
	char cfgfsfile[PATH_MAX];
	char board_name[PATH_MAX];
	int o, rc;
	image_info_t im;

	memset(&im, 0, sizeof(im));
	memset(kernelfile, 0, sizeof(kernelfile));
	memset(rootfsfile, 0, sizeof(rootfsfile));
	memset(board_name, 0, sizeof(board_name));

	strcpy(im.outputfile, DEFAULT_OUTPUT_FILE);
	strcpy(im.version, DEFAULT_VERSION);

	while ((o = getopt(argc, argv, OPTIONS)) != -1)
	{
		switch (o) {
		case 'c':
			if (optarg)
				strncpy(cfgfsfile, optarg, sizeof(cfgfsfile));
			break;
		case 'C':
			if (optarg) {
				im.cfg.enable = 1;
				im.cfg.size = atoi(optarg);
			} else {
				usage(argv[0]);
				return -1;
			}
			break;
		case 'v':
			if (optarg)
				strncpy(im.version, optarg, sizeof(im.version));
			break;
		case 'o':
			if (optarg)
				strncpy(im.outputfile, optarg, sizeof(im.outputfile));
			break;
		case 'h':
			usage(argv[0]);
			return -1;
		case 'k':
			if (optarg)
				strncpy(kernelfile, optarg, sizeof(kernelfile));
			break;
		case 'r':
			if (optarg)
				strncpy(rootfsfile, optarg, sizeof(rootfsfile));
			break;
		case 'B':
			if (optarg)
				strncpy(board_name, optarg, sizeof(board_name));
			break;
		}
	}
	if (strlen(board_name) == 0)
		strcpy(board_name, "XS2"); /* default to XS2 */

	if (strlen(kernelfile) == 0)
	{
		ERROR("Kernel file is not specified, cannot continue\n");
		usage(argv[0]);
		return -2;
	}

	if (strlen(rootfsfile) == 0)
	{
		ERROR("Root FS file is not specified, cannot continue\n");
		usage(argv[0]);
		return -2;
	}

	if ((rc = create_image_layout(kernelfile, rootfsfile, cfgfsfile,
	    board_name, &im)) != 0)
	{
		ERROR("Failed creating firmware layout description - error code: %d\n", rc);
		return -3;
	}

	if ((rc = validate_image_layout(&im)) != 0)
	{
		ERROR("Failed validating firmware layout - error code: %d\n", rc);
		return -4;
	}

	print_image_info(&im);

	if ((rc = build_image(&im)) != 0)
	{
		ERROR("Failed building image file '%s' - error code: %d\n", im.outputfile, rc);
		return -5;
	}

	return 0;
}
