/* SPDX-License-Identifier: GPL-3.0-or-later
 * copyright 2025-2026 mannikim <mannikim[at]proton[dot]me>
 * this file is part of pie
 * see LICENSE file for the license text */

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WIN_TITLE "pie"

#include "common.h"
#include "msg.h"

struct Image {
	struct ColorRGBA *data;
	int w, h;
};

struct Canvas {
	struct Image img, drw;
	double scale;
	struct Rect r;
	unsigned int imgTex, drwTex;
	struct ImgShader sh, bgSh;
	unsigned int vao;
};

struct Recti {
	struct Vec2i pos, size;
};

struct Area {
	bool selecting, pointSet, areaActive;
	struct Recti r;
};

struct pie {
	bool useStdin, useStdout, quit, m0Down, m1Down;
	struct Area area;
	struct Canvas canvas;
	struct ColorRGBA color;
	double brushSize;
	struct Vec2f m, lastM;
	struct Vec2i win;
	int sockfd;
};

static const char *canvasFragSrc = "#version 330 core\n"
				   "in vec2 texCoord;"
				   "out vec4 FragColor;"
				   "uniform sampler2D tex;"
				   "void main() {"
				   "FragColor = texture(tex, texCoord);"
				   "}";

static const char *bgFragSrc = "#version 330 core\n"
			       "in vec2 texCoord;"
			       "out vec4 FragColor;"
			       "uniform sampler2D tex;"
			       "void main() {"
			       "float c = 0.05;"
			       "ivec2 s = textureSize(tex, 0);"
			       "if (mod(texCoord.x * s.x / 8,2.f) < 1 ^^ "
			       "mod(texCoord.y * s.y / 8,2.f) < 1) {"
			       "c = 0.1f;"
			       "}"
			       "FragColor = vec4(c,c,c,1);"
			       "}";

#define UI_CANVAS_W 1
#define UI_CANVAS_H 1

static const char socketPath[] = "/tmp/pie.sock";
static const char *colorPickCmd[] = {"pie-cp", socketPath, NULL};

#define KEY_COLOR_PALETTE GLFW_KEY_Q
#define KEY_BRUSH_INC_SIZE GLFW_KEY_P
#define KEY_BRUSH_DEC_SIZE GLFW_KEY_O
#define KEY_QUIT_NOSAVE GLFW_KEY_ESCAPE
#define KEY_SAMPLE GLFW_KEY_S
#define KEY_AREA_SELECT GLFW_KEY_A
#define KEY_AREA_FILL GLFW_KEY_F
#define KEY_AREA_RESET GLFW_KEY_D

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

