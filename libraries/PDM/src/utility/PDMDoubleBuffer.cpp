/* Copyright (C) Arduino SRL (Daniele Aimo)
 * SPDX-License-Identifier: MPL-2.0 */

#include <stdlib.h>
#include <string.h>

#include "PDMDoubleBuffer.h"

PDMDoubleBuffer::PDMDoubleBuffer() : _size(DEFAULT_PDM_BUFFER_SIZE) {
	_buffer[0] = NULL;
	_buffer[1] = NULL;
	reset();
}

void PDMDoubleBuffer::clear() {
	if (_buffer[0] == NULL) {
		k_free(_buffer[0]);
		_buffer[0] = NULL;
	}

	if (_buffer[1] == NULL) {
		k_free(_buffer[1]);
		_buffer[1] = NULL;
	}
}

PDMDoubleBuffer::~PDMDoubleBuffer() {
	clear();
}

void PDMDoubleBuffer::setSize(int size) {
	_size = size;
	reset();
}

size_t PDMDoubleBuffer::getSize() {
	return _size;
}

void PDMDoubleBuffer::reset() {
	clear();

	_buffer[0] = (uint8_t *)k_malloc(_size);
	_buffer[1] = (uint8_t *)k_malloc(_size);

	memset(_buffer[0], 0x00, _size);
	memset(_buffer[1], 0x00, _size);

	_index = 0;
	_length[0] = 0;
	_length[1] = 0;
	_readOffset[0] = 0;
	_readOffset[1] = 0;
}

size_t PDMDoubleBuffer::availableForWrite() {
	return (_size - (_length[_index] - _readOffset[_index]));
}

size_t PDMDoubleBuffer::write(const void *buffer, size_t size) {
	size_t space = availableForWrite();

	if (size > space) {
		size = space;
	}

	if (size == 0) {
		return 0;
	}

	memcpy(&_buffer[_index][_length[_index]], buffer, size);

	_length[_index] += size;

	return size;
}

size_t PDMDoubleBuffer::read(void *buffer, size_t size) {
	size_t avail = available();

	if (size > avail) {
		size = avail;
	}

	if (size == 0) {
		return 0;
	}

	memcpy(buffer, &_buffer[_index][_readOffset[_index]], size);
	_readOffset[_index] += size;

	return size;
}

size_t PDMDoubleBuffer::peek(void *buffer, size_t size) {
	size_t avail = available();

	if (size > avail) {
		size = avail;
	}

	if (size == 0) {
		return 0;
	}

	memcpy(buffer, &_buffer[_index][_readOffset[_index]], size);

	return size;
}

void *PDMDoubleBuffer::data() {
	return (void *)_buffer[_index];
}

size_t PDMDoubleBuffer::available() {
	return _length[_index] - _readOffset[_index];
}

void PDMDoubleBuffer::swap(int length) {
	if (_index == 0) {
		_index = 1;
	} else {
		_index = 0;
	}

	_length[_index] = length;
	_readOffset[_index] = 0;
}
