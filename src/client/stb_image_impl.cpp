// Single translation unit that compiles the stb_image implementation. Only PNG
// is needed (our sprites). Keep this the ONLY place STB_IMAGE_IMPLEMENTATION is
// defined.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
