default: wavy

wavy: wavy.c
	gcc -lSDL2 -lm -o wavy wavy.c

run: wavy
	./wavy

test: wavy
	./wavy test.mp3

clean: wavy
	rm wavy

install: wavy
	cp wavy /usr/local/bin
