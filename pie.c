/* SPDX-License-Identifier: GPL-3.0-or-later
 * copyright 2025 mannikim <mannikim[at]proton[dot]me>
 * this file is part of pie
 * see LICENSE file for the license text

mannikim's personal image editor

MAXIMUM FILE COUNT: 7
MAXIMUM LINE COUNT:
pie.c: 1777
Makefile: 77
pcp: 77
.git and dependencies not included

the limit doesn't care about comments or empty lines
this limit should not compromise the project readability or quality
in other words, code like there is no limit, until we reach it

+++ todo +++
- [ ] draw straight line when shift is pressed

# stuff for release
- [ ] proper project description
+++ end todo +++
*/

#include <stdbool.h>
#include <sys/types.h>
#include <unistd.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define ALWAYS_INLINE __attribute__((always_inline)) inline static

struct Vec2f {
	double x, y;
};

struct TextureData {
	double x, y, w, h;
};

struct Transform {
	struct Vec2f pos, size;
};

struct ColorRGBA {
	unsigned char r, g, b, a;
};

struct Image {
	struct ColorRGBA *data;
	int w, h;
};

struct ImageGRData {
	unsigned int tex;
};

struct ImageShaderData {
	unsigned int id, uTex, uTr, uWinWidth, uWinHeight;
};

struct Canvas {
	struct Image img, drw;
	double scale;
	struct Transform tr;
	// TODO: wtf is this field
	struct TextureData tex;
	struct ImageGRData grImg, grDrw;
	struct ImageShaderData sh;
	unsigned int vao;
};

static struct Globals {
	bool m0Down, m1Down;
	float aspectRatio;
} GLOBALS;

struct pie {
	int argc;
	char **argv;
	bool useStdin, useStdout, hasInput;

	struct Canvas canvas;
	struct ColorRGBA color;
};

static const char *canvasVertSrc =
	"#version 330 core\n"
	"layout (location = 0) in vec2 aPos;"
	"out vec2 texCoord;"
	"uniform vec4 uTr;"
	"uniform vec4 uTex;"
	"uniform int uWinWidth;"
	"uniform int uWinHeight;"
	"void main() {"
	"vec2 outPos = vec2(uTr.x, uTr.y) + "
	"vec2(aPos.x * uTr.z, aPos.y * uTr.w);"
	"gl_Position = vec4(outPos.x / (uWinWidth / 2) - 1,"
	"outPos.y / (uWinHeight / -2) + 1, 0, 1);"
	"texCoord = vec2("
	"(gl_VertexID & 0x1) * uTex.z + uTex.x,"
	"((gl_VertexID & 0x2) >> 1) * uTex.w + uTex.y);"
	"}";

static const char *canvasFragSrc = "#version 330 core\n"
				   "in vec2 texCoord;"
				   "out vec4 FragColor;"
				   "uniform sampler2D tex;"
				   "void main() {"
				   "FragColor = texture(tex, texCoord);"
				   "}";
/* FDECL */

static void
askColor(struct ColorRGBA *out);

/* CONFIG */

#define WIN_TITLE "pie"

#define WIDTH 800
#define HEIGHT 800

#define UI_CANVAS_SIZE HEIGHT

/* color used to fill a new blank canvas */
#define BG_COLOR (struct ColorRGBA){0xff, 0xff, 0xff, 0xff}

/* TODO: make proper pcp script */
static char *colorPaletteCmd[] = {"pcp", NULL};

/* MATH */

ALWAYS_INLINE bool
mtBounds(double x, double y, double bx, double by, double w, double h)
{
	return x > bx && x < bx + w && y > by && y < by + h;
}

ALWAYS_INLINE bool
mtBoundsZero(double x0, double y0, double x1, double y1)
{
	return x0 > 0 && x0 < x1 && y0 > 0 && y0 < y1;
}

ALWAYS_INLINE double
mtScaleFitIn(double w0, double h0, double w1, double h1)
{
	double r0 = w1 / w0;
	double r1 = h1 / h0;
	return r0 < r1 ? r0 : r1;
}

ALWAYS_INLINE double
mtClampd(double x, double min, double max)
{
	if (x > max)
		return max;

	if (x < min)
		return min;

	return x;
}

