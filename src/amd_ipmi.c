/*
 * AMD IPMI LED control
 * Copyright (C) 2019, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/file.h>

#if _HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

#include "config.h"
#include "ibpi.h"
#include "list.h"
#include "utils.h"
#include "amd.h"
#include "ipmi.h"

static uint8_t amd_ibpi_ipmi_register[] = {
	[IBPI_PATTERN_PFA] = 0x41,
	[IBPI_PATTERN_LOCATE] = 0x42,
	[IBPI_PATTERN_FAILED_DRIVE] = 0x44,
	[IBPI_PATTERN_FAILED_ARRAY] = 0x45,
	[IBPI_PATTERN_REBUILD] = 0x46,
	[IBPI_PATTERN_HOTSPARE] = 0x47,
};

/* The path we are given should be similar to
 * /sys/devices/pci0000:e0/0000:e0:03.3/0000:e3:00.0
 *                                      ^^^^^^^^^^
 * We need to retrieve the address from the path (indicated above)
 * then use it to find the corresponding address for a slot in
 * /sys/bus/pci_slots to determine the icorrect port for the NVMe device.
 */
static int _get_ipmi_nvme_port(char *path)
{
	int rc;
	char *p, *f;
	struct list dir;
	const char *dir_path;
	char *port_name;
	int port = -1;

	p = strrchr(path, '/');
	p++;

	/* p now points to the address, remove the bits after the '.' */
	f = strchr(p, '.');
	*f = '\0';

	rc = scan_dir("/sys/bus/pci/slots", &dir);
	if (rc)
		return -1;

	list_for_each(&dir, dir_path) {
		port_name = get_text(dir_path, "address");
		if (!strcmp(port_name, p)) {
			char *dname = strrchr(dir_path, '/');

			dname++;
			port = atoi(dname);
			break;
		}
	}

	list_erase(&dir);

	/* Some platfroms require an adjustment to the port value based
	 * on how they are numbered by the BIOS.
	 */
	switch (amd_platform) {
	case AMD_PLATFORM_DAYTONA_X:
		port -= 2;
		break;
	case AMD_PLATFORM_ETHANOL_X:
		port -= 7;
		break;
	default:
		break;
	}

	/* Validate port. Some BIOSes provide provide port values that are
	 * not valid.
	 */
	if ((port < 0) || (port > 24)) {
		log_error("Invalid NVMe physical port %d\n", port);
		port = -1;
	}

	return port;
}

static int _get_ipmi_sata_port(const char *start_path)
{
	int port;
	char *p, *t;
	char path[PATH_MAX];

	strncpy(path, start_path, PATH_MAX);
	path[PATH_MAX - 1] = 0;
	t = p = strstr(path, "ata");

	if (!p)
		return -1;

	/* terminate the path after the ataXX/ part */
	p = strchr(p, '/');
	if (!p)
		return -1;
	*p = '\0';

	/* skip past 'ata' to get the ata port number */
	t += 3;
	port = strtoul(t, NULL, 10);

	return port;
}

static int _get_amd_ipmi_drive(const char *start_path,
			       struct amd_drive *drive)
{
	int found;
	char path[PATH_MAX];

	found = _find_file_path(start_path, "nvme", path, PATH_MAX);
	if (found) {
		drive->port = _get_ipmi_nvme_port(path);
		if (drive->port < 0) {
			log_error("Could not retrieve port number\n");
			return -1;
		}

		drive->drive_bay = 1 << (drive->port - 1);
		drive->dev = AMD_NVME_DEVICE;
	} else {
		int shift;

		drive->port = _get_ipmi_sata_port(start_path);
		if (drive->port < 0) {
			log_error("Could not retrieve port number\n");
			return -1;
		}

		/* IPMI control is handled through the MG9098 chips on
		 * the platform, where each MG9098 chip can control up
		 * to 8 drives. Since we can have multiple MG9098 chips,
		 * we need the drive bay relative to the set of 8 controlled
		 * by the MG9098 chip.
		 */
		shift = drive->port - 1;
		if (shift >= 8)
			shift %= 8;

		drive->drive_bay = 1 << shift;
		drive->dev = AMD_SATA_DEVICE;
	}

