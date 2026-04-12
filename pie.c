/* SPDX-License-Identifier: GPL-3.0-or-later
 * copyright 2025-2026 mannikim <mannikim[at]proton[dot]me>
 * this file is part of pie
 * see LICENSE file for the license text */

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define WIN_TITLE "pie"
#define WINW 800
#define WINH 800

#include "common.h"

struct Vec2i {
	int x, y;
};

struct Image {
	struct ColorRGBA *data;
	int w, h;
};

struct Canvas {
	struct Image img, drw;
	double scale;
	struct Rect r;
	unsigned int imgTex, drwTex;
	struct ImgShader sh;
	unsigned int vao;
};

struct pie {
	bool useStdin, useStdout, quit, m0Down;
	struct Canvas canvas;
	struct ColorRGBA color;
	double brushSize;
	struct Vec2f m, lastM;
	int fifofd;
};

static const char *canvasFragSrc = "#version 330 core\n"
				   "in vec2 texCoord;"
				   "out vec4 FragColor;"
				   "uniform sampler2D tex;"
				   "void main() {"
				   "FragColor = texture(tex, texCoord);"
				   "}";

static void
runCmd(const char **cmd);

static inline void
sampleImg(struct Image i, int x, int y, struct ColorRGBA *out);

static inline void
mouseJustUp(struct Canvas *canvas);

#define UI_CANVAS_SIZE WINH

/* color used to fill a new blank canvas */
#define BG_COLOR (struct ColorRGBA){0xff, 0xff, 0xff, 0xff}

static const char *colorPickCmd[] = {"pie-cp", NULL};

#define KEY_COLOR_PALETTE GLFW_KEY_Q
#define KEY_BRUSH_INC_SIZE GLFW_KEY_P
#define KEY_BRUSH_DEC_SIZE GLFW_KEY_O
#define KEY_QUIT_NOSAVE GLFW_KEY_ESCAPE
#define KEY_SAMPLE GLFW_KEY_S

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

inline double
mtScaleFitIn(double w0, double h0, double w1, double h1)
{
	double r0 = w1 / w0;
	double r1 = h1 / h0;
	return r0 < r1 ? r0 : r1;
}

static inline struct ColorRGBA
mtBlend(struct ColorRGBA a, struct ColorRGBA b)
{
	double aa = (double)a.a / 255.0;
	double ba = (double)b.a / 255.0;
	double k = ba * (1 - aa);
	double alpha = aa + k;

	struct ColorRGBA out;
	out.r = (uint8_t)((a.r * aa + b.r * ba * k) / alpha);
	out.g = (uint8_t)((a.g * aa + b.g * ba * k) / alpha);
	out.b = (uint8_t)((a.b * aa + b.b * ba * k) / alpha);
	out.a = (uint8_t)alpha * 0xff;

	return out;
}

static inline struct Vec2f
mtScreen2Canvas(struct Vec2f mp, struct Canvas *c)
{
	return (struct Vec2f){(mp.x - c->r.pos.x) / c->scale,
			      (mp.y - c->r.pos.y) / c->scale};
}

static void
inMouseCallback(GLFWwindow *window, int button, int action, int mod)
{
	(void)mod;
	struct pie *pie = glfwGetWindowUserPointer(window);

	glfwMakeContextCurrent(window);

	if (button != GLFW_MOUSE_BUTTON_LEFT)
		return;

	pie->m0Down = action == GLFW_PRESS;
	if (action == GLFW_PRESS)
		return;

	mouseJustUp(&pie->canvas);
}

static void
inKeyboardCallback(GLFWwindow *window, int key, int scan, int action, int mod)
{
	(void)scan, (void)mod;

	glfwMakeContextCurrent(window);
	struct pie *pie = glfwGetWindowUserPointer(window);
	if (key == KEY_COLOR_PALETTE && action == GLFW_RELEASE)
		runCmd(colorPickCmd);
	if (key == KEY_SAMPLE && action != GLFW_RELEASE)
	{
		struct Vec2f rs = mtScreen2Canvas(pie->m, &pie->canvas);
		sampleImg(pie->canvas.img, (int)rs.x, (int)rs.y, &pie->color);
	}
	if (key == KEY_BRUSH_DEC_SIZE && action != GLFW_RELEASE)
		pie->brushSize--;
	if (key == KEY_BRUSH_INC_SIZE && action != GLFW_RELEASE)
		pie->brushSize++;
	if (key == KEY_QUIT_NOSAVE && action != GLFW_RELEASE &&
	    mod == GLFW_MOD_SHIFT)
	{
		pie->useStdout = false;
		pie->quit = true;
	}
}

