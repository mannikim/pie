/* SPDX-License-Identifier: GPL-3.0-or-later
 * copyright 2025-2026 mannikim <mannikim[at]proton[dot]me>
 * this file is part of pie
 * see LICENSE file for the license text */

#include <stdbool.h>
#include <stdio.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

struct ColorRGBA {
	unsigned char r, g, b, a;
};

struct Vec2i {
	int x, y;
};

struct Vec2f {
	double x, y;
};

struct Rect {
	struct Vec2f pos, size;
};

struct ImgShader {
	unsigned int id, uWin, uTr;
};

static const char *imgVertSrc =
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

/* Vec2 p, Rect r */
#define BOUNDS(p, r) \
	(p.x > r.pos.x && p.x < r.pos.x + r.size.x && p.y > r.pos.y && \
	 p.y < r.pos.y + r.size.y)

#define BOUNDS_ZERO(x0, y0, x1, y1) \
	((x0) > 0 && (x0) < (x1) && (y0) > 0 && (y0) < (y1))

#define CLAMP(x, min, max) ((x) > (max) ? (max) : ((x) < (min) ? (min) : (x)))

/* Vec2 p, Rect r */
#define RECT_UV(p, r) \
	(struct Vec2f) \
	{ \
		(p.x - r.pos.x) / r.size.x, (p.y - r.pos.y) / r.size.y \
	}

static unsigned int
grImgGenVAO(void)
{
	static const float verts[] = {0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0};
	static const unsigned int index[] = {0, 1, 2, 1, 3, 2};

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
	glBufferData(
		GL_ELEMENT_ARRAY_BUFFER, sizeof(index), index, GL_STATIC_DRAW);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
	glEnableVertexAttribArray(0);

	return vao;
}

static unsigned int
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

static unsigned int
grGenShader(const char *vertSrc, const char *fragSrc)
{
	unsigned int shv = grCompileShader(GL_VERTEX_SHADER, vertSrc);
	unsigned int shf = grCompileShader(GL_FRAGMENT_SHADER, fragSrc);

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

static inline void
grImgInitGr(struct ImgShader *sh, const char *fragSrc)
{
	sh->id = grGenShader(imgVertSrc, fragSrc);
	sh->uTr = glGetUniformLocation(sh->id, "uTr");
	sh->uWin = glGetUniformLocation(sh->id, "uWin");
}

static inline void
grImgUpdate(struct ImgShader *sh, struct Rect r, double winW, double winH)
{
	glUniform4f(sh->uTr, r.pos.x, r.pos.y, r.size.x, r.size.y);
	glUniform2f(sh->uWin, winW, winH);
}

static inline bool
grInit(void *data,
       GLFWwindow **window,
       struct Vec2i win,
       bool resize,
       GLFWmousebuttonfun mouse,
       GLFWkeyfun key,
       GLFWwindowsizefun winSize)
{
	if (!glfwInit())
		return false;

	glfwWindowHint(GLFW_RESIZABLE, resize);
	*window = glfwCreateWindow(win.x, win.y, WIN_TITLE, NULL, NULL);

	if (window == NULL)
		return false;

	glfwMakeContextCurrent(*window);
	glfwSetWindowUserPointer(*window, data);
	glfwSetMouseButtonCallback(*window, mouse);
	glfwSetKeyCallback(*window, key);
	glfwSetWindowSizeCallback(*window, winSize);

	glfwSwapInterval(0);

	glewInit();

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	return true;
}
