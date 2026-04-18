/* SPDX-License-Identifier: GPL-3.0-or-later
 * copyright 2025-2026 mannikim <mannikim[at]proton[dot]me>
 * this file is part of pie
 * see LICENSE file for the license text */

#include <math.h>
#include <stdlib.h>

#define WIN_TITLE "pcp"
#define WINW 300
#define WINH 332

#include "common.h"

enum SelectionType { SEL_NONE, SEL_HSV_WHEEL, SEL_VAL_BAR };

struct ColorHSV {
	double h, s, v;
};

struct HSVWheel {
	struct ImgShader sh;
	struct Rect r;
	struct Vec2f pos;
	struct ColorHSV c;
};

struct ValueBar {
	struct ImgShader sh;
	struct Rect r;
	double v;
};

struct pcp {
	struct HSVWheel hsvWheel;
	struct ValueBar valBar;
	struct ColorRGBA color;
	bool m0Down, quit;
	enum SelectionType selection;
};

static const char *hsvWheelFragSrc =
	"#version 330 core\n"
	"#define PI 3.14159265358979\n"
	"in vec2 texCoord;"
	"out vec4 FragColor;"
	"void main() {"
	"vec4 color = vec4(0,0,0,0);"
	"vec2 pos = texCoord - vec2(0.5,0.5);"
	"float l = length(pos);"
	"if (l < 0.5) {"
	"float hue = atan(pos.y, -pos.x) / PI / 2;"
	"vec4 k = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);"
	"vec3 p = abs(fract(vec3(hue,hue,hue) + k.xyz) * 6.0 - k.www);"
	"color = vec4(mix(k.xxx, clamp(p - k.xxx, 0.0, 1.0), l * 2),1);"
	"}"
	"FragColor = color;"
	"}";

static const char *valBarFragSrc = "#version 330 core\n"
				   "in vec2 texCoord;"
				   "out vec4 FragColor;"
				   "void main() {"
				   "FragColor = vec4(texCoord.xxx, 1);"
				   "}";

#define PI 3.14159265358979

#define UI_VAL_HEIGHT 32

#define KEY_CONFIRM GLFW_KEY_Q

#define FRAC(x) ((x) - (double)(int64_t)(x))
#define LERP(x, y, a) ((x) + ((y) - (x)) * (a))
/* Vec2 p, Rect r */
#define RECT_INVUV(p, r) \
	(struct Vec2f) { p.x *r.size.x + r.pos.x, p.y *r.size.y + r.pos.y }

static inline struct ColorRGBA
mtHSV2RGBA(struct ColorHSV c)
{
	double px = fabs(FRAC(c.h + 1) * 6 - 3);
	double py = fabs(FRAC(c.h + 2 / 3.) * 6 - 3);
	double pz = fabs(FRAC(c.h + 1 / 3.) * 6 - 3);
	double pcx = CLAMP(px - 1, 0, 1);
	double pcy = CLAMP(py - 1, 0, 1);
	double pcz = CLAMP(pz - 1, 0, 1);
	uint8_t r = (uint8_t)(LERP(1, pcx, c.s) * c.v * 0xff);
	uint8_t g = (uint8_t)(LERP(1, pcy, c.s) * c.v * 0xff);
	uint8_t b = (uint8_t)(LERP(1, pcz, c.s) * c.v * 0xff);
	return (struct ColorRGBA){r, g, b, 0xff};
}