	if (drive->port == -1) {
		log_info("Couldn't find drive for %s\n", start_path);
		return -1;
	}

	log_debug("AMD Drive: port: %d, bay %x\n", drive->port,
		  drive->drive_bay);

	return 0;
}

static uint8_t _ipmi_platform_channel(void)
{
	uint8_t channel;

	switch (amd_platform) {
	case AMD_PLATFORM_ETHANOL_X:
		channel =  0xd;
		break;
	case AMD_PLATFORM_DAYTONA_X:
		channel = 0x17;
		break;
	default:
		channel = -1;
		log_error("AMD Platform does not have a defined IPMI channel\n");
		break;
	}

	return channel;
}

static uint8_t _ipmi_platform_slave_address(struct amd_drive *drive)
{
	uint8_t addr;

	switch (amd_platform) {
	case AMD_PLATFORM_ETHANOL_X:
		addr =  0xc0;
		break;
	case AMD_PLATFORM_DAYTONA_X:
		if (!drive) {
			/* Assume base slave address, we may not be able
			 * to retrieve a valid amd_drive yet.
			 */
			addr = 0xc0;
		} else if (drive->dev == AMD_NVME_DEVICE) {
			/* On DaytonaX systems only drive bays 19 - 24
			 * support NVMe devices so use the slave address
			 * for the corresponding MG9098 chip.
			 */
			addr = 0xc4;
		} else {
			if (drive->port <= 8)
				addr = 0xc0;
			else if (drive->port > 8 && drive->port < 17)
				addr = 0xc2;
			else
				addr = 0xc4;
		}

		break;
	default:
		addr = -1;
		log_error("AMD Platform does not have a defined IPMI slave address\n");
		break;
	}

	return addr;
}

static int _set_ipmi_register(int enable, uint8_t reg,
			      struct amd_drive *drive)
{
	int rc;
	int status, data_sz;
	uint8_t drives_status;
	uint8_t new_drives_status;
	uint8_t cmd_data[5];
	uint8_t channel, slave_addr;

	memset(cmd_data, 0, sizeof(cmd_data));

	channel = _ipmi_platform_channel();
	slave_addr = _ipmi_platform_slave_address(drive);
	if ((channel < 0) || (slave_addr < 0))
		return -1;

	cmd_data[0] = channel;
	cmd_data[1] = slave_addr;
	cmd_data[2] = 0x1;
	cmd_data[3] = reg;

	/* Find current register setting */
	status = 0;

	log_debug("Retrieving current register status\n");
	log_debug(REG_FMT_2, "channel", cmd_data[0], "slave addr", cmd_data[1]);
	log_debug(REG_FMT_2, "len", cmd_data[2], "register", cmd_data[3]);

	rc = ipmicmd(BMC_SA, 0x0, 0x6, 0x52, 4, &cmd_data, 1, &data_sz,
		     &status);
	if (rc) {
		log_error("Could not determine current register %x setting\n",
			  reg);
		return rc;
	}

	drives_status = status;

	if (enable)
		new_drives_status = drives_status | drive->drive_bay;
	else
		new_drives_status = drives_status & ~drive->drive_bay;

	/* Set the appropriate status */
	status = 0;
	cmd_data[4] = new_drives_status;

	log_debug("Updating register status: %x -> %x\n", drives_status,
		  new_drives_status);
	log_debug(REG_FMT_2, "channel", cmd_data[0], "slave addr", cmd_data[1]);
	log_debug(REG_FMT_2, "len", cmd_data[2], "register", cmd_data[3]);
	log_debug(REG_FMT_1, "status", cmd_data[4]);

	rc = ipmicmd(BMC_SA, 0x0, 0x6, 0x52, 5, &cmd_data, 1, &data_sz,
		     &status);
	if (rc) {
		log_error("Could not enable register %x\n", reg);
		return rc;
	}

