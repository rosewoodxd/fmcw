#include "vector.h"
#include <fcntl.h>
#include <ftdi.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define VENDOR_ID 0x0403
#define MODEL_ID 0x6010
#define BITMASK_ON 0xFF
#define CHUNKSIZE 0x10000
#define PACKETS_PER_TRANSFER 8
#define TRANSFERS_PER_CALLBACK 256
#define LATENCY 2
#define TRUE 1
#define FALSE 0
#define BYTE_BITS 8
#define START_FLAG 0xFF
#define STOP_FLAG 0x8F
#define NS_TO_S 1e-9
#define sample_t int

static struct ftdi_context *ftdi = NULL;
static pthread_t producer_thread;
pthread_mutex_t *mutex = NULL;
struct ProducerData *prod_data = NULL;
struct ConsumerData *cons_data = NULL;
static int _sample_bits;
static int _sample_bytes;
static int _nflags;
static int _fft;
static int _sweep_len;
static int _start_flags;
static int _stop_flags;
static int _sweep_idx;
static int _sweep_valid = 0;
static FILE *_log_file = NULL;
static sample_t *sweep = NULL;
static sample_t _last_sample;
static int _byte_idx;
static uint64_t _uval;
static int _cancel;
static struct Vector *write_data = NULL;

/**
 * Nearest greater or equal power of 2.
 */
static int pow2ceil(int val);
/**
 * Asynchronous read callback.
 */
static int callback(uint8_t *buffer, int length, FTDIProgressInfo *progress, void *userdata);
/**
 * Initialize the radar.
 */
int fmcw_open();
/**
 * Free the radar.
 */
void fmcw_close();
/**
 * Begin asynchronous reading.
 *
 * @log_path is the absolute path (null-terminated) of a file to which
 * all read data should be written. If set to NULL, no data is logged.
 *
 * @sample_bits is the number of bits in each sample sent from the
 * radar. This is needed to extract the sample payload from the full
 * bitstream. Each sample is MSB-padded with 0s until it is
 * byte-aligned. The MSB of the sample + padding is guaranteed to be
 * zero. That is, at least 1 0-bit is padded to each sample. This
 * allows us to distinguish a start or stop flag from a sample. The
 * flag length is therefore equal to the number of bytes of the sample
 * + padding.
 *
 * @sweep_len is the number of samples in each sweep.
 *
 * Returns TRUE on success and FALSE on failure.
 */
int fmcw_start_acquisition(char *log_path, int sample_bits, int sweep_len, int fft);
/**
 * Retrieves the next sweep if one is available, or NULL otherwise.
 */
int fmcw_read_sweep(int *arr);
/**
 * Producer function to read data from radar.
 */
static void *producer(void *arg);
static int num_flags(int sample_bits);
static int sample_bytes(int sample_bits);
/**
 * Increment @read_idx and check if it equals @length. If it does
 * return 0. Otherwise return the new value of @read_dix.
 */
static int inc_check_idx(int read_idx, int length);
/**
 * Read stop flags. Returns 0 if we've read to the end of the buffer.
 */
static int read_stop_seq(uint8_t *buffer, int length, int read_idx);
static int read_sample_seq(uint8_t *buffer, int length, int read_idx);
static int read_start_seq(uint8_t *buffer, int length, int read_idx);
static sample_t sample_val(uint64_t uval);

int fmcw_open()
{
	if ((ftdi = ftdi_new()) == 0) {
		fprintf(stderr, "ftdi_new failed\n");
		fmcw_close();
		return FALSE;
	}

	if (ftdi_set_interface(ftdi, INTERFACE_A) < 0) {
		fprintf(stderr, "ftdi_set_interface failed\n");
		fmcw_close();
		return FALSE;
	}

	if (ftdi_usb_open_desc(ftdi, VENDOR_ID, MODEL_ID, NULL, NULL) < 0) {
		fprintf(stderr, "Can't open ftdi device: %s\n", ftdi_get_error_string(ftdi));
		fmcw_close();
		return FALSE;
	}

	if (ftdi_set_latency_timer(ftdi, LATENCY)) {
		fprintf(stderr, "Can't set latency, Error %s\n", ftdi_get_error_string(ftdi));
		fmcw_close();
		return FALSE;
	}

	/* Configures FT2232H for synchronous FIFO mode. */
	if (ftdi_set_bitmode(ftdi, BITMASK_ON, BITMODE_SYNCFF) < 0) {
		fprintf(stderr, "Can't set synchronous fifo mode, Error %s\n",
			ftdi_get_error_string(ftdi));
		fmcw_close();
		return FALSE;
	}

	if (ftdi_read_data_set_chunksize(ftdi, CHUNKSIZE) < 0) {
		fprintf(stderr, "Unable to set read chunk size %s\n", ftdi_get_error_string(ftdi));
		fmcw_close();
		return FALSE;
	}

	if (ftdi_write_data_set_chunksize(ftdi, CHUNKSIZE) < 0) {
		fprintf(stderr, "Unable to set write chunk size %s\n", ftdi_get_error_string(ftdi));
		fmcw_close();
		return FALSE;
	}

	if (ftdi_setflowctrl(ftdi, SIO_RTS_CTS_HS) < 0) {
		fprintf(stderr, "Unable to set flow control %s\n", ftdi_get_error_string(ftdi));
		fmcw_close();
		return FALSE;
	}

	/* TODO this will be deprecated for ftdi_tcioflush. */
	if (ftdi_usb_purge_buffers(ftdi) < 0) {
		fprintf(stderr, "Unable to purge tx/rx buffers %s\n", ftdi_get_error_string(ftdi));
		fmcw_close();
		return FALSE;
	}

	write_data = vector_new();

	return TRUE;
}

