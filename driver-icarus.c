/*
 * Copyright 2012 Luke Dashjr
 * Copyright 2012 Xiangfu <xiangfu@openmobilefree.com>
 * Copyright 2012 Andrew Smith
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

/*
 * Those code should be works fine with V2 and V3 bitstream of Icarus.
 * Operation:
 *   No detection implement.
 *   Input: 64B = 32B midstate + 20B fill bytes + last 12 bytes of block head.
 *   Return: send back 32bits immediately when Icarus found a valid nonce.
 *           no query protocol implemented here, if no data send back in ~11.3
 *           seconds (full cover time on 32bit nonce range by 380MH/s speed)
 *           just send another work.
 * Notice:
 *   1. Icarus will start calculate when you push a work to them, even they
 *      are busy.
 *   2. The 2 FPGAs on Icarus will distribute the job, one will calculate the
 *      0 ~ 7FFFFFFF, another one will cover the 80000000 ~ FFFFFFFF.
 *   3. It's possible for 2 FPGAs both find valid nonce in the meantime, the 2
 *      valid nonce will all be send back.
 *   4. Icarus will stop work when: a valid nonce has been found or 32 bits
 *      nonce range is completely calculated.
 */

#include "config.h"
#include "miner.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#ifndef WIN32
  #include <termios.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #ifndef O_CLOEXEC
    #define O_CLOEXEC 0
  #endif
#else
  #include <windows.h>
  #include <io.h>
#endif

#include "elist.h"
#include "fpgautils.h"

// *** deke ***
// The serial I/O speed - Linux uses a define 'B115200' in bits/termios.h
#define ICARUS_IO_SPEED 3000000 

// The size of a successful nonce read
#define ICARUS_WRITE_SIZE 175

// The size of a successful nonce read
#define ICARUS_READ_SIZE 17

// Ensure the sizes are correct for the Serial read
#if (ICARUS_READ_SIZE != 17) 
#error ICARUS_READ_SIZE must be 17
// *** /DM/ ***

#endif
#define ASSERT1(condition) __maybe_unused static char sizeof_uint32_t_must_be_4[(condition)?1:-1]
ASSERT1(sizeof(uint32_t) == 4);

#define ICARUS_READ_TIME(baud) ((double)ICARUS_READ_SIZE * (double)8.0 / (double)(baud))

// Fraction of a second, USB timeout is measured in
// i.e. 10 means 1/10 of a second
#define TIME_FACTOR 10
// It's 10 per second, thus value = 10/TIME_FACTOR =
#define ICARUS_READ_FAULT_DECISECONDS 1

// In timing mode: Default starting value until an estimate can be obtained
// 5 seconds allows for up to a ~840MH/s device
#define ICARUS_READ_COUNT_TIMING	(5 * TIME_FACTOR)

// For a standard Icarus REV3 (to 5 places)
// Since this rounds up a the last digit - it is a slight overestimate
// Thus the hash rate will be a VERY slight underestimate
// (by a lot less than the displayed accuracy)
#define ICARUS_REV3_HASH_TIME 0.0000000026316
#define NANOSEC 1000000000.0

// Icarus Rev3 doesn't send a completion message when it finishes
// the full nonce range, so to avoid being idle we must abort the
// work (by starting a new work) shortly before it finishes
//
// Thus we need to estimate 2 things:
//	1) How many hashes were done if the work was aborted
//	2) How high can the timeout be before the Icarus is idle,
//		to minimise the number of work started
//	We set 2) to 'the calculated estimate' - 1
//	to ensure the estimate ends before idle
//
// The simple calculation used is:
//	Tn = Total time in seconds to calculate n hashes
//	Hs = seconds per hash
//	Xn = number of hashes
//	W  = code overhead per work
//
// Rough but reasonable estimate:
//	Tn = Hs * Xn + W	(of the form y = mx + b)
//
// Thus:
//	Line of best fit (using least squares)
//
//	Hs = (n*Sum(XiTi)-Sum(Xi)*Sum(Ti))/(n*Sum(Xi^2)-Sum(Xi)^2)
//	W = Sum(Ti)/n - (Hs*Sum(Xi))/n
//
// N.B. W is less when aborting work since we aren't waiting for the reply
//	to be transferred back (ICARUS_READ_TIME)
//	Calculating the hashes aborted at n seconds is thus just n/Hs
//	(though this is still a slight overestimate due to code delays)
//

// Both below must be exceeded to complete a set of data
// Minimum how long after the first, the last data point must be
#define HISTORY_SEC 60
// Minimum how many points a single ICARUS_HISTORY should have
#define MIN_DATA_COUNT 5
// The value above used is doubled each history until it exceeds:
#define MAX_MIN_DATA_COUNT 100

static struct timeval history_sec = { HISTORY_SEC, 0 };

// Store the last INFO_HISTORY data sets
// [0] = current data, not yet ready to be included as an estimate
// Each new data set throws the last old set off the end thus
// keeping a ongoing average of recent data
#define INFO_HISTORY 10

struct ICARUS_HISTORY {
	struct timeval finish;
	double sumXiTi;
	double sumXi;
	double sumTi;
	double sumXi2;
	uint32_t values;
	uint32_t hash_count_min;
	uint32_t hash_count_max;
};

// *** deke ***
unsigned int fpga_freq[MAX_FPGA];
int nonce_found[MAX_FPGA]; 
int nonce_counter = 0; 
unsigned int nonce_d = 0;
int fpga_volt_1[MAX_FPGA];
int fpga_volt_2[MAX_FPGA];
int fpga_volt_3[MAX_FPGA];
float fpga_temp_1[MAX_FPGA];
float fpga_temp_2[MAX_FPGA];
float fpga_temp_3[MAX_FPGA];
// *** /DM/ ***

enum timing_mode { MODE_DEFAULT, MODE_SHORT, MODE_LONG, MODE_VALUE };

static const char *MODE_DEFAULT_STR = "default";
static const char *MODE_SHORT_STR = "short";
static const char *MODE_LONG_STR = "long";
static const char *MODE_VALUE_STR = "value";
static const char *MODE_UNKNOWN_STR = "unknown";

// Tyler Edit
#define CORE_MASK 0x1FFFFFFF // 8 core version, hardcoded as requested
#define MAX_CORES 256
#define MAX_CORE_HISTORY_SAMPLES 10
#define HASHRATE_AVG_OVER_SECS 5
#define SECONDS_PER_NONCE_RANGE 10

struct CORE_HISTORY_SAMPLE {
	struct timeval sample_time;
	uint32_t hashrate;
};

struct CORE_HISTORY {
	struct CORE_HISTORY_SAMPLE samples[MAX_CORE_HISTORY_SAMPLES];
};
//

struct ICARUS_INFO {
	// time to calculate the golden_ob
	uint64_t golden_hashes;
	struct timeval golden_tv;

	struct ICARUS_HISTORY history[INFO_HISTORY+1];
	uint32_t min_data_count;

	// seconds per Hash
	double Hs;
	int read_count;

	enum timing_mode timing_mode;
	bool do_icarus_timing;

	double fullnonce;
	int count;
	double W;
	uint32_t values;
	uint64_t hash_count_range;

	// Determine the cost of history processing
	// (which will only affect W)
	uint64_t history_count;
	struct timeval history_time;

	// icarus-options
	int baud;
	int work_division;
	int fpga_count;
	uint32_t nonce_mask;
	
	//Tyler Edit
	bool work_changed;
	bool first_timeout;
	uint64_t enabled_cores;
	int active_core_count;
	struct timeval work_start;
	struct timeval last_interval_timeout;
	struct timeval prev_hashcount_return;
	uint64_t prev_hashrate;
	uint64_t prev_hashcount;
	uint8_t expected_cores;
	struct CORE_HISTORY core_history[MAX_CORES];
	//
};

#define END_CONDITION 0x0000ffff

// One for each possible device
static struct ICARUS_INFO **icarus_info;

// Looking for options in --icarus-timing and --icarus-options:
//
// Code increments this each time we start to look at a device
// However, this means that if other devices are checked by
// the Icarus code (e.g. BFL) they will count in the option offset
//
// This, however, is deterministic so that's OK
//
// If we were to increment after successfully finding an Icarus
// that would be random since an Icarus may fail and thus we'd
// not be able to predict the option order
//
// This also assumes that serial_detect() checks them sequentially
// and in the order specified on the command line
//
static int option_offset = -1;
static int clock_offset = -1;		

struct device_drv icarus_drv;

static void rev(unsigned char *s, size_t l)
{
	size_t i, j;
	unsigned char t;

	for (i = 0, j = l - 1; i < j; i++, j--) {
		t = s[i];
		s[i] = s[j];
		s[j] = t;
	}
}

#define icarus_open2(devpath, baud, purge)  serial_open(devpath, baud, ICARUS_READ_FAULT_DECISECONDS, purge)
#define icarus_open(devpath, baud)  icarus_open2(devpath, baud, false)

#define ICA_GETS_ERROR -1
#define ICA_GETS_OK 0
#define ICA_GETS_RESTART 1
#define ICA_GETS_TIMEOUT 2

static int icarus_gets(unsigned char *buf, int fd, struct timeval *tv_finish, struct thr_info *thr, int read_count)
{
	ssize_t ret = 0;
	int rc = 0;
	int read_amount = ICARUS_READ_SIZE;
	bool first = true;

	// Read reply 1 byte at a time to get earliest tv_finish
	while (true) {
		ret = read(fd, buf, 1);
		if (ret < 0)
			return ICA_GETS_ERROR;

		if (first)
			cgtime(tv_finish);

		if (ret >= read_amount)
		{		
			return ICA_GETS_OK;			
		}

		if (ret > 0) {
			buf += ret;
			read_amount -= ret;
			first = false;
			continue;
		}
			
		rc++;
		if (rc >= read_count) {			
			if (opt_debug) {				
				applog(LOG_DEBUG,
					"Icarus Read: No data in %.2f seconds",
					(float)rc/(float)TIME_FACTOR);								
			}
			return ICA_GETS_TIMEOUT;
		}

		if (thr && thr->work_restart) {
			if (opt_debug) {
				applog(LOG_DEBUG,
					"Icarus Read: Work restart at %.2f seconds",
					(float)(rc)/(float)TIME_FACTOR);
			}
			return ICA_GETS_RESTART;
		}
	}
}

