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

struct Vec2f {
	double x, y;
};

struct Rect {
	struct Vec2f pos, size;
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
