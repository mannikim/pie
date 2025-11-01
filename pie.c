/*
mannikim's personal image editor

MAXIMUM FILE COUNT: 7
MAXIMUM LINE COUNT:
pie.c: 1777
Makefile: 77
.git and dependencies not included

the limit doesn't care about comments or empty lines
this limit should not compromise the project readability or quality
in other words, code like there is no limit, until we reach it

+++ todo +++
- [x] add file editing
- [ ] add color picker (maybe a separate program?)
  i was thinking about this issue and i thought on these solutions:

  1- the program reads from stdin
  simplest solution but kinda awkward to implement and use

  using graphical + console program is weird as fuck and i don't recall any
  other program doing this. also loading color palettes be damned; it's going
  to be hell to make an interface for it and keeping the save file to stdout
  functional

  2- implement a color picker inside the program
  the most obvious one. also the most complex of them all

  the math for it isn't that bad. the issue is ui. i'm thinking the code for it
  might be too complex for just the picker. although i'm planning on adding
  other stuff for ui too.

  not only that, it kills a nice opportunity to make the program a bit more
  modular

  3- use an external program like dmenu
  keeps the complexity away from this program

  pros:
  - keeps the program modular
  - for a while i've been annoyed by the lack of functionality of a lot of
    different color pickers out there. if i were to use an external program,
    i would make it myself and fix all the little annoyances i have with them
  - simple interface to allow users to use their own pickers, if they please
  - i could be lazy and just call dmenu directly for input
  - style points

  cons:
  - needs another program for it, complicates the editor a bit
  - has a similar issue with displaying color, read the next solution for
  context

  4- keybinds which change the current color
  only alternative where we don't need to implement a color parser

  displaying the current color becomes an issue since i don't have any obvious
  way of displaying it. writing on the screen would require me to implement
  font rendering which i don't want to deal with at this time, and would
  increase the complexity a lot. wouldn't be an issue if i was using some
  higher-level libraries like x11 or sdl but this is not the case here

  i could change the window title for the current color selected. this is
  probably the cleanest way possible

  this is the alternative that i enjoy the least

- [x] brush draws lines instead of setting a pixel every frame
- [x] main() requires some cleanup
- [ ] separate image from canvas to facilitate layers later
- [ ] fix canvas not rendering transparent colors

# stuff for release
- [ ] license
- [ ] proper project description
+++ end todo +++
*/

#include <stdbool.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define ALWAYS_INLINE __attribute__((always_inline)) inline static

#define WIDTH 800
#define HEIGHT 600

#define UI_CANVAS_SIZE 600

/* color used to fill a new blank canvas */
#define BG_COLOR (struct ColorRGBA){0xff, 0xff, 0xff, 0xff}

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
	char r, g, b, a;
};

struct Canvas {
	struct ColorRGBA *pixels;

	int width;
	int height;

	double scale;

	struct Transform tr;
	struct TextureData tex;

	struct {
		unsigned int texture, shader, uTex, uTr;
	} ids;
};

struct Globals {
	bool m0Down, m1Down;
	float aspectRatio;
} static GLOBALS;

struct pie {
	int argc;
	char **argv;
	bool useStdin, useStdout, hasInput;

	struct Canvas canvas;
	struct ColorRGBA color;
};

const static char *canvasVertSrc =
	"#version 330 core\n"
	"layout (location = 0) in vec2 aPos;"
	"out vec2 texCoord;"
	"uniform vec4 uTr;"
	"uniform vec4 uTex;"
	"void main() {"
	"vec2 outPos = vec2(uTr.x, uTr.y) + "
	"vec2(aPos.x * uTr.z, aPos.y * uTr.w);"
	"gl_Position = vec4(outPos.x / 400 - 1, outPos.y / -300 + 1, 0, 1);"
	"texCoord = vec2("
	"(gl_VertexID & 0x1) * uTex.z + uTex.x,"
	"((gl_VertexID & 0x2) >> 1) * uTex.w + uTex.y);"
	"}";

const static char *canvasFragSrc = "#version 330 core\n"
				   "in vec2 texCoord;"
				   "out vec4 FragColor;"
				   "uniform sampler2D tex;"
				   "void main() {"
				   "FragColor = texture(tex, texCoord);"
				   "}";

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

/* INPUT */