static int icarus_write(int fd, const void *buf, size_t bufLen)
{
	size_t ret;

	ret = write(fd, buf, bufLen);
	if (unlikely(ret != bufLen))
		return 1;

	return 0;
}

// *** deke ***
static bool cairnsmore_send_cmd(int fd, uint8_t cmd, uint16_t data, bool probe)
{
	unsigned int freq;
  
	freq = (data * 5) / 2;
	unsigned char pll[ICARUS_WRITE_SIZE] = { 0xaa,0xaa,0xaa,0xaa,0x12,0x08,0x00,0x85,0xed,0xf7,0xff,0x7a,0xbb,0xbb,0xbb,0xbb,
							   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
							   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
							   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
							   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
							   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
							   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
							   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,				
							   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,				
							   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,				
							   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
	
	if(freq < 425) // 400 MHz 
	{		
		pll[4] = 0x12; pll[5] = 0x08; pll[6] = 0x00; pll[7] = 0x85; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 450) // 425 MHz 
	{		
		pll[4] = 0x12; pll[5] = 0x09; pll[6] = 0x00; pll[7] = 0x8e; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 475) // 450 MHz 
	{	
		pll[4] = 0x12; pll[5] = 0x49; pll[6] = 0x00; pll[7] = 0x96; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 500) // 475 MHz 
	{
		pll[4] = 0x12; pll[5] = 0x4a; pll[6] = 0x00; pll[7] = 0x9e; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 525) // 500 MHz 
	{
		pll[4] = 0x12; pll[5] = 0x8a; pll[6] = 0x00; pll[7] = 0xa7; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 550) // 525 MHz 
	{
		pll[4] = 0x12; pll[5] = 0x8b; pll[6] = 0x00; pll[7] = 0xaf; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 575) // 550 MHz 
	{
		pll[4] = 0x12; pll[5] = 0xcb; pll[6] = 0x00; pll[7] = 0xb7; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 600) // 575 MHz 
	{
		pll[4] = 0x12; pll[5] = 0xcc; pll[6] = 0x00; pll[7] = 0xc0; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 625) // 600 MHz 
	{
		pll[4] = 0x13; pll[5] = 0x0c; pll[6] = 0x00; pll[7] = 0xc8; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 650) // 625 MHz 
	{
		pll[4] = 0x13; pll[5] = 0x0d; pll[6] = 0x00; pll[7] = 0xd0; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 675) // 650 MHz 
	{
		pll[4] = 0x13; pll[5] = 0x4d; pll[6] = 0x00; pll[7] = 0xd9; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 700) // 675 MHz 
	{
		pll[4] = 0x13; pll[5] = 0x4e; pll[6] = 0x00; pll[7] = 0xe1; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 725) // 700 MHz 
	{
		pll[4] = 0x13; pll[5] = 0x8e; pll[6] = 0x00; pll[7] = 0xe9; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 750) // 725 MHz 
	{
		pll[4] = 0x13; pll[5] = 0x8f; pll[6] = 0x00; pll[7] = 0xf2; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 775) // 750 MHz 
	{
		pll[4] = 0x13; pll[5] = 0xcf; pll[6] = 0x00; pll[7] = 0xfa; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 800) // 775 MHz 
	{
		pll[4] = 0x13; pll[5] = 0xd0; pll[6] = 0x01; pll[7] = 0x02; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 825) // 800 MHz 
	{
		pll[4] = 0x14; pll[5] = 0x10; pll[6] = 0x01; pll[7] = 0x0b; pll[8] = 0x00; pll[9] = 0xc3; pll[10] = 0x00; pll[11] = 0x00;
	}
	else if(freq < 850) // 825 MHz 
	{
		pll[4] = 0x14; pll[5] = 0x11; pll[6] = 0x01; pll[7] = 0x13; pll[8] = 0x01; pll[9] = 0x86; pll[10] = 0x00; pll[11] = 0x40;
	}
	else if(freq < 875) // 850 MHz 
	{
		pll[4] = 0x14; pll[5] = 0x51; pll[6] = 0x01; pll[7] = 0x1b; pll[8] = 0x01; pll[9] = 0x86; pll[10] = 0x00; pll[11] = 0x40;
	}
	else if(freq < 900) // 875 MHz 
	{
		pll[4] = 0x14; pll[5] = 0x52; pll[6] = 0x01; pll[7] = 0x24; pll[8] = 0x01; pll[9] = 0x86; pll[10] = 0x00; pll[11] = 0x40;
	}
	else if(freq < 925) // 900 MHz 
	{
		pll[4] = 0x14; pll[5] = 0x92; pll[6] = 0x01; pll[7] = 0x2c; pll[8] = 0x01; pll[9] = 0x86; pll[10] = 0x00; pll[11] = 0x40;
	}
	else if(freq < 950) // 925 MHz 
	{
		pll[4] = 0x14; pll[5] = 0x93; pll[6] = 0x01; pll[7] = 0x34; pll[8] = 0x01; pll[9] = 0x86; pll[10] = 0x00; pll[11] = 0x40;
	}
	else if(freq < 975) // 950 MHz 
	{
		pll[4] = 0x14; pll[5] = 0xd3; pll[6] = 0x01; pll[7] = 0x3d; pll[8] = 0x01; pll[9] = 0x86; pll[10] = 0x00; pll[11] = 0x40;
	}
	else if(freq < 1000) // 975 MHz 
	{
		pll[4] = 0x14; pll[5] = 0xd4; pll[6] = 0x01; pll[7] = 0x45; pll[8] = 0x01; pll[9] = 0x86; pll[10] = 0x00; pll[11] = 0x40;
	}
	else // 1000 MHz 
	{
		pll[4] = 0x15; pll[5] = 0x14; pll[6] = 0x01; pll[7] = 0x4d; pll[8] = 0x01; pll[9] = 0x86; pll[10] = 0x00; pll[11] = 0x40;
	}
    
	return write(fd, pll, sizeof(pll)) == sizeof(pll);
}
// *** /DM/ ***

#define icarus_close(fd) close(fd)

static void do_icarus_close(struct thr_info *thr)
{
	struct cgpu_info *icarus = thr->cgpu;
	icarus_close(icarus->device_fd);
	icarus->device_fd = -1;
}

static const char *timing_mode_str(enum timing_mode timing_mode)
{
	switch(timing_mode) {
	case MODE_DEFAULT:
		return MODE_DEFAULT_STR;
	case MODE_SHORT:
		return MODE_SHORT_STR;
	case MODE_LONG:
		return MODE_LONG_STR;
	case MODE_VALUE:
		return MODE_VALUE_STR;
	default:
		return MODE_UNKNOWN_STR;
	}
}

static void set_timing_mode(int this_option_offset, struct cgpu_info *icarus)
{
	struct ICARUS_INFO *info = icarus_info[icarus->device_id];
	double Hs;
	char buf[BUFSIZ+1];
	char *ptr, *comma, *eq;
	size_t max;
	int i;

	if (opt_icarus_timing == NULL)
		buf[0] = '\0';
	else {
		ptr = opt_icarus_timing;
		for (i = 0; i < this_option_offset; i++) {
			comma = strchr(ptr, ',');
			if (comma == NULL)
				break;
			ptr = comma + 1;
		}

		comma = strchr(ptr, ',');
		if (comma == NULL)
			max = strlen(ptr);
		else
			max = comma - ptr;

		if (max > BUFSIZ)
			max = BUFSIZ;
		strncpy(buf, ptr, max);
		buf[max] = '\0';
	}

	info->Hs = 0;
	info->read_count = 0;

	if (strcasecmp(buf, MODE_SHORT_STR) == 0) {
		info->Hs = ICARUS_REV3_HASH_TIME;
		info->read_count = ICARUS_READ_COUNT_TIMING;

		info->timing_mode = MODE_SHORT;
		info->do_icarus_timing = true;
	} else if (strcasecmp(buf, MODE_LONG_STR) == 0) {
		info->Hs = ICARUS_REV3_HASH_TIME;
		info->read_count = ICARUS_READ_COUNT_TIMING;

		info->timing_mode = MODE_LONG;
		info->do_icarus_timing = true;
	} else if ((Hs = atof(buf)) != 0) {
		info->Hs = Hs / NANOSEC;
		info->fullnonce = info->Hs * (((double)0xffffffff) + 1);

		if ((eq = strchr(buf, '=')) != NULL)
			info->read_count = atoi(eq+1);

		if (info->read_count < 1)
			info->read_count = (int)(info->fullnonce * TIME_FACTOR) - 1;

		if (unlikely(info->read_count < 1))
			info->read_count = 1;

		info->timing_mode = MODE_VALUE;
		info->do_icarus_timing = false;
	} else {
		// Anything else in buf just uses DEFAULT mode

		info->Hs = ICARUS_REV3_HASH_TIME;
		info->fullnonce = info->Hs * (((double)0xffffffff) + 1);

		if ((eq = strchr(buf, '=')) != NULL)
			info->read_count = atoi(eq+1);

		if (info->read_count < 1)
			info->read_count = (int)(info->fullnonce * TIME_FACTOR) - 1;

		info->timing_mode = MODE_DEFAULT;
		info->do_icarus_timing = false;
	}

	info->min_data_count = MIN_DATA_COUNT;

	applog(LOG_DEBUG, "Cairnsmore1: Init: %d mode=%s read_count=%d Hs=%e",
		icarus->device_id, timing_mode_str(info->timing_mode), info->read_count, info->Hs);
}

static uint32_t mask(int work_division)
{
	char err_buf[BUFSIZ+1];
	uint32_t nonce_mask = 0x7fffffff;

	// yes we can calculate these, but this way it's easy to see what they are
	switch (work_division) {
	case 1:
		nonce_mask = 0xffffffff;
		break;
	case 2:
		nonce_mask = 0x7fffffff;
		break;
	case 4:
		nonce_mask = 0x3fffffff;
		break;
	case 8:
		nonce_mask = 0x1fffffff;
		break;
	default:
		sprintf(err_buf, "Invalid2 icarus-options for work_division (%d) must be 1, 2, 4 or 8", work_division);
		quit(1, err_buf);
	}

	return nonce_mask;
}

static void get_options(int this_option_offset, int *baud, int *work_division, int *fpga_count)
{
	char err_buf[BUFSIZ+1];
	char buf[BUFSIZ+1];
	char *ptr, *comma, *colon, *colon2;
	size_t max;
	int i, tmp;

	if (opt_icarus_options == NULL)
		buf[0] = '\0';
	else {
		ptr = opt_icarus_options;
		for (i = 0; i < this_option_offset; i++) {
			comma = strchr(ptr, ',');
			if (comma == NULL)
				break;
			ptr = comma + 1;
		}

		comma = strchr(ptr, ',');
		if (comma == NULL)
			max = strlen(ptr);
		else
			max = comma - ptr;

		if (max > BUFSIZ)
			max = BUFSIZ;
		strncpy(buf, ptr, max);
		buf[max] = '\0';
	}

	*baud = ICARUS_IO_SPEED;
	*work_division = 2;
	*fpga_count = 2;		

	if (*buf) {
		colon = strchr(buf, ':');
		if (colon)
			*(colon++) = '\0';

		if (*buf) {
			tmp = atoi(buf);
			// *** deke ***      
			switch (tmp) {			
				case 3000000: 
					*baud = 3000000;
				break;   
			
				default:			
					sprintf(err_buf, "Invalid icarus-options for baud (%s) must be 3000000", buf);								
					quit(1, err_buf);
			}
			// *** /DM/ ***
		}

		if (colon && *colon) {
			colon2 = strchr(colon, ':');
			if (colon2)
				*(colon2++) = '\0';

			if (*colon) {
				tmp = atoi(colon);
				if (tmp == 1 || tmp == 2 || tmp == 4 || tmp == 8) {
					*work_division = tmp;
					*fpga_count = tmp;	// default to the same
				} else {
					sprintf(err_buf, "Invalid icarus-options for work_division (%s) must be 1, 2, 4 or 8", colon);
					quit(1, err_buf);
				}
			}

			if (colon2 && *colon2) {
				tmp = atoi(colon2);
				if (tmp > 0 && tmp <= *work_division)
					*fpga_count = tmp;
				else {
					sprintf(err_buf, "Invalid icarus-options for fpga_count (%s) must be >0 and <=work_division (%d)", colon2, *work_division);
					quit(1, err_buf);
				}
			}
		}
	}
}

static void get_clocks(int this_option_offset, int *cainsmore_clock)
{
	char err_buf[BUFSIZ+1];
	char buf[BUFSIZ+1];
	char *ptr, *comma;
	size_t max;
	int i, tmp;

	if (opt_cainsmore_clock == NULL)
		buf[0] = '\0';
	else {
		ptr = opt_cainsmore_clock;
		for (i = 0; i < this_option_offset; i++) {
			comma = strchr(ptr, ',');
			if (comma == NULL)
				break;
			ptr = comma + 1;
		}

		comma = strchr(ptr, ',');
		if (comma == NULL)
			max = strlen(ptr);
		else
			max = comma - ptr;

		if (max > BUFSIZ)
			max = BUFSIZ;
		strncpy(buf, ptr, max);
		buf[max] = '\0';
	}

	if (*buf) {
		tmp = atoi(buf);

		// *** deke ***		
		if (tmp >= 400 && tmp <= 800)
			*cainsmore_clock = tmp * 2 / 5;	// NB 2.5Mhz units
		else {			
			sprintf(err_buf, "Invalid VCU1525 clock must be between 400 and 1000MHz", buf);
			quit(1, err_buf);
		}
		// *** /DM/ ***
	}
}

static bool icarus_detect_one(const char *devpath)
{
	int this_option_offset = ++option_offset;
	int this_clock_offset = ++clock_offset;

	struct ICARUS_INFO *info;
	struct timeval tv_start, tv_finish;
	int fd;

    // *** deke ***
	const char golden_ob[] =				
		"00000000000000000000000000000000"
		"00000000000000000000000000000000"
		"00000000000000000000000000000000"
		"00000000000000000000000000000000"
		"00000000000000000000000000000000"
		"00000000000000000000000000000000"
		"00000000000000000000000000000000"
		"00000000000000000000000000000000"
		"00000000000000000000000000000000"
		"00000000000000000000000000000000"
		"0000000000000000000000000000000";

	const char golden_nonce[] = "bb00000000000000000000000004000000";
	const uint32_t golden_nonce_val = 0x00000000;	
	
	unsigned char ob_bin[ICARUS_WRITE_SIZE], nonce_bin[ICARUS_READ_SIZE];
	// *** /DM/ ***
	
	char *nonce_hex;

	int baud, work_division, fpga_count;

	get_options(this_option_offset, &baud, &work_division, &fpga_count);

	int cainsmore_clock_speed = 70;		
	get_clocks(this_option_offset, &cainsmore_clock_speed);	

	applog(LOG_DEBUG, "Icarus Detect: Attempting to open %s", devpath);

	fd = icarus_open2(devpath, baud, true);
	if (unlikely(fd == -1)) {
		applog(LOG_ERR, "Icarus Detect: Failed to open %s", devpath);
		return false;
	}

	hex2bin(ob_bin, golden_ob, sizeof(ob_bin));
	icarus_write(fd, ob_bin, sizeof(ob_bin));
	cgtime(&tv_start);

	memset(nonce_bin, 0, sizeof(nonce_bin));
	icarus_gets(nonce_bin, fd, &tv_finish, NULL, 6);
	
	int cainsmore_ret = cairnsmore_send_cmd(fd, 0, cainsmore_clock_speed, false);

	icarus_close(fd);

	nonce_hex = bin2hex(nonce_bin, sizeof(nonce_bin));
	// *** deke ***	
	//if (strncmp(nonce_hex, golden_nonce, 8)) {
	if (nonce_bin[0] != 0xbb) {// && strncmp(&nonce_hex[4], &golden_nonce[4], 30)) {
		applog(LOG_ERR,
			//"Icarus Detect: "
			"Xilinx VCU1525 Detect: "
			// *** /DM/ ***
			"Test failed at %s: get %s, should: %s",
			devpath, nonce_hex, golden_nonce);
#if 0	// ENABLE/DISABLE TEST
		free(nonce_hex);
		return false;
#endif
	}
	applog(LOG_DEBUG,
		"Icarus Detect: "
		"Test succeeded at %s: got %s",
			devpath, nonce_hex);
	free(nonce_hex);

	/* We have a real Icarus! */
	struct cgpu_info *icarus;
	icarus = calloc(1, sizeof(struct cgpu_info));
	icarus->drv = &icarus_drv;
	icarus->device_path = strdup(devpath);
	icarus->device_fd = -1;
	icarus->threads = 1;
	add_cgpu(icarus);
	icarus_info = realloc(icarus_info, sizeof(struct ICARUS_INFO *) * (total_devices + 1));

	applog(LOG_INFO, "Found Icarus at %s, mark as %d",
		devpath, icarus->device_id);

	// *** deke ***
    unsigned int freq;  
     
	freq = ((cainsmore_clock_speed * 5) / 2);
    
	if(freq < 425) 
		fpga_freq[icarus->device_id] = 400;
	else if(freq < 450) 
		fpga_freq[icarus->device_id] = 425;
	else if(freq < 475)
		fpga_freq[icarus->device_id] = 450;
	else if(freq < 500)
		fpga_freq[icarus->device_id] = 475;
	else if(freq < 525)
		fpga_freq[icarus->device_id] = 500;
	else if(freq < 550)
		fpga_freq[icarus->device_id] = 525;
	else if(freq < 575)
		fpga_freq[icarus->device_id] = 550;
	else if(freq < 600)
		fpga_freq[icarus->device_id] = 575;
	else if(freq < 625)
		fpga_freq[icarus->device_id] = 600;
	else if(freq < 650)
		fpga_freq[icarus->device_id] = 625;
	else if(freq < 675)
		fpga_freq[icarus->device_id] = 650;
	else if(freq < 700)
		fpga_freq[icarus->device_id] = 675;
	else if(freq < 725)
		fpga_freq[icarus->device_id] = 700;
	else if(freq < 750)
		fpga_freq[icarus->device_id] = 725;
	else if(freq < 775)
		fpga_freq[icarus->device_id] = 750;
	else if(freq < 800)
		fpga_freq[icarus->device_id] = 775;
	else if(freq < 825)
		fpga_freq[icarus->device_id] = 800;
	else if(freq < 850)
		fpga_freq[icarus->device_id] = 825;
	else if(freq < 875)
		fpga_freq[icarus->device_id] = 850;
	else if(freq < 900)
		fpga_freq[icarus->device_id] = 875;
	else if(freq < 925)
		fpga_freq[icarus->device_id] = 900;
	else if(freq < 950)
		fpga_freq[icarus->device_id] = 925;
	else if(freq < 975)
		fpga_freq[icarus->device_id] = 950;
	else if(freq < 1000)
		fpga_freq[icarus->device_id] = 975;
	else 
		fpga_freq[icarus->device_id] = 1000;

    fpga_freq[icarus->device_id] = ((cainsmore_clock_speed * 5) / 2);
	applog(LOG_WARNING, "Xilinx VCU1525:[id=%d][clock=%dMHz][baud=%dBd]", icarus->device_id, ((cainsmore_clock_speed * 5) / 2), baud);
	// *** /DM/ ***

	// Since we are adding a new device on the end it needs to always be allocated
	icarus_info[icarus->device_id] = (struct ICARUS_INFO *)malloc(sizeof(struct ICARUS_INFO));
	if (unlikely(!(icarus_info[icarus->device_id])))
		quit(1, "Failed to malloc ICARUS_INFO");

	info = icarus_info[icarus->device_id];

	// Initialise everything to zero for a new device
	memset(info, 0, sizeof(struct ICARUS_INFO));

	info->baud = baud;
	info->work_division = work_division;
	info->fpga_count = fpga_count;
	info->nonce_mask = mask(work_division);
	info->enabled_cores = 0;
	info->active_core_count = 0;

	memset(info->core_history, 0, sizeof(info->core_history));
	cgtime(&info->core_history[0].samples[0].sample_time);
	info->core_history[0].samples[0].hashrate = cainsmore_clock_speed*5/2*1000000;
	for (int i = 1; i < MAX_CORES; ++i)
		memcpy(&info->core_history[i].samples[0], &info->core_history[0].samples[0], sizeof(struct CORE_HISTORY_SAMPLE));

	if (nonce_bin[0] == 0xbb)	
	{
		info->expected_cores = nonce_bin[1];
		applog(LOG_ERR, "Bitstream is for %d cores.", info->expected_cores);
	}
	else
	{
		info->expected_cores = 9;
		applog(LOG_ERR, "Can't confirm bitstream core count. Expecting %d cores.", info->expected_cores);
	}

	info->golden_hashes = (golden_nonce_val & info->nonce_mask) * fpga_count;
	timersub(&tv_finish, &tv_start, &(info->golden_tv));

	set_timing_mode(this_option_offset, icarus);

	return true;
}

static void icarus_detect()
{
	serial_detect(&icarus_drv, icarus_detect_one);
}

static bool icarus_prepare(struct thr_info *thr)
{
	struct cgpu_info *icarus = thr->cgpu;

	struct timeval now;

	icarus->device_fd = -1;

	int fd = icarus_open(icarus->device_path, icarus_info[icarus->device_id]->baud);
	if (unlikely(-1 == fd)) {
		applog(LOG_ERR, "Failed to open Icarus on %s",
		       icarus->device_path);
		return false;
	}

	icarus->device_fd = fd;

	applog(LOG_INFO, "Opened Icarus on %s", icarus->device_path);
	cgtime(&now);
	get_datestamp(icarus->init, &now);

	return true;
}

// Tyler Edit
void icarus_statline(char *logline, struct cgpu_info *cgpu)
{
	char str_active_cores[256];
	struct ICARUS_INFO *info;
	info = icarus_info[cgpu->device_id];

	for (int i = 0; i < info->expected_cores; i++)
	{
		if ((info->enabled_cores >> i) & 0x1)
			str_active_cores[i] = '1';
		else
			str_active_cores[i] = '0';

	}
	str_active_cores[info->expected_cores] = 0;

	sprintf(logline, ", %d Active Cores: %s", info->active_core_count, str_active_cores);

}

bool icarus_prepare_work(struct thr_info __maybe_unused *thr, struct work __maybe_unused *work)
{
	struct cgpu_info *icarus;
	struct ICARUS_INFO *info;
	icarus = thr->cgpu;
	info = icarus_info[icarus->device_id];
	info->work_changed = true;
	info->first_timeout = true;
	return true;
}

void disable_core(struct ICARUS_INFO *info, uint8_t core_num)
{
	// if already enabled, return
	if (((info->enabled_cores >> core_num) & 0x1) == 0)
		return;

	if (info->active_core_count == 0)
		applog(LOG_ERR, "Active core count underrun for enabled_cores: %u, trying to disable core %u", info->enabled_cores, core_num);
	info->active_core_count --; 

	info->enabled_cores &= ~(0x1 << core_num);
}

void enable_core(struct ICARUS_INFO *info, uint8_t core_num)
{
	// if already enabled, return
	if ((info->enabled_cores >> core_num) & 0x1)
		return;

	info->active_core_count ++;
	if (info->active_core_count > 18)
		applog(LOG_ERR, "Active core count overrun for enabled_cores: %u, trying to enable core %u", info->enabled_cores, core_num);
	info->enabled_cores |= 0x1 << core_num;
	if (core_num + 1 > info->expected_cores)
		info->expected_cores = core_num + 1;
}

uint32_t new_hashcount_since_last_return(struct ICARUS_INFO *info, struct timeval *until)
{
	struct timeval elapsed;

	timersub(until, &info->prev_hashcount_return, &elapsed);
	copy_time(&info->prev_hashcount_return, until);
	uint32_t device_hashcount_this_period = (double)info->prev_hashrate * ((double)(elapsed.tv_sec) + ((double)(elapsed.tv_usec))/((double)1000000)); 

	info->prev_hashcount += device_hashcount_this_period;

	return device_hashcount_this_period;
}

// force_retain_all - if true, forces the update to keep all previous updates, otherwise overwrites 
// the previous update if it is from the same work.
// caveat: the check for whether a previous update is from the same work is based on whether an update's
// sample_time is since the new work start time. It is possible to submit a sample from old work with a 
// time later than the start of new work and therefore this could introduce some inaccuracy in 
// later hashrate calculations. Recommendation is to not submit old work for updating the history.
void update_core_history(struct ICARUS_INFO *info, uint8_t core_num, struct timeval *sample_time, uint32_t hashrate, bool force_retain_all)
{
	struct CORE_HISTORY *history = &info->core_history[core_num];
	struct CORE_HISTORY_SAMPLE *prev_sample = &history->samples[0];
	double work_start = (double)(info->work_start.tv_sec) + ((double)(info->work_start.tv_usec))/((double)1000000); 
	double sample_finish= (double)(sample_time->tv_sec) + ((double)(sample_time->tv_usec))/((double)1000000); 
	double prev_sample_finish= (double)(prev_sample->sample_time.tv_sec) + ((double)(prev_sample->sample_time.tv_usec))/((double)1000000); 
	
	// We will only log a new sample per work normally, since the most accurate hashrate for a given work
	// is the last update. All previous updates were for a smaller portion of the FPGA's 
	// continuous processing of the given work.
	if (force_retain_all || work_start > prev_sample_finish)
	{
		for (int i = MAX_CORE_HISTORY_SAMPLES-1; i > 0; --i)
			memcpy(&history->samples[i-1], &history->samples[i], sizeof(struct CORE_HISTORY_SAMPLE));
	}
	copy_time(&prev_sample->sample_time, sample_time);
	prev_sample->hashrate = hashrate;
}

// 'from_time' is the time reference we are going to look from and go back 'seconds' to average over.
uint32_t get_core_hashrate_average(struct ICARUS_INFO *info, uint8_t core_num, double seconds, struct timeval *from_time)
{
	uint64_t hashrate_sum = 0;
	uint16_t sample_count = 0;
	struct timeval elapsed;
	struct CORE_HISTORY *history = &info->core_history[core_num];
	struct CORE_HISTORY_SAMPLE *sample = history->samples;

	for (int i=0; i < MAX_CORE_HISTORY_SAMPLES; ++i, ++sample)
	{
		if (sample->sample_time.tv_sec == 0 && sample->sample_time.tv_usec == 0)
			break;

		timersub(from_time, &sample->sample_time, &elapsed);
		double time_ago = (double)(elapsed.tv_sec) + ((double)(elapsed.tv_usec))/((double)1000000);
		if (time_ago > seconds)
			break;

		sample_count ++;
		hashrate_sum += sample->hashrate;
	}
	if (sample_count == 0)
		return 0;

	return hashrate_sum / sample_count;
}

// 'from_time' is the time reference we are going to look from and go back 'seconds' to average over.
void disable_inactive_cores_since_work_start(struct ICARUS_INFO *info)
{

	for (int i=0; i < info->expected_cores; i ++)
	{
		double work_start = (double)(info->work_start.tv_sec) + ((double)(info->work_start.tv_usec))/((double)1000000); 
		struct timeval *sample_time = &info->core_history[i].samples[0].sample_time;
		double sample_finish = (double)(sample_time->tv_sec) + ((double)(sample_time->tv_usec))/((double)1000000); 

		if (sample_finish < work_start)
			disable_core(info, i);
	}
}

// 'from_time' is the time reference we are going to look from and go back 'seconds' to average over.
uint64_t get_fastest_core_hashrate_this_work(struct ICARUS_INFO *info, struct timeval *from_time)
{
	uint64_t fastest_hashrate = 0;
	struct timeval elapsed;

	timersub(from_time, &info->work_start, &elapsed);
	double seconds = (double)(elapsed.tv_sec) + ((double)(elapsed.tv_usec))/((double)1000000); 

	for (int i=0; i < info->expected_cores; i ++)
	{
		uint32_t hashrate = info->core_history[i].samples[0].hashrate;
		if (hashrate == 0)
			hashrate = get_core_hashrate_average(info, i, seconds, from_time);

//		applog(LOG_ERR, "Core %d hashrate avg = %u", i, hashrate);
		if (hashrate > fastest_hashrate)
			fastest_hashrate = hashrate;
	}
	return fastest_hashrate;
}

// 'from_time' is the time reference we are going to look from and go back 'seconds' to average over.
uint64_t get_device_hashrate_average(struct ICARUS_INFO *info, uint32_t seconds, struct timeval *from_time, bool disable_inactive)
{
	uint64_t hashrate_sum = 0;

	for (int i=0; i < info->expected_cores; i ++)
	{
		uint32_t hashrate = get_core_hashrate_average(info, i, seconds, from_time);
//		applog(LOG_ERR, "Core %d hashrate avg = %u", i, hashrate);
		if (hashrate != 0)
		{
			hashrate_sum += hashrate;
			if ((info->enabled_cores >> i) & 0x1 == 0)
				enable_core(info, i);
		}
		else if (disable_inactive)
			disable_core(info, i);
	}
	return hashrate_sum;
}

bool hashcount_beyond_new_work_threshold(struct ICARUS_INFO *info, uint32_t hash_count)
{
	return (hash_count > (double)0xffffffff*(double)info->expected_cores*0.75);
}

// Use the fastest core to determine the estimate of what hashcount we are at and signal to move
// to new work if beyond a threshold
bool beyond_new_work_threshold(struct ICARUS_INFO *info, struct timeval *until_time)
{
	struct timeval elapsed;
	timersub(until_time, &info->work_start, &elapsed);
	double seconds = (double)(elapsed.tv_sec) + ((double)(elapsed.tv_usec))/((double)1000000); 
	uint64_t fastest_core_hashrate = get_fastest_core_hashrate_this_work(info, until_time);
	uint64_t core_hashcount = fastest_core_hashrate * seconds;
	return (core_hashcount > (double)0xffffffff*0.75*info->expected_cores);
}

uint64_t get_hashcount_estimate_for_return(struct ICARUS_INFO *info, struct work *work, struct timeval *until_time)
{
	struct timeval elapsed;


	timersub(until_time, &info->prev_hashcount_return, &elapsed);
	uint64_t hash_count = (double)info->prev_hashrate * ((double)(elapsed.tv_sec) + ((double)(elapsed.tv_usec))/((double)1000000)); 

	info->prev_hashcount += hash_count;

	if (beyond_new_work_threshold(info, until_time))
	{
//		applog(LOG_ERR, "Forcing abandon_work to avoid idle FPGA. Previous hashrate: %lu, Elapsed time: %f, Hash Count: %09lX", info->prev_hashrate, (double)(elapsed.tv_sec) + ((double)(elapsed.tv_usec))/((double)1000000), info->prev_hashcount);
		work->blk.nonce = 0xffffffff;
	}
	copy_time(&info->prev_hashcount_return, until_time);

	return hash_count;
}

bool is_response_for_current_work(uint8_t *nonce_bin, struct work *work)
{
	return (memcmp(&nonce_bin[9], work->data, 3) == 0);
}

//
// since_time - we use this time to look for activity from cores since then. If no activity
// we will assume the core is inactive. This reference point should be when new work begins.
// Since all cores report in with a counter, the next time we have a timeout we can assume they
// all should have reported in. So, this function should only be called once per work.
// sample_time - if a core goes inactive we need the time that we should add a 0 hashrate sample at
//
// return - whether an update occurred
//
bool update_active_core(struct ICARUS_INFO *info, struct timeval *since_time, uint16_t core_num, struct timeval *sample_time)
{
	struct CORE_HISTORY *history = &info->core_history[core_num];
	struct CORE_HISTORY_SAMPLE *sample = history->samples;

	// there has been a response since reference time so don't set inactive.
	if (sample[0].sample_time.tv_sec > since_time->tv_sec)
		return false;

	// there has been a response since reference time so don't set inactive.
	if ((sample[0].sample_time.tv_sec == since_time->tv_sec) && (sample[0].sample_time.tv_usec >  since_time->tv_usec))
		return false;

	// inactive since reference time
	disable_core(info, core_num);
	update_core_history(info, core_num, sample_time, 0, true);

	return true;
}

//
// since_time - we use this time to look for activity from cores since then. If no activity
// we will assume the core is inactive. This reference point should be when new work begins.
// Since all cores report in with a counter, the next time we have a timeout we can assume they
// all should have reported in. So, this function should only be called once per work.
// sample_time - if a core goes inactive we need the time that we should add a 0 hashrate sample at
//
// return - whether an update occurred
//
bool update_active_device(struct ICARUS_INFO *info, struct timeval *since_time, struct timeval *sample_time)
{
	bool updated = false;
	for (int i=0; i < info->expected_cores; i ++)
	{
		if (update_active_core(info, since_time, i, sample_time))
			updated = true;
	}
	if (updated)
	{
		uint32_t new_device_hashrate = get_device_hashrate_average(info, HASHRATE_AVG_OVER_SECS, sample_time, false);
		info->prev_hashrate = new_device_hashrate;
	}

	return updated;
}

//

// *** deke ***
void sha512_midstate(unsigned char *input, unsigned char *output)
{
	#define GET_ULONG(n,b,i)                               \
	{                                                      \
	    (n) = ( (unsigned long long) (b)[(i) + 0] << 56 )  \
	        | ( (unsigned long long) (b)[(i) + 1] << 48 )  \
	        | ( (unsigned long long) (b)[(i) + 2] << 40 )  \
	        | ( (unsigned long long) (b)[(i) + 3] << 32 )  \
	        | ( (unsigned long long) (b)[(i) + 4] << 24 )  \
	        | ( (unsigned long long) (b)[(i) + 5] << 16 )  \
	        | ( (unsigned long long) (b)[(i) + 6] <<  8 )  \
	        | ( (unsigned long long) (b)[(i) + 7] <<  0 ); \
	}

	#define PUT_ULONG(n,b,i)                       \
	{                                              \
	    (b)[(i) + 0] = (unsigned char)((n) >> 56); \
	    (b)[(i) + 1] = (unsigned char)((n) >> 48); \
	    (b)[(i) + 2] = (unsigned char)((n) >> 40); \
	    (b)[(i) + 3] = (unsigned char)((n) >> 32); \
	    (b)[(i) + 4] = (unsigned char)((n) >> 24); \
	    (b)[(i) + 5] = (unsigned char)((n) >> 16); \
	    (b)[(i) + 6] = (unsigned char)((n) >>  8); \
	    (b)[(i) + 7] = (unsigned char)((n) >>  0); \
	}

	#define SHR(x,n) ((x & 0xFFFFFFFFFFFFFFFF) >> n)
	#define ROTR(x,n) (SHR(x,n) | (x << (64 - n)))
	
	#define S2(x) (ROTR(x,28) ^ ROTR(x,34) ^ ROTR(x,39))
	#define S3(x) (ROTR(x,14) ^ ROTR(x,18) ^ ROTR(x,41))
	#define S0(x) (ROTR(x, 1) ^ ROTR(x, 8) ^  SHR(x, 7))
	#define S1(x) (ROTR(x,19) ^ ROTR(x,61) ^  SHR(x, 6))
	
	#define F0(x,y,z) ((x & y) | (z & (x | y)))
	#define F1(x,y,z) (z ^ (x & (y ^ z)))
	
	#define R(t)                             \
	(                                        \
		W[t] = S1(W[t -  2]) + W[t -  7] +   \
			   S0(W[t - 15]) + W[t - 16]     \
	)
	
	#define P(a,b,c,d,e,f,g,h,x,K)            \
	{                                         \
		t1 = h + S3(e) + F1(e,f,g) + K + x;   \
		t2 = S2(a) + F0(a,b,c);               \
		d += t1; h = t1 + t2;                 \
	}
	
	unsigned long long a, b, c, d, e, f, g, h;
	unsigned long long t1, t2, W[64];	
	unsigned char midstate[64];			
	unsigned long long t1_p1;
	unsigned char t1p1[8];	
	unsigned char state[56];		
	unsigned char i;
	
	unsigned char M[128] =
	{
		0x00, 0x00, 0x00, 0x20, 0x56, 0xf6, 0x42, 0x58, 
		0xe0, 0xdd, 0x43, 0x7d, 0xfd, 0x05, 0x82, 0xa9, 
		0xd0, 0x6e, 0xfc, 0x79, 0xe6, 0x69, 0x80, 0x98, 
		0x69, 0xfa, 0x1d, 0x7e, 0xc1, 0x7e, 0x00, 0x00, 
		0x00, 0x00, 0x00, 0x00, 0x6a, 0x18, 0x3c, 0x7e, 
		0xb6, 0x2c, 0x3f, 0x76, 0x1c, 0x9c, 0xb1, 0x98, 
		0xef, 0xa4, 0x1f, 0xd7, 0xa7, 0x53, 0x87, 0xb7, 
		0x08, 0x84, 0xa0, 0x22, 0x79, 0xe8, 0x32, 0xd0, 
		0x27, 0x1e, 0x76, 0xc4, 0x9a, 0xde, 0x21, 0x63, 
		0xa3, 0xe8, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x00,
		//0xa3, 0xe8, 0x00, 0x1b, 0x01, 0x43, 0x4e, 0x59,
		0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x80
	};
	
	for(i = 0; i < 76; i++)
		M[i] = *input++;

	a = 0x22312194FC2BF72C;  
	b = 0x9F555FA3C84C64C2;  
	c = 0x2393B86B6F53B151; 
	d = 0x963877195940EABD;
	e = 0x96283EE2A88EFFE3;  
	f = 0xBE5E1E2553863992; 
	g = 0x2B0199FC2C85B8AA;  
	h = 0x0EB72DDC81C52CA2;
		
	GET_ULONG(W[0],  M,  0)
	GET_ULONG(W[1],  M,  8)
	GET_ULONG(W[2],  M, 16)
	GET_ULONG(W[3],  M, 24)
	GET_ULONG(W[4],  M, 32)
	GET_ULONG(W[5],  M, 40)
	GET_ULONG(W[6],  M, 48)
	GET_ULONG(W[7],  M, 56)
	GET_ULONG(W[8],  M, 64)
	GET_ULONG(W[9],  M, 72)
	GET_ULONG(W[10], M, 80)
	GET_ULONG(W[11], M, 88)
	GET_ULONG(W[12], M, 96)
	GET_ULONG(W[13], M, 104)
	GET_ULONG(W[14], M, 112)
	GET_ULONG(W[15], M, 120)
	
	P(a, b, c, d, e, f, g, h, W[ 0], 0x428a2f98d728ae22);
	P(h, a, b, c, d, e, f, g, W[ 1], 0x7137449123ef65cd);
	P(g, h, a, b, c, d, e, f, W[ 2], 0xb5c0fbcfec4d3b2f);
	P(f, g, h, a, b, c, d, e, W[ 3], 0xe9b5dba58189dbbc);
	P(e, f, g, h, a, b, c, d, W[ 4], 0x3956c25bf348b538);
	P(d, e, f, g, h, a, b, c, W[ 5], 0x59f111f1b605d019);
	P(c, d, e, f, g, h, a, b, W[ 6], 0x923f82a4af194f9b);
	P(b, c, d, e, f, g, h, a, W[ 7], 0xab1c5ed5da6d8118);
	
	t1_p1 = g;
	P(a, b, c, d, e, f, g, h, W[ 8], 0xd807aa98a3030242);
	
	PUT_ULONG(t1_p1, t1p1, 0)
	PUT_ULONG(f, state, 0)
	PUT_ULONG(e, state, 8)
	PUT_ULONG(d, state, 16)
	PUT_ULONG(c, state, 24)
	PUT_ULONG(b, state, 32)
	PUT_ULONG(a, state, 40)
	PUT_ULONG(h, state, 48)
	
	for(i = 0; i < 8; i++)
		midstate[i] = t1p1[i];
			
	for(i = 0; i < 56; i++)
		midstate[i+8] = state[i];
	
	for(i = 0; i < 64; i++)
		*output++ = midstate[i];
}	
// *** /DM/ ***

static int64_t icarus_scanhash(struct thr_info *thr, struct work *work,
				__maybe_unused int64_t max_nonce)
{
	struct cgpu_info *icarus;
	int fd;
	int ret;

	struct ICARUS_INFO *info;

	// *** deke ***	
	unsigned char ob_bin[ICARUS_WRITE_SIZE], nonce_bin[ICARUS_READ_SIZE];
	char nonce_tmp[4];	
	// *** /DM/ ***
	char *ob_hex;
	uint32_t nonce;
	int64_t hash_count;
	struct timeval tv_start, tv_finish, elapsed;
	struct timeval tv_history_start, tv_history_finish;
	double Ti, Xi;
	int curr_hw_errors, i;
	bool was_hw_error;

	struct ICARUS_HISTORY *history0, *history;
	int count;
	double Hs, W, fullnonce;
	int read_count;
	int64_t estimate_hashes;
	uint32_t values;
	int64_t hash_count_range;

	elapsed.tv_sec = elapsed.tv_usec = 0;

	icarus = thr->cgpu;
	// Tyler Edit
	icarus->result_is_nonce = false;
	icarus->result_is_counter = false;
	icarus->result_is_estimate = false;
	//

	if (icarus->device_fd == -1)
		if (!icarus_prepare(thr)) {
			applog(LOG_ERR, "%s%i: Comms error", icarus->drv->name, icarus->device_id);
			dev_error(icarus, REASON_DEV_COMMS_ERROR);

			// fail the device if the reopen attempt fails
			return -1;
		}

	fd = icarus->device_fd;


#if 0	
	unsigned char test[76] = 
	{
		0x00, 0x00, 0x00, 0x20, 0x56, 0xf6, 0x42, 0x58, 
		0xe0, 0xdd, 0x43, 0x7d, 0xfd, 0x05, 0x82, 0xa9, 
		0xd0, 0x6e, 0xfc, 0x79, 0xe6, 0x69, 0x80, 0x98, 
		0x69, 0xfa, 0x1d, 0x7e, 0xc1, 0x7e, 0x00, 0x00, 
		0x00, 0x00, 0x00, 0x00, 0x6a, 0x18, 0x3c, 0x7e,
		0xb6, 0x2c, 0x3f, 0x76, 0x1c, 0x9c, 0xb1, 0x98, 
		0xef, 0xa4, 0x1f, 0xd7, 0xa7, 0x53, 0x87, 0xb7, 
		0x08, 0x84, 0xa0, 0x22, 0x79, 0xe8, 0x32, 0xd0, 
		0x27, 0x1e, 0x76, 0xc4, 0x9a, 0xde, 0x21, 0x63, 
		0xa3, 0xe8, 0x00, 0x1b
	};
	
	for (i = 0; i < 76; i++){
		ob_bin[i+64] = test[i];				
	}
#endif 
	
#if 0
	applog(LOG_WARNING, "%s %d: OB_BIN: %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x",
		thr->cgpu->drv->name, thr->cgpu->device_id, 
		ob_bin[ 0], ob_bin[ 1], ob_bin[ 2], ob_bin[ 3], ob_bin[ 4], ob_bin[ 5], ob_bin[ 6], ob_bin[ 7], ob_bin[ 8], ob_bin[ 9], ob_bin[10], ob_bin[11], ob_bin[12], ob_bin[13], ob_bin[14], ob_bin[15],
		ob_bin[16], ob_bin[17], ob_bin[18], ob_bin[19], ob_bin[20], ob_bin[21], ob_bin[22], ob_bin[23], ob_bin[24], ob_bin[25], ob_bin[26], ob_bin[27], ob_bin[28], ob_bin[29], ob_bin[30], ob_bin[31],
		ob_bin[32], ob_bin[33], ob_bin[34], ob_bin[35], ob_bin[36], ob_bin[37], ob_bin[38], ob_bin[39], ob_bin[40], ob_bin[41], ob_bin[42], ob_bin[43], ob_bin[44], ob_bin[45], ob_bin[46], ob_bin[47],
		ob_bin[48], ob_bin[49], ob_bin[50], ob_bin[51], ob_bin[52], ob_bin[53], ob_bin[54], ob_bin[55], ob_bin[56], ob_bin[57], ob_bin[58], ob_bin[59], ob_bin[60], ob_bin[61], ob_bin[62], ob_bin[63],
		ob_bin[64], ob_bin[65], ob_bin[66], ob_bin[67], ob_bin[68], ob_bin[69], ob_bin[70], ob_bin[71], ob_bin[72], ob_bin[73], ob_bin[74], ob_bin[75], ob_bin[76], ob_bin[77], ob_bin[78], ob_bin[79],
		ob_bin[80], ob_bin[81], ob_bin[82], ob_bin[83], ob_bin[84], ob_bin[85], ob_bin[86], ob_bin[87], ob_bin[88], ob_bin[89], ob_bin[90], ob_bin[91], ob_bin[92], ob_bin[93], ob_bin[94], ob_bin[95],
		ob_bin[ 96], ob_bin[ 97], ob_bin[ 98], ob_bin[ 99], ob_bin[100], ob_bin[101], ob_bin[102], ob_bin[103], ob_bin[104], ob_bin[105], ob_bin[106], ob_bin[107], ob_bin[108], ob_bin[109], ob_bin[110], ob_bin[111],
		ob_bin[112], ob_bin[113], ob_bin[114], ob_bin[115], ob_bin[116], ob_bin[117], ob_bin[118], ob_bin[119], ob_bin[120], ob_bin[121], ob_bin[122], ob_bin[123], ob_bin[124], ob_bin[125], ob_bin[126], ob_bin[127],
		ob_bin[128], ob_bin[129], ob_bin[130], ob_bin[131], ob_bin[132], ob_bin[133], ob_bin[134], ob_bin[135], ob_bin[136], ob_bin[137], ob_bin[138], ob_bin[139]);
#endif       
	// *** /DM/ ***
                    
#ifndef WIN32
	tcflush(fd, TCOFLUSH);
#endif
	//Tyler Edit
	info = icarus_info[icarus->device_id];
	if (info->work_changed)
	{
		info->work_changed = false;
	//
		//ret = icarus_write(fd, ob_bin, sizeof(ob_bin));

		// skip sending nonce
/*
  		uint8_t data[180];
		hex2bin(data, "0000000122c10f7f1c8400000000000000029ca3814215d060fcc2ef2c003b21e3b8ea63385c9cd17dddc21bd9a7ca942a0e137837e54a4d54310a09788b53265ec3003416d97cb02987eb735eda3e492f73ed94208d264989b159a03b7151dd66fdba748fdcf6693dc84517000000000010ee5b8aefa76ca17b944ad3c6676c91deb73f0725dc75d06aeae20d2e292587010000637a4d565253356d4f513235466454496b41593877677a566e6f467044746754", 180);
		memcpy(work->data, data, sizeof(work->data));

*/
		unsigned char buf[ICARUS_WRITE_SIZE];
		memcpy(&buf[3], &work->data[8], ICARUS_WRITE_SIZE-3);
		// 3 bytes that are iterated in each new work call to scanhash across all devices.
		memcpy(buf, work->data, 3);

/*
  	char input_str[sizeof(buf)*2+1];
	for (int i=0; i<sizeof(buf); i++)
	{
		sprintf(input_str+i*2, "%02X", buf[i]);	
	}
	input_str[sizeof(buf)*2] = 0;
	applog(LOG_WARNING, "Writing input vector to FPGA: %s", input_str);
*/	
//		applog(LOG_ERR, "pool nonce %08x, write nonce %08x", *(uint32_t*)work->data, *(uint32_t*)buf);

		ret = icarus_write(fd, buf, ICARUS_WRITE_SIZE);
	
		if (ret) {
			do_icarus_close(thr);
			applog(LOG_ERR, "%s%i: Comms error", icarus->drv->name, icarus->device_id);
			dev_error(icarus, REASON_DEV_COMMS_ERROR);
			return 0;	/* This should never happen */
		}
		cgtime(&tv_start);
		copy_time(&info->work_start, &tv_start);
		copy_time(&info->prev_hashcount_return, &tv_start);
		info->prev_hashcount = 0;
	}
	else
		cgtime(&tv_start);

	if (opt_debug) {
		ob_hex = bin2hex(ob_bin, sizeof(ob_bin));
		applog(LOG_DEBUG, "Icarus %d sent: %s",
			icarus->device_id, ob_hex);
		free(ob_hex);
	}

	/* Icarus will return 4 bytes (ICARUS_READ_SIZE) nonces or nothing */
	memset(nonce_bin, 0, sizeof(nonce_bin));
	ret = icarus_gets(nonce_bin, fd, &tv_finish, thr, info->read_count);
	
	if (!ret)
	{
	//Tyler Edit
/*	
		if (nonce_bin[0] == 1 || nonce_bin[0] == 0xbb)
		{
			char result_nonce_str[17*3+1];
			for (int i=0; i<17; i++)
			{
				sprintf(result_nonce_str+i*3, "%02X ", nonce_bin[i]);	
			}
			result_nonce_str[17*3] = 0;
			applog(LOG_WARNING, "Received nonce_result: %s", result_nonce_str);
		}
*/		
	}
	//

	if (ret == ICA_GETS_ERROR) {
		do_icarus_close(thr);
		applog(LOG_ERR, "%s%i: Comms error", icarus->drv->name, icarus->device_id);
		dev_error(icarus, REASON_DEV_COMMS_ERROR);

		return 0;
	}

	// Tyler Edit
	// This was for the Icarus board. For VCU/BCU/FK33 we decided to return to avoid
	// time that might be on stale work, etc.
	//work->blk.nonce = 0xffffffff;
	//

	// aborted before becoming idle, get new work
	if (ret == ICA_GETS_TIMEOUT || ret == ICA_GETS_RESTART) {
		if (info->first_timeout)
		{
			disable_inactive_cores_since_work_start(info);
			update_active_device(info, &info->work_start, &tv_finish);
			info->first_timeout = false;
			copy_time(&info->last_interval_timeout, &tv_finish);
		}
		else
		{
			timersub(&tv_finish, &info->work_start, &elapsed);
			double seconds = (double)(elapsed.tv_sec) + ((double)(elapsed.tv_usec))/((double)1000000); 

			// TODO: change this to a calculation of time based on nonce_range and hashes per second
			// the following could occur if a number of cores are inactive or misbehaving and therefore our hashrate is really low. If that is the case then we can disable cores that don't produce a nonce within some range of time. 
			if (seconds > SECONDS_PER_NONCE_RANGE)
			{
				update_active_device(info, &info->last_interval_timeout, &tv_finish);
				copy_time(&info->last_interval_timeout, &tv_finish);
			}
			
		}

		hash_count = get_hashcount_estimate_for_return(info, work, &tv_finish);
		icarus->result_is_estimate = true;

		return hash_count;
		//
		timersub(&tv_finish, &tv_start, &elapsed);

		// ONLY up to just when it aborted
		// We didn't read a reply so we don't subtract ICARUS_READ_TIME
		estimate_hashes = ((double)(elapsed.tv_sec)
					+ ((double)(elapsed.tv_usec))/((double)1000000)) / info->Hs;

		// If some Serial-USB delay allowed the full nonce range to
		// complete it can't have done more than a full nonce
		if (unlikely(estimate_hashes > 0xffffffff))
			estimate_hashes = 0xffffffff;

		if (opt_debug) {
			applog(LOG_DEBUG, "Icarus %d no nonce = 0x%08lX hashes (%ld.%06lds)",
					icarus->device_id, (long unsigned int)estimate_hashes,
					elapsed.tv_sec, elapsed.tv_usec);
		}
	
		 return estimate_hashes;
	}
	
	unsigned int volt_raw, temp_raw;	
	
	if ((nonce_bin[0] == 0xbb) && !nonce_bin[2] && !nonce_bin[3] && !nonce_bin[4] && !nonce_bin[5] && !nonce_bin[6] && !nonce_bin[7] && !nonce_bin[8])
	{
		// nonce
		nonce_tmp[0] = nonce_bin[13];
		nonce_tmp[1] = nonce_bin[14];
		nonce_tmp[2] = nonce_bin[15];
		nonce_tmp[3] = nonce_bin[16];
		
		memcpy((char *)&nonce, nonce_tmp, sizeof(nonce_tmp));
		icarus->result_is_counter = true;
	}
	else if ((nonce_bin[0] == 0x01) && !nonce_bin[2] && !nonce_bin[3] && !nonce_bin[4] && !nonce_bin[5] && !nonce_bin[6] && !nonce_bin[7] && !nonce_bin[8])
	{
		applog(LOG_INFO, "RECEIVED NONCE RESPONSE");
		// nonce
		nonce_tmp[0] = nonce_bin[13];
		nonce_tmp[1] = nonce_bin[14];
		nonce_tmp[2] = nonce_bin[15];
		nonce_tmp[3] = nonce_bin[16];
		
		memcpy((char *)&nonce, nonce_tmp, sizeof(nonce_tmp));
	}
	else if (nonce_bin[0] == 0xaa)
	{
		// voltage -> SENSOR 1
		volt_raw = nonce_bin[1];
		volt_raw <<= 8;
		volt_raw |= nonce_bin[2];
		volt_raw *= 3000;	
		if(volt_raw)
			fpga_volt_1[icarus->device_id] = (volt_raw / 65536);
		else
			fpga_volt_1[icarus->device_id] = 0;
		// voltage -> SENSOR 2
		volt_raw = nonce_bin[3];
		volt_raw <<= 8;
		volt_raw |= nonce_bin[4];
		volt_raw *= 3000;	
		if(volt_raw)
			fpga_volt_2[icarus->device_id] = (volt_raw / 65536);
		else
			fpga_volt_2[icarus->device_id] = 0;
		// voltage -> SENSOR 3
		volt_raw = nonce_bin[5];
		volt_raw <<= 8;
		volt_raw |= nonce_bin[6];
		volt_raw *= 3000;	
		if(volt_raw)
			fpga_volt_3[icarus->device_id] = (volt_raw / 65536);
		else
			fpga_volt_3[icarus->device_id] = 0;	
		// teperature -> SENSOR 1
		temp_raw = nonce_bin[9];
		temp_raw <<= 8;
		temp_raw |= nonce_bin[10];
		if(temp_raw)
			fpga_temp_1[icarus->device_id] = ((((float)temp_raw * 509.3140064) / 65536 ) - 280.2308787);
		else
			fpga_temp_1[icarus->device_id] = 0;
		// teperature -> SENSOR 2
		temp_raw = nonce_bin[11];
		temp_raw <<= 8;
		temp_raw |= nonce_bin[12];	
		if(temp_raw)
			fpga_temp_2[icarus->device_id] = ((((float)temp_raw * 509.3140064) / 65536 ) - 280.2308787);
		else
			fpga_temp_2[icarus->device_id] = 0;
		// teperature -> SENSOR 3
		temp_raw = nonce_bin[13];
		temp_raw <<= 8;
		temp_raw |= nonce_bin[14];	
		if(temp_raw)
			fpga_temp_3[icarus->device_id] = ((((float)temp_raw * 509.3140064) / 65536 ) - 280.2308787);
		else
			fpga_temp_3[icarus->device_id] = 0;
		

		hash_count = get_hashcount_estimate_for_return(info, work, &tv_finish);
		icarus->result_is_estimate = true;

		return hash_count;
	}
	else
	{
		applog(LOG_ERR, "Unknown message from FPGA. Byte: 0x%01X", nonce_bin[0]);
		return 0;
	}

#if !defined (__BIG_ENDIAN__) && !defined(MIPSEB)
	nonce = swab32(nonce);
#endif

	//applog(LOG_WARNING, "VCU1525 %d: Nonce found %08x", icarus->device_id, nonce_d);
	nonce_d = nonce_bin[12];
//	applog(LOG_WARNING, "VCU1525 %d: Core %08x", icarus->device_id, nonce_d);

	// if not enabled
	if ((info->enabled_cores >> (nonce_d)) ^ 0x1)
		enable_core(info, nonce_d);

	//applog(LOG_WARNING, "VCU1525 %d: nonce %08x [core %d] Job ID: %s, Nonce1: %s, Nonce2: %s, Hs: %f", icarus->device_id, nonce, nonce_d, work->job_id, work->nonce1, work->nonce2, info->Hs);
	//
	
	nonce_found[icarus->device_id]++;
	nonce_counter++;
	// *** /DM/ ***

	curr_hw_errors = icarus->hw_errors;
	// Tyler Edit
	if (!icarus->result_is_counter)
	//
	{
		uint64_t real_nonce = ((uint64_t)nonce_d<<32)+nonce;
		if (info->expected_cores >= 9)
		{
			uint32_t nonce2 = *(uint32_t*)work->data;
			nonce2 &= 0xffffff;
			nonce2 = bswap_32(nonce2);
			real_nonce |= ((uint64_t)htole32(nonce2))<<32;
/*
			char nonce_str[8*2+1];
			uint8_t *buf = &nonce2;
			for (int i =0; i < 4; i++)
				sprintf(nonce_str+i*2, "%02X", buf[i]);	
			nonce_str[4*2] = 0;
			applog(LOG_ERR, "Nonce 2 %s", nonce_str);
			buf = &real_nonce;
			for (int i =0; i < 8; i++)
				sprintf(nonce_str+i*2, "%02X", buf[i]);	
			nonce_str[8*2] = 0;
			applog(LOG_ERR, "Real Nonce %s", nonce_str);
*/
		}
		applog(LOG_WARNING, "VCU1525 %d: nonce %016lx, [core %d]", icarus->device_id, real_nonce, nonce_d);
		uint64_t flip_nonce = bswap_64(real_nonce);
//		applog(LOG_WARNING, "real nonce = 0x%0llX, flip nonce = 0x%0llX", real_nonce, flip_nonce);
		submit_nonce(thr, work, flip_nonce);
	}
	was_hw_error = (curr_hw_errors > icarus->hw_errors);

	// TODO: check if this is still valid; applicable and functioning
	// Force a USB close/reopen on any hw error
	if (was_hw_error)
		do_icarus_close(thr);

	if (is_response_for_current_work(nonce_bin, work))
	{
		hash_count = nonce;
		hash_count++;
	
		timersub(&tv_finish, &info->work_start, &elapsed);
	
		// Calculate the core hashrate based on its progress since work was assigned
		uint32_t core_hashrate = (double)hash_count / ((double)(elapsed.tv_sec) + ((double)(elapsed.tv_usec))/((double)1000000)); 
	
		update_core_history(info, nonce_d, &tv_finish, core_hashrate, false);
	
		// device hashrate
		// Store this so we can calculate hash_count on future returns when we have no nonce or counter; timeouts and voltage reports.
		uint64_t new_device_hashrate = get_device_hashrate_average(info, HASHRATE_AVG_OVER_SECS, &tv_finish, false);
	
		info->prev_hashrate = new_device_hashrate;
	
		hash_count = new_hashcount_since_last_return(info, &tv_finish);
	
	//	applog(LOG_ERR, "Core %d update: Hashrate update instance: %u, Previous hashrate: %lu, Elapsed time: %f, Total Hash Count: %08lX, Hash Count: %08X", nonce_d, core_hashrate, info->prev_hashrate, (double)(elapsed.tv_sec) + ((double)(elapsed.tv_usec))/((double)1000000), info->prev_hashcount, (uint32_t)hash_count);
	
		// applog(LOG_WARNING, "Hashrate updated to: %u", info->prev_hashrate);
		//
		if (hashcount_beyond_new_work_threshold(info, hash_count))
		{
//			applog(LOG_ERR, "Forcing abandon_work to avoid idle FPGA. Previous hashrate: %u, Elapsed time: %f, Hash Count: %09llX", info->prev_hashrate, (double)(elapsed.tv_sec) + ((double)(elapsed.tv_usec))/((double)1000000), info->prev_hashcount);
			work->blk.nonce = 0xffffffff;
		}
	
		icarus->result_is_nonce = true;
	}
	else
	{
		applog(LOG_DEBUG, "Got old work response!!!!");
		hash_count = get_hashcount_estimate_for_return(info, work, &tv_finish);
		icarus->result_is_estimate = true;
	}
	if (opt_debug || info->do_icarus_timing)
		timersub(&tv_finish, &tv_start, &elapsed);

	if (opt_debug) {
		applog(LOG_DEBUG, "Icarus %d nonce = 0x%08x = 0x%08lX hashes (%ld.%06lds)",
				icarus->device_id, nonce, (long unsigned int)hash_count,
				elapsed.tv_sec, elapsed.tv_usec);
	}

	// TODO: check if this is still valid; applicable and functioning  
	// ignore possible end condition values ... and hw errors
	if (info->do_icarus_timing
	&&  !was_hw_error
	&&  ((nonce & info->nonce_mask) > END_CONDITION)
	&&  ((nonce & info->nonce_mask) < (info->nonce_mask & ~END_CONDITION))) {
		cgtime(&tv_history_start);

		history0 = &(info->history[0]);

		if (history0->values == 0)
			timeradd(&tv_start, &history_sec, &(history0->finish));

		Ti = (double)(elapsed.tv_sec)
			+ ((double)(elapsed.tv_usec))/((double)1000000)
			- ((double)ICARUS_READ_TIME(info->baud));
		Xi = (double)hash_count;
		history0->sumXiTi += Xi * Ti;
		history0->sumXi += Xi;
		history0->sumTi += Ti;
		history0->sumXi2 += Xi * Xi;

		history0->values++;

		if (history0->hash_count_max < hash_count)
			history0->hash_count_max = hash_count;
		if (history0->hash_count_min > hash_count || history0->hash_count_min == 0)
			history0->hash_count_min = hash_count;

		if (history0->values >= info->min_data_count
		&&  timercmp(&tv_start, &(history0->finish), >)) {
			for (i = INFO_HISTORY; i > 0; i--)
				memcpy(&(info->history[i]),
					&(info->history[i-1]),
					sizeof(struct ICARUS_HISTORY));

			// Initialise history0 to zero for summary calculation
			memset(history0, 0, sizeof(struct ICARUS_HISTORY));

			// We just completed a history data set
			// So now recalc read_count based on the whole history thus we will
			// initially get more accurate until it completes INFO_HISTORY
			// total data sets
			count = 0;
			for (i = 1 ; i <= INFO_HISTORY; i++) {
				history = &(info->history[i]);
				if (history->values >= MIN_DATA_COUNT) {
					count++;

					history0->sumXiTi += history->sumXiTi;
					history0->sumXi += history->sumXi;
					history0->sumTi += history->sumTi;
					history0->sumXi2 += history->sumXi2;
					history0->values += history->values;

					if (history0->hash_count_max < history->hash_count_max)
						history0->hash_count_max = history->hash_count_max;
					if (history0->hash_count_min > history->hash_count_min || history0->hash_count_min == 0)
						history0->hash_count_min = history->hash_count_min;
				}
			}

			// All history data
			Hs = (history0->values*history0->sumXiTi - history0->sumXi*history0->sumTi)
				/ (history0->values*history0->sumXi2 - history0->sumXi*history0->sumXi);
			W = history0->sumTi/history0->values - Hs*history0->sumXi/history0->values;
			hash_count_range = history0->hash_count_max - history0->hash_count_min;
			values = history0->values;
			
			// Initialise history0 to zero for next data set
			memset(history0, 0, sizeof(struct ICARUS_HISTORY));

			fullnonce = W + Hs * (((double)0xffffffff) + 1);
			read_count = (int)(fullnonce * TIME_FACTOR) - 1;

			info->Hs = Hs;
			applog(LOG_WARNING, "updated estimated hash rate to: %f", Hs);
			info->read_count = read_count;

			info->fullnonce = fullnonce;
			info->count = count;
			info->W = W;
			info->values = values;
			info->hash_count_range = hash_count_range;

			if (info->min_data_count < MAX_MIN_DATA_COUNT)
				info->min_data_count *= 2;
			else if (info->timing_mode == MODE_SHORT)
				info->do_icarus_timing = false;

//			applog(LOG_WARNING, "Icarus %d Re-estimate: read_count=%d fullnonce=%fs history count=%d Hs=%e W=%e values=%d hash range=0x%08lx min data count=%u", icarus->device_id, read_count, fullnonce, count, Hs, W, values, hash_count_range, info->min_data_count);
			applog(LOG_WARNING, "Icarus %d Re-estimate: Hs=%e W=%e read_count=%d fullnonce=%.3fs",
					icarus->device_id, Hs, W, read_count, fullnonce);
		}
		info->history_count++;
		cgtime(&tv_history_finish);

		timersub(&tv_history_finish, &tv_history_start, &tv_history_finish);
		timeradd(&tv_history_finish, &(info->history_time), &(info->history_time));
	}

	return hash_count;
}

static struct api_data *icarus_api_stats(struct cgpu_info *cgpu)
{
	struct api_data *root = NULL;
	struct ICARUS_INFO *info = icarus_info[cgpu->device_id];

	// Warning, access to these is not locked - but we don't really
	// care since hashing performance is way more important than
	// locking access to displaying API debug 'stats'
	// If locking becomes an issue for any of them, use copy_data=true also
	root = api_add_int(root, "read_count", &(info->read_count), false);
	root = api_add_double(root, "fullnonce", &(info->fullnonce), false);
	root = api_add_int(root, "count", &(info->count), false);
	root = api_add_hs(root, "Hs", &(info->Hs), false);
	root = api_add_double(root, "W", &(info->W), false);
	root = api_add_uint(root, "total_values", &(info->values), false);
	root = api_add_uint64(root, "range", &(info->hash_count_range), false);
	root = api_add_uint64(root, "history_count", &(info->history_count), false);
	root = api_add_timeval(root, "history_time", &(info->history_time), false);
	root = api_add_uint(root, "min_data_count", &(info->min_data_count), false);
	root = api_add_uint(root, "timing_values", &(info->history[0].values), false);
	root = api_add_const(root, "timing_mode", timing_mode_str(info->timing_mode), false);
	root = api_add_bool(root, "is_timing", &(info->do_icarus_timing), false);
	root = api_add_int(root, "baud", &(info->baud), false);
	root = api_add_int(root, "work_division", &(info->work_division), false);
	root = api_add_int(root, "fpga_count", &(info->fpga_count), false);

	return root;
}

static void icarus_shutdown(struct thr_info *thr)
{
	do_icarus_close(thr);
}
// *** deke ***
struct device_drv icarus_drv = {
	.drv_id = DRIVER_ICARUS,
	.dname = "VCU1525",
	.name = "VCU",
	.drv_detect = icarus_detect,
	.get_api_stats = icarus_api_stats,
	.thread_prepare = icarus_prepare,
	.scanhash = icarus_scanhash,
	.thread_shutdown = icarus_shutdown,
	.get_statline = icarus_statline,
	.prepare_work = icarus_prepare_work,
};
// *** /DM/ ***
