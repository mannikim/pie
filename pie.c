/* SPDX-License-Identifier: GPL-3.0-or-later
 * copyright 2025 mannikim <mannikim[at]proton[dot]me>
 * this file is part of pie
 * see LICENSE file for the license text

pie: mannikim's personal image editor

+++ todo +++
- [ ] draw straight line when shift is pressed
- [ ] command line flags:
  - [ ] create file with custom width/height
  - [x] open existing file / read from stdin
  - [x] write to file / write to stdout
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

struct Vec2i {
	int x, y;
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
	unsigned int id, uTr, uWin;
};

struct Canvas {
	struct Image img, drw;
	double scale;
	struct Transform tr;
	struct ImageGRData grImg, grDrw;
	struct ImageShaderData sh;
	unsigned int vao;
};

struct pie {
	char *inFile, *outFile;
	bool useStdin, useStdout;

	struct Canvas canvas;
	struct ColorRGBA color;
	double brushSize;

	bool m0Down;
};

static const char *canvasVertSrc =
	"#version 330 core\n"
	"layout (location = 0) in vec2 aPos;"
	"out vec2 texCoord;"
	"uniform vec4 uTr;"
	"uniform vec2 uWin;"
	"void main() {"
	"vec2 outPos = vec2(uTr.x, uTr.y) + "
	"vec2(aPos.x * uTr.z, aPos.y * uTr.w);"
	"gl_Position = vec4(outPos.x / uWin.x * 2 - 1,"
	"outPos.y / uWin.y * -2 + 1, 0, 1);"
	"texCoord = vec2(gl_VertexID & 1, (gl_VertexID & 0x2) >> 1);"
	"}";

static const char *canvasFragSrc = "#version 330 core\n"
				   "in vec2 texCoord;"
				   "out vec4 FragColor;"
				   "uniform sampler2D tex;"
				   "void main() {"
				   "FragColor = texture(tex, texCoord);"
				   "}";

static void
askColor(struct ColorRGBA *out);

ALWAYS_INLINE void
mouseJustUp(struct Canvas *canvas);

#define WIN_TITLE "pie"

#define WIDTH 800
#define HEIGHT 800

#define UI_CANVAS_SIZE HEIGHT

/* color used to fill a new blank canvas */
#define BG_COLOR (struct ColorRGBA){0xff, 0xff, 0xff, 0xff}

/* TODO: make proper pcp script */
static char *colorPaletteCmd[] = {"pcp", NULL};

#define KEY_COLOR_PALETTE GLFW_KEY_Q
#define KEY_BRUSH_INC_SIZE GLFW_KEY_P
#define KEY_BRUSH_DEC_SIZE GLFW_KEY_O

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
	return x > max ? max : (x < min ? min : x);
}

ALWAYS_INLINE int
mtMax(int a, int b)
{
	return a > b ? a : b;
}