ALWAYS_INLINE struct ColorRGBA
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

/* INPUT */

static void
inMouseCallback(GLFWwindow *window, int button, int action, int mod)
{
	(void)mod, (void)window;

	glfwMakeContextCurrent(window);
	switch (button)
	{
	case GLFW_MOUSE_BUTTON_LEFT:
		GLOBALS.m0Down = action == GLFW_PRESS;
		break;
	case GLFW_MOUSE_BUTTON_RIGHT:
		GLOBALS.m1Down = action == GLFW_PRESS;
		break;
	}
}

static void
inKeyboardCallback(GLFWwindow *window, int key, int scan, int action, int mod)
{
	(void)scan, (void)mod;

	glfwMakeContextCurrent(window);
	struct pie *pie = glfwGetWindowUserPointer(window);
	if (key == GLFW_KEY_Q && action == GLFW_RELEASE)
		askColor(&pie->color);
}

/* GRAPHICS */

static void
grFramebufferCallback(GLFWwindow *window, int width, int height)
{
	glfwMakeContextCurrent(window);
	if ((float)width / (float)height > (float)WIDTH / (float)HEIGHT)
	{
		float t = (float)height / WIDTH;
		int o = (int)(((float)width - (HEIGHT * t)) / 2.f);
		glViewport(o, 0, (int)(WIDTH * t), height);
		return;
	}
	float t = (float)width / WIDTH;
	int o = (int)(((float)height - (HEIGHT * t)) / 2.f);
	glViewport(0, o, width, (int)(HEIGHT * t));
}

ALWAYS_INLINE void
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

inline static unsigned int
grCompileShader(int type, const char *src)
{
	unsigned int out = glCreateShader(type);
	glShaderSource(out, 1, &src, 0);
	glCompileShader(out);

	int success;
	glGetShaderiv(out, GL_COMPILE_STATUS, &success);
	if (!success)
	{
		char infolog[512];
		glGetShaderInfoLog(out, 512, 0, infolog);
		fprintf(stderr, "%s\n", infolog);
	}

	return out;
}

inline static unsigned int
grCanvasGenShader(void)
{
	unsigned int shv = grCompileShader(GL_VERTEX_SHADER, canvasVertSrc);
	unsigned int shf = grCompileShader(GL_FRAGMENT_SHADER, canvasFragSrc);

	unsigned int shader = glCreateProgram();

	glAttachShader(shader, shv);
	glAttachShader(shader, shf);

	glLinkProgram(shader);

	int success;
	glGetProgramiv(shader, GL_LINK_STATUS, &success);
	if (!success)
	{
		char infolog[512];
		glGetProgramInfoLog(shader, 512, 0, infolog);
		fprintf(stderr, "%s\n", infolog);
	}

	glUseProgram(shader);

	glDeleteShader(shv);
	glDeleteShader(shf);

	return shader;
}

static unsigned int
grCanvasGenVAO(void)
{
	static const float verts[] = {0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0};
	static const unsigned int indices[] = {0, 1, 2, 1, 3, 2};

	unsigned int vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	unsigned int vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	unsigned int ebo;
	glGenBuffers(1, &ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
		     sizeof(indices),
		     indices,
		     GL_STATIC_DRAW);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
	glEnableVertexAttribArray(0);

	return vao;
}