void fmcw_close()
{
	pthread_mutex_lock(mutex);
	_cancel = 1;
	pthread_mutex_unlock(mutex);
	pthread_join(producer_thread, NULL);
	_cancel = 0;
	if (_log_file) {
		fclose(_log_file);
		_log_file = NULL;
	}
	vector_free(write_data);
	write_data = NULL;
	ftdi_usb_purge_buffers(ftdi);
	ftdi_usb_close(ftdi);
	ftdi_free(ftdi);
	ftdi = NULL;
	free(mutex);
	mutex = NULL;
	free(sweep);
	sweep = NULL;
}

int fmcw_start_acquisition(char *log_path, int sample_bits, int sweep_len, int fft)
{
	mutex = malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(mutex, NULL);
	_fft = fft;
	_sample_bits = sample_bits;
	_sample_bytes = sample_bytes(_sample_bits);
	_nflags = num_flags(_sample_bits);
	_sweep_len = sweep_len;
	if (log_path) {
		if ((_log_file = fopen(log_path, "w")) < 0) {
			fputs("Failed to open log file.\n", stderr);
			return FALSE;
		}
	}
	sweep = malloc(_sweep_len * sizeof(int));

	pthread_create(&producer_thread, NULL, &producer, NULL);
	return TRUE;
}

int fmcw_read_sweep(int *arr)
{
	int ret = FALSE;
	pthread_mutex_lock(mutex);
	if (_sweep_valid) {
		ret = TRUE;
		for (int i = 0; i < _sweep_len; ++i) {
			arr[i] = sweep[i];
		}
		_sweep_valid = 0;
	}
	pthread_mutex_unlock(mutex);
	return ret;
}

int fmcw_add_write(uint32_t val, int nbytes)
{
	unsigned char buf[nbytes];
	for (int i = 0; i < nbytes; ++i) {
		buf[i] = (val >> (BYTE_BITS * i)) & 0xFF;
	}

	if (vector_push(write_data, buf, nbytes) != nbytes) {
		return FALSE;
	}

	return TRUE;
}

int fmcw_write_pending()
{
	if (ftdi_write_data(ftdi, write_data->buf, write_data->size) != write_data->size) {
		return FALSE;
	}
	vector_empty(write_data);
	return TRUE;
}

void *producer(void *arg)
{
	ftdi_readstream(ftdi, &callback, NULL, PACKETS_PER_TRANSFER, TRANSFERS_PER_CALLBACK);
	return NULL;
}

int callback(uint8_t *buffer, int length, FTDIProgressInfo *progress, void *userdata)
{
	pthread_mutex_lock(mutex);
	if (_cancel) {
		pthread_mutex_unlock(mutex);
		return 1;
	}

	if (length == 0 || _sweep_valid) {
		pthread_mutex_unlock(mutex);
		return 0;
	}

	int read_idx = 0;

	/* break out if we read the full buffer. */
	while (1) {
		if (_stop_flags) {
			read_idx = read_stop_seq(buffer, length, read_idx);
			goto log;
		}
		if (_sweep_idx) {
			if (!(read_idx = read_sample_seq(buffer, length, read_idx))) {
				goto log;
			}
			read_idx = read_stop_seq(buffer, length, read_idx);
			goto log;
		}

		if (!(read_idx = read_start_seq(buffer, length, read_idx))) {
			goto log;
		}
		if (!(read_idx = read_sample_seq(buffer, length, read_idx))) {
			goto log;
		}
		read_idx = read_stop_seq(buffer, length, read_idx);
		goto log;
	}

log:
	if (_log_file) {
		if (!read_idx) {
			fwrite(buffer, sizeof(uint8_t), length, _log_file);
		} else {
			fwrite(buffer, sizeof(uint8_t), read_idx, _log_file);
		}
	}
	pthread_mutex_unlock(mutex);
	return 0;
}

