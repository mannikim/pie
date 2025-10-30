/*
mannikim's personal image editor

+++ todo +++
- [ ] add file editing
- [ ] add color picker (maybe a separate program?)
- [ ] brush draws lines instead of setting a pixel every frame
- [ ] main() requires some cleanup
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

#define WIDTH 800
#define HEIGHT 600

#define ALWAYS_INLINE __attribute__((always_inline)) inline static

#define UI_CANVAS_SIZE 512

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

	double zoom;

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

	canvas->zoom = s;
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
	const static char *vert_src =
		"#version 330 core\n"
		"layout (location = 0) in vec2 aPos;"
		"out vec2 texCoord;"
		"uniform vec4 uTr;"
		"uniform vec4 uTex;"
		"void main() {"
		"vec2 out_pos = vec2(uTr.x, uTr.y) + "
		"vec2(aPos.x * uTr.z, aPos.y * uTr.w);"
		"gl_Position = vec4(out_pos.x / 400 - 1, out_pos.y / -300 + "
		"1, 0, 1);"
		"texCoord = vec2("
		"(gl_VertexID & 0x1) * uTex.z + uTex.x,"
		"((gl_VertexID & 0x2) >> 1) * uTex.w + uTex.y);"
		"}";

	const static char *frag_src = "#version 330 core\n"
				      "in vec2 texCoord;"
				      "out vec4 FragColor;"
				      "uniform sampler2D tex;"
				      "void main() {"
				      "FragColor = texture(tex, texCoord);"
				      "}";

	unsigned int shv = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(shv, 1, &vert_src, 0);
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
	glShaderSource(shf, 1, &frag_src, 0);
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
read_file_full(FILE *file, size_t *out_size)
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
			unsigned char *new_buffer = realloc(buffer, capacity);

			if (!new_buffer)
			{
				free(buffer);
				return NULL;
			}

			buffer = new_buffer;
		}
		buffer[size++] = (unsigned char)c;
		c = fgetc(file);
	}

	*out_size = size;
	return buffer;
}

static void
write_stdout_image(void *context, void *data, int size)
{
	(void)context;
	fwrite(data, 1, (size_t)size, stdout);
}

int
main(int argc, char **argv)
{
	struct Canvas canvas = {NULL, 128, 256, 0, {0}, {0, 0, 1, 1}, {0}};

	if (argc >= 2 && strcmp(argv[1], "-") == 0)
	{
		int c;
		size_t size;
		uint8_t *data = read_file_full(stdin, &size);
		if (data == NULL)
			return EXIT_FAILURE;
		canvas.pixels = (void *)stbi_load_from_memory(
			data, (int)size, &canvas.width, &canvas.height, &c, 4);
		free(data);
	} else
	{
		canvas.pixels = calloc(1,
				       (unsigned int)canvas.width *
					       (unsigned int)canvas.height *
					       sizeof(*canvas.pixels));
	}

	if (canvas.pixels == NULL)
	{
		fprintf(stderr, "Failed to allocate memory.\n");
		return EXIT_FAILURE;
	}

	GLFWwindow *window;
	if (grInit(&window) == EXIT_FAILURE)
		return EXIT_FAILURE;

	/*
		for (int i = 0; i < canvas.width * canvas.height; i++)
		{
			canvas.pixels[i] = (struct ColorRGBA){0xff, 0xff, 0xff,
	   0xff};
		}*/

	obCanvasAlign(&canvas);
	grCanvasInitGr(&canvas);

	while (!glfwWindowShouldClose(window))
	{
		glClear(GL_COLOR_BUFFER_BIT);

		if (GLOBALS.m0Down)
		{
			double mx, my;
			glfwGetCursorPos(window, &mx, &my);
			mx = (mx - canvas.tr.pos.x) / canvas.zoom;
			my = (my - canvas.tr.pos.y) / canvas.zoom;
			if (mtBounds(
				    mx, my, 0, 0, canvas.width, canvas.height))
			{
				size_t id = (size_t)mx +
					    (size_t)my * (size_t)canvas.width;
				canvas.pixels[id].r = 0xff;
				canvas.pixels[id].g = 0;
				canvas.pixels[id].b = 0;
				grCanvasUpdate(
					&canvas, (int)mx, (int)my, 1, 1);
			}
		}

		grDrawCanvas(&canvas);

		glfwSwapBuffers(window);
		glfwPollEvents();
	}


	if (argc >= 3 && strcmp(argv[2], "-") == 0)
	{
		stbi_write_png_to_func(write_stdout_image,
				       NULL,
				       canvas.width,
				       canvas.height,
				       4,
				       canvas.pixels,
				       canvas.width * (int)sizeof(*canvas.pixels));
	}

	glfwTerminate();
	free(canvas.pixels);

	return EXIT_SUCCESS;
}
