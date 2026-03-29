/* SPDX-License-Identifier: GPL-3.0-or-later
 * copyright 2025-2026 mannikim <mannikim[at]proton[dot]me>
 * this file is part of pie
 * see LICENSE file for the license text

pcp: mannikim's personal color picker

+++ todo +++
+++ end todo +++
*/

#include <math.h>
#include <stdlib.h>

#include "common.h"

enum SelectionType { SEL_NONE, SEL_HSV_WHEEL, SEL_VAL_BAR };

struct ColorHSV {
	double h, s, v;
};

struct ImgShader {
	unsigned int id, uWin, uTr;
};

struct HSVWheel {
	struct ImgShader sh;
	struct Transform tr;
	struct Vec2f pos;
	struct ColorHSV c;
};

struct ValueBar {
	struct ImgShader sh;
	struct Transform tr;
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
				   "#define PI 3.14159265358979\n"
				   "in vec2 texCoord;"
				   "out vec4 FragColor;"
				   "void main() {"
				   "FragColor = vec4(texCoord.xxx, 1);"
				   "}";

#define PI 3.14159265358979

#define WIN_TITLE "pcp"

#define WIDTH 300
#define HEIGHT 332

#define UI_VAL_HEIGHT 32

#define KEY_CONFIRM GLFW_KEY_Q

ALWAYS_INLINE double
mtFrac(double x)
{
	return x - (double)(int64_t)x;
}

ALWAYS_INLINE double
mtLerp(double x, double y, double a)
{
	return x + (y - x) * a;
}

ALWAYS_INLINE struct Vec2f
mtTransfromInvRel(struct Vec2f pos, struct Transform tr)
{
	return (struct Vec2f){pos.x * tr.size.x + tr.pos.x,
			      pos.y * tr.size.y + tr.pos.y};
}

ALWAYS_INLINE struct ColorRGBA
mtHSV2RGBA(struct ColorHSV c)
{
	double px = fabs(mtFrac(c.h + 1) * 6 - 3);
	double py = fabs(mtFrac(c.h + 2 / 3.) * 6 - 3);
	double pz = fabs(mtFrac(c.h + 1 / 3.) * 6 - 3);
	double pcx = mtClampd(px - 1, 0, 1);
	double pcy = mtClampd(py - 1, 0, 1);
	double pcz = mtClampd(pz - 1, 0, 1);
	uint8_t r = (uint8_t)(mtLerp(1, pcx, c.s) * c.v * 0xff);
	uint8_t g = (uint8_t)(mtLerp(1, pcy, c.s) * c.v * 0xff);
	uint8_t b = (uint8_t)(mtLerp(1, pcz, c.s) * c.v * 0xff);
	return (struct ColorRGBA){r, g, b, 0xff};
}

static void
inMouseCallback(GLFWwindow *window, int button, int action, int mod)
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
	if (mtBounds(m, pcp->hsvWheel.tr))
	{
		pcp->selection = SEL_HSV_WHEEL;
		return;
	}
	if (mtBounds(m, pcp->valBar.tr))
	{
		pcp->selection = SEL_VAL_BAR;
		return;
	}
}