ALWAYS_INLINE int
mtMin(int a, int b)
{
	return a < b ? a : b;
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

ALWAYS_INLINE double
mtStepCount(struct Vec2i d, double w, double h)
{
	return d.x > d.y ? d.x / w : d.y / h;
}

ALWAYS_INLINE struct Vec2f
mtScreen2Canvas(struct Vec2f mp, struct Vec2f cp, double cs)
{
	return (struct Vec2f){(mp.x - cp.x) / cs, (mp.y - cp.y) / cs};
}

static void
inMouseCallback(GLFWwindow *window, int button, int action, int mod)
{
	(void)mod, (void)window;
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
		askColor(&pie->color);
	if (key == KEY_BRUSH_DEC_SIZE && action != GLFW_RELEASE)
		pie->brushSize -= .5;
	if (key == KEY_BRUSH_INC_SIZE && action != GLFW_RELEASE)
		pie->brushSize += .5;
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
	canvas->sh.uWin = glGetUniformLocation(canvas->sh.id, "uWin");

	const struct Transform tr = canvas->tr;
	glUniform4f(canvas->sh.uTr, tr.pos.x, tr.pos.y, tr.size.x, tr.size.y);
	glUniform2f(canvas->sh.uWin, WIDTH, HEIGHT);
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
	glfwSetKeyCallback(*window, inKeyboardCallback);

	glfwSwapInterval(0);

	glewInit();

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	return true;
}

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
		if (size < capacity)
		{
			buffer[size++] = (unsigned char)c;
			c = fgetc(file);
			continue;
		}

		capacity *= 2;
		unsigned char *newBuffer = realloc(buffer, capacity);
		if (!newBuffer)
		{
			free(buffer);
			return NULL;
		}

		buffer = newBuffer;
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
printUsage(FILE *f, const char *prog)
{
	fprintf(f, "%s [-h] [-i file] [-o file] [-stdin] [-stdout]\n", prog);
}

static inline void
parseArguments(struct pie *pie, int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-stdin") == 0)
		{
			pie->useStdin = true;
			continue;
		}
		if (strcmp(argv[i], "-stdout") == 0)
		{
			pie->useStdout = true;
			continue;
		}
		if (strcmp(argv[i], "-o") == 0)
		{
			i++;
			if (i >= argc)
			{
				fprintf(stderr, "Missing output file name\n");
				exit(EXIT_FAILURE);
			}
			pie->outFile = argv[i];
			continue;
		}
		if (strcmp(argv[i], "-i") == 0)
		{
			i++;
			if (i >= argc)
			{
				fprintf(stderr, "Missing input file name\n");
				exit(EXIT_FAILURE);
			}
			pie->inFile = argv[i];
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
	}
	if (pie->useStdin && pie->inFile)
	{
		fprintf(stderr, "Can't use both -i and -stdin.\n");
		exit(EXIT_FAILURE);
	}
	if (pie->useStdout && pie->outFile)
	{
		fprintf(stderr, "Can't use both -o and -stdout.\n");
		exit(EXIT_FAILURE);
	}
}

ALWAYS_INLINE void
writeOutput(struct pie *pie, struct Image img)
{
	if (pie->outFile)
	{
		stbi_write_png(pie->outFile,
			       img.w,
			       img.h,
			       4,
			       img.data,
			       img.w * (int)sizeof(*img.data));
		return;
	}

	if (!pie->useStdout)
		return;

	int stride = img.w * (int)sizeof(*img.data);
	stbi_write_png_to_func(
		writeStdoutImage, NULL, img.w, img.h, 4, img.data, stride);
}

ALWAYS_INLINE void
loadStdin(struct Canvas *canvas)
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

	canvas->img.data = (void *)stbi_load_from_memory(
		data, (int)size, &canvas->img.w, &canvas->img.h, NULL, 4);
	free(data);

	if (canvas->img.data == NULL)
	{
		fprintf(stderr, "Failed to parse standard input\n");
		exit(EXIT_FAILURE);
	}

	size_t pixels = (size_t)(canvas->img.w * canvas->img.h);
	canvas->drw.data = calloc(1, sizeof(*canvas->drw.data) * pixels);

	if (canvas->drw.data == NULL)
	{
		free(canvas->img.data);
		fprintf(stderr, "Failed to parse standard input\n");
		exit(EXIT_FAILURE);
	}

	canvas->drw.w = canvas->img.w;
	canvas->drw.h = canvas->img.h;
}