static void
grDrawImage(unsigned int vao)
{
	glBindVertexArray(vao);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

ALWAYS_INLINE void
grCanvasInitGr(struct Canvas *canvas)
{
	canvas->vao = grCanvasGenVAO();

	grImageGenTexture(canvas->img, &canvas->grImg.tex);
	grImageGenTexture(canvas->img, &canvas->grDrw.tex);
	canvas->sh.id = grCanvasGenShader();
	canvas->sh.uTr = glGetUniformLocation(canvas->sh.id, "uTr");
	canvas->sh.uTex = glGetUniformLocation(canvas->sh.id, "uTex");
	canvas->sh.uWinWidth =
		glGetUniformLocation(canvas->sh.id, "uWinWidth");
	canvas->sh.uWinHeight =
		glGetUniformLocation(canvas->sh.id, "uWinHeight");

	const struct Transform tr = canvas->tr;
	glUniform4f(canvas->sh.uTr, tr.pos.x, tr.pos.y, tr.size.x, tr.size.y);

	glUniform4f(canvas->sh.uTex,
		    canvas->tex.x,
		    canvas->tex.y,
		    canvas->tex.w,
		    canvas->tex.h);

	glUniform1i(canvas->sh.uWinWidth, WIDTH);
	glUniform1i(canvas->sh.uWinHeight, HEIGHT);
}

ALWAYS_INLINE void
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

ALWAYS_INLINE bool
grInit(struct pie *pie, GLFWwindow **window)
{
	glfwInit();

	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	*window = glfwCreateWindow(WIDTH, HEIGHT, WIN_TITLE, NULL, NULL);

	if (window == NULL)
		return false;

	glfwMakeContextCurrent(*window);
	glfwSetWindowUserPointer(*window, pie);

	glfwSetMouseButtonCallback(*window, inMouseCallback);
	glfwSetFramebufferSizeCallback(*window, grFramebufferCallback);
	glfwSetKeyCallback(*window, inKeyboardCallback);

	glfwSwapInterval(0);

	glewInit();

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	return true;
}

/* ORPHANS */

inline static void
canvasAlign(struct Canvas *canvas)
{
	double s = mtScaleFitIn(
		canvas->img.w, canvas->img.h, UI_CANVAS_SIZE, UI_CANVAS_SIZE);

	canvas->scale = s;
	canvas->tr.size.x = canvas->img.w * s;
	canvas->tr.size.y = canvas->img.h * s;
	canvas->tr.pos.x = (UI_CANVAS_SIZE - canvas->img.w * s) / 2.;
	canvas->tr.pos.y = (UI_CANVAS_SIZE - canvas->img.h * s) / 2.;
}

static uint8_t *
readFileFull(FILE *file, size_t *outSize)
{
	size_t size = 0;
	size_t capacity = 4096;
	unsigned char *buffer = malloc(capacity);
	if (!buffer)
		return NULL;

	int c = fgetc(file);
	while (c != EOF)
	{
		if (size >= capacity)
		{
			capacity *= 2;
			unsigned char *newBuffer = realloc(buffer, capacity);

			if (!newBuffer)
			{
				free(buffer);
				return NULL;
			}

			buffer = newBuffer;
		}

		buffer[size++] = (unsigned char)c;
		c = fgetc(file);
	}

	*outSize = size;
	return buffer;
}

static void
writeStdoutImage(void *context, void *data, int size)
{
	(void)context;
	fwrite(data, 1, (size_t)size, stdout);
}

static void
printUsage(char *progName)
{
	printf("%s -h\n%s <file|-> [infile|-]\n", progName, progName);
}

static inline void
parseArguments(struct pie *pie, int argc, char **argv)
{
	/* required args */

	pie->argc = argc;
	pie->argv = argv;

	if (argc <= 1)
	{
		fprintf(stderr, "No output file specified\n");
		exit(EXIT_FAILURE);
	}

	if (strcmp(argv[1], "-h") == 0)
	{
		printUsage(argv[0]);
		exit(EXIT_SUCCESS);
	}

	if (strcmp(argv[1], "-") == 0)
		pie->useStdout = true;

	/* optional args */

	if (argc < 3)
		return;

	pie->hasInput = true;

	if (strcmp(argv[2], "-") == 0)
		pie->useStdin = true;
}

ALWAYS_INLINE void
closeProgram(struct pie *pie, struct Canvas *canvas)
{
	if (pie->useStdout)
	{
		stbi_write_png_to_func(writeStdoutImage,
				       NULL,
				       canvas->img.w,
				       canvas->img.h,
				       4,
				       canvas->img.data,
				       canvas->img.w *
					       (int)sizeof(*canvas->img.data));
		return;
	}

	stbi_write_png(pie->argv[1],
		       canvas->img.w,
		       canvas->img.h,
		       4,
		       canvas->img.data,
		       canvas->img.w * (int)sizeof(*canvas->img.data));
}

ALWAYS_INLINE void
loadStdin(struct pie *pie)
{
	size_t size;

	/* stb_image.h requires a seekable file, so we can't use stdin
	 * it is possible in some cases, like if you do
	 * pie out.png - < img.png
	 * but assuming we always can't makes the code simpler */
	uint8_t *data = readFileFull(stdin, &size);

	if (data == NULL)
	{
		perror("Error while reading standard input");
		exit(EXIT_FAILURE);
	}

	pie->canvas.img.data =
		(void *)stbi_load_from_memory(data,
					      (int)size,
					      &pie->canvas.img.w,
					      &pie->canvas.img.h,
					      NULL,
					      4);
	free(data);

	if (pie->canvas.img.data == NULL)
	{
		fprintf(stderr, "Failed to parse standard input\n");
		exit(EXIT_FAILURE);
	}

	pie->canvas.drw.data = calloc(1,
				      sizeof(*pie->canvas.drw.data) *
					      (size_t)pie->canvas.img.w *
					      (size_t)pie->canvas.img.h);

	if (pie->canvas.drw.data == NULL)
	{
		free(pie->canvas.img.data);
		fprintf(stderr, "Failed to parse standard input\n");
		exit(EXIT_FAILURE);
	}
}

ALWAYS_INLINE void
loadArgFile(struct pie *pie)
{
	FILE *file = fopen(pie->argv[2], "r");
	if (file == NULL)
	{
		perror("Failed to open input file");
		exit(EXIT_FAILURE);
	}

	pie->canvas.img.data = (void *)stbi_load_from_file(
		file, &pie->canvas.img.w, &pie->canvas.img.h, NULL, 4);

	if (pie->canvas.img.data == NULL)
	{
		fclose(file);
		fprintf(stderr, "Failed to parse input file\n");
		exit(EXIT_FAILURE);
	}

	fclose(file);
}

static void
loadInputFile(struct pie *pie)
{
	if (pie->useStdin)
	{
		loadStdin(pie);
		return;
	}

	if (pie->hasInput)
	{
		loadArgFile(pie);
		return;
	}

	/* new blank canvas */

	unsigned int pixels =
		(unsigned int)(pie->canvas.img.h * pie->canvas.img.w);
	pie->canvas.img.data = malloc(pixels * sizeof(*pie->canvas.img.data));
	if (pie->canvas.img.data == NULL)
	{
		perror("Failed to create blank image");
		exit(EXIT_FAILURE);
	}

	pie->canvas.drw.data = malloc(pixels * sizeof(*pie->canvas.drw.data));
	if (pie->canvas.drw.data == NULL)
	{
		free(pie->canvas.img.data);
		perror("Failed to create blank image");
		exit(EXIT_FAILURE);
	}

	for (unsigned int i = 0; i < pixels; i++)
	{
		pie->canvas.img.data[i] = BG_COLOR;
		pie->canvas.drw.data[i] = (struct ColorRGBA){0, 0, 0, 0};
	}
}

static void
strokePencil(struct Image read,
	     struct Image write,
	     struct ColorRGBA color,
	     struct Vec2f v0,
	     struct Vec2f v1)
{
	struct Vec2f step = {0};

	v0.x = floor(v0.x);
	v0.y = floor(v0.y);
	v1.x = floor(v1.x);
	v1.y = floor(v1.y);

	struct Vec2f d = {v1.x - v0.x, v1.y - v0.y};
	struct Vec2f absd = {fabs(d.x), fabs(d.y)};
	double count;

	if (absd.x > absd.y)
		count = absd.x;
	else
		count = absd.y;

	step.x = d.x / count;
	step.y = d.y / count;

	struct Vec2f cur = v0;

	for (size_t i = 0; i < (size_t)(count + 1); i++)
	{
		size_t id = (size_t)cur.x + (size_t)cur.y * (size_t)read.w;

		write.data[id] = color;

		cur.x += step.x;
		cur.y += step.y;
	}
}

static void
mouseDown(struct pie *pie,
	  struct Image buffer,
	  struct Vec2f start,
	  struct Vec2f end)
{
	/* canvas relative start position */
	struct Vec2f rs;
	rs.x = (start.x - pie->canvas.tr.pos.x) / pie->canvas.scale;
	rs.y = (start.y - pie->canvas.tr.pos.y) / pie->canvas.scale;

	if (mtBounds(rs.x, rs.y, 0, 0, pie->canvas.img.w, pie->canvas.img.h))
	{
		struct Vec2f re;

		re.x = (end.x - pie->canvas.tr.pos.x) / pie->canvas.scale;
		re.x = mtClampd(re.x, 0, pie->canvas.img.w - 1);

		re.y = (end.y - pie->canvas.tr.pos.y) / pie->canvas.scale;
		re.y = mtClampd(re.y, 0, pie->canvas.img.h - 1);

		strokePencil(pie->canvas.img, buffer, pie->color, rs, re);

		glBindTexture(GL_TEXTURE_2D, pie->canvas.grDrw.tex);
		grImageUpdate(pie->canvas.drw);
	}
}

static void
mouseUp(struct Canvas *canvas)
{
	for (size_t i = 0; i < (size_t)(canvas->drw.w * canvas->drw.h); i++)
	{
		struct ColorRGBA color =
			mtBlend(canvas->drw.data[i], canvas->img.data[i]);
		canvas->img.data[i] = color;
		canvas->drw.data[i] = (struct ColorRGBA){0, 0, 0, 0};
	}

	glBindTexture(GL_TEXTURE_2D, canvas->grImg.tex);
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
askColor(struct ColorRGBA *out)
{
	int fd[2];

	if (pipe(fd) == -1)
	{
		perror("Pipe failed");
		return;
	}

	pid_t pid = fork();

	if (pid < 0)
	{
		perror("Fork failed");
		return;
	}

	if (pid == 0)
	{
		close(fd[0]);

		dup2(fd[1], STDOUT_FILENO);
		close(fd[1]);

		execvp(colorPaletteCmd[0], colorPaletteCmd);

		perror("exec failed");
		_exit(EXIT_FAILURE);
	}

	close(fd[1]);

	char buffer[16] = {0};
	if (read(fd[0], buffer, sizeof(buffer) - 1) <= 0)
	{
		close(fd[0]);
		return;
	}

	buffer[15] = 0;

	storgba(buffer, out);

	close(fd[0]);
}

int
main(int argc, char **argv)
{
	struct pie pie = {0};
	parseArguments(&pie, argc, argv);

	pie.color = (struct ColorRGBA){0xff, 0, 0, 0xff};
	pie.canvas = (struct Canvas){.img = {0, 50, 50},
				     .drw = {0, 50, 50},
				     .scale = 0,
				     .tr = {{0, 0}, {0, 0}},
				     .tex = {0, 0, 1, 1},
				     .grImg = {0},
				     .grDrw = {0},
				     .sh = {0},
				     .vao = 0};

	loadInputFile(&pie);

	GLFWwindow *window;
	if (!grInit(&pie, &window))
		return EXIT_FAILURE;

	canvasAlign(&pie.canvas);
	grCanvasInitGr(&pie.canvas);

	struct Vec2f m, lastM;
	glfwGetCursorPos(window, &m.x, &m.y);

	bool m0Released = true;

	while (!glfwWindowShouldClose(window))
	{
		lastM.x = m.x;
		lastM.y = m.y;
		glfwGetCursorPos(window, &m.x, &m.y);

		glClear(GL_COLOR_BUFFER_BIT);
		glBindTexture(GL_TEXTURE_2D, pie.canvas.grImg.tex);
		grDrawImage(pie.canvas.vao);

		if (GLOBALS.m0Down)
		{
			glBindTexture(GL_TEXTURE_2D, pie.canvas.grDrw.tex);
			grDrawImage(pie.canvas.vao);
		}

		glfwSwapBuffers(window);
		glfwPollEvents();

		if (GLOBALS.m0Down)
		{
			m0Released = false;
			mouseDown(&pie, pie.canvas.drw, lastM, m);
		} else if (!m0Released)
		{
			m0Released = true;
			mouseUp(&pie.canvas);
		}
	}

	closeProgram(&pie, &pie.canvas);

	glfwTerminate();
	free(pie.canvas.img.data);
	free(pie.canvas.drw.data);

	return EXIT_SUCCESS;
}
