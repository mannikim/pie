pie: pie.c
	$(CC) $< -o $@ -lGL -lglfw -lGLEW -lm -Os -std=c99