static void
grImageGenTexture(struct Image img, unsigned int *out)
{
	glGenTextures(1, out);
	glBindTexture(GL_TEXTURE_2D, *out);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glTexImage2D(GL_TEXTURE_2D,
		     0,
		     GL_RGBA,
		     img.w,
		     img.h,
		     0,
		     GL_RGBA,
		     GL_UNSIGNED_BYTE,
		     img.data);
}

static inline void
grImageUpdate(struct Image img)
{
	glTexSubImage2D(GL_TEXTURE_2D,
			0,
			0,
			0,
			img.w,
			img.h,
			GL_RGBA,
			GL_UNSIGNED_BYTE,
			img.data);
}

static inline void
canvasAlign(struct Canvas *canvas)
{
	double s = mtScaleFitIn(
		canvas->img.w, canvas->img.h, UI_CANVAS_SIZE, UI_CANVAS_SIZE);
	canvas->scale = s;
	canvas->r.size.x = canvas->img.w * s;
	canvas->r.size.y = canvas->img.h * s;
	canvas->r.pos.x = (UI_CANVAS_SIZE - canvas->img.w * s) / 2.;
	canvas->r.pos.y = (UI_CANVAS_SIZE - canvas->img.h * s) / 2.;
}

static void
printUsage(FILE *f, const char *prog)
{
	fprintf(f, "%s [-h] [-i] [-o] [-width w] [-height h]\n", prog);
}

static inline void
parseArguments(struct pie *pie, int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-i") == 0)
		{
			pie->useStdin = true;
			continue;
		}
		if (strcmp(argv[i], "-o") == 0)
		{
			pie->useStdout = true;
			continue;
		}
		if (strcmp(argv[i], "-h") == 0)
		{
			if (i + 1 < argc)
			{
				fprintf(stderr, "Excess arguments.\n");
				exit(EXIT_FAILURE);
			}
			printUsage(stdout, argv[0]);
			exit(EXIT_SUCCESS);
		}
		if (strcmp(argv[i], "-width") == 0)
		{
			i++;
			if (i >= argc)
			{
				fprintf(stderr, "Missing width\n");
				exit(EXIT_FAILURE);
			}
			int size = atoi(argv[i]);
			pie->canvas.img.w = size;
			pie->canvas.drw.w = size;
			continue;
		}
		if (strcmp(argv[i], "-height") == 0)
		{
			i++;
			if (i >= argc)
			{
				fprintf(stderr, "Missing height\n");
				exit(EXIT_FAILURE);
			}
			int size = atoi(argv[i]);
			pie->canvas.img.h = size;
			pie->canvas.drw.h = size;
			continue;
		}

		fprintf(stderr, "Failed to parse flag %s\n", argv[i]);
		exit(EXIT_FAILURE);
	}
}

static void
ffwrite(FILE *f, struct Image img)
{
	fputs("farbfeld", f);
	uint32_t x = htonl((uint32_t)img.w);
	fwrite(&x, sizeof(x), 1, f);
	x = htonl((uint32_t)img.h);
	fwrite(&x, sizeof(x), 1, f);
	for (int i = 0; i < img.w * img.h; i++)
	{
		uint16_t y = img.data[i].r * 257;
		fwrite(&y, sizeof(uint16_t), 1, f);
		y = img.data[i].g * 257;
		fwrite(&y, sizeof(uint16_t), 1, f);
		y = img.data[i].b * 257;
		fwrite(&y, sizeof(uint16_t), 1, f);
		y = img.data[i].a * 257;
		fwrite(&y, sizeof(uint16_t), 1, f);
	}
}

static void
ffread(FILE *f, struct Canvas *canvas)
{
	uint32_t header[4];
	fread(header, sizeof(header), 1, f);
	if (header[0] != 0x62726166 && header[1] != 0x646c6566)
	{
		fprintf(stderr, "failed to parse farbfeld magic value\n");
		exit(EXIT_FAILURE);
	}

	canvas->img.w = (signed)ntohl(header[2]);
	canvas->img.h = (signed)ntohl(header[3]);
	size_t pixels = (size_t)(canvas->img.w * canvas->img.h);
	canvas->img.data = malloc(pixels * sizeof(struct ColorRGBA));
	if (canvas->img.data == NULL)
	{
		perror("malloc failed");
		exit(EXIT_FAILURE);
	}

	canvas->drw.data = calloc(1, sizeof(*canvas->drw.data) * pixels);
	if (canvas->drw.data == NULL)
	{
		free(canvas->img.data);
		perror("calloc failed");
		exit(EXIT_FAILURE);
	}

	for (size_t i = 0; i < pixels; i++)
	{
		struct ColorRGBA c;
		uint16_t x;
		fread(&x, sizeof(x), 1, f);
		c.r = x / 257;
		fread(&x, sizeof(x), 1, f);
		c.g = x / 257;
		fread(&x, sizeof(x), 1, f);
		c.b = x / 257;
		fread(&x, sizeof(x), 1, f);
		c.a = x / 257;
		canvas->img.data[i] = c;
	}

	canvas->drw.w = canvas->img.w;
	canvas->drw.h = canvas->img.h;
}

