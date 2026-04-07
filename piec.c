/* SPDX-License-Identifier: GPL-3.0-or-later
 * copyright 2025-2026 mannikim <mannikim[at]proton[dot]me>
 * this file is part of pie
 * see LICENSE file for the license text

piec: communicate with a pie instance via a unix domain socket */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "msg.h"

struct ColorRGBA {
	unsigned char r, g, b, a;
};

static bool
stobyte(const char *str, uint8_t *out)
{
	uint8_t n = 0;

	if (str[0] >= 'a' && str[0] <= 'f')
		n |= (uint8_t)(str[0] - 'a' + 10) << 4;
	else if (str[0] >= '0' && str[0] <= '9')
		n |= (uint8_t)(str[0] - '0') << 4;
	else
		return false;

	if (str[1] >= 'a' && str[1] <= 'f')
		n |= (uint8_t)(str[1] - 'a' + 10);
	else if (str[1] >= '0' && str[1] <= '9')
		n |= (uint8_t)(str[1] - '0');
	else
		return false;

	*out = n;
	return true;
}

static bool
storgba(const char *str, struct ColorRGBA *out)
{
	union {
		uint32_t b;
		struct ColorRGBA c;
	} color = {0};

	for (size_t i = 0; i < 4; i++)
	{
		if (str[i] == '\0' || str[i + 1] == '\0')
			return false;

		uint8_t byte;
		if (!stobyte(&str[i * 2], &byte))
			return false;

		color.b |= (uint32_t)byte << (8 * i);
	}

	*out = color.c;

	return true;
}

int
main(int argc, char **argv)
{
	if (argc < 3)
		return EXIT_FAILURE;

	int fd;
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
	{
		perror("socket failed");
		return EXIT_FAILURE;
	}
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, argv[1]);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
	{
		perror("connect failed");
		goto exit_fail;
	}

	if (strcmp(argv[2], "setcolor") == 0)
	{
		if (argc != 4)
		{
			fprintf(stderr, "Missing color for setcolor\n");
			goto exit_fail;
		}
		struct Msg m = {MSG_SET_COLOR, 0};
		if (!storgba(argv[3], (struct ColorRGBA *)&m.data))
		{
			fprintf(stderr, "Failed to parse color %s\n", argv[3]);
			goto exit_fail;
		}
		if (write(fd, &m, sizeof(m)) == -1)
		{
			perror("write failed");
			goto exit_fail;
		}
		goto arg_found;
	}

	if (strcmp(argv[2], "getcolor") == 0)
	{
		struct Msg m = {MSG_GET_COLOR, 0};
		if (write(fd, &m, sizeof(m)) == -1)
		{
			perror("write failed");
			goto exit_fail;
		}

		struct ColorRGBA color;
		if (read(fd, &color, sizeof(color)) != sizeof(color))
		{
			perror("read failed");
			goto exit_fail;
		}
		printf("%02x%02x%02x%02x", color.r, color.g, color.b, color.a);
		goto arg_found;
	}

	fprintf(stderr, "Unknown command: %s\n", argv[2]);
exit_fail:
	close(fd);
	return EXIT_FAILURE;

arg_found:
	close(fd);
	return EXIT_SUCCESS;
}