static void
inKeyboardCallback(GLFWwindow *window, int key, int scan, int action, int mod)
{
	(void)scan, (void)mod;
	glfwMakeContextCurrent(window);
	struct pcp *pcp = glfwGetWindowUserPointer(window);
	if (key == KEY_CONFIRM && action != GLFW_RELEASE)
		pcp->quit = true;
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
grGenImgShader(struct ImgShader *sh, const char *vertSrc, const char *fragSrc)
{
	sh->id = grGenShader(vertSrc, fragSrc);
	sh->uTr = glGetUniformLocation(sh->id, "uTr");
	sh->uWin = glGetUniformLocation(sh->id, "uWin");
}

ALWAYS_INLINE void
grHSVWheelInitGr(struct HSVWheel *w)
{
	grGenImgShader(&w->sh, imgVertSrc, hsvWheelFragSrc);
	glUniform4f(w->sh.uTr,
		    w->tr.pos.x,
		    w->tr.pos.y,
		    w->tr.size.x,
		    w->tr.size.y);
	glUniform2f(w->sh.uWin, WIDTH, HEIGHT);
}

ALWAYS_INLINE void
grValBarInitGr(struct ValueBar *v)
{
	grGenImgShader(&v->sh, imgVertSrc, valBarFragSrc);
	glUniform4f(v->sh.uTr,
		    v->tr.pos.x,
		    v->tr.pos.y,
		    v->tr.size.x,
		    v->tr.size.y);
	glUniform2f(v->sh.uWin, WIDTH, HEIGHT);
}

ALWAYS_INLINE bool
grInit(struct pcp *pcp, GLFWwindow **window)
{
	glfwInit();

	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	*window = glfwCreateWindow(WIDTH, HEIGHT, WIN_TITLE, NULL, NULL);

	if (window == NULL)
		return false;

	glfwMakeContextCurrent(*window);
	glfwSetWindowUserPointer(*window, pcp);

	glfwSetMouseButtonCallback(*window, inMouseCallback);
	glfwSetKeyCallback(*window, inKeyboardCallback);

	glfwSwapInterval(0);

	glewInit();

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	return true;
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
	double s = mtClampd(sqrt(pow(pos.x, 2) + pow(pos.y, 2)) * 2, 0, 1);
	return (struct ColorHSV){h, s, 1};
}

ALWAYS_INLINE void
mouseDown(struct pcp *pcp, GLFWwindow *window)
{
	struct Vec2f m;
	glfwGetCursorPos(window, &m.x, &m.y);
	switch (pcp->selection)
	{
	case SEL_HSV_WHEEL:
	{
		struct Vec2f hsvRel = mtTransfromRel(m, pcp->hsvWheel.tr);
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
		pcp->hsvWheel.pos =
			mtTransfromInvRel(hsvRel, pcp->hsvWheel.tr);
		break;
	}
	case SEL_VAL_BAR:
	{
		double x =
			mtClampd(m.x,
				 pcp->valBar.tr.pos.x,
				 pcp->valBar.tr.size.x + pcp->valBar.tr.pos.x);
		pcp->valBar.v =
			(x - pcp->valBar.tr.pos.x) / pcp->valBar.tr.size.x;
		break;
	}
	default:
		break;
	}

	struct ColorHSV hsv = {
		pcp->hsvWheel.c.h, pcp->hsvWheel.c.s, pcp->valBar.v};
	pcp->color = mtHSV2RGBA(hsv);
}

int
main(void)
{
	struct pcp pcp = {0};

	GLFWwindow *window;
	if (!grInit(&pcp, &window))
		return EXIT_FAILURE;

	unsigned int vao = grImgGenVAO();

	pcp.hsvWheel.tr.size = (struct Vec2f){WIDTH, WIDTH};
	pcp.hsvWheel.pos = (struct Vec2f){WIDTH / 2., WIDTH / 2.};
	grHSVWheelInitGr(&pcp.hsvWheel);
	pcp.valBar.tr.pos = (struct Vec2f){0, HEIGHT - UI_VAL_HEIGHT};
	pcp.valBar.tr.size = (struct Vec2f){WIDTH, UI_VAL_HEIGHT};
	pcp.color.a = 0xff;
	grValBarInitGr(&pcp.valBar);

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
		grDrawMark(pcp.hsvWheel.pos, (struct Vec2f){WIDTH, HEIGHT});
		glColor3ub(0xff, 0, 0);
		struct Vec2f markPos = {pcp.valBar.v * WIDTH,
					HEIGHT - UI_VAL_HEIGHT / 2.};
		grDrawMark(markPos, (struct Vec2f){WIDTH, HEIGHT});

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