static void
inMouseCallback(GLFWwindow *window, int button, int action, int mod)
{
	(void)mod;
	(void)window;

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
grCanvasGenTexture(struct Canvas *canvas)
{
	glGenTextures(1, &canvas->ids.texture);
	glBindTexture(GL_TEXTURE_2D, canvas->ids.texture);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glTexImage2D(GL_TEXTURE_2D,
		     0,
		     GL_RGBA,
		     (int)canvas->width,
		     (int)canvas->height,
		     0,
		     GL_RGBA,
		     GL_UNSIGNED_BYTE,
		     canvas->pixels);
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
	const static float verts[] = {
		0.0,
		0.0,
		1.0,
		0.0,
		0.0,
		1.0,
		1.0,
		1.0,
	};

	const static unsigned int indices[] = {0, 1, 2, 1, 3, 2};

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
grDrawCanvas(struct Canvas *canvas)
{
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

ALWAYS_INLINE void
grCanvasInitGr(struct Canvas *canvas)
{
	grCanvasGenVAO();

	grCanvasGenTexture(canvas);
	canvas->ids.shader = grCanvasGenShader();
	canvas->ids.uTr = glGetUniformLocation(canvas->ids.shader, "uTr");
	canvas->ids.uTex = glGetUniformLocation(canvas->ids.shader, "uTex");


	const struct Transform tr = canvas->tr;
	glUniform4f(canvas->ids.uTr, tr.pos.x, tr.pos.y, tr.size.x, tr.size.y);

	glUniform4f(canvas->ids.uTex,
		    canvas->tex.x,
		    canvas->tex.y,
		    canvas->tex.w,
		    canvas->tex.h);
}

ALWAYS_INLINE void
grCanvasUpdate(struct Canvas *canvas)
{
	glTexSubImage2D(GL_TEXTURE_2D,
			0,
			0,
			0,
			canvas->width,
			canvas->height,
			GL_RGBA,
			GL_UNSIGNED_BYTE,
			canvas->pixels);
}

ALWAYS_INLINE unsigned int
grInit(GLFWwindow **window)
{
	glfwInit();

	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	*window = glfwCreateWindow(WIDTH, HEIGHT, "pie", NULL, NULL);

	if (window == NULL)
		return EXIT_FAILURE;

	glfwMakeContextCurrent(*window);

	glfwSetMouseButtonCallback(*window, inMouseCallback);
	glfwSetFramebufferSizeCallback(*window, grFramebufferCallback);

	glfwSwapInterval(0);

	glewInit();

	return EXIT_SUCCESS;
}

/* ORPHANS */

inline static void
canvasAlign(struct Canvas *canvas)
{
	double s = mtScaleFitIn(
		canvas->width, canvas->height, UI_CANVAS_SIZE, UI_CANVAS_SIZE);

	canvas->scale = s;
	canvas->tr.size.x = canvas->width * s;
	canvas->tr.size.y = canvas->height * s;
	canvas->tr.pos.x = (UI_CANVAS_SIZE - canvas->width * s) / 2.;
	canvas->tr.pos.y = (UI_CANVAS_SIZE - canvas->height * s) / 2.;
}

static uint8_t *
readFileFull(FILE *file, size_t *outSize)
{
	size_t size = 0;
	size_t capacity = 4096;
	unsigned char *buffer = malloc(capacity);
	if (!buffer)
	{
		return NULL;
	}

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
				       canvas->width,
				       canvas->height,
				       4,
				       canvas->pixels,
				       canvas->width *
					       (int)sizeof(*canvas->pixels));
	} else
	{
		stbi_write_png(pie->argv[1],
			       canvas->width,
			       canvas->height,
			       4,
			       canvas->pixels,
			       canvas->width * (int)sizeof(*canvas->pixels));
	}
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

	pie->canvas.pixels = (void *)stbi_load_from_memory(data,
							   (int)size,
							   &pie->canvas.width,
							   &pie->canvas.height,
							   NULL,
							   4);
	free(data);

	if (pie->canvas.pixels == NULL)
	{
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

	pie->canvas.pixels = (void *)stbi_load_from_file(
		file, &pie->canvas.width, &pie->canvas.height, NULL, 4);

	if (pie->canvas.pixels == NULL)
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
		(unsigned int)(pie->canvas.height * pie->canvas.width);
	pie->canvas.pixels = calloc(1, pixels * sizeof(*pie->canvas.pixels));

	if (pie->canvas.pixels == NULL)
	{
		perror("Failed to create blank image");
		exit(EXIT_FAILURE);
	}

	for (unsigned int i = 0; i < pixels; i++)
	{
		pie->canvas.pixels[i] = BG_COLOR;
	}
}

static void
strokePencil(struct Canvas *canvas,
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
	{
		count = absd.x;
	} else
	{
		count = absd.y;
	}

	step.x = d.x / count;
	step.y = d.y / count;

	struct Vec2f cur = v0;


	for (size_t i = 0; i < (size_t)(count + 1); i++)
	{
		size_t id =
			(size_t)cur.x + (size_t)cur.y * (size_t)canvas->width;

		canvas->pixels[id] = color;

		cur.x += step.x;
		cur.y += step.y;
	}
}

static void
mouseDown(struct pie *pie, struct Vec2f start, struct Vec2f end)
{
	/* canvas relative start position */
	struct Vec2f rs;
	rs.x = (start.x - pie->canvas.tr.pos.x) / pie->canvas.scale;
	rs.y = (start.y - pie->canvas.tr.pos.y) / pie->canvas.scale;

	if (mtBounds(rs.x, rs.y, 0, 0, pie->canvas.width, pie->canvas.height))
	{
		struct Vec2f re;

		re.x = (end.x - pie->canvas.tr.pos.x) / pie->canvas.scale;
		re.x = mtClampd(re.x, 0, pie->canvas.width - 1);

		re.y = (end.y - pie->canvas.tr.pos.y) / pie->canvas.scale;
		re.y = mtClampd(re.y, 0, pie->canvas.height - 1);

		strokePencil(&pie->canvas, pie->color, rs, re);
		grCanvasUpdate(&pie->canvas);
	}
}

int
main(int argc, char **argv)
{
	struct pie pie = {0};
	parseArguments(&pie, argc, argv);

	pie.color = (struct ColorRGBA){0xff, 0, 0, 0xff};
	pie.canvas = (struct Canvas){NULL, 50, 50, 0, {0}, {0, 0, 1, 1}, {0}};

	loadInputFile(&pie);

	GLFWwindow *window;
	if (grInit(&window) == EXIT_FAILURE)
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

		glClear(GL_COLOR_BUFFER_BIT);

		if (GLOBALS.m0Down)
			mouseDown(&pie, lastM, m);

		grDrawCanvas(&pie.canvas);

		glfwSwapBuffers(window);
		glfwPollEvents();
	}

	closeProgram(&pie, &pie.canvas);

	glfwTerminate();
	free(pie.canvas.pixels);

	return EXIT_SUCCESS;
}