static void
newBlankCanvas(struct Canvas *canvas)
{
	unsigned int pixels = (unsigned int)(canvas->img.h * canvas->img.w);
	canvas->img.data = malloc(pixels * sizeof(*canvas->img.data));
	if (canvas->img.data == NULL)
	{
		perror("Failed to create blank image");
		exit(EXIT_FAILURE);
	}

	canvas->drw.data = malloc(pixels * sizeof(*canvas->drw.data));
	if (canvas->drw.data == NULL)
	{
		free(canvas->img.data);
		perror("Failed to create blank image");
		exit(EXIT_FAILURE);
	}

	for (unsigned int i = 0; i < pixels; i++)
	{
		canvas->img.data[i] = BG_COLOR;
		canvas->drw.data[i] = (struct ColorRGBA){0, 0, 0, 0};
	}
}

static void
loadInputFile(struct pie *pie)
{
	if (pie->useStdin)
	{
		ffread(stdin, &pie->canvas);
		return;
	}
	newBlankCanvas(&pie->canvas);
}

static void
strokeSizePencil(struct Image read,
		 struct Image write,
		 struct ColorRGBA color,
		 double size,
		 struct Vec2i v0,
		 struct Vec2i v1)
{
	struct Vec2i d = {v1.x - v0.x, v1.y - v0.y};
	struct Vec2i absd = {abs(d.x), abs(d.y)};
	double count = MAX(absd.x, absd.y);
	struct Vec2f step = {d.x / count, d.y / count};
	struct Vec2f cur = {v0.x, v0.y};

	for (size_t i = 0; i < (size_t)(count + 1); i++)
	{
		double maxSize = size * 2;
		double sx = size - cur.x;
		int x = (int)MAX(sx, 0);
		int w = (int)MIN(sx + read.w, maxSize);
		double sy = size - cur.y;
		int y = (int)MAX(sy, 0);
		int h = (int)MIN(sy + read.w, maxSize);

		for (int j = y; j < h; j++)
			for (int k = x; k < w; k++)
			{
				struct Vec2i fp = {k + (int)cur.x - (int)size,
						   j + (int)cur.y - (int)size};
				size_t id = (size_t)(fp.x + fp.y * read.w);
				write.data[id] = color;
			}

		cur.x += step.x;
		cur.y += step.y;
	}
}

static inline void
mouseDown(struct pie *pie,
	  struct Image buffer,
	  struct Vec2f start,
	  struct Vec2f end)
{
	struct Vec2f rs = mtScreen2Canvas(start, &pie->canvas);
	if (BOUNDS_ZERO(rs.x, rs.y, pie->canvas.img.w, pie->canvas.img.h))
	{
		struct Vec2f re = mtScreen2Canvas(end, &pie->canvas);
		re.x = CLAMP(re.x, 0, pie->canvas.img.w - 1);
		re.y = CLAMP(re.y, 0, pie->canvas.img.h - 1);
		strokeSizePencil(pie->canvas.img,
				 buffer,
				 pie->color,
				 pie->brushSize / 2,
				 (struct Vec2i){(int)rs.x, (int)rs.y},
				 (struct Vec2i){(int)re.x, (int)re.y});
		glBindTexture(GL_TEXTURE_2D, pie->canvas.drwTex);
		grImageUpdate(pie->canvas.drw);
	}
}

static inline void
mouseJustUp(struct Canvas *canvas)
{
	for (size_t i = 0; i < (size_t)(canvas->drw.w * canvas->drw.h); i++)
	{
		struct ColorRGBA color =
			mtBlend(canvas->drw.data[i], canvas->img.data[i]);
		canvas->img.data[i] = color;
		canvas->drw.data[i] = (struct ColorRGBA){0, 0, 0, 0};
	}

	glBindTexture(GL_TEXTURE_2D, canvas->imgTex);
	grImageUpdate(canvas->img);
}

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

static void
runCmd(const char **cmd)
{
	pid_t pid = fork();

	if (pid < 0)
	{
		perror("\r\033[Kfork failed");
		return;
	}

	if (pid == 0)
	{
		execvp(cmd[0], (void *)cmd);

		perror("\r\033[Kexec failed");
		_exit(EXIT_FAILURE);
	}
}

static inline void
setupFifo(int *outFd)
{
	int fd = open("/tmp/pie.fifo", O_RDWR | O_NONBLOCK);
	if (fd < 0)
	{
		perror("Failed to open /tmp/pie.fifo");
		exit(EXIT_FAILURE);
	}
	*outFd = fd;
}