static void
grDrawImage(unsigned int shader)
{
	glUseProgram(shader);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

static void
grDrawMark(struct Vec2f pos, struct Vec2f win)
{
	double x0 = (pos.x - 4) / win.x * 2 - 1;
	double x1 = (pos.x + 3) / win.x * 2 - 1;
	double x2 = pos.x / win.x * 2 - 1;
	double y0 = (pos.y - 4) / win.y * -2 + 1;
	double y1 = (pos.y + 3) / win.y * -2 + 1;
	double y2 = pos.y / win.y * -2 + 1;
	glBegin(GL_LINES);
	glVertex2d(x0, y2);
	glVertex2d(x1, y2);
	glVertex2d(x2, y0);
	glVertex2d(x2, y1);
	glEnd();
}

static struct ColorHSV
HSVWheelAt(struct Vec2f pos)
{
	double h = atan2(pos.y, -pos.x) / PI / 2;
	double len = sqrt(pow(pos.x, 2) + pow(pos.y, 2)) * 2;
	double s = CLAMP(len, 0, 1);
	return (struct ColorHSV){h, s, 1};
}

static inline void
mouseDown(struct pcp *pcp, GLFWwindow *window)
{
	struct Vec2f m;
	glfwGetCursorPos(window, &m.x, &m.y);
	switch (pcp->selection)
	{
	case SEL_HSV_WHEEL:
	{
		struct Vec2f hsvRel = RECT_UV(m, pcp->hsvWheel.r);
		hsvRel.x -= .5;
		hsvRel.y -= .5;
		double d = sqrt(pow(hsvRel.x, 2) + pow(hsvRel.y, 2)) * 2;
		if (d > 1)
		{
			hsvRel.x /= d;
			hsvRel.y /= d;
		}
		pcp->hsvWheel.c = HSVWheelAt(hsvRel);
		hsvRel.x += .5;
		hsvRel.y += .5;
		pcp->hsvWheel.pos = RECT_INVUV(hsvRel, pcp->hsvWheel.r);
		break;
	}
	case SEL_VAL_BAR:
	{
		struct Rect r = pcp->valBar.r;
		double x = CLAMP(m.x, r.pos.x, r.size.x + r.pos.x);
		pcp->valBar.v =
			(x - pcp->valBar.r.pos.x) / pcp->valBar.r.size.x;
		break;
	}
	default:
		break;
	}

	struct ColorHSV hsv = {
		pcp->hsvWheel.c.h, pcp->hsvWheel.c.s, pcp->valBar.v};
	pcp->color = mtHSV2RGBA(hsv);
}

static void
cbMouse(GLFWwindow *window, int button, int action, int mod)
{
	(void)mod;
	struct pcp *pcp = glfwGetWindowUserPointer(window);

	glfwMakeContextCurrent(window);

	if (button != GLFW_MOUSE_BUTTON_LEFT)
		return;

	pcp->m0Down = action == GLFW_PRESS;

	pcp->selection = SEL_NONE;
	if (action != GLFW_PRESS)
		return;

	struct Vec2f m;
	glfwGetCursorPos(window, &m.x, &m.y);
	if (BOUNDS(m, pcp->hsvWheel.r))
	{
		pcp->selection = SEL_HSV_WHEEL;
		return;
	}
	if (BOUNDS(m, pcp->valBar.r))
		pcp->selection = SEL_VAL_BAR;
}

static void
cbKeyboard(GLFWwindow *window, int key, int scan, int action, int mod)
{
	(void)scan, (void)mod;
	glfwMakeContextCurrent(window);
	struct pcp *pcp = glfwGetWindowUserPointer(window);
	if (key == KEY_CONFIRM && action != GLFW_RELEASE)
		pcp->quit = true;
}

int
main(void)
{
	struct pcp pcp = {0};

	GLFWwindow *window;
	struct Vec2i win = {WINW, WINH};
	if (!grInit(&pcp, &window, win, false, cbMouse, cbKeyboard, NULL))
		return EXIT_FAILURE;

	unsigned int vao = grImgGenVAO();

	pcp.hsvWheel.r.size = (struct Vec2f){WINW, WINW};
	pcp.hsvWheel.pos = (struct Vec2f){WINW / 2., WINW / 2.};
	grImgInitGr(&pcp.hsvWheel.sh, hsvWheelFragSrc);
	grImgUpdate(&pcp.hsvWheel.sh, pcp.hsvWheel.r, WINW, WINH);
	pcp.valBar.r.pos = (struct Vec2f){0, WINH - UI_VAL_HEIGHT};
	pcp.valBar.r.size = (struct Vec2f){WINW, UI_VAL_HEIGHT};
	pcp.color.a = 0xff;
	grImgInitGr(&pcp.valBar.sh, valBarFragSrc);
	grImgUpdate(&pcp.valBar.sh, pcp.valBar.r, WINW, WINH);

	while (!glfwWindowShouldClose(window) && !pcp.quit)
	{
		glClearColor(pcp.color.r / 255.f,
			     pcp.color.g / 255.f,
			     pcp.color.b / 255.f,
			     1);
		glClear(GL_COLOR_BUFFER_BIT);

		grDrawImage(pcp.hsvWheel.sh.id);
		grDrawImage(pcp.valBar.sh.id);

		glUseProgram(0);
		glColor3ub(0, 0, 0);
		grDrawMark(pcp.hsvWheel.pos, (struct Vec2f){WINW, WINH});
		glColor3ub(0xff, 0, 0);
		struct Vec2f markPos = {pcp.valBar.v * WINW,
					WINH - UI_VAL_HEIGHT / 2.};
		grDrawMark(markPos, (struct Vec2f){WINW, WINH});

		glfwSwapBuffers(window);
		glfwPollEvents();

		if (pcp.m0Down)
			mouseDown(&pcp, window);
	}

	printf("%02x%02x%02x%02x",
	       pcp.color.r,
	       pcp.color.g,
	       pcp.color.b,
	       pcp.color.a);

	glDeleteVertexArrays(1, &vao);
	glDeleteProgram(pcp.hsvWheel.sh.id);
	glDeleteProgram(pcp.valBar.sh.id);

	glfwTerminate();

	return EXIT_SUCCESS;
}
