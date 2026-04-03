#---------------------------------------------------------------------------------
# gl2citro3d - OpenGL ES 1.1 -> Citro3D Translation Layer
#
# Top-level Makefile: builds the static library and all examples
#---------------------------------------------------------------------------------

.PHONY: all lib examples clean

all: lib examples

lib:
	@$(MAKE) -C src

examples: lib
	@$(MAKE) -C examples/01_triangle
	@$(MAKE) -C examples/02_textured_quad
	@$(MAKE) -C examples/03_rotating_cube

clean:
	@$(MAKE) -C src clean
	@$(MAKE) -C examples/01_triangle clean
	@$(MAKE) -C examples/02_textured_quad clean
	@$(MAKE) -C examples/03_rotating_cube clean