int read_stop_seq(uint8_t *buffer, int length, int read_idx)
{
	while (_stop_flags < _nflags) {
		if (buffer[read_idx] == STOP_FLAG) {
			++_stop_flags;
		} else {
			read_idx = inc_check_idx(read_idx, length);
			goto cleanup;
		}
		/* first check must occur first, otherwise read_idx
		 * might get a 0 value when it shouldn't. */
		if (_stop_flags < _nflags && !(read_idx = inc_check_idx(read_idx, length))) {
			return FALSE;
		}
	}
	_sweep_valid = 1;
	sweep[_sweep_idx] = _last_sample;
cleanup:
	_sweep_idx = 0;
	_start_flags = 0;
	_stop_flags = 0;
	return read_idx;
}

int read_sample_seq(uint8_t *buffer, int length, int read_idx)
{
	while (_sweep_idx < _sweep_len) {
		while (_byte_idx < _sample_bytes) {
			_uval |= ((uint64_t)buffer[read_idx]
				  << (BYTE_BITS * (_sample_bytes - 1 - _byte_idx++)));
			if (!(read_idx = inc_check_idx(read_idx, length))) {
				return FALSE;
			}
		}
		_byte_idx = 0;
		/* fmcw_read_sweep will only perform a read if the
		 * queue has at least one full sweep. This delays
		 * writing the last value to the queue until we get
		 * the full stop sequence (see
		 * read_stop_seq). Otherwise, there is a small
		 * possibility that an invalid sweep could be read. */
		if (_sweep_idx < _sweep_len - 1) {
			sweep[_sweep_idx] = sample_val(_uval);
		} else {
			_last_sample = sample_val(_uval);
		}
		++_sweep_idx;
		_uval = 0;
	}
	return read_idx;
}

int read_start_seq(uint8_t *buffer, int length, int read_idx)
{
	while (_start_flags < _nflags) {
		if (buffer[read_idx] == START_FLAG) {
			++_start_flags;
		} else {
			_start_flags = 0;
		}
		if (!(read_idx = inc_check_idx(read_idx, length))) {
			return FALSE;
		}
	}
	return read_idx;
}

int inc_check_idx(int read_idx, int length)
{
	if (++read_idx == length) {
		return FALSE;
	}
	return read_idx;
}

int num_flags(int sample_bits)
{
	if (_fft) {
		sample_bits *= 2;
	}
	int bytes = sample_bits / BYTE_BITS + 1;
	return pow2ceil(bytes);
}

int pow2ceil(int val)
{
	int pow2val = 1;
	while (val > pow2val) {
		pow2val *= 2;
	}
	return pow2val;
}

int sample_bytes(int sample_bits)
{
	if (_fft) {
		sample_bits *= 2;
	}
	int bytes = sample_bits / BYTE_BITS;
	if (sample_bits % BYTE_BITS != 0) {
		++bytes;
	}
	return pow2ceil(bytes);
}

sample_t sample_val(uint64_t uval)
{
	uint64_t mask = 0x1 << (_sample_bits - 1);
	if (!_fft) {
		return (sample_t)(-(uval & mask) + (uval & ~mask));
	}
	uint64_t lumask = (0x1 << _sample_bits) - 1;
	uint64_t luval = uval & lumask;
	sample_t lval = (sample_t)(-(luval & mask) + (luval & ~mask));

	uint64_t uumask = lumask << _sample_bits;
	uint64_t uuval = (uval & uumask) >> _sample_bits;
	sample_t upval = (sample_t)(-(uuval & mask) + (uuval & ~mask));

	return (sample_t)round(sqrt(pow(lval, 2) + pow(upval, 2)));
}

double tsec(struct timespec tspec) { return tspec.tv_sec + NS_TO_S * tspec.tv_nsec; }

/* int main() */
/* { */
/* 	if (!fmcw_open()) { */
/* 		return EXIT_FAILURE; */
/* 	} */

/* 	if (!fmcw_start_acquisition("log.bin", 12, 20480)) { */
/* 		return EXIT_FAILURE; */
/* 	} */

/* 	struct timespec tspec; */
/* 	double tstart; */
/* 	double tcur; */
/* 	int ctr; */
/* 	int *arr = malloc(20480 * sizeof(int)); */
/* 	clock_gettime(CLOCK_MONOTONIC, &tspec); */
/* 	tstart = tsec(tspec); */
/* 	tcur = tstart; */
/* 	while (tcur - tstart < 2) { */
/* 		if ((fmcw_read_sweep(arr))) { */
/* 			++ctr; */
/* 			/\* for (int i = 0; i < 20480; ++i) { *\/ */
/* 			/\* 	printf("%d\n", arr[i]); *\/ */
/* 			/\* } *\/ */
/* 		} */
/* 		clock_gettime(CLOCK_MONOTONIC, &tspec); */
/* 		tcur = tsec(tspec); */
/* 	} */
/* 	printf("samples acquired: %d\n", ctr); */
/* 	printf("bandwidth: %e\n", ctr * 20480 * 2 / (tcur - tstart)); */
/* 	free(arr); */
/* 	fmcw_close(); */
/* } */
