pie: pie.c
	$(CC) $< -o $@ -lGL -lglfw -lGLEW -lm -std=c99
