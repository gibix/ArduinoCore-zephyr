/* Copyright (C) Arduino SRL (Daniele Aimo)
 * SPDX-License-Identifier: MPL-2.0 */

#ifndef _PDM_DOUBLE_BUFFER_H_INCLUDED
#define _PDM_DOUBLE_BUFFER_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h> // Include native Zephyr kernel

#define DEFAULT_PDM_BUFFER_SIZE 512

class PDMDoubleBuffer {
public:
	PDMDoubleBuffer();
	virtual ~PDMDoubleBuffer();

	void setSize(int size);
	size_t getSize();

	void reset();

	size_t availableForWrite();
	size_t write(const void *buffer, size_t size);
	size_t read(void *buffer, size_t size);
	size_t peek(void *buffer, size_t size);
	void *data();
	size_t available();
	void swap(int length = 0);

private:
	void clear();
	uint8_t *_buffer[2] __attribute__((aligned(16)));
	int _size;
	volatile int _length[2];
	volatile int _readOffset[2];
	volatile int _index;
};

#endif
