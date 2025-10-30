/*
mannikim's personal image editor

+++ todo +++
- [ ] add file editing
- [ ] add color picker (maybe a separate program?)
- [ ] brush draws lines instead of setting a pixel every frame
- [x] main() requires some cleanup
- [ ] separate image from canvas to facilitate layers later
+++ end todo +++
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define ALWAYS_INLINE __attribute__((always_inline)) inline static

#define WIDTH 800
#define HEIGHT 600

#define UI_CANVAS_SIZE 512

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
};

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

inline static void
obCanvasAlign(struct Canvas *canvas)
{
	double s = mtScaleFitIn(
		canvas->width, canvas->height, UI_CANVAS_SIZE, UI_CANVAS_SIZE);

	canvas->scale = s;
	canvas->tr.size.x = canvas->width * s;
	canvas->tr.size.y = canvas->height * s;
	canvas->tr.pos.x = (UI_CANVAS_SIZE - canvas->width * s) / 2.;
	canvas->tr.pos.y = (UI_CANVAS_SIZE - canvas->height * s) / 2.;
}

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
grCanvasGenShader(void)
{
	const static char *vertSrc =
		"#version 330 core\n"
		"layout (location = 0) in vec2 aPos;"
		"out vec2 texCoord;"
		"uniform vec4 uTr;"
		"uniform vec4 uTex;"
		"void main() {"
		"vec2 outPos = vec2(uTr.x, uTr.y) + "
		"vec2(aPos.x * uTr.z, aPos.y * uTr.w);"
		"gl_Position = vec4(outPos.x / 400 - 1, outPos.y / -300 + "
		"1, 0, 1);"
		"texCoord = vec2("
		"(gl_VertexID & 0x1) * uTex.z + uTex.x,"
		"((gl_VertexID & 0x2) >> 1) * uTex.w + uTex.y);"
		"}";

	const static char *fragSrc = "#version 330 core\n"
				     "in vec2 texCoord;"
				     "out vec4 FragColor;"
				     "uniform sampler2D tex;"
				     "void main() {"
				     "FragColor = texture(tex, texCoord);"
				     "}";

	unsigned int shv = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(shv, 1, &vertSrc, 0);
	glCompileShader(shv);

	int success;
	glGetShaderiv(shv, GL_COMPILE_STATUS, &success);
	if (!success)
	{
		char infolog[512];
		glGetShaderInfoLog(shv, 512, 0, infolog);
		printf("%s\n", infolog);
	}

	unsigned int shf = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(shf, 1, &fragSrc, 0);
	glCompileShader(shf);

	glGetShaderiv(shf, GL_COMPILE_STATUS, &success);
	if (!success)
	{
		char infolog[512];
		glGetShaderInfoLog(shf, 512, 0, infolog);
		printf("%s\n", infolog);
	}

	unsigned int shader = glCreateProgram();

	glAttachShader(shader, shv);
	glAttachShader(shader, shf);

	glLinkProgram(shader);

	glGetProgramiv(shader, GL_LINK_STATUS, &success);
	if (!success)
	{
		char infolog[512];
		glGetProgramInfoLog(shader, 512, 0, infolog);
		printf("%s\n", infolog);
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
grCanvasUpdate(struct Canvas *canvas, int x, int y, int width, int height)
{
	glTexSubImage2D(GL_TEXTURE_2D,
			0,
			x,
			y,
			width,
			height,
			GL_RGBA,
			GL_UNSIGNED_BYTE,
			&canvas->pixels[x + y * canvas->width]);
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
	{
		pie->useStdout = true;
	}

	/* optional args */

	if (argc < 3)
		return;

	pie->hasInput = true;

	if (strcmp(argv[2], "-") == 0)
	{
		pie->useStdin = true;
	}
}

/* TODO this is bad interface. that means the data isn't properly bundled. */
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

static void
loadInputFile(struct pie *pie, struct Canvas *canvas)
{
	if (pie->useStdin)
	{
		size_t size;
		uint8_t *data = readFileFull(stdin, &size);

		if (data == NULL)
		{
			perror("Error while reading standard input");
			exit(EXIT_FAILURE);
		}

		canvas->pixels = (void *)stbi_load_from_memory(data,
							       (int)size,
							       &canvas->width,
							       &canvas->height,
							       NULL,
							       4);
		free(data);

		if (canvas->pixels == NULL)
		{
			fprintf(stderr, "Failed to parse standard input\n");
			exit(EXIT_FAILURE);
		}

		return;
	}

	if (pie->hasInput)
	{
		FILE *file = fopen(pie->argv[2], "r");
		if (file == NULL)
		{
			perror("Failed to open input file");
			exit(EXIT_FAILURE);
		}

		canvas->pixels = (void *)stbi_load_from_file(
			file, &canvas->width, &canvas->height, NULL, 4);

		if (canvas->pixels == NULL)
		{
			fprintf(stderr, "Failed to parse input file\n");
		}

		fclose(file);

		return;
	}

	/* new blank canvas */

	unsigned int pixels = (unsigned int)(canvas->height * canvas->width);
	canvas->pixels = calloc(1, pixels * sizeof(*canvas->pixels));

	if (canvas->pixels == NULL)
	{
		perror("Failed to create blank image");
		exit(EXIT_FAILURE);
	}

	for (unsigned int i = 0; i < pixels; i++)
	{
		canvas->pixels[i] = BG_COLOR;
	}
}

static void
mouseDown(GLFWwindow *window, struct Canvas *canvas)
{
	/* painting system */
	/* TODO create a function specifically for painting */

	double mx, my;
	glfwGetCursorPos(window, &mx, &my);

	mx = (mx - canvas->tr.pos.x) / canvas->scale;
	my = (my - canvas->tr.pos.y) / canvas->scale;

	if (mtBounds(mx, my, 0, 0, canvas->width, canvas->height))
	{
		size_t id = (size_t)mx + (size_t)my * (size_t)canvas->width;
		canvas->pixels[id].r = 0xff;
		canvas->pixels[id].g = 0;
		canvas->pixels[id].b = 0;
		grCanvasUpdate(canvas, (int)mx, (int)my, 1, 1);
	}
}

int
main(int argc, char **argv)
{
	struct pie pie = {0};
	parseArguments(&pie, argc, argv);

	struct Canvas canvas = {NULL, 128, 256, 0, {0}, {0, 0, 1, 1}, {0}};

	/* read input file */
	loadInputFile(&pie, &canvas);

	GLFWwindow *window;
	if (grInit(&window) == EXIT_FAILURE)
		return EXIT_FAILURE;

	obCanvasAlign(&canvas);
	grCanvasInitGr(&canvas);

	while (!glfwWindowShouldClose(window))
	{
		glClear(GL_COLOR_BUFFER_BIT);

		if (GLOBALS.m0Down)
		{
			mouseDown(window, &canvas);
		}

		grDrawCanvas(&canvas);

		glfwSwapBuffers(window);
		glfwPollEvents();
	}

	closeProgram(&pie, &canvas);

	glfwTerminate();
	free(canvas.pixels);

	return EXIT_SUCCESS;
}