static inline void
runFifoCmd(struct pie *pie, const char *cmd)
{
	if (strncmp(cmd, "setcolor ", 9) == 0)
	{
		const char *arg = &cmd[9];
		struct ColorRGBA out;
		if (!storgba(arg, &out))
		{
			fprintf(stderr,
				"\r\033[KFailed to parse color %s\n",
				arg);
			return;
		}
		pie->color = out;
		return;
	}

	fprintf(stderr, "\r\033[KFailed to parse command \"%s\"\n", cmd);
}

static inline void
pollFifo(struct pie *pie, fd_set *rfds)
{
	FD_ZERO(rfds);
	FD_SET(pie->fifofd, rfds);
	int res = select(
		pie->fifofd + 1, rfds, NULL, NULL, &(struct timeval){0, 0});
	switch (res)
	{
	case -1:
		perror("select failed");
		exit(EXIT_FAILURE);
	case 0:
		return;
	}

	if (!FD_ISSET(pie->fifofd, rfds))
		return;

	char buffer[512];
	ssize_t n = read(pie->fifofd, buffer, sizeof(buffer) - 1);
	if (n == -1)
	{
		perror("read of fifo failed");
		exit(EXIT_FAILURE);
	}
	buffer[n] = '\0';
	buffer[strcspn(buffer, "\n")] = '\0';
	runFifoCmd(pie, buffer);
}

static inline void
sampleImg(struct Image i, int x, int y, struct ColorRGBA *out)
{
	if (BOUNDS_ZERO(x, y, i.w, i.h))
		*out = i.data[x + y * i.w];
}

static inline void
run(struct pie *pie, GLFWwindow *window)
{
	glfwGetCursorPos(window, &pie->m.x, &pie->m.y);
	fd_set rfst;
	while (!glfwWindowShouldClose(window) && !pie->quit)
	{
		pie->lastM.x = pie->m.x;
		pie->lastM.y = pie->m.y;
		glfwGetCursorPos(window, &pie->m.x, &pie->m.y);
		struct Vec2f rs = mtScreen2Canvas(pie->m, &pie->canvas);
		fprintf(stderr,
			"\r\033[K%dx%d \tsize %.1f\t%.1f\t%.1f\tcolor "
			"%02x%02x%02x%02x",
			pie->canvas.img.w,
			pie->canvas.img.h,
			pie->brushSize,
			rs.x,
			rs.y,
			pie->color.r,
			pie->color.g,
			pie->color.b,
			pie->color.a);

		glClear(GL_COLOR_BUFFER_BIT);
		glBindTexture(GL_TEXTURE_2D, pie->canvas.imgTex);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

		if (pie->m0Down)
		{
			glBindTexture(GL_TEXTURE_2D, pie->canvas.drwTex);
			glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
		}

		glfwSwapBuffers(window);
		glfwPollEvents();
		pollFifo(pie, &rfst);

		if (pie->m0Down)
			mouseDown(pie, pie->canvas.drw, pie->lastM, pie->m);
	}
}

static inline void
quit(struct pie *pie)
{
	fputc('\n', stderr);
	if (pie->useStdout)
		ffwrite(stdout, pie->canvas.img);
	glDeleteTextures(1, &pie->canvas.imgTex);
	glDeleteTextures(1, &pie->canvas.drwTex);
	glDeleteVertexArrays(1, &pie->canvas.vao);
	glDeleteProgram(pie->canvas.sh.id);
	glfwTerminate();
	free(pie->canvas.img.data);
	free(pie->canvas.drw.data);
	close(pie->fifofd);
}

int
main(int argc, char **argv)
{
	struct pie pie = {0};
	pie.canvas.img = (struct Image){0, 50, 50};
	pie.canvas.drw = (struct Image){0, 50, 50};
	pie.color = (struct ColorRGBA){0xff, 0, 0, 0xff};
	pie.brushSize = 1;

	parseArguments(&pie, argc, argv);
	loadInputFile(&pie);
	setupFifo(&pie.fifofd);

	GLFWwindow *window;
	if (!grInit(&pie, &window, inMouseCallback, inKeyboardCallback))
		return EXIT_FAILURE;

	canvasAlign(&pie.canvas);
	pie.canvas.vao = grImgGenVAO();
	grImageGenTexture(pie.canvas.img, &pie.canvas.imgTex);
	grImageGenTexture(pie.canvas.img, &pie.canvas.drwTex);
	grImgInitGr(&pie.canvas.sh, pie.canvas.r, WINW, WINH, canvasFragSrc);

	run(&pie, window);
	quit(&pie);
	return EXIT_SUCCESS;
}