ALWAYS_INLINE void
loadArgFile(char *filename, struct Canvas *canvas)
{
	FILE *file = fopen(filename, "r");
	if (file == NULL)
	{
		perror("Failed to open input file");
		exit(EXIT_FAILURE);
	}

	canvas->img.data = (void *)stbi_load_from_file(
		file, &canvas->img.w, &canvas->img.h, NULL, 4);

	fclose(file);

	if (canvas->img.data == NULL)
	{
		fprintf(stderr, "Failed to parse input file\n");
		exit(EXIT_FAILURE);
	}

	size_t pixels = (size_t)(canvas->img.w * canvas->img.h);
	canvas->drw.data = calloc(1, sizeof(*canvas->drw.data) * pixels);

	if (canvas->drw.data == NULL)
	{
		free(canvas->img.data);
		fprintf(stderr, "Failed to parse standard input\n");
		exit(EXIT_FAILURE);
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
		loadStdin(&pie->canvas);
		return;
	}

	if (pie->inFile)
	{
		loadArgFile(pie->inFile, &pie->canvas);
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
	size /= 2;
	struct Vec2i d = {v1.x - v0.x, v1.y - v0.y};
	struct Vec2i absd = {abs(d.x), abs(d.y)};
	double count = absd.x > absd.y ? absd.x : absd.y;
	struct Vec2f step = {d.x / count, d.y / count};
	struct Vec2f cur = {v0.x, v0.y};

	for (size_t i = 0; i < (size_t)(count + 1); i++)
	{
		int x = mtMax((int)(size - cur.x), 0);
		int w = mtMin((int)(size - cur.x) + read.w, (int)(size * 2));
		int y = mtMax((int)(size - cur.y), 0);
		int h = mtMin((int)(size - cur.y) + read.h, (int)(size * 2));

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

ALWAYS_INLINE void
mouseDown(struct pie *pie,
	  struct Image buffer,
	  struct Vec2f start,
	  struct Vec2f end)
{
	struct Vec2f rs =
		mtScreen2Canvas(start, pie->canvas.tr.pos, pie->canvas.scale);

	if (mtBoundsZero(rs.x, rs.y, pie->canvas.img.w, pie->canvas.img.h))
	{
		struct Vec2f re;

		re.x = (end.x - pie->canvas.tr.pos.x) / pie->canvas.scale;
		re.x = mtClampd(re.x, 0, pie->canvas.img.w - 1);

		re.y = (end.y - pie->canvas.tr.pos.y) / pie->canvas.scale;
		re.y = mtClampd(re.y, 0, pie->canvas.img.h - 1);

		strokeSizePencil(pie->canvas.img,
				 buffer,
				 pie->color,
				 pie->brushSize,
				 (struct Vec2i){(int)rs.x, (int)rs.y},
				 (struct Vec2i){(int)re.x, (int)re.y});

		glBindTexture(GL_TEXTURE_2D, pie->canvas.grDrw.tex);
		grImageUpdate(pie->canvas.drw);
	}
}

ALWAYS_INLINE void
mouseJustUp(struct Canvas *canvas)
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

	char buffer[9] = {0};
	if (read(fd[0], buffer, sizeof(buffer) - 1) <= 0)
	{
		close(fd[0]);
		fprintf(stderr, "Failed to read from color picker\n");
		return;
	}

	buffer[8] = 0;

	if (!storgba(buffer, out))
		fprintf(stderr, "Failed to parse color %s\n", buffer);

	close(fd[0]);
}

int
main(int argc, char **argv)
{
	struct pie pie = {0};
	pie.canvas.img = (struct Image){0, 50, 50};
	pie.canvas.img = (struct Image){0, 50, 50};
	parseArguments(&pie, argc, argv);

	pie.color = (struct ColorRGBA){0xff, 0, 0, 0xff};
	pie.brushSize = 1;

	loadInputFile(&pie);

	GLFWwindow *window;
	if (!grInit(&pie, &window))
		return EXIT_FAILURE;

	canvasAlign(&pie.canvas);
	grCanvasInitGr(&pie.canvas);

	struct Vec2f m, lastM;
	glfwGetCursorPos(window, &m.x, &m.y);

	while (!glfwWindowShouldClose(window))
	{
		lastM.x = m.x;
		lastM.y = m.y;
		glfwGetCursorPos(window, &m.x, &m.y);
		struct Vec2f rs = mtScreen2Canvas(
			m, pie.canvas.tr.pos, pie.canvas.scale);
		fprintf(stderr,
			"\r\033[K%dx%d \tsize %.1f\t%.1f\t%.1f ",
			pie.canvas.img.w,
			pie.canvas.img.h,
			pie.brushSize,
			rs.x,
			rs.y);

		glClear(GL_COLOR_BUFFER_BIT);
		glBindTexture(GL_TEXTURE_2D, pie.canvas.grImg.tex);
		grDrawImage(pie.canvas.vao);

		if (pie.m0Down)
		{
			glBindTexture(GL_TEXTURE_2D, pie.canvas.grDrw.tex);
			grDrawImage(pie.canvas.vao);
		}

		glfwSwapBuffers(window);
		glfwPollEvents();

		if (pie.m0Down)
			mouseDown(&pie, pie.canvas.drw, lastM, m);
	}

	fputc('\n', stderr);
	writeOutput(&pie, pie.canvas.img);

	glDeleteTextures(1, &pie.canvas.grImg.tex);
	glDeleteTextures(1, &pie.canvas.grDrw.tex);
	glDeleteVertexArrays(1, &pie.canvas.vao);
	glDeleteProgram(pie.canvas.sh.id);

	glfwTerminate();
	free(pie.canvas.img.data);
	free(pie.canvas.drw.data);

	return EXIT_SUCCESS;
}