inline double
mtScaleFitIn(double w0, double h0, double w1, double h1)
{
	double r0 = w1 / w0, r1 = h1 / h0;
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

static inline struct Vec2f
mtCanvas2Screen(struct Vec2f mp, struct Canvas *c) {
	return (struct Vec2f){mp.x * c->scale + c->r.pos.x,
			      mp.y * c->scale + c->r.pos.y};
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
grDrawArea(struct Area *s, struct Canvas *c, struct Vec2i win)
{
	glBegin(GL_LINE_LOOP);
	glColor3ub(0xff, 0xff, 0xff);
	struct Vec2f t = {s->r.pos.x, s->r.pos.y};
	struct Vec2f st = mtCanvas2Screen(t, c);
	struct Vec2f b = {s->r.size.x + s->r.pos.x, s->r.size.y + s->r.pos.y};
	struct Vec2f sb = mtCanvas2Screen(b, c);
	st.x = st.x / win.x * 2 - 1;
	st.y = st.y / win.y * -2 + 1;
	sb.x = sb.x / win.x * 2 - 1;
	sb.y = sb.y / win.y * -2 + 1;
	glVertex2d(st.x, st.y);
	glVertex2d(st.x, sb.y);
	glVertex2d(sb.x, sb.y);
	glVertex2d(sb.x, st.y);
	glEnd();
}

static inline void
canvasAlign(struct Canvas *canvas, struct Vec2i win)
{
	double s = mtScaleFitIn(canvas->img.w,
				canvas->img.h,
				UI_CANVAS_W * win.x,
				UI_CANVAS_H * win.y);
	canvas->scale = s;
	canvas->r.size.x = canvas->img.w * s;
	canvas->r.size.y = canvas->img.h * s;
	canvas->r.pos.x = (UI_CANVAS_W * win.x - canvas->img.w * s) / 2.;
	canvas->r.pos.y = (UI_CANVAS_H * win.y - canvas->img.h * s) / 2.;
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
	fwrite(&x, sizeof x, 1, f);
	x = htonl((uint32_t)img.h);
	fwrite(&x, sizeof x, 1, f);
	for (int i = 0; i < img.w * img.h; i++)
	{
		uint16_t y = img.data[i].r * 257;
		fwrite(&y, sizeof y, 1, f);
		y = img.data[i].g * 257;
		fwrite(&y, sizeof y, 1, f);
		y = img.data[i].b * 257;
		fwrite(&y, sizeof y, 1, f);
		y = img.data[i].a * 257;
		fwrite(&y, sizeof y, 1, f);
	}
}

static void
ffread(FILE *f, struct Canvas *canvas)
{
	uint32_t header[4];
	fread(header, sizeof header, 1, f);
	if (header[0] != 0x62726166 && header[1] != 0x646c6566)
	{
		fprintf(stderr, "failed to parse farbfeld magic value\n");
		exit(EXIT_FAILURE);
	}

	canvas->img.w = (signed)ntohl(header[2]);
	canvas->img.h = (signed)ntohl(header[3]);
	size_t pixels = (size_t)(canvas->img.w * canvas->img.h);
	canvas->img.data = malloc(pixels * sizeof *canvas->img.data);
	if (canvas->img.data == NULL)
	{
		perror("malloc failed");
		exit(EXIT_FAILURE);
	}

	canvas->drw.data = calloc(1, sizeof *canvas->drw.data * pixels);
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
		fread(&x, sizeof x, 1, f);
		c.r = x / 257;
		fread(&x, sizeof x, 1, f);
		c.g = x / 257;
		fread(&x, sizeof x, 1, f);
		c.b = x / 257;
		fread(&x, sizeof x, 1, f);
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
	canvas->img.data = calloc(1, pixels * sizeof *canvas->img.data);
	if (canvas->img.data == NULL)
	{
		perror("Failed to create blank image");
		exit(EXIT_FAILURE);
	}

	canvas->drw.data = calloc(1, pixels * sizeof *canvas->drw.data);
	if (canvas->drw.data == NULL)
	{
		free(canvas->img.data);
		perror("Failed to create blank image");
		exit(EXIT_FAILURE);
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
				int px = k + (int)cur.x - (int)size;
				int py = j + (int)cur.y - (int)size;
				size_t id = (size_t)(px + py * read.w);
				write.data[id] = color;
			}

		cur.x += step.x;
		cur.y += step.y;
	}
}

static inline void
areaAbort(struct Area *s)
{
	s->selecting = false;
	s->pointSet = false;
	s->areaActive = false;
}

static inline void
areaP2(struct Area *s, struct Vec2i p)
{
	if (p.x < s->r.pos.x)
	{
		s->r.size.x = s->r.pos.x - p.x + 1;
		s->r.pos.x = p.x;
	} else
		s->r.size.x = p.x - s->r.pos.x + 1;

	if (p.y < s->r.pos.y)
	{
		s->r.size.y = s->r.pos.y - p.y + 1;
		s->r.pos.y = p.y;
	} else
		s->r.size.y = p.y - s->r.pos.y + 1;

	s->selecting = false;
	s->pointSet = false;
}

static inline void
commitDraw(struct Image img, struct Image drw)
{
	for (size_t i = 0; i < (size_t)(drw.w * drw.h); i++)
	{
		struct ColorRGBA color = mtBlend(drw.data[i], img.data[i]);
		img.data[i] = color;
		drw.data[i] = (struct ColorRGBA){0, 0, 0, 0};
	}
}

static inline void
imageFill(struct Image i, struct Recti r, struct ColorRGBA c)
{
	for (int x = r.pos.x; x < r.size.x + r.pos.x; x++)
		for (int y = r.pos.y; y < r.size.y + r.pos.y; y++)
			i.data[x + y * i.w] = c;
}

static inline void
mouseDown(struct pie *pie, struct Vec2f start, struct Vec2f end)
{
	if (pie->area.selecting)
		return;
	struct Vec2f rs = mtScreen2Canvas(start, &pie->canvas);
	if (BOUNDS_ZERO(rs.x, rs.y, pie->canvas.img.w, pie->canvas.img.h))
	{
		struct Vec2f re = mtScreen2Canvas(end, &pie->canvas);
		re.x = CLAMP(re.x, 0, pie->canvas.img.w - 1);
		re.y = CLAMP(re.y, 0, pie->canvas.img.h - 1);
		strokeSizePencil(pie->canvas.img,
				 pie->canvas.drw,
				 pie->color,
				 pie->brushSize / 2,
				 (struct Vec2i){(int)rs.x, (int)rs.y},
				 (struct Vec2i){(int)re.x, (int)re.y});
		glBindTexture(GL_TEXTURE_2D, pie->canvas.drwTex);
		grImageUpdate(pie->canvas.drw);
	}
}

static inline void
mouse2Down(struct pie *pie, struct Vec2f start, struct Vec2f end)
{
	struct Vec2f rs = mtScreen2Canvas(start, &pie->canvas);
	if (BOUNDS_ZERO(rs.x, rs.y, pie->canvas.img.w, pie->canvas.img.h))
	{
		struct Vec2f re = mtScreen2Canvas(end, &pie->canvas);
		re.x = CLAMP(re.x, 0, pie->canvas.img.w - 1);
		re.y = CLAMP(re.y, 0, pie->canvas.img.h - 1);
		strokeSizePencil(pie->canvas.img,
				 pie->canvas.img,
				 (struct ColorRGBA){0, 0, 0, 0},
				 pie->brushSize / 2,
				 (struct Vec2i){(int)rs.x, (int)rs.y},
				 (struct Vec2i){(int)re.x, (int)re.y});
		glBindTexture(GL_TEXTURE_2D, pie->canvas.imgTex);
		grImageUpdate(pie->canvas.img);
	}
}

static inline void
mouseJustUp(struct pie *pie)
{
	if (pie->area.selecting)
	{
		if (!pie->area.pointSet)
		{
			pie->area.pointSet = true;
			return;
		}

		struct Vec2f m = mtScreen2Canvas(pie->m, &pie->canvas);
		struct Vec2i mi = {(int)m.x, (int)m.y};
		areaP2(&pie->area, mi);
		return;
	}

	commitDraw(pie->canvas.img, pie->canvas.drw);
	glBindTexture(GL_TEXTURE_2D, pie->canvas.imgTex);
	grImageUpdate(pie->canvas.img);
}

static inline void
mouseJustDown(struct pie *pie)
{
	if (!pie->area.selecting || pie->area.pointSet)
		return;
	struct Vec2f m = mtScreen2Canvas(pie->m, &pie->canvas);
	if (BOUNDS_ZERO(m.x, m.y, pie->canvas.img.w, pie->canvas.img.h))
		pie->area.r.pos = (struct Vec2i){(int)m.x, (int)m.y};
	else
		areaAbort(&pie->area);
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
setupSock(const char *path, int *outFd)
{
	int fd;
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
	{
		perror("socket failed");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, path);

	unlink(path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
	{
		perror("bind failed");
		exit(EXIT_FAILURE);
	}
	if (listen(fd, 8) == -1)
	{
		perror("listen failed");
		exit(EXIT_FAILURE);
	}
	*outFd = fd;
}

inline void
runMsg(struct pie *pie, struct Msg m, int fd)
{
	switch (m.id)
	{
	case MSG_GET_COLOR:
		write(fd, &pie->color, sizeof(pie->color));
		return;
	case MSG_SET_COLOR:
		pie->color = m.data.color;
		return;
	default:
		fprintf(stderr, "\r\033[KUnknown message id \"%lu\"\n", m.id);
	}
}

static inline void
pollSock(struct pie *pie)
{
	struct pollfd pfd = {pie->sockfd, POLLIN, 0};
	int res = poll(&pfd, 1, 0);
	switch (res)
	{
	case -1:
		perror("poll failed");
		exit(EXIT_FAILURE);
	case 0:
		return;
	}

	if (!(pfd.revents & POLLIN))
		return;

	int clientfd;
	if ((clientfd = accept(pie->sockfd, NULL, NULL)) == -1)
	{
		perror("accept failed");
		exit(EXIT_FAILURE);
	}

	struct Msg msg;
	ssize_t n = read(clientfd, &msg, sizeof(msg));
	if (n != sizeof(msg))
	{
		perror("read failed");
		exit(EXIT_FAILURE);
	}
	runMsg(pie, msg, clientfd);
	if (close(clientfd) == -1)
	{
		perror("close failed");
		exit(EXIT_FAILURE);
	}
}

static inline void
sampleImg(struct Image i, int x, int y, struct ColorRGBA *out)
{
	if (BOUNDS_ZERO(x, y, i.w, i.h))
		*out = i.data[x + y * i.w];
}

static void
cbMouse(GLFWwindow *window, int mb, int action, int mod)
{
	(void)mod;
	struct pie *pie = glfwGetWindowUserPointer(window);

	glfwMakeContextCurrent(window);

	pie->m0Down = mb == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS;
	pie->m1Down = mb == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS;
	if (mb != GLFW_MOUSE_BUTTON_LEFT)
		return;

	if (action == GLFW_PRESS)
		mouseJustDown(pie);
	if (action == GLFW_RELEASE)
		mouseJustUp(pie);
}

static void
cbKeyboard(GLFWwindow *window, int key, int scan, int action, int mod)
{
	(void)scan, (void)mod;

	glfwMakeContextCurrent(window);
	struct pie *pie = glfwGetWindowUserPointer(window);
	if (key == KEY_COLOR_PALETTE && action == GLFW_RELEASE)
		runCmd(colorPickCmd);
	if (key == KEY_AREA_SELECT && action == GLFW_PRESS)
	{
		if (pie->area.selecting)
			areaAbort(&pie->area);
		else
		{
			pie->area.selecting = true;
			pie->area.pointSet = mod == GLFW_MOD_SHIFT;
		}
	}
	if (key == KEY_AREA_RESET && action == GLFW_PRESS)
	{
		areaAbort(&pie->area);
		pie->area.r = (struct Recti){{0, 0}, {0, 0}};
	}
	if (key == KEY_AREA_FILL && action == GLFW_PRESS)
	{
		imageFill(pie->canvas.img, pie->area.r, pie->color);
		glBindTexture(GL_TEXTURE_2D, pie->canvas.imgTex);
		grImageUpdate(pie->canvas.img);
	}
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
cbWinSize(struct GLFWwindow *window, int w, int h)
{
	struct pie *pie = glfwGetWindowUserPointer(window);
	pie->win = (struct Vec2i){w, h};
	glViewport(0, 0, w, h);
	canvasAlign(&pie->canvas, pie->win);
	glUseProgram(pie->canvas.sh.id);
	grImgUpdate(&pie->canvas.sh, pie->canvas.r, w, h);
	glUseProgram(pie->canvas.bgSh.id);
	grImgUpdate(&pie->canvas.bgSh, pie->canvas.r, w, h);
}

static inline void
run(struct pie *pie, GLFWwindow *window)
{
	glfwGetCursorPos(window, &pie->m.x, &pie->m.y);
	while (!glfwWindowShouldClose(window) && !pie->quit)
	{
		pie->lastM.x = pie->m.x;
		pie->lastM.y = pie->m.y;
		glfwGetCursorPos(window, &pie->m.x, &pie->m.y);
		struct Vec2f rs = mtScreen2Canvas(pie->m, &pie->canvas);
		fprintf(stderr,
			"\r\033[K%dx%d \tsize %.1f\t%.1f\t%.1f\tcolor "
			"%02x%02x%02x%02x\tarea%c%d,%d%c%dx%d",
			pie->canvas.img.w,
			pie->canvas.img.h,
			pie->brushSize,
			rs.x,
			rs.y,
			pie->color.r,
			pie->color.g,
			pie->color.b,
			pie->color.a,
			pie->area.selecting && !pie->area.pointSet
				? '>'
				: ' ',
			pie->area.r.pos.x,
			pie->area.r.pos.y,
			pie->area.selecting && pie->area.pointSet
				? '>'
				: ' ',
			pie->area.r.size.x,
			pie->area.r.size.y);
		glClear(GL_COLOR_BUFFER_BIT);
		glUseProgram(pie->canvas.bgSh.id);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
		glUseProgram(pie->canvas.sh.id);
		glBindTexture(GL_TEXTURE_2D, pie->canvas.imgTex);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

		if (pie->m0Down)
		{
			glBindTexture(GL_TEXTURE_2D, pie->canvas.drwTex);
			glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
		}

		glUseProgram(0);
		grDrawArea(&pie->area, &pie->canvas, pie->win);

		glfwSwapBuffers(window);
		glfwPollEvents();
		pollSock(pie);

		if (pie->m0Down)
			mouseDown(pie, pie->lastM, pie->m);
		if (pie->m1Down)
			mouse2Down(pie, pie->lastM, pie->m);
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
	glDeleteProgram(pie->canvas.bgSh.id);
	glfwTerminate();
	free(pie->canvas.img.data);
	free(pie->canvas.drw.data);
	close(pie->sockfd);
}

int
main(int argc, char **argv)
{
	struct pie pie = {0};
	pie.win = (struct Vec2i){800, 800};
	pie.canvas.img = (struct Image){0, 32, 32};
	pie.canvas.drw = (struct Image){0, 32, 32};
	pie.color = (struct ColorRGBA){0xff, 0, 0, 0xff};
	pie.brushSize = 1;

	parseArguments(&pie, argc, argv);
	loadInputFile(&pie);
	setupSock(socketPath, &pie.sockfd);

	GLFWwindow *window;
	if (!grInit(&pie, &window, pie.win, 1, cbMouse, cbKeyboard, cbWinSize))
		return EXIT_FAILURE;

	canvasAlign(&pie.canvas, pie.win);
	pie.canvas.vao = grImgGenVAO();
	grImageGenTexture(pie.canvas.img, &pie.canvas.imgTex);
	grImageGenTexture(pie.canvas.img, &pie.canvas.drwTex);
	grImgInitGr(&pie.canvas.sh, canvasFragSrc);
	grImgInitGr(&pie.canvas.bgSh, bgFragSrc);
	cbWinSize(window, pie.win.x, pie.win.y);

	run(&pie, window);
	quit(&pie);
	return EXIT_SUCCESS;
}