	return 0;
}

static int _enable_smbus_control(struct amd_drive *drive)
{
	log_debug("Enabling SMBUS Control\n");
	return _set_ipmi_register(1, 0x3c, drive);
}

static int _enable_ibpi_state(struct amd_drive *drive, enum ibpi_pattern ibpi)
{
	log_debug("Enabling %s LED\n", ibpi2str(ibpi));
	return _set_ipmi_register(1, amd_ibpi_ipmi_register[ibpi], drive);
}

static int _disable_ibpi_state(struct amd_drive *drive, enum ibpi_pattern ibpi)
{
	log_debug("Disabling %s LED\n", ibpi2str(ibpi));
	return _set_ipmi_register(0, amd_ibpi_ipmi_register[ibpi], drive);
}

static int _disable_all_ibpi_states(struct amd_drive *drive)
{
	int rc;

	rc = _disable_ibpi_state(drive, IBPI_PATTERN_PFA);
	rc |= _disable_ibpi_state(drive, IBPI_PATTERN_LOCATE);
	rc |= _disable_ibpi_state(drive, IBPI_PATTERN_FAILED_DRIVE);
	rc |= _disable_ibpi_state(drive, IBPI_PATTERN_FAILED_ARRAY);
	rc |= _disable_ibpi_state(drive, IBPI_PATTERN_REBUILD);

	return rc;
}

int _amd_ipmi_em_enabled(const char *path)
{
	int rc;
	int status, data_sz;
	uint8_t cmd_data[4];
	uint8_t channel, slave_addr;

	channel = _ipmi_platform_channel();
	slave_addr = _ipmi_platform_slave_address(NULL);
	if ((channel < 0) || (slave_addr < 0))
		return -1;

	cmd_data[0] = channel;
	cmd_data[1] = slave_addr;
	cmd_data[2] = 0x1;
	cmd_data[3] = 0x63;

	status = 0;
	rc = ipmicmd(BMC_SA, 0x0, 0x6, 0x52, 4, &cmd_data, 1,
		     &data_sz, &status);

	if (rc) {
		log_error("Can't determine MG9098 Status\n");
		return 0;
	}

	if (status != 98) {
		log_error("Not a MG9098\n");
		return 0;
	}

	return 1;
}

int _amd_ipmi_write(struct block_device *device, enum ibpi_pattern ibpi)
{
	int rc;
	struct amd_drive drive;

	log_info("\n");
	log_info("Setting %s...", ibpi2str(ibpi));

	log_debug("Cntrl Path: %s\n", device->cntrl_path);
	log_debug("Sysfs Path: %s\n", device->sysfs_path);

	rc = _get_amd_ipmi_drive(device->cntrl_path, &drive);
	if (rc)
		return rc;

	if ((ibpi == IBPI_PATTERN_NORMAL) ||
	    (ibpi == IBPI_PATTERN_ONESHOT_NORMAL)) {
		rc = _disable_all_ibpi_states(&drive);
		return rc;
	}

	if (ibpi == IBPI_PATTERN_LOCATE_OFF) {
		rc = _disable_ibpi_state(&drive, IBPI_PATTERN_LOCATE);
		return rc;
	}

	rc = _enable_smbus_control(&drive);
	if (rc)
		return rc;

	rc = _enable_ibpi_state(&drive, ibpi);
	if (rc)
		return rc;

	return 0;
}

char *_amd_ipmi_get_path(const char *cntrl_path, const char *sysfs_path)
{
	char *p, *t;

	/* For NVMe devices we can just dup the path sysfs path */
	p = strstr(cntrl_path, "nvme");
	if (p)
		return strdup(sysfs_path);

	/* For SATA devices we need everything up to 'ataXX/' in the path */
	p = strdup(cntrl_path);
	t = strstr(p, "ata");
	t = strchr(t, '/');
	*++t = '\0';

	return p;
}

