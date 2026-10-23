OBS ?= .deps/obs-studio/libobs
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -fPIC -I$(OBS) -I.deps/include -I.deps/simde
build/obs-screen-glide.so: src/plugin.c
	@mkdir -p build
	$(CC) $(CFLAGS) -shared -o $@ $< -lm
clean:
	rm -rf build
